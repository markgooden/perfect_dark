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
 * rt64_host.cpp compiles to nothing unless PDRT64_WITH_RT64 is defined, so the
 * port's build - which globs every .cpp under port/ - can carry the file
 * without needing RT64 on its include path.
 *
 * Threading: every call is made from the thread that submits frames.
 */

#include <cstddef>
#include <cstdint>

#include "rt64_mem.h"
#include "rt64_registers.h"

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
 * before RT64 is reached. */
enum class HostResult : uint8_t {
    Ok,
    NotCompiledIn,          // built without PDRT64_WITH_RT64
    AlreadyInitialised,
    BadConfig,
    DynamicLibrariesNotFound,
    InvalidGraphicsApi,
    GraphicsApiNotFound,
    GraphicsDeviceNotFound,
    UcodeSelectionFailed,   // RT64 accepted the device but has no GBI to run
    Unknown,
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
 * Note on render-to-RAM: docs/interfaces/rt64_host.h declared a
 * hostSetRenderToRam here, but RT64 has no API for it - it is an extended GBI
 * command carried in the stream (rt64_gbi_extended.cpp:183-186). It lives on
 * Translator::setRenderToRam instead.
 */

} // namespace pdrt64
