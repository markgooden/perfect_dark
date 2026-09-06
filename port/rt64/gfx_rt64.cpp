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

    /* Totals reported at shutdown. The framebuffer path leaves no other trace:
     * a copy is requested between frames and a created framebuffer may go
     * unused for the whole run, so without these a clean log says nothing
     * about whether either was exercised. */
    uint64_t totalBlits = 0;
    uint32_t framebuffersCreated = 0;
    uint64_t droppedPerOpcode[256] = {};

    /* Per-frame CSV for the perf gate (SCAFFOLD T12, risk R1). Written from
     * here rather than derived from the log because the numbers that decide
     * the gate - translate time against wall-clock frame time - only exist
     * together at this point in the frame. */
    FILE *statsCsv = nullptr;
    double lastFrameTime = 0.0;

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
    /* The framebuffers the game asked to be "the same size as the main one"
     * follow the mode too, or the pause blur composites a screen-sized image
     * through a rectangle sized for the previous mode. */
    g_be.fbs->setNativeSize(w, h);
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
    /* Before any createFb call: three of the game's four framebuffers ask for
     * "the same size as the main one" and are resolved against this. */
    g_be.fbs->setNativeSize(
        gfx_current_native_viewport.width ? gfx_current_native_viewport.width : 320,
        gfx_current_native_viewport.height ? gfx_current_native_viewport.height : 220);

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

    const char *statsPath = sysArgGetString("--rt64-stats");
    if (statsPath && *statsPath) {
        g_be.statsCsv = fopen(statsPath, "w");
        if (g_be.statsCsv) {
            fprintf(g_be.statsCsv,
                "frame,commandsIn,commandsOut,dropped,trianglesIn,marshalledBytes,"
                "texBlocks,texHits,texMisses,blits,translateMs,frameMs\n");
            sysLogPrintf(LOG_NOTE, "rt64: writing per-frame stats to %s", statsPath);
        } else {
            sysLogPrintf(LOG_WARNING, "rt64: could not open %s for stats", statsPath);
        }
    }

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
    sysLogPrintf(LOG_NOTE, "rt64: %llu frames, %u framebuffers created, %llu blits",
        (unsigned long long)g_be.frameCount, g_be.framebuffersCreated,
        (unsigned long long)g_be.totalBlits);

    /* Which opcodes were dropped, over the whole run. Per-frame counters reset
     * every frame and the CSV records only the total, so without this a run
     * that dropped tens of thousands of commands cannot say what they were -
     * which is precisely the list T13 has to account for. */
    uint64_t droppedTotal = 0;
    for (int i = 0; i < 256; ++i) {
        droppedTotal += g_be.droppedPerOpcode[i];
    }
    if (droppedTotal) {
        sysLogPrintf(LOG_NOTE, "rt64: %llu commands dropped across the run:",
            (unsigned long long)droppedTotal);
        for (int i = 0; i < 256; ++i) {
            if (!g_be.droppedPerOpcode[i]) {
                continue;
            }
            const char *name = gfxOpcodeName((uint8_t)i);
            sysLogPrintf(LOG_NOTE, "rt64:   0x%02x %-24s %llu", i, name ? name : "?",
                (unsigned long long)g_be.droppedPerOpcode[i]);
        }
    }
    /* The host goes first: RT64 holds the arena and the register block, and
     * must stop reading them before they are freed. */
    if (g_be.statsCsv) {
        fclose(g_be.statsCsv);
        g_be.statsCsv = nullptr;
    }
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

    const TranslateStats &fs = g_be.tr->stats();
    g_be.totalBlits += fs.framebufferBlits;
    for (int i = 0; i < 256; ++i) {
        g_be.droppedPerOpcode[i] += fs.droppedPerOpcode[i];
    }

    if (g_be.statsCsv) {
        /* Wall-clock frame time, measured across the same boundary the port's
         * own frame loop uses, so it includes RT64's present and any wait the
         * framerate limiter imposed. Comparing translateMs against it is the
         * whole point: a translator inside budget on a frame that took 30 ms
         * has not proved anything. */
        const double now = g_be.wapi->get_time();
        const double frameMs = g_be.lastFrameTime > 0.0 ? (now - g_be.lastFrameTime) * 1000.0 : 0.0;
        g_be.lastFrameTime = now;
        fprintf(g_be.statsCsv, "%llu,%u,%u,%u,%u,%llu,%u,%u,%u,%u,%.4f,%.4f\n",
            (unsigned long long)g_be.frameCount, fs.commandsIn, fs.commandsOut,
            fs.droppedCommands, fs.trianglesIn, (unsigned long long)fs.marshalledBytes,
            fs.textureBlocks, fs.textureCacheHits, fs.textureCacheMisses,
            fs.framebufferBlits, fs.translateMs, frameMs);
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
 * `upscale` is fast3d's business: it renders framebuffers at window resolution
 * and scales the small ones up. Here every image is native-resolution in the
 * arena and RT64 upscales the lot internally, so there is nothing to select.
 * `autoresize` does matter, and reaches the registry.
 */
int rt64CreateFramebuffer(uint32_t width, uint32_t height, int upscale, int autoresize)
{
    (void)upscale;
    if (!g_be.fbs) {
        return 0;
    }
    const int fb = g_be.fbs->createFb(width, height, autoresize != 0);
    uint32_t w = 0, h = 0;
    g_be.fbs->fbSize(fb, &w, &h);
    /* Four or five per run, so logging each one costs nothing and is the only
     * record that the game asked for a framebuffer at all. */
    sysLogPrintf(LOG_NOTE, "rt64: framebuffer %d = %ux%u%s", fb, w, h,
        (autoresize || !width || !height) ? " (follows the video mode)" : "");
    ++g_be.framebuffersCreated;
    return fb;
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
 * Retarget subsequent drawing at one of the game's framebuffers.
 *
 * Nothing in this codebase calls videoSetFramebuffer or videoResetFramebuffer
 * - checked across src/ and port/, the only references are the declarations
 * and video.c's own wrappers - so this mapping is unexercised. It is written
 * anyway, and written to be the obvious thing rather than nothing, because a
 * silent no-op here would be indistinguishable from a rendering bug if a
 * caller ever appeared.
 *
 * `noiseScale` is fast3d's dither seed for the framebuffer it is about to draw
 * into; RT64 has no equivalent knob on this path.
 */
void rt64SetFramebuffer(int fb, float noiseScale)
{
    (void)noiseScale;
    if (!g_be.ready || fb <= 0) {
        return;
    }
    g_be.tr->setMainColorImage(g_be.fbs->fbAddress(fb));
}

void rt64ResetFramebuffer(void)
{
    if (!g_be.ready) {
        return;
    }
    /* Back to whichever main colour image this frame belongs to, not to
     * image 0 - they alternate. */
    g_be.tr->setMainColorImage(g_be.fbs->mainColorImage(g_be.parity));
}

/*
 * Copy the frame that was just rendered into one of the game's framebuffers.
 *
 * This is how the pause blur is seeded: the whole screen is downscaled into a
 * 40x30 buffer (menugfx.c:133) which the display list then magnifies back up
 * with G_TF_BLUR_EXT, and the scheduler does the same into g_BlurFb every
 * frame it is asked to (pdsched.c:392).
 *
 * It arrives between frames - schedConsiderScreenshot runs after videoEndFrame
 * (pdsched.c:300-303) - so the source is the image the frame just ended drew
 * into, which is parity^1 by the time this is called. Recording the resolved
 * address rather than the handle is what makes that unambiguous. The copy
 * itself is queued and performed as commands at the head of the next stream:
 * RT64 has no copy API, and doing it on the CPU would mean reading the
 * framebuffer back every frame, which is the one thing render-to-RAM exists to
 * avoid in normal play.
 */
void rt64CopyFramebuffer(int dst, int src, int left, int top, int useBack)
{
    (void)left;
    (void)top;
    (void)useBack;
    if (!g_be.ready || dst <= 0) {
        return;
    }

    Translator::FbBlitRequest req;
    req.dstFb = dst;
    if (src <= 0) {
        req.srcImage = g_be.fbs->mainColorImage(g_be.parity ^ 1);
        req.srcWidth = g_be.nativeWidth;
        req.srcHeight = g_be.nativeHeight;
    } else {
        req.srcImage = g_be.fbs->fbAddress(src);
        g_be.fbs->fbSize(src, &req.srcWidth, &req.srcHeight);
    }
    g_be.tr->queueFramebufferBlit(req);
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
