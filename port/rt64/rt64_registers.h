#pragma once

/*
 * Synthesized N64 register block for RT64's Application::Core.
 *
 * RT64's HLE path never executes microcode, so most of these registers only
 * have to exist. The VI set is different: RT64 decodes it (rt64_vi.h) to work
 * out where the frame being presented lives, how big it is and what format it
 * is in, and it matches the decoded address against the framebuffers the
 * display list created. Get the VI wrong and the geometry is fine while
 * nothing reaches the screen.
 *
 * === Two things here are easy to get wrong and are pinned by tests ===
 *
 * The origin is not the image address. VI::fbAddress subtracts one row before
 * returning (rt64_vi.cpp:78-93), because real games point VI_ORIGIN a row or
 * two into the buffer and RT64 compensates. A synthesized block that stores
 * the image address directly makes RT64 read one row above it. So the row is
 * added here, and regsScanoutAddress reports what RT64 will actually compute.
 *
 * The origin is a tagged extended-RDRAM address, like every other address the
 * translator emits. RT64 matches it against the framebuffers the stream
 * created with G_SETCIMG (rt64_present_queue.cpp:146), and those carry the
 * tag, so the VI has to as well or the lookup finds nothing.
 *
 * The height comes out of a decode that rounds. VI::fbSize derives it from the
 * vertical region and the y scale, adds two rows and rounds to a multiple of
 * four (rt64_vi.cpp:95-125). Rather than reverse-engineer NTSC constants until
 * the arithmetic happens to land, the vertical region is set so the decode is
 * exact: at a 1:1 y scale, vEnd - vStart = 2 * height - 4 gives back exactly
 * `height` for any height that is a multiple of four. Both of PD's modes are
 * (220 and 480).
 */

#include <cstdint>

#include "rt64_mem.h"

namespace pdrt64 {

/*
 * Backing storage for every pointer in RT64's Application::Core. Laid out as
 * plain words rather than RT64's VI struct so this file keeps the no-RT64
 * property the rest of the backend has; the tests include the real header and
 * check the two agree.
 */
struct Registers {
    uint32_t miIntr;
    uint32_t dpc[8];       // START,END,CURRENT,STATUS,CLOCK,BUFBUSY,PIPEBUSY,TMEM

    uint32_t viStatus;
    uint32_t viOrigin;
    uint32_t viWidth;
    uint32_t viIntr;
    uint32_t viVCurrentLine;
    uint32_t viBurst;
    uint32_t viVSync;
    uint32_t viHSync;
    uint32_t viLeap;
    uint32_t viHStart;
    uint32_t viVStart;
    uint32_t viVBurst;
    uint32_t viXScale;
    uint32_t viYScale;
};

/* NTSC constants for the given native mode. Everything not part of the VI set
 * is zeroed. */
void regsInit(Registers *regs, uint32_t nativeWidth, uint32_t nativeHeight);

/* Re-derives the VI set for a native-mode change. PD's resolution is dynamic -
 * pdsched.c:213 switches between lo- and hi-res - so this is not a one-off. */
void regsSetVideoMode(Registers *regs, uint32_t nativeWidth, uint32_t nativeHeight);

/* Points the VI at `colorImage` for the frame being presented, adding the row
 * RT64 subtracts back. Call once per frame, alternating the main colour images
 * so RT64's present logic sees a buffer flip. */
void regsSetScanout(Registers *regs, RdramAddr colorImage);

/* The address RT64 will decode out of viOrigin, i.e. what regsSetScanout was
 * given. Exists so a test can state the round trip rather than restate the
 * row arithmetic. */
RdramAddr regsScanoutAddress(const Registers *regs);

} // namespace pdrt64
