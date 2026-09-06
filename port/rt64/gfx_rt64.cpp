/*
 * RT64 backend for the port's frontend graphics contract (SCAFFOLD task T10).
 *
 * Composition only. Every piece this file reaches for is already written and
 * already tested against captures: the arena, the marshaller, the translator,
 * the register block, the host wrapper. What was missing was the thing that
 * runs them against live game memory once per frame, and that is all this is.
 *
 * The seam it sits on is described in ../fast3d/gfx_graphics_api.h. Two
 * consequences of that seam shape this file:
 *
 * The eight globals gfx_api.h declares are shared state, not fast3d's. video.c
 * reads seven of them directly, so a backend that implements only the
 * functions leaves the window dimensions, native viewport and MSAA level
 * stale - a failure that presents as the game drawing at the wrong size rather
 * than as anything to do with rendering. start_frame updates them here for the
 * same reason f3d_start_frame does.
 *
 * Nothing here includes an RT64 header. The port builds under MinGW and RT64
 * only under MSVC, so every call into the renderer goes through rt64_host.h,
 * whose implementation for this build loads rt64shim.dll (docs/BUILD-RT64.md).
 * That is also why a failure to reach RT64 is reported as a result code with a
 * name rather than as a missing symbol at link time.
 */

#include <cstdint>
#include <cstdio>
#include <memory>

#include "rt64_arena.h"
#include "rt64_host.h"
#include "rt64_mem.h"
#include "rt64_regions.h"
#include "rt64_registers.h"
#include "rt64_translate.h"

extern "C" {
#include "platform.h"
#include "system.h"
#include "config.h"
#include "../fast3d/gfx_graphics_api.h"
#include "rt64_capture.h"
}

/* The port globals the live reader spans. Declared rather than included: the
 * headers that define them drag in the game's own types, and this file has no
 * business with those. Matches how rt64_capture.cpp reaches them. */
extern "C" {
extern u8 *g_MempHeap;
extern u32 g_MempHeapSize;
extern u8 *g_RomFile;
extern u32 g_RomFileSize;
}

namespace {

using namespace pdrt64;

/* Config, registered below. Sizes are megabytes. */
s32 g_rdramMB = 128;
s32 g_validate = -1;    // -1 = build default, 0 = off, 1 = on

/*
 * How the synthetic RDRAM is divided.
 *
 * The framebuffer region holds three full 640x480 RGBA16 images up front
 * (FbRegistry) plus whatever the game's own framebuffer handles need, and it
 * must stay below 16 MB: RT64 masks VI_ORIGIN to 24 bits when it decodes which
 * image is being scanned out (rt64_application.cpp:48), so a colour image
 * above that line would present nothing while the geometry looked perfect.
 *
 * The frame region is sized off the census rather than guessed: the worst
 * gameplay frame measured was ~10,500 commands and ~396 KB of referenced data
 * (docs/census.md), so 32 MB is roughly eighty times the worst case seen. The
 * rest is the persistent texture cache.
 */
constexpr size_t kFbRegionBytes = 12u << 20;
constexpr size_t kFrameRegionBytes = 32u << 20;

struct Backend {
    struct GfxWindowManagerAPI *wapi = nullptr;

    std::unique_ptr<Arena> arena;
    std::unique_ptr<FbRegistry> fbs;
    std::unique_ptr<LiveMemReader> mem;
    std::unique_ptr<Translator> tr;
    Registers regs = {};

    uint32_t nativeWidth = 0;
    uint32_t nativeHeight = 0;

    /* Which of the two main colour images this frame draws into. The VI
     * alternates between them so RT64's present logic sees a flip, the way the
     * game's own double buffering would. */
    int parity = 0;

    bool ready = false;
    /* Render-to-RAM writes the native-resolution output back into the arena so
     * it can be hashed. Off unless asked for: it costs a readback per frame. */
    bool hashFrames = false;
    uint64_t frameCount = 0;

