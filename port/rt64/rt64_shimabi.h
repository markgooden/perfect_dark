#pragma once

/*
 * The C-ABI boundary between the MinGW-built port and rt64shim.dll.
 *
 * The port builds MinGW-only and RT64 builds MSVC-only (docs/BUILD-RT64.md),
 * so the two halves cannot share a C++ ABI: no name mangling, no exceptions,
 * no std:: types, no ownership, and no C++ at all may cross this line. What
 * crosses is declared here, once, and this file is compiled by both
 * toolchains - the MinGW loader in rt64_hostdll.cpp and the MSVC exports in
 * tools/rt64shim/rt64shim.cpp both include it.
 *
 * Calling convention is not specified anywhere below because on Windows x64
 * there is only one; a 32-bit build would have to name __cdecl explicitly.
 *
 * === The rules, and where they come from ===
 *
 * The synthetic RDRAM arena and the register block are allocated on the port
 * side and merely pointed at from the DLL. RT64 stores both pointers rather
 * than copying (rt64_host.cpp wires them into Application::Core), so they must
 * outlive the host, and the DLL must never free or reallocate either. The two
 * halves may be linked against different C runtimes with separate heaps, so a
 * pointer allocated on one side and freed on the other is not merely bad
 * style, it is a crash.
 *
 * Nothing is returned across the boundary but integers, for the same reason.
 * Even a `const char *` error string would be a pointer into the DLL's image
 * that stops being valid the moment it unloads; result codes are numbers, and
 * hostResultName turns them into text on the port side.
 *
 * === Versioning ===
 *
 * A stale rt64shim.dll next to a fresh executable is the expected failure, not
 * an exotic one - they are built by two different toolchains, by two different
 * commands, and nothing forces them to be rebuilt together. So both sides
 * carry a version and the sizes of the structs they believe in, and
 * rt64ShimInit refuses a mismatch with a distinct code instead of reading a
 * struct laid out the way an older build laid it out.
 */

#include <stdint.h>

/* Loaded by name from the directory holding the executable. See
 * rt64_hostdll.cpp for why beside-the-executable is the only location that
 * works, and docs/BUILD-RT64.md for what else has to be staged with it. */
#define PDRT64_SHIM_DLL_NAME "rt64shim.dll"

/* Bump on any change to the struct below or to an exported signature. Adding
 * a new export does not require a bump; the loader resolves each one and
 * reports which is missing.
 *
 * 2 (2026-09-13): Rt64ShimLight gained roomradius and camdist, so a version 1
 * shim paired with this port would read a light's colour where its room size
 * should be. The static_assert against sizeof(pdrt64Light) caught the same
 * mismatch inside one build; this catches it across the DLL boundary, which is
 * the half a compiler cannot see. */
#define PDRT64_SHIM_ABI_VERSION 2u

/*
 * The exports carry dllexport when the DLL itself is being built and nothing
 * otherwise. There is deliberately no dllimport case: the port resolves every
 * entry point with GetProcAddress, so it neither has nor wants an import
 * library, and a missing rt64shim.dll has to be a reportable condition rather
 * than a process that refuses to start.
 */
#if defined(PDRT64_SHIM_BUILDING)
#  define PDRT64_SHIM_API __declspec(dllexport)
#else
#  define PDRT64_SHIM_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Result codes. The low range is shared with pdrt64::HostResult, which takes
 * its values from these names so the mapping cannot drift; the loader-only
 * failures live above the range the DLL can produce (rt64_host.h).
 */
enum {
    RT64SHIM_OK = 0,
    RT64SHIM_NOT_COMPILED_IN = 1,
    RT64SHIM_ALREADY_INITIALISED = 2,
    RT64SHIM_BAD_CONFIG = 3,
    RT64SHIM_DYNAMIC_LIBRARIES_NOT_FOUND = 4,
    RT64SHIM_INVALID_GRAPHICS_API = 5,
    RT64SHIM_GRAPHICS_API_NOT_FOUND = 6,
    RT64SHIM_GRAPHICS_DEVICE_NOT_FOUND = 7,
    RT64SHIM_UCODE_SELECTION_FAILED = 8,
    /* The DLL caught a C++ exception on its way out. Nothing may propagate
     * across the boundary - the unwinder on the other side is a different
     * implementation entirely - so every export swallows and reports. */
    RT64SHIM_EXCEPTION = 9,
    RT64SHIM_ABI_MISMATCH = 10,
    RT64SHIM_UNKNOWN = 11
};

