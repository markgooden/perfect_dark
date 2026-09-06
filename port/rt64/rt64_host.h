#pragma once

/*
 * Thin lifecycle wrapper around RT64::Application.
 *
 * rt64_host.cpp is the ONLY file in the backend that includes RT64 headers,
 * and this header deliberately includes none, so everything else - translator,
 * arena, registers, debug - compiles and is tested without RT64 present.
 *
 * That separation is not tidiness, it is what makes the toolchain split
 * workable. The port builds MinGW-only and RT64 builds MSVC-only
 * (docs/BUILD-RT64.md), so the implementation is compiled by MSVC and reached
 * across a C-ABI DLL boundary, while every caller stays MinGW and sees only
 * the declarations below. The replay harness skips the DLL and links RT64
 * directly, which is why it can isolate translator bugs from boundary bugs.
 *
 * Which implementation answers these declarations is a build-time choice:
 * rt64_host.cpp calls RT64 directly (PDRT64_WITH_RT64, MSVC, used by the
 * replay harness), rt64_hostdll.cpp calls rt64shim.dll (PDRT64_WITH_RT64_DLL,
 * what the MinGW port uses), and with neither macro the calls resolve to a
 * stub that fails at run time with a reason. That is what lets the port's
 * build glob every .cpp under port/ without needing RT64 on its include path.
 *
 * Threading: every call is made from the thread that submits frames.
 */

#include <cstddef>
#include <cstdint>

#include "rt64_mem.h"
#include "rt64_registers.h"
#include "rt64_shimabi.h"

namespace pdrt64 {

struct HostConfig {
    /* Native window handle: HWND on Windows. The replay harness passes a
     * hidden one. RT64 must not be given a window that already carries an
     * OpenGL context. */
    void *nativeWindow = nullptr;

    /* Synthetic RDRAM (Arena::rdramBase/rdramSize). RT64 keeps the pointer, so
     * the arena must outlive the host, and the DLL boundary must never free or
     * reallocate it - it is owned by the port side. */
    uint8_t *rdram = nullptr;
    size_t rdramSize = 0;

    /* Register block, wired 1:1 into Application::Core's pointers. Must
     * outlive the host, for the same reason. */
    Registers *regs = nullptr;

    /* Where RT64 keeps its configuration and shader cache. */
    const char *dataPath = nullptr;
};

/* Why hostInit failed, so a caller can say something better than "no". The
 * values mirror RT64's Application::SetupResult plus the cases that stop us
 * before RT64 is reached.
 *
 * The first group takes its values from rt64_shimabi.h rather than restating
 * them, because these are exactly the codes that cross the DLL boundary and
 * two independent lists would drift the first time one gained an entry. The
 * second group is produced by the loader itself and never travels. */
enum class HostResult : uint8_t {
    Ok                       = RT64SHIM_OK,
    NotCompiledIn            = RT64SHIM_NOT_COMPILED_IN,
    AlreadyInitialised       = RT64SHIM_ALREADY_INITIALISED,
    BadConfig                = RT64SHIM_BAD_CONFIG,
    DynamicLibrariesNotFound = RT64SHIM_DYNAMIC_LIBRARIES_NOT_FOUND,
    InvalidGraphicsApi       = RT64SHIM_INVALID_GRAPHICS_API,
    GraphicsApiNotFound      = RT64SHIM_GRAPHICS_API_NOT_FOUND,
    GraphicsDeviceNotFound   = RT64SHIM_GRAPHICS_DEVICE_NOT_FOUND,
    UcodeSelectionFailed     = RT64SHIM_UCODE_SELECTION_FAILED,  // device fine, no GBI to run
    ShimException            = RT64SHIM_EXCEPTION,
    ShimAbiMismatch          = RT64SHIM_ABI_MISMATCH,
    Unknown                  = RT64SHIM_UNKNOWN,

    /* Loader-side only: the DLL never returns these because reaching it is
     * what failed. */
    ShimNotFound             = 64,
    ShimEntryPointMissing    = 65,
};

const char *hostResultName(HostResult r);

/* Creates the Application and brings up a device and swap chain. There is no
 * mid-flight renderer switch: a failure here is terminal for the backend. */
HostResult hostInit(const HostConfig &cfg);

void hostShutdown();

/* True once hostInit has succeeded and hostShutdown has not run. */
bool hostReady();

/* Feeds one translated stream to RT64 (processDisplayLists with isHLE true).
 * The addresses are tagged extended-RDRAM addresses; their low 31 bits are the
 * byte offsets into the buffer handed over as `rdram`. */
void hostProcessDl(RdramAddr dlStart, RdramAddr dlEnd);

/* Presents the frame. Call once per frame, after every hostProcessDl. */
void hostUpdateScreen();

/*
 * Forwards the port's texture-filter choice onto RT64's live user
 * configuration. `filterMode` carries the port's own `enum FilteringMode`
 * values (gfx_rendering_api.h:15) as an int, because the shim ABI moves POD
 * only: 0 none, 1 linear, 2 three-point. The exact mapping onto RT64's
 * Filtering enum, and why `mipmapMode` and `anisotropy` are accepted and
 * dropped, are documented at the implementation in rt64_host.cpp.
 *
 * Safe before hostInit: the request is remembered and applied at setup. That
 * matters because the port sets the filter from videoInit (video.c:175), which
 * need not run after the backend is up.
 */
void hostSetTextureFiltering(int filterMode, int mipmapMode, uint32_t anisotropy);

/*
 * Note on render-to-RAM: docs/interfaces/rt64_host.h declared a
 * hostSetRenderToRam here, but RT64 has no API for it - it is an extended GBI
 * command carried in the stream (rt64_gbi_extended.cpp:183-186). It lives on
 * Translator::setRenderToRam instead.
 */

} // namespace pdrt64
