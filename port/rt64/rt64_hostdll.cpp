/*
 * The port side of the DLL boundary: implements rt64_host.h by loading
 * rt64shim.dll and calling the C surface in rt64_shimabi.h.
 *
 * This exists because the two halves cannot be linked. The port builds under
 * MinGW only and RT64 under MSVC only (docs/BUILD-RT64.md), so the RT64 half
 * is compiled separately into a DLL that exports nothing but C functions, and
 * this file is the only thing on our side that knows the DLL exists. Callers
 * see the same pdrt64::host* declarations they would see in a direct-link
 * build, which is what lets the replay harness link RT64 directly and the game
 * go through the DLL while both exercise one translator.
 *
 * hostResultName lives here rather than beside either implementation because
 * this is the one translation unit compiled in all three configurations (see
 * rt64_host.cpp's header comment); putting it in the guarded halves would mean
 * two copies of one switch, kept in step by hope.
 *
 * Diagnostics go to stderr rather than through the port's logging, for the
 * same reason nothing else here includes a port header: this file is compiled
 * by the MSVC harness build too. The messages are the ones you actually need
 * when the boundary fails - which file was looked for, which symbol was
 * missing, what Windows said - and they are printed at most once per run.
 */

#include "rt64_host.h"

#include <cstdio>

namespace pdrt64 {

const char *hostResultName(HostResult r)
{
    switch (r) {
    case HostResult::Ok:                       return "ok";
    case HostResult::NotCompiledIn:            return "built without RT64";
    case HostResult::AlreadyInitialised:       return "already initialised";
    case HostResult::BadConfig:                return "incomplete host configuration";
    case HostResult::DynamicLibrariesNotFound: return "graphics dynamic libraries not found";
    case HostResult::InvalidGraphicsApi:       return "invalid graphics API";
    case HostResult::GraphicsApiNotFound:      return "graphics API not found";
    case HostResult::GraphicsDeviceNotFound:   return "no usable graphics device";
    case HostResult::UcodeSelectionFailed:     return "could not select the F3DPD microcode";
    case HostResult::ShimException:            return "the renderer DLL threw";
    case HostResult::ShimAbiMismatch:          return "rt64shim.dll does not match this build";
    case HostResult::ShimNotFound:             return "rt64shim.dll could not be loaded";
    case HostResult::ShimEntryPointMissing:    return "rt64shim.dll is missing an entry point";
    case HostResult::Unknown:                  break;
    }
    return "unknown";
}

} // namespace pdrt64

#ifdef PDRT64_WITH_RT64_DLL

#ifndef _WIN32
#error "PDRT64_WITH_RT64_DLL is a Windows-only workaround for the MinGW/MSVC toolchain split; elsewhere both halves are built by one compiler and link directly with PDRT64_WITH_RT64."
#endif

#include <windows.h>

#include <cwchar>

#include "rt64_lights.h"

namespace pdrt64 {

namespace {

using AbiVersionFn = uint32_t (*)(void);
using InitFn = int32_t (*)(const Rt64ShimConfig *);
using ShutdownFn = void (*)(void);
using ReadyFn = int32_t (*)(void);
using ProcessDlFn = void (*)(uint32_t, uint32_t);
using UpdateScreenFn = void (*)(void);
using SetTextureFilteringFn = void (*)(int32_t, int32_t, uint32_t);
using SetRoomLightsFn = void (*)(const Rt64ShimLight *, int32_t);

struct Shim {
    HMODULE module = nullptr;
    AbiVersionFn abiVersion = nullptr;
    InitFn init = nullptr;
    ShutdownFn shutdown = nullptr;
    ReadyFn ready = nullptr;
    ProcessDlFn processDl = nullptr;
    UpdateScreenFn updateScreen = nullptr;
    SetTextureFilteringFn setTextureFiltering = nullptr;
    SetRoomLightsFn setRoomLights = nullptr;

