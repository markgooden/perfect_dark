/*
 * Synthesized VI register block. Rationale is in rt64_registers.h.
 *
 * Every constant below is chosen so RT64's own decode returns what we mean,
 * and test_registers.cpp checks that by running the real VI implementation
 * over these words rather than by restating the arithmetic.
 */

#include "rt64_registers.h"

#include <cstring>

namespace pdrt64 {

namespace {

/* VI_STATUS type field (rt64_vi.h:14-17). 16-bit is what the arena's images
 * are, and anything other than BLANK is what VI::visible needs. */
constexpr uint32_t kStatusType16Bit = 2;

/* Bit positions in VI_STATUS, from the bitfield order in rt64_vi.h:26-45. */
constexpr uint32_t kStatusAaModeShift = 8;
constexpr uint32_t kAaModeNone = 3;

/* The NTSC visible region. Only two things depend on these: VI::visible wants
 * hStart above zero (rt64_vi.cpp:44), and the interlaced width estimate uses
 * the horizontal span - which never runs, because serrate is off. */
constexpr uint32_t kHStart = 108;
constexpr uint32_t kHSpan = 640;
constexpr uint32_t kVStart = 37;

/* A 1:1 vertical scale, so the height decode below is exact. */
constexpr uint32_t kYScaleUnit = 1024;

/* Free-running values RT64 stores but does not interpret on the HLE path. */
constexpr uint32_t kVSync = 525;
constexpr uint32_t kHSync = 3093;

uint32_t packRegion(uint32_t start, uint32_t end)
{
    /* HRegion and VRegion are {end:10, pad:6, start:10} (rt64_vi.h:80-100). */
    return ((start & 0x3ff) << 16) | (end & 0x3ff);
}

uint32_t packTransform(uint32_t scale, uint32_t offset)
{
    /* XTransform and YTransform are {scale:12, pad:4, offset:12}. */
    return ((offset & 0xfff) << 16) | (scale & 0xfff);
}

void fillVideoMode(Registers *regs, uint32_t width, uint32_t height)
{
    regs->viStatus = kStatusType16Bit | (kAaModeNone << kStatusAaModeShift);
    regs->viWidth = width;
    regs->viVCurrentLine = 0;

    regs->viHStart = packRegion(kHStart, kHStart + kHSpan);

    /* VI::fbSize computes lround((vEnd - vStart) / (2 * yScaleFloat)), adds
     * two rows, then rounds to a multiple of four (rt64_vi.cpp:95-125). With
     * yScaleFloat 1.0 that is (vEnd - vStart) / 2 + 2, so a span of
     * 2 * height - 4 decodes back to exactly `height` whenever height is a
     * multiple of four - which both of PD's modes are. */
    const uint32_t vSpan = height >= 2 ? (height * 2u) - 4u : 0u;
    regs->viVStart = packRegion(kVStart, kVStart + vSpan);

    /* The horizontal scale matters only to the interlaced estimate, but keep
     * it consistent with the region so the block reads as a coherent NTSC
     * frame: 1024 is 1:1 against the 640-wide span. */
    regs->viXScale = packTransform((width * 1024u) / kHSpan, 0);
    regs->viYScale = packTransform(kYScaleUnit, 0);

    regs->viVSync = kVSync;
    regs->viHSync = kHSync;
    regs->viLeap = 0;
    regs->viBurst = 0;
    regs->viVBurst = 0;
    regs->viIntr = 0;
}

/* Bytes in one row of the scanned-out image, which is what VI::fbAddress
 * subtracts. 16-bit pixels, so two bytes each (rt64_vi.cpp:84). */
uint32_t rowBytes(const Registers *regs)
{
    return regs->viWidth * 2u;
}

} // namespace

void regsInit(Registers *regs, uint32_t nativeWidth, uint32_t nativeHeight)
{
    memset(regs, 0, sizeof(*regs));
    fillVideoMode(regs, nativeWidth, nativeHeight);
}

void regsSetVideoMode(Registers *regs, uint32_t nativeWidth, uint32_t nativeHeight)
{
    /* Deliberately does not clear the origin: a mode change should not blank
     * the frame that is already being scanned out. */
    fillVideoMode(regs, nativeWidth, nativeHeight);
}

void regsSetScanout(Registers *regs, RdramAddr colorImage)
{
    /* Add back the row RT64 will subtract, so its decode lands on the image
     * itself. Without this the presented frame is one row above the arena
     * allocation - inside the previous framebuffer, which renders as a picture
     * that is subtly wrong rather than obviously broken. */
    regs->viOrigin = colorImage + rowBytes(regs);
}

RdramAddr regsScanoutAddress(const Registers *regs)
{
    const uint32_t offset = rowBytes(regs);
    return regs->viOrigin >= offset ? regs->viOrigin - offset : regs->viOrigin;
}

} // namespace pdrt64