/*
 * Fields are ordered so the layout is identical under both compilers without
 * relying on either one's padding: the four 32-bit words first, then only
 * 8-byte members. 56 bytes, 8-byte aligned, no holes. Both sides assert it.
 */
typedef struct Rt64ShimConfig {
    uint32_t structSize;    /* sizeof(Rt64ShimConfig) as the caller sees it */
    uint32_t abiVersion;    /* PDRT64_SHIM_ABI_VERSION as the caller saw it */
    uint32_t regsSize;      /* sizeof(pdrt64::Registers) as the caller saw it */
    uint32_t reserved;      /* zero */

    /* Native window handle; HWND on Windows. Must not already carry an
     * OpenGL context - RT64 creates its own device and swap chain against
     * it (gfx_sdl2.cpp hardwires SDL_WINDOW_OPENGL today; task T9). */
    void *nativeWindow;

    /* The synthetic RDRAM arena. Owned by the port side, borrowed by RT64
     * for as long as the host lives. */
    uint8_t *rdram;
    uint64_t rdramSize;

    /* pdrt64::Registers. Borrowed the same way, and read through every
     * frame by Application::Core::decodeVI. */
    void *regs;

    /* Where RT64 keeps its configuration and shader cache; null lets RT64
     * detect its own. Copied by RT64 during init, so the string only has to
     * outlive the rt64ShimInit call. */
    const char *dataPath;
} Rt64ShimConfig;

/* Returns PDRT64_SHIM_ABI_VERSION as the DLL was built with. Resolved and
 * called before anything else, so a mismatch is reported rather than acted
 * on. */
PDRT64_SHIM_API uint32_t rt64ShimAbiVersion(void);

/* One of the RT64SHIM_ codes above. */
PDRT64_SHIM_API int32_t rt64ShimInit(const Rt64ShimConfig *cfg);

PDRT64_SHIM_API void rt64ShimShutdown(void);

/* Nonzero once init has succeeded and shutdown has not run. */
PDRT64_SHIM_API int32_t rt64ShimReady(void);

/* Bounds of one translated stream, as offsets into the rdram passed to init.
 * Tagged addresses are accepted; the tag is stripped on the far side. */
PDRT64_SHIM_API void rt64ShimProcessDl(uint32_t dlStart, uint32_t dlEnd);

PDRT64_SHIM_API void rt64ShimUpdateScreen(void);

/* Texture filtering. `filterMode` carries the port's own enum FilteringMode
 * (gfx_rendering_api.h:15) as an int32: 0 none, 1 linear, 2 three-point.
 * `mipmapMode` and `anisotropy` are accepted and dropped - RT64 has no
 * configuration for either. The full mapping, and why only one RT64 field is
 * touched, is documented at the implementation in rt64_host.cpp.
 *
 * Adding this export needed no ABI bump by the rule above: the struct and the
 * existing signatures are untouched, and the loader resolves each entry point
 * by name and names the one it cannot find. */
PDRT64_SHIM_API void rt64ShimSetTextureFiltering(int32_t filterMode, int32_t mipmapMode,
                                                 uint32_t anisotropy);

/* Hands the path tracer the game's own room lights for this frame, replacing the previous
 * frame's set whole.
 *
 * They cannot come through the display list. Perfect Dark bakes its level lighting into
 * vertex colours and its RSP light path carries 32 of 5878 vertices on an in-level frame,
 * so a tracer that waited for lights in the stream would wait forever. These come from
 * struct light (src/include/types.h:5308), gathered from the on-screen rooms.
 *
 * `lights` points at `count` Rt64ShimLight, and need not outlive the call: the
 * implementation copies what it needs.
 *
 * Adding this export needed no ABI bump, by the rule above: the struct and the existing
 * signatures are untouched, and the loader resolves each entry point by name. */
typedef struct Rt64ShimLight {
    float x, y, z;          /* centre of the light's quad */
    float radius;           /* centre to furthest corner - the emitter's size */
    float dirx, diry, dirz; /* direction */
    float r, g, b;          /* colour times brightness */
    float roomradius;       /* the room the light is in - its reach */
    float camdist;          /* distance from the camera, for ranking */
    int32_t roomnum;
    int32_t sparking;       /* damaged and flickering */
} Rt64ShimLight;

PDRT64_SHIM_API void rt64ShimSetRoomLights(const Rt64ShimLight *lights, int32_t count);

#ifdef __cplusplus
} /* extern "C" */
#endif