    /* The port sets the texture filter from videoInit (video.c:175), which can
     * run before the DLL is up. Remembered here and replayed after init, so
     * the two builds - this one and the direct one in rt64_host.cpp - behave
     * the same way for a caller that sets it early. Default 1 is the port's
     * own FILTER_LINEAR (video.c:59). */
    int32_t filterMode = 1;
    bool live = false;      // init succeeded and shutdown has not run
};

Shim g_shim;

/*
 * GetProcAddress returns FARPROC, and casting a function pointer of one shape
 * straight to another is what -Wcast-function-type warns about. The round trip
 * through void * is the documented way to say "yes, I mean it" - the ABI
 * header is what guarantees the shapes agree, not the cast.
 */
template <typename Fn>
bool resolve(HMODULE module, const char *name, Fn *out)
{
    FARPROC p = GetProcAddress(module, name);
    if (p == nullptr) {
        fprintf(stderr, "rt64shim: %s is missing from " PDRT64_SHIM_DLL_NAME "\n", name);
        return false;
    }
    *out = reinterpret_cast<Fn>(reinterpret_cast<void *>(p));
    return true;
}

/*
 * The DLL is loaded by absolute path from the directory holding the running
 * executable, never by bare name.
 *
 * By bare name the search would reach the working directory and then PATH, so
 * whichever rt64shim.dll a user happened to have somewhere could be loaded
 * against a mismatched executable - and since it carries a whole statically
 * linked renderer, that failure would surface as graphics corruption rather
 * than as a load error.
 *
 * LOAD_WITH_ALTERED_SEARCH_PATH makes the DLL's own directory the first place
 * its dependencies are looked for. That matters because rt64shim.dll imports
 * SDL2.dll, and the default order would search the executable's directory
 * instead - the same directory today, but not if the renderer is ever staged
 * in a subdirectory of its own. RT64's other runtime dependencies do not need
 * it: it loads dxil.dll and dxcompiler.dll itself, by a path built from the
 * module it is linked into (rt64_dynamic_libraries.cpp:32-53), which is
 * rt64shim.dll. So those two must sit beside the DLL, not beside the exe.
 */
HMODULE loadShim()
{
    WCHAR path[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        fprintf(stderr, "rt64shim: could not locate the executable (error %lu)\n",
                GetLastError());
        return nullptr;
    }

    WCHAR *slash = nullptr;
    for (WCHAR *p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') {
            slash = p;
        }
    }
    if (slash == nullptr) {
        fprintf(stderr, "rt64shim: executable path has no directory\n");
        return nullptr;
    }
    slash[1] = L'\0';

    static const WCHAR kName[] = L"" PDRT64_SHIM_DLL_NAME;
    if (wcslen(path) + wcslen(kName) >= MAX_PATH) {
        fprintf(stderr, "rt64shim: path to the executable is too long\n");
        return nullptr;
    }
    wcscat(path, kName);

    HMODULE module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
        /* Error 126 here usually means the DLL was found but one of its own
         * dependencies was not, which is the common staging mistake. */
        fprintf(stderr, "rt64shim: LoadLibraryExW(%ls) failed with error %lu\n", path,
                GetLastError());
    }
    return module;
}

/* Resolves every entry point once. Kept separate from hostInit so a partially
 * resolved module is never left behind for a later call to trip over. */
HostResult openShim()
{
    if (g_shim.module != nullptr) {
        return HostResult::Ok;
    }

    HMODULE module = loadShim();
    if (module == nullptr) {
        return HostResult::ShimNotFound;
    }

    Shim s;
    s.module = module;
    if (!resolve(module, "rt64ShimAbiVersion", &s.abiVersion) ||
        !resolve(module, "rt64ShimInit", &s.init) ||
        !resolve(module, "rt64ShimShutdown", &s.shutdown) ||
        !resolve(module, "rt64ShimReady", &s.ready) ||
        !resolve(module, "rt64ShimProcessDl", &s.processDl) ||
        !resolve(module, "rt64ShimUpdateScreen", &s.updateScreen) ||
        !resolve(module, "rt64ShimSetTextureFiltering", &s.setTextureFiltering) ||
        !resolve(module, "rt64ShimSetRoomLights", &s.setRoomLights)) {
        FreeLibrary(module);
        return HostResult::ShimEntryPointMissing;
    }

    /* Asked before anything else is called, so a DLL built against a different
     * boundary is refused rather than handed a struct it will misread. */
    const uint32_t theirs = s.abiVersion();
    if (theirs != PDRT64_SHIM_ABI_VERSION) {
        fprintf(stderr,
                "rt64shim: " PDRT64_SHIM_DLL_NAME " speaks ABI %u, this build speaks %u."
                " Rebuild tools/rt64shim.\n",
                theirs, PDRT64_SHIM_ABI_VERSION);
        FreeLibrary(module);
        return HostResult::ShimAbiMismatch;
    }

    g_shim = s;
    return HostResult::Ok;
}

} // namespace

