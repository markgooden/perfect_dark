/*
 * Marshalling into RT64's emulator-convention RDRAM, and region-classified
 * reads of live game memory.
 *
 * The rules and the evidence for them are in rt64_mem.h. This file is the
 * only place they are implemented (CLAUDE.md invariant 5) and it deliberately
 * has no dependency on the port, RT64 or SDL, so the unit tests can compile it
 * on its own.
 */

#include "rt64_mem.h"

#include <cassert>
#include <cstring>

namespace pdrt64 {

namespace {

/* A native-LE u16 element at src offset i belongs at dst offset i^2, value
 * unchanged. */
void swizzleU16(uint8_t *d, const uint8_t *s, size_t len)
{
    for (size_t i = 0; i + 1 < len; i += 2) {
        const size_t o = i ^ 2u;
        d[o] = s[i];
        d[o + 1] = s[i + 1];
    }
}

/* A byte at src offset i belongs at dst offset i^3.
 *
 * This is simultaneously the U8 rule (native byte-addressed data) and the
 * BE32 rule (raw big-endian source, reversed per 32-bit word): both map
 * i -> 3-i within each word. rt64_mem.h keeps them as separate enum values
 * because the reasons differ, and picking the wrong one for the wrong data is
 * the bug the split exists to prevent - but the byte movement is identical,
 * so one loop backs both.
 */
void swizzleByteReverse(uint8_t *d, const uint8_t *s, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        d[i ^ 3u] = s[i];
    }
}

/*
 * One PD vertex, 12 bytes / 3 words.
 *
 * Source (native LE, include/PR/gbi.h:986-999):
 *   0: s16 x   2: s16 y   4: s16 z   6: u8 flags   7: u8 colour
 *   8: s16 s  10: s16 t
 *
 * Destination, as RT64 reads it (RSP::VertexPD, rt64_rsp.h:63-68):
 *   0: s16 y   2: s16 x   4: u16 ci  6: s16 z   8: s16 t  10: s16 s
 *
 * Word 1 is the subtle one. The N64 word holds (z << 16) | (flags << 8) |
 * colour, so read back as host-LE halves RT64 sees ci = (flags << 8) | colour
 * at offset 4 and z at offset 6. setVertexPD then masks ci & 0xFF to recover
 * the colour index (rt64_rsp.cpp:390), which is why colour must land in the
 * low byte.
 *
 * Note this is NOT a bswap32 of the word: z is a native-LE halfword that has
 * to keep its own byte order while moving to a new offset, whereas flags and
 * colour are separate bytes that swap. Applying BE32 here would put z's bytes
 * backwards.
 */
void swizzleVtxPD(uint8_t *d, const uint8_t *s, size_t len)
{
    for (size_t v = 0; v + 12 <= len; v += 12) {
        const uint8_t *sv = s + v;
        uint8_t *dv = d + v;

        /* word 0: halves swap, bytes within each half preserved */
        dv[0] = sv[2]; dv[1] = sv[3];   /* y */
        dv[2] = sv[0]; dv[3] = sv[1];   /* x */

        /* word 1: z moves to the high half intact; flags/colour reverse */
        dv[4] = sv[7];                  /* colour -> ci low byte */
        dv[5] = sv[6];                  /* flags  -> ci high byte */
        dv[6] = sv[4]; dv[7] = sv[5];   /* z */

        /* word 2: halves swap */
        dv[8] = sv[10]; dv[9] = sv[11]; /* t */
        dv[10] = sv[8]; dv[11] = sv[9]; /* s */
    }
}

bool spanContains(uintptr_t base, size_t size, uintptr_t addr, size_t len)
{
    if (!base || !size || !len) {
        return false;
    }
    /* Guard the overflow before trusting the range check. */
    if (addr < base || len > size) {
        return false;
    }
    return (addr - base) <= (size - len);
}

} // namespace

void marshalCopy(void *dst, const void *src, size_t len, Swizzle type)
{
    auto *d = static_cast<uint8_t *>(dst);
    const auto *s = static_cast<const uint8_t *>(src);

    assert((reinterpret_cast<uintptr_t>(d) & 3u) == 0 && "dst must be 4-byte aligned");
    assert((len & 3u) == 0 && "len must be a multiple of 4");

    switch (type) {
    case Swizzle::U32:
        /* Values and offsets both unchanged. */
        memcpy(d, s, len);
        break;
    case Swizzle::U16:
        swizzleU16(d, s, len);
        break;
    case Swizzle::U8:
    case Swizzle::BE32:
        swizzleByteReverse(d, s, len);
        break;
    case Swizzle::VtxPD:
        assert((len % 12u) == 0 && "VtxPD length must be a multiple of 12");
        swizzleVtxPD(d, s, len);
        break;
    }
}

LiveMemReader::LiveMemReader(const uint8_t *heapBase, size_t heapSize,
                             const uint8_t *romBase, size_t romSize,
                             const uint8_t *imageBase, size_t imageSize,
                             TrackedAllocFn tracked)
    : heap_{reinterpret_cast<uintptr_t>(heapBase), heapSize},
      rom_{reinterpret_cast<uintptr_t>(romBase), romSize},
      image_{reinterpret_cast<uintptr_t>(imageBase), imageSize},
      tracked_(tracked)
{
}

Region LiveMemReader::regionOf(uintptr_t src, size_t len) const
{
    if (spanContains(heap_.base, heap_.size, src, len)) {
        return Region::Heap;
    }
    if (spanContains(rom_.base, rom_.size, src, len)) {
        return Region::Rom;
    }
    if (spanContains(image_.base, image_.size, src, len)) {
        return Region::Image;
    }
    /* Last, and only if a registry was supplied: the spans are cheap bounds
     * tests, the registry is a binary search. */
    if (tracked_ && src && len && tracked_(src, len)) {
        return Region::Malloc;
    }
    return Region::None;
}

bool LiveMemReader::read(uintptr_t src, void *dst, size_t len)
{
    /* Only ever dereference memory we have classified. An unclassified pointer
     * is not necessarily invalid, but we cannot prove it is mapped, and a bad
     * read here would take the game down mid-frame. */
    if (regionOf(src, len) == Region::None) {
        return false;
    }
    memcpy(dst, reinterpret_cast<const void *>(src), len);
    return true;
}

} // namespace pdrt64