    /* One line per distinct translation failure, not one per frame. A backend
     * that has started failing every list would otherwise fill the log faster
     * than it could be read. */
    bool reportedTranslateError = false;
};

Backend g_be;

/* sysMemIsTracked returns s32 and takes u32; the translator's predicate is
 * bool(uintptr_t, size_t). Adapting here keeps rt64_mem.cpp free of port
 * headers, which is what lets the unit tests compile it with nothing. */
bool trackedAlloc(uintptr_t addr, size_t len)
{
    return sysMemIsTracked((const void *)addr, (u32)len) != 0;
}

/* Validation defaults to on in debug builds and in every test run
 * (invariant 6). Video.RT64Validate can force it either way; it exists so a
 * performance measurement is not measuring the validator. */
bool validationDefault()
{
#ifdef NDEBUG
    return false;
#else
    return true;
#endif
}

/*
 * The game's native resolution is dynamic - pdsched.c:213 switches between
 * lo- and hi-res - so the VI register block has to be re-derived rather than
 * set once. Called every frame; does nothing until the mode actually changes.
 */
void syncNativeMode()
{
    const uint32_t w = gfx_current_native_viewport.width;
    const uint32_t h = gfx_current_native_viewport.height;
    if (!w || !h || (w == g_be.nativeWidth && h == g_be.nativeHeight)) {
        return;
    }
    g_be.nativeWidth = w;
    g_be.nativeHeight = h;
    regsSetVideoMode(&g_be.regs, w, h);
    sysLogPrintf(LOG_NOTE, "rt64: native mode %ux%u", w, h);
}

void rt64Init(const struct GfxInitSettings *settings)
{
    g_be.wapi = settings->wapi;

    /* The window comes first: RT64 creates its device and swap chain against
     * it, and it must not carry a GL context (task T9 - the no_gl flag is set
     * in videoInit from the selected backend). */
    g_be.wapi->init(&settings->window_settings);

    /* The same globals f3d_init seeds. video.c reads these directly. */
    gfx_current_dimensions.internal_mul = 1;
    gfx_current_game_window_viewport.width = gfx_current_dimensions.width =
        settings->window_settings.width;
    gfx_current_game_window_viewport.height = gfx_current_dimensions.height =
        settings->window_settings.height;

    if (gfx_msaa_level > 1) {
        /* RT64 owns its own sampling; the port's MSAA level does not reach it,
         * and leaving the global set would have video.c reporting a setting
         * that does nothing. */
        sysLogPrintf(LOG_WARNING, "rt64: Video.MSAA=%u ignored; RT64 manages its own sampling",
            gfx_msaa_level);
        gfx_msaa_level = 1;
    }

    regionsInit();
    if (!regionsImageSize()) {
        /* Light and viewport blocks reached via G_MOVEMEM live in the
         * executable's image (docs/census.md). Without the span they classify
         * as unreadable and translation fails on the first one, so say why
         * here rather than let it look like a translator bug. */
        sysLogPrintf(LOG_WARNING, "rt64: could not find the executable image span; "
            "display lists referencing static data will fail to translate");
    }

    const size_t totalBytes = (size_t)g_rdramMB << 20;
    if (totalBytes <= kFbRegionBytes + kFrameRegionBytes) {
        sysFatalError("rt64: Video.RT64RdramMB=%d is too small; needs more than %u MB",
            (int)g_rdramMB, (unsigned)((kFbRegionBytes + kFrameRegionBytes) >> 20));
    }

    ArenaConfig cfg;
    cfg.totalBytes = totalBytes;
    cfg.fbRegionBytes = kFbRegionBytes;
    cfg.frameRegionBytes = kFrameRegionBytes;
    g_be.arena.reset(new Arena(cfg));
    g_be.fbs.reset(new FbRegistry(*g_be.arena));

    g_be.mem.reset(new LiveMemReader(g_MempHeap, g_MempHeapSize, g_RomFile, g_RomFileSize,
        (const uint8_t *)regionsImageBase(), regionsImageSize(), &trackedAlloc));

    g_be.tr.reset(new Translator(*g_be.arena, *g_be.fbs, *g_be.mem));
    /* --rt64-validate wins over the config value, which wins over the build
     * default, mirroring how --renderer overrides Video.Renderer. The flag
     * exists because the bring-up runs have to assert a clean validation log
     * on a release build, where the default is off. */
    const s32 validateArg = sysArgGetInt("--rt64-validate", -1);
    const s32 validateCfg = (validateArg >= 0) ? validateArg : g_validate;
    const bool validate = (validateCfg < 0) ? validationDefault() : (validateCfg != 0);
    g_be.tr->setValidationEnabled(validate);

    g_be.hashFrames = sysArgCheck("--rt64-hash") != 0;
    g_be.tr->setRenderToRam(g_be.hashFrames);

    /* Seed the register block before RT64 reads it. The native viewport is
     * whatever videoInit set; syncNativeMode picks up every later change. */
    const uint32_t w = gfx_current_native_viewport.width ? gfx_current_native_viewport.width : 320;
    const uint32_t h = gfx_current_native_viewport.height ? gfx_current_native_viewport.height : 220;
    regsInit(&g_be.regs, w, h);
    g_be.nativeWidth = w;
    g_be.nativeHeight = h;

    void *native = g_be.wapi->get_native_window_handle();
    if (!native) {
        sysFatalError("rt64: the window layer returned no native handle.\n"
            "RT64 needs one to create its swap chain.");
    }

    HostConfig host;
    host.nativeWindow = native;
    host.rdram = g_be.arena->rdramBase();
    host.rdramSize = g_be.arena->rdramSize();
    host.regs = &g_be.regs;
    host.dataPath = nullptr;    // let RT64 detect its own

    const HostResult r = hostInit(host);
    if (r != HostResult::Ok) {
        sysFatalError("rt64: could not start the renderer: %s\n"
            "Set Video.Renderer=0 or pass --renderer opengl to use the OpenGL path.",
            hostResultName(r));
    }

    g_be.ready = true;
    sysLogPrintf(LOG_NOTE, "rt64: ready - %d MB arena, validation %s, native %ux%u",
        (int)g_rdramMB, validate ? "on" : "off", w, h);
}

void rt64Destroy(void)
{
    if (!g_be.ready) {
        return;
    }
    g_be.ready = false;
    /* The host goes first: RT64 holds the arena and the register block, and
     * must stop reading them before they are freed. */
    hostShutdown();
    g_be.tr.reset();
    g_be.mem.reset();
    g_be.fbs.reset();
    g_be.arena.reset();
}

struct GfxRenderingAPI *rt64GetRenderingApi(void)
{
    /* RT64 is not layered on a GfxRenderingAPI - it replaces the whole
     * frontend, not the triangle backend under it (SCAFFOLD ADR-2). */
    return NULL;
}

void rt64StartFrame(void)
{
    if (!g_be.ready) {
        return;
    }

    g_be.wapi->handle_events();
    int32_t posX = 0, posY = 0;
    g_be.wapi->get_dimensions(&gfx_current_window_dimensions.width,
        &gfx_current_window_dimensions.height, &posX, &posY);

    if (gfx_current_window_dimensions.height == 0) {
        gfx_current_window_dimensions.height = 1;  // avoid division by zero
    }
    gfx_current_window_dimensions.aspect_ratio =
        (float)gfx_current_window_dimensions.width / (float)gfx_current_window_dimensions.height;

    gfx_current_dimensions = gfx_current_window_dimensions;
    gfx_current_game_window_viewport.width = gfx_current_dimensions.width;
    gfx_current_game_window_viewport.height = gfx_current_dimensions.height;

    syncNativeMode();

    /* Refreshed per frame rather than installed once, so the translator's
     * per-command cost is a null test whenever capture is idle - the same deal
     * fast3d gets. pdCaptureCommandHook becomes non-null only while capture is
     * armed, which can happen at any time via the F9 hotkey. */
    g_be.tr->setCommandHook(pdCaptureCommandHook);

    g_be.arena->beginFrame();
    g_be.tr->beginFrame();

    /* The image the VI will scan out and the image the translator draws into
     * have to be the same one, set together. They were not, until T8: the
     * translator always named image 0 while the VI alternated, so half the
     * presented frames came from a buffer nothing had written. */
    const RdramAddr colorImage = g_be.fbs->mainColorImage(g_be.parity);
    regsSetScanout(&g_be.regs, colorImage);
    g_be.tr->setMainColorImage(colorImage);
}

void rt64Run(Gfx *commands)
{
    if (!g_be.ready || !commands) {
        return;
    }

    RdramAddr start = 0, end = 0;
    const TranslateStatus s = g_be.tr->translate((uintptr_t)commands, &start, &end);
    if (s != TranslateStatus::Ok) {
        if (!g_be.reportedTranslateError) {
            g_be.reportedTranslateError = true;
            sysLogPrintf(LOG_ERROR, "rt64: translation failed: %s (further failures are silent)",
                g_be.tr->lastError().c_str());
        }
        return;
    }

    hostProcessDl(start, end);
}

void rt64EndFrame(void)
{
    if (!g_be.ready) {
        return;
    }

    hostUpdateScreen();

    if (g_be.hashFrames) {
        /* Render-to-RAM put the native-resolution output back in the arena.
         * Hashing it is what lets an in-game frame be compared against the
         * same display list replayed through dlreplay - same code path, same
         * inputs, so the two hashes must match exactly. */
        const RdramAddr img = g_be.fbs->mainColorImage(g_be.parity);
        const size_t bytes = (size_t)g_be.nativeWidth * (size_t)g_be.nativeHeight * 2u;
        const uint64_t hash = hashRdramRange(g_be.arena->rdramBase(), img, bytes);
        const TranslateStats &st = g_be.tr->stats();
        sysLogPrintf(LOG_NOTE, "rt64: frame %llu image %d %ux%u hash %016llx (%u in -> %u out, %u dropped)",
            (unsigned long long)g_be.frameCount, g_be.parity, g_be.nativeWidth, g_be.nativeHeight,
            (unsigned long long)hash, st.commandsIn, st.commandsOut, st.droppedCommands);
    }

    g_be.parity ^= 1;
    ++g_be.frameCount;

    /* The port's framerate limiter still applies; the no-GL path's
     * swap_buffers_begin does the pacing and skips the GL swap, because RT64
     * has already presented (task T9). */
    g_be.wapi->swap_buffers_begin();
    g_be.wapi->swap_buffers_end();
}

void rt64SetTargetFps(int fps)
{
    if (g_be.wapi) {
        g_be.wapi->set_target_fps(fps);
    }
}

/*
 * Texture filtering is not wired to RT64 yet: hostSetTextureFiltering is
 * declared in the interface header but has no implementation and no export on
 * the shim (docs/TASKLOG.md). RT64 applies its own enhancement configuration
 * meanwhile, so these are accepted and dropped rather than refused - refusing
 * would make the options menu unusable on this path for a setting that is
 * cosmetic here.
 */
void rt64SetTextureFilter(enum FilteringMode mode) { (void)mode; }
void rt64SetMipmapFilter(enum MipmapFilteringMode mode) { (void)mode; }
int rt64GetMaxAnisotropyLevel(void) { return 1; }
void rt64SetAnisotropyLevel(int level) { (void)level; }

/*
 * Texture-cache invalidation. The arena's persistent region keys marshalled
 * blocks on their source pointer, which is the same contract fast3d's own
 * texture cache runs on (gfx_pc.h:23-28) and is invalidated by these same
 * calls - so this is no more of an assumption than the renderer being matched
 * already makes.
 */
void rt64TextureCacheClear(void)
{
    if (g_be.arena) {
        g_be.arena->clearCache();
    }
}

void rt64TextureCacheDelete(const uint8_t *addr)
{
    if (g_be.arena) {
        g_be.arena->invalidateCache((uintptr_t)addr);
    }
}

void rt64TextureCacheDeleteRange(const uint8_t *start, const uint8_t *end)
{
    if (g_be.arena) {
        g_be.arena->invalidateCacheRange((uintptr_t)start, (uintptr_t)end);
    }
}

/*
 * Offscreen framebuffers. The registry allocates colour images in the arena
 * and the translator turns the port's _EXT framebuffer commands into canonical
 * G_SETCIMG / G_SETTIMG against them, so RT64's own framebuffer manager
 * handles the copies and reinterpretation (SCAFFOLD ADR-4).
 *
 * upscale and autoresize are fast3d's business: it renders framebuffers at
 * window resolution and has to be told which ones follow a resize. Here every
 * image is native-resolution in the arena and RT64 upscales internally, so
 * there is nothing for the flags to select.
 */
int rt64CreateFramebuffer(uint32_t width, uint32_t height, int upscale, int autoresize)
{
    (void)upscale;
    (void)autoresize;
    if (!g_be.fbs) {
        return 0;
    }
    return g_be.fbs->createFb(width, height);
}

void rt64ResizeFramebuffer(int fb, uint32_t width, uint32_t height, int upscale, int autoresize)
{
    (void)upscale;
    (void)autoresize;
    if (g_be.fbs) {
        g_be.fbs->resizeFb(fb, width, height);
    }
}

/*
 * Binding a framebuffer as the render target, and the blur/noise effects on
 * top of it, are task T11. The translator already lowers the display list's
 * own _EXT framebuffer commands, which is what the menus actually use; these
 * three entry points are the video.c-driven half and are inert until T11.
 */
void rt64SetFramebuffer(int fb, float noiseScale) { (void)fb; (void)noiseScale; }
void rt64ResetFramebuffer(void) {}
void rt64CopyFramebuffer(int dst, int src, int left, int top, int useBack)
{
    (void)dst;
    (void)src;
    (void)left;
    (void)top;
    (void)useBack;
}

} // namespace

extern "C" const struct GfxGraphicsAPI gfx_rt64_api = {
    "rt64",
    rt64Init,
    rt64Destroy,
    rt64GetRenderingApi,
    rt64StartFrame,
    rt64Run,
    rt64EndFrame,
    rt64SetTargetFps,
    rt64SetTextureFilter,
    rt64SetMipmapFilter,
    rt64TextureCacheClear,
    rt64TextureCacheDelete,
    rt64TextureCacheDeleteRange,
    rt64CreateFramebuffer,
    rt64ResizeFramebuffer,
    rt64SetFramebuffer,
    rt64ResetFramebuffer,
    rt64CopyFramebuffer,
    rt64GetMaxAnisotropyLevel,
    rt64SetAnisotropyLevel,
};

PD_CONSTRUCTOR static void rt64ConfigInit(void)
{
    /* Upper bound is 1024 rather than 2048 so every arena offset stays inside
     * 31 bits with room to spare (rt64_mem.h invariant 4). */
    configRegisterInt("Video.RT64RdramMB", &g_rdramMB, 64, 1024);
    configRegisterInt("Video.RT64Validate", &g_validate, -1, 1);
}