HostResult hostInit(const HostConfig &cfg)
{
    if (g_shim.live) {
        return HostResult::AlreadyInitialised;
    }
    /* Checked here as well as in the DLL: a null that never crosses cannot be
     * blamed on the boundary. */
    if (!cfg.nativeWindow || !cfg.rdram || !cfg.rdramSize || !cfg.regs) {
        return HostResult::BadConfig;
    }

    const HostResult opened = openShim();
    if (opened != HostResult::Ok) {
        return opened;
    }

    Rt64ShimConfig c = {};
    c.structSize = uint32_t(sizeof(c));
    c.abiVersion = PDRT64_SHIM_ABI_VERSION;
    c.regsSize = uint32_t(sizeof(Registers));
    c.reserved = 0;
    c.nativeWindow = cfg.nativeWindow;
    c.rdram = cfg.rdram;
    c.rdramSize = uint64_t(cfg.rdramSize);
    c.regs = cfg.regs;
    c.dataPath = cfg.dataPath;

    const HostResult r = HostResult(uint8_t(g_shim.init(&c)));
    g_shim.live = (r == HostResult::Ok);
    if (g_shim.live) {
        /* Replay whatever was asked for before the DLL existed. Without it
         * RT64 keeps three-point filtering on while the port believes it
         * selected linear. */
        g_shim.setTextureFiltering(g_shim.filterMode, 0, 0);
    }
    return r;
}

void hostShutdown()
{
    if (!g_shim.live) {
        return;
    }
    g_shim.shutdown();
    g_shim.live = false;

    /*
     * The module stays loaded on purpose. Unloading it would take the graphics
     * device, its driver DLLs and the D3D12 runtime down with it, and those do
     * not reliably survive being unloaded and reloaded inside one process. The
     * cost of keeping it is address space in a process that is exiting anyway;
     * the cost of unloading it is a class of crash that only appears on the
     * second init.
     */
}

bool hostReady()
{
    return g_shim.live && g_shim.ready() != 0;
}

void hostProcessDl(RdramAddr dlStart, RdramAddr dlEnd)
{
    if (!g_shim.live) {
        return;
    }
    g_shim.processDl(dlStart, dlEnd);
}

void hostUpdateScreen()
{
    if (!g_shim.live) {
        return;
    }
    g_shim.updateScreen();
}

/* Contract in rt64_lights.h. Rt64ShimLight and pdrt64Light are the same POD in the same
 * order and both halves compile the same headers, so this reinterprets rather than
 * converting - a conversion here would be a second place for the two to drift. */
void hostSetRoomLights(const ::pdrt64Light *lights, int count)
{
    static_assert(sizeof(Rt64ShimLight) == sizeof(pdrt64Light),
                  "Rt64ShimLight and pdrt64Light must stay the same POD");

    if (!g_shim.setRoomLights) {
        return;
    }

    g_shim.setRoomLights(reinterpret_cast<const Rt64ShimLight *>(lights), int32_t(count));
}

void hostSetTextureFiltering(int filterMode, int mipmapMode, uint32_t anisotropy)
{
    g_shim.filterMode = int32_t(filterMode);
    if (!g_shim.live) {
        return;
    }
    g_shim.setTextureFiltering(int32_t(filterMode), int32_t(mipmapMode), anisotropy);
}

} // namespace pdrt64

#endif // PDRT64_WITH_RT64_DLL
