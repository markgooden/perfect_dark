/*
 * Disassemblers, command shape and hashing. Rationale is in rt64_debug.h.
 *
 * Every field decoding below cites the line of fast3d (port dialect) or RT64
 * (canonical dialect) it mirrors. Those citations are the point: a
 * disassembler that decodes a field its renderer does not is worse than no
 * disassembler, because it produces a golden that looks authoritative.
 */

#include "rt64_debug.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

/* Opcodes the port adds on top of stock F3D (src/include/gbiex.h:187-199).
 * gbi.h pulls gbiex.h in, so they are already visible; listed here only so a
 * reader knows where they come from. */

namespace pdrt64 {

namespace {

/* Field extraction, matching fast3d's C0/C1 (gfx_pc.cpp:91-92). */
inline uint32_t c0(const Gfx &g, uint32_t pos, uint32_t width)
{
    return (uint32_t)((g.words.w0 >> pos) & ((1ULL << width) - 1));
}

inline uint32_t c1(const Gfx &g, uint32_t pos, uint32_t width)
{
    return (uint32_t)((g.words.w1 >> pos) & ((1ULL << width) - 1));
}

std::string fmt(const char *f, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, f);
    const int n = vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return std::string(buf, n > 0 ? (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1 : 0);
}

const char *regionTag(Region r)
{
    switch (r) {
    case Region::Heap:   return "heap";
    case Region::Rom:    return "rom";
    case Region::Image:  return "image";
    case Region::Malloc: return "mem";
    case Region::None:   break;
    }
    return "unmapped";
}

/*
 * Names addresses so a dump is reproducible across runs.
 *
 * Host pointers move (ASLR, allocation order), so a golden that printed them
 * would fail on the second run for no reason. Blocks are numbered in
 * first-reference order along the walk, which depends only on the display
 * list, and later references inside a known block print as an offset from it.
 *
 * A reference that extends past a known block's recorded end widens that
 * block rather than starting a new one - the same coalescing rule the capture
 * reader applies (rt64_pddl.cpp:213-245), for the same reason: the renderer
 * reads overlapping windows of one array (two G_MOVEMEM eight bytes apart)
 * and calling those two blocks would be a lie about the memory.
 */
class AddrNamer {
public:
    std::string name(uintptr_t addr, uint32_t len, Region region)
    {
        for (Blk &b : blks_) {
            if (addr >= b.base && addr <= b.base + b.len) {
                const uint32_t end = (uint32_t)(addr - b.base) + len;
                if (end > b.len) {
                    b.len = end;
                }
                const uintptr_t off = addr - b.base;
                return off ? fmt("%s:b%u+0x%llx", regionTag(b.region), b.ord,
                                 (unsigned long long)off)
                           : fmt("%s:b%u", regionTag(b.region), b.ord);
            }
        }
        blks_.push_back(Blk{addr, len ? len : 1u, next_, region});
        return fmt("%s:b%u", regionTag(region), next_++);
    }

private:
    struct Blk {
        uintptr_t base;
        uint32_t len;
        uint32_t ord;
        Region region;
    };
    std::vector<Blk> blks_;
    uint32_t next_ = 0;
};

} // namespace

/*
 * === Command shape and references ===
 */

uint32_t gfxCommandWords(uint8_t opcode)
{
    /* fast3d advances over the operand words inline (gfx_pc.cpp:2435-2503). */
    switch (opcode) {
    case (uint8_t)G_TEXRECT:
    case (uint8_t)G_TEXRECTFLIP:
    case G_TEXRECT_WIDE_EXT:
    case G_IMAGERECT_EXT:
        return 3;
    case G_FILLRECT_WIDE_EXT:
        return 2;
    default:
        return 1;
    }
}

uintptr_t gfxSegResolve(uintptr_t w1, const uintptr_t segments[16],
                        bool *outUnbound)
{
    if (outUnbound) {
        *outUnbound = false;
    }

    /* gfx_pc.cpp:2285-2296. Segment 0 is reserved and does not count. */
    if (w1 & 1) {
        const uintptr_t seg = (w1 & 0x0f000000) >> 24;
        if (seg && segments[seg]) {
            return segments[seg] + (w1 & 0x00fffffe);
        }
        /* fast3d falls through and dereferences w1 as a pointer. Reproduce it:
         * a capture that recorded something else would disagree with the
         * renderer about what the frame touched, which is the one thing it
         * exists to get right. */
        if (outUnbound) {
            *outUnbound = true;
        }
    }
    return w1;
}

void GfxWalkState::reset(const uintptr_t *seed)
{
    if (seed) {
        memcpy(segments, seed, sizeof(segments));
    } else {
        memset(segments, 0, sizeof(segments));
    }
    texAddr = 0;
    texSiz = 0;
    texWidth = 0;
}

GfxRef gfxStep(GfxWalkState &st, const Gfx *cmd)
{
    const Gfx &g = *cmd;
    const uint8_t opcode = (uint8_t)(g.words.w0 >> 24);

    GfxRef ref;

    /* Segment bindings come from the stream (gfx_sp_moveword, gfx_pc.cpp:1769).
     * fast3d indexes a 16-entry table with & 0xff, which would write out of
     * bounds for a segment above 15; the game never emits one (every
     * SPSEGMENT_* is 0-15, src/include/constants.h:3927-3935), so mask to the
     * range seg_addr can actually read rather than reproduce the overrun. */
    if (opcode == (uint8_t)G_MOVEWORD && c0(g, 0, 8) == G_MW_SEGMENT) {
        st.segments[(c0(g, 8, 16) >> 2) & 0x0f] = (uintptr_t)g.words.w1;
        return ref;
    }

    auto resolve = [&](RefKind kind, uint32_t bytes, uint32_t startOffset = 0) {
        ref.kind = kind;
        ref.bytes = bytes;
        ref.startOffset = startOffset;
        if (g.words.w1 & 1) {
            ref.segmented = true;
            ref.segment = (uint8_t)((g.words.w1 & 0x0f000000) >> 24);
            ref.segOffset = (uint32_t)(g.words.w1 & 0x00fffffe);
        }
        ref.addr = gfxSegResolve((uintptr_t)g.words.w1, st.segments, &ref.unbound);
    };

    switch (opcode) {
    case G_DL:
        /* The caller decides whether to follow; sizing a display list means
         * walking it, which is not this function's job. */
        resolve(RefKind::DisplayList, 0);
        break;

    case G_MTX:
        resolve(RefKind::Matrix, sizeof(Mtx));
        break;

    case G_MOVEMEM:
        /* Lights, viewport and lookat blocks are all 16 bytes, and 16 is the
         * most fast3d reads (gfx_sp_movemem, gfx_pc.cpp:1735-1754). These sit
         * in small allocations, so do not round up. */
        resolve(RefKind::MoveMem, 16);
        break;

    case G_VTX:
        /* gSPVertex packs sizeof(Vtx)*n into w0[0:16] (gbi.h:1722). */
        resolve(RefKind::Vertices, c0(g, 0, 16));
        break;

    case G_COL:
        /* gSPColor packs sizeof(Col)*n into w0[0:16] (gbiex.h:15-16). */
        resolve(RefKind::Colours, c0(g, 0, 16));
        break;

    case (uint8_t)G_SETTIMG:
        /* Records the pending image; the following load command sizes it
         * (gfx_dp_set_texture_image, gfx_pc.cpp:2380). */
        st.texSiz = c0(g, 19, 2);
        st.texWidth = c0(g, 0, 10);
        st.texAddr = gfxSegResolve((uintptr_t)g.words.w1, st.segments);
        break;

    case (uint8_t)G_LOADBLOCK: {
        /* gfx_pc.cpp:1876 - lrs is a texel count. */
        const uint32_t lrs = c1(g, 12, 12);
        ref.kind = RefKind::Texels;
        ref.addr = st.texAddr;
        ref.bytes = ((lrs + 1) << st.texSiz) >> 1;
        break;
    }

    case (uint8_t)G_LOADTILE: {
        /* gfx_pc.cpp:1900-1912. */
        const uint32_t uls = c0(g, 12, 12), ult = c0(g, 0, 12);
        const uint32_t lrs = c1(g, 12, 12), lrt = c1(g, 0, 12);
        const uint32_t offsetX = uls >> G_TEXTURE_IMAGE_FRAC;
        const uint32_t offsetY = ult >> G_TEXTURE_IMAGE_FRAC;
        const uint32_t tileW = ((lrs - uls) >> G_TEXTURE_IMAGE_FRAC) + 1;
        const uint32_t tileH = ((lrt - ult) >> G_TEXTURE_IMAGE_FRAC) + 1;
        const uint32_t fullW = st.texWidth + 1;
        const uint32_t offsetXBytes = (offsetX << st.texSiz) >> 1;
        const uint32_t tileLine = (tileW << st.texSiz) >> 1;
        const uint32_t fullLine = (fullW << st.texSiz) >> 1;
        ref.kind = RefKind::Texels;
        ref.addr = st.texAddr;
        ref.bytes = tileLine * tileH;
        ref.startOffset = fullLine * offsetY + offsetXBytes;
        break;
    }

    case (uint8_t)G_LOADTLUT: {
        /* gfx_pc.cpp:1843-1847; entries are 16-bit. */
        const uint32_t uls = c0(g, 14, 10), ult = c0(g, 2, 10);
        const uint32_t lrs = c1(g, 14, 10), lrt = c1(g, 2, 10);
        const uint32_t width = lrs - uls + 1;
        const uint32_t height = lrt - ult + 1;
        const uint32_t pitch = st.texWidth + 1;
        ref.kind = RefKind::Tlut;
        ref.addr = st.texAddr;
        ref.bytes = (pitch * ult + uls + width * height) * 2;
        break;
    }

    default:
        break;
    }

    return ref;
}

/*
 * === Hashing ===
 */

uint64_t hashBytes(const void *data, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

uint64_t hashRdramRange(const uint8_t *rdramBase, RdramAddr start, size_t len)
{
    return hashBytes(rdramBase + (start & ~kExtendedAddrBit), len);
}

/*
 * === Opcode names ===
 */

const char *gfxOpcodeName(uint8_t opcode)
{
    switch (opcode) {
    case (uint8_t)G_POPMTX:             return "G_POPMTX";
    case G_MTX:                         return "G_MTX";
    case G_MOVEMEM:                     return "G_MOVEMEM";
    case G_VTX:                         return "G_VTX";
    case G_DL:                          return "G_DL";
    case G_COL:                         return "G_COL";
    case G_SETFB_EXT:                   return "G_SETFB_EXT";
    case G_SETTIMG_FB_EXT:              return "G_SETTIMG_FB_EXT";
    case G_INVALTEXCACHE_EXT:           return "G_INVALTEXCACHE_EXT";
    case G_TEXRECT_WIDE_EXT:            return "G_TEXRECT_WIDE_EXT";
    case G_FILLRECT_WIDE_EXT:           return "G_FILLRECT_WIDE_EXT";
    case G_SETGRAYSCALE_EXT:            return "G_SETGRAYSCALE_EXT";
    case G_EXTRAGEOMETRYMODE_EXT:       return "G_EXTRAGEOMETRYMODE_EXT";
    case G_SETINTENSITY_EXT:            return "G_SETINTENSITY_EXT";
    case G_COPYFB_EXT:                  return "G_COPYFB_EXT";
    case G_IMAGERECT_EXT:               return "G_IMAGERECT_EXT";
    case G_RDPFLUSH_EXT:                return "G_RDPFLUSH_EXT";
    case G_CLEAR_DEPTH_EXT:             return "G_CLEAR_DEPTH_EXT";
    case G_SETSUBPIXELOFFSET_EXT:       return "G_SETSUBPIXELOFFSET_EXT";
    case (uint8_t)G_TRI4:               return "G_TRI4";
    case (uint8_t)G_RDPHALF_CONT:       return "G_RDPHALF_CONT";
    case (uint8_t)G_RDPHALF_2:          return "G_RDPHALF_2";
    case (uint8_t)G_RDPHALF_1:          return "G_RDPHALF_1";
    case (uint8_t)G_CLEARGEOMETRYMODE:  return "G_CLEARGEOMETRYMODE";
    case (uint8_t)G_SETGEOMETRYMODE:    return "G_SETGEOMETRYMODE";
    case (uint8_t)G_ENDDL:              return "G_ENDDL";
    case (uint8_t)G_SETOTHERMODE_L:     return "G_SETOTHERMODE_L";
    case (uint8_t)G_SETOTHERMODE_H:     return "G_SETOTHERMODE_H";
    case (uint8_t)G_TEXTURE:            return "G_TEXTURE";
    case (uint8_t)G_MOVEWORD:           return "G_MOVEWORD";
    case (uint8_t)G_TRI1:               return "G_TRI1";
    case (uint8_t)G_NOOP:               return "G_NOOP";
    case (uint8_t)G_TEXRECT:            return "G_TEXRECT";
    case (uint8_t)G_TEXRECTFLIP:        return "G_TEXRECTFLIP";
    case (uint8_t)G_RDPLOADSYNC:        return "G_RDPLOADSYNC";
    case (uint8_t)G_RDPPIPESYNC:        return "G_RDPPIPESYNC";
    case (uint8_t)G_RDPTILESYNC:        return "G_RDPTILESYNC";
    case (uint8_t)G_RDPFULLSYNC:        return "G_RDPFULLSYNC";
    case (uint8_t)G_SETSCISSOR:         return "G_SETSCISSOR";
    case (uint8_t)G_SETPRIMCOLOR:       return "G_SETPRIMCOLOR";
    case (uint8_t)G_SETENVCOLOR:        return "G_SETENVCOLOR";
    case (uint8_t)G_SETCOMBINE:         return "G_SETCOMBINE";
    case (uint8_t)G_SETTIMG:            return "G_SETTIMG";
    case (uint8_t)G_SETZIMG:            return "G_SETZIMG";
    case (uint8_t)G_SETCIMG:            return "G_SETCIMG";
    case (uint8_t)G_SETTILE:            return "G_SETTILE";
    case (uint8_t)G_LOADTILE:           return "G_LOADTILE";
    case (uint8_t)G_LOADBLOCK:          return "G_LOADBLOCK";
    case (uint8_t)G_SETTILESIZE:        return "G_SETTILESIZE";
    case (uint8_t)G_LOADTLUT:           return "G_LOADTLUT";
    case (uint8_t)G_SETFILLCOLOR:       return "G_SETFILLCOLOR";
    case (uint8_t)G_SETFOGCOLOR:        return "G_SETFOGCOLOR";
    case (uint8_t)G_FILLRECT:           return "G_FILLRECT";
    case (uint8_t)G_RDPSETOTHERMODE:    return "G_RDPSETOTHERMODE";
    default:                            break;
    }
    return nullptr;
}

const char *canonicalOpcodeName(uint8_t opcode)
{
    /* RT64's F3DPD map (rt64_gbi_f3dpd.cpp:23-27 over rt64_gbi_f3d.cpp).
     * Where the numbering collides with the port's, the name differs and that
     * is the whole reason this table is separate: 0x00 is G_POPMTX to the port
     * (gbi.h:113, dispatched at gfx_pc.cpp:2325) and G_SPNOOP - the RT64
     * extended-GBI hook - to RT64. */
    switch (opcode) {
    case 0x00: return "G_SPNOOP";
    case 0x01: return "G_MTX";
    case 0x03: return "G_MOVEMEM";
    case 0x04: return "G_VTX";
    case 0x06: return "G_DL";
    case 0x07: return "G_VTXCOLOR_PD";
    case 0x09: return "G_SPRITE2D_BASE";
    case 0xB1: return "G_TRIX";
    case 0xB3: return "G_RDPHALF_2";
    case 0xB4: return "G_RDPHALF_1";
    case 0xB5: return "G_QUAD";
    case 0xB6: return "G_CLEARGEOMETRYMODE";
    case 0xB7: return "G_SETGEOMETRYMODE";
    case 0xB8: return "G_ENDDL";
    case 0xB9: return "G_SETOTHERMODE_L";
    case 0xBA: return "G_SETOTHERMODE_H";
    case 0xBB: return "G_TEXTURE";
    case 0xBC: return "G_MOVEWORD";
    case 0xBD: return "G_POPMTX";
    case 0xBE: return "G_CULLDL";
    case 0xBF: return "G_TRI1";
    case 0xC0: return "G_NOOP";
    default:   break;
    }
    /* RDP opcodes are shared verbatim between the dialects. */
    if (opcode >= 0xE4) {
        return gfxOpcodeName(opcode);
    }
    return nullptr;
}

/*
 * === Port-dialect disassembly ===
 */

namespace {

struct PortDisasm {
    MemReader &mem;
    const DisasmOptions &opts;
    GfxWalkState st;
    AddrNamer namer;
    std::string out;
    size_t emitted = 0;
    std::vector<uint8_t> scratch;

    PortDisasm(MemReader &m, const DisasmOptions &o) : mem(m), opts(o) {}

    /* Names a reference without ever printing a host address. An unresolved
     * segmented reference names the segment instead, which is both stable and
     * more informative than the zero it resolved to. */
    std::string refName(const GfxRef &r, uint32_t len)
    {
        /* An unbound segment is the one case where the resolved address is a
         * wild pointer fast3d would dereference. Naming the segment is both
         * stable and the more useful thing to see. */
        if (r.unbound) {
            return fmt("seg[%u]+0x%x <unbound>", r.segment, r.segOffset);
        }
        if (!r.addr) {
            return "null";
        }
        return namer.name(r.addr, len, mem.regionOf(r.addr, len ? len : 1));
    }

    /* Same, for the commands whose w1 is an address the walker does not model
     * as a GfxRef (image and cache-invalidation targets). */
    std::string addrName(uintptr_t w1)
    {
        bool unbound = false;
        const uintptr_t a = gfxSegResolve(w1, st.segments, &unbound);
        if (unbound) {
            return fmt("seg[%u]+0x%x <unbound>", (unsigned)((w1 & 0x0f000000) >> 24),
                       (unsigned)(w1 & 0x00fffffe));
        }
        return a ? namer.name(a, 1, mem.regionOf(a, 1)) : std::string("null");
    }

    /* `name size hash` for a referenced payload - never the bytes themselves
     * (CLAUDE.md invariant 7). The hash covers the whole extent the capture
     * records, [addr, addr + startOffset + bytes), so a golden taken live and
     * one replayed from a capture hash the same range. */
    std::string payload(const GfxRef &r)
    {
        const uint32_t total = r.startOffset + r.bytes;
        const std::string nm = refName(r, total);
        if (!opts.hashPayloads || !r.addr || r.unbound || !total) {
            return nm;
        }
        scratch.resize(total);
        if (!mem.read(r.addr, scratch.data(), total)) {
            return nm + fmt(" %uB <unreadable>", total);
        }
        return nm + fmt(" %uB h=%016llx", total,
                        (unsigned long long)hashBytes(scratch.data(), total));
    }

    void line(int depth, const Gfx &g, const std::string &fields)
    {
        const uint8_t opcode = (uint8_t)(g.words.w0 >> 24);
        const char *nm = gfxOpcodeName(opcode);
        for (int i = 0; i < depth; ++i) {
            out += "  ";
        }
        out += fmt("%-24s w0=%08x", nm ? nm : fmt("G_UNK_%02x", opcode).c_str(),
                   (uint32_t)g.words.w0);
        if (!fields.empty()) {
            out += " | ";
            out += fields;
        }
        out += '\n';
    }

    bool readCmd(uintptr_t at, Gfx *dst, uint32_t words)
    {
        return mem.read(at, dst, sizeof(Gfx) * words);
    }

    void walk(uintptr_t at, int depth);
};

void PortDisasm::walk(uintptr_t at, int depth)
{
    /* 16 levels is far past anything the game emits and stops a corrupt or
     * self-referential list from exhausting the stack. */
    if (depth > 16) {
        out += "  <nesting too deep>\n";
        return;
    }

    for (;;) {
        if (emitted >= opts.maxCommands) {
            out += "  <command limit reached>\n";
            return;
        }

        Gfx g[3] = {};
        if (!readCmd(at, &g[0], 1)) {
            out += fmt("  <unreadable command at %s>\n",
                       namer.name(at, sizeof(Gfx), mem.regionOf(at, sizeof(Gfx))).c_str());
            return;
        }

        const uint8_t opcode = (uint8_t)(g[0].words.w0 >> 24);
        const uint32_t words = gfxCommandWords(opcode);
        if (words > 1 && !readCmd(at, &g[0], words)) {
            out += "  <unreadable operand words>\n";
            return;
        }

        const GfxRef ref = gfxStep(st, &g[0]);
        ++emitted;

        std::string f;
        switch (opcode) {
        case (uint8_t)G_POPMTX:
            /* gfx_pc.cpp:2325-2327: the operand is ignored, one pop always. */
            f = "n=1";
            break;

        case G_MTX:
            /* gfx_pc.cpp:2322 */
            f = fmt("params=%02x mtx=%s", c0(g[0], 16, 8), payload(ref).c_str());
            break;

        case G_MOVEMEM:
            /* gfx_pc.cpp:2331 */
            f = fmt("index=%02x src=%s", c0(g[0], 16, 8), payload(ref).c_str());
            break;

        case (uint8_t)G_MOVEWORD: {
            /* gfx_pc.cpp:2334, gfx_sp_moveword at :1757-1773. G_MW_SEGMENT is
             * the one case whose operand is an address; print the binding, not
             * the pointer. */
            const uint32_t index = c0(g[0], 0, 8);
            const uint32_t offset = c0(g[0], 8, 16);
            if (index == G_MW_SEGMENT) {
                const uint32_t seg = (offset >> 2) & 0x0f;
                const uintptr_t base = (uintptr_t)g[0].words.w1;
                f = fmt("SEGMENT seg=%u -> %s", seg,
                        base ? namer.name(base, 1, mem.regionOf(base, 1)).c_str()
                             : "null");
            } else {
                f = fmt("index=%02x offset=%04x w1=%08x", index, offset,
                        (uint32_t)g[0].words.w1);
            }
            break;
        }

        case (uint8_t)G_TEXTURE:
            /* gfx_pc.cpp:2337 */
            f = fmt("sc=%u tc=%u level=%u tile=%u on=%u", c1(g[0], 16, 16),
                    c1(g[0], 0, 16), c0(g[0], 11, 3), c0(g[0], 8, 3),
                    c0(g[0], 0, 8));
            break;

        case G_VTX:
            /* gfx_pc.cpp:2340: the count is a BYTE total, not a vertex count. */
            f = fmt("n=%u idx=%u vtx=%s", c0(g[0], 0, 16) / (uint32_t)sizeof(Vtx),
                    c0(g[0], 16, 4), payload(ref).c_str());
            break;

        case G_COL:
            /* gfx_pc.cpp:2376 */
            f = fmt("n=%u col=%s", c0(g[0], 0, 16) / (uint32_t)sizeof(Col),
                    payload(ref).c_str());
            break;

        case G_DL:
            /* gfx_pc.cpp:2342-2354: w0[16] selects call (0) or branch (1). */
            f = fmt("%s -> %s", c0(g[0], 16, 1) ? "branch" : "call",
                    refName(ref, sizeof(Gfx)).c_str());
            break;

        case (uint8_t)G_SETGEOMETRYMODE:
        case (uint8_t)G_CLEARGEOMETRYMODE:
            /* gfx_pc.cpp:2358-2363 */
            f = fmt("mode=%08x", (uint32_t)g[0].words.w1);
            break;

        case G_EXTRAGEOMETRYMODE_EXT:
            /* gfx_pc.cpp:2365: the clear mask is the inverse of w0[0:24]. */
            f = fmt("clear=%08x set=%08x", ~c0(g[0], 0, 24),
                    (uint32_t)g[0].words.w1);
            break;

        case (uint8_t)G_TRI1:
            /* gfx_pc.cpp:2368: indices are stored times 10. */
            f = fmt("v=%u,%u,%u", c1(g[0], 16, 8) / 10, c1(g[0], 8, 8) / 10,
                    c1(g[0], 0, 8) / 10);
            break;

        case (uint8_t)G_TRI4: {
            /* gfx_sp_tri4, gfx_pc.cpp:1618-1650. Four triangles, each dropped
             * when all three indices are zero. */
            f = "v=";
            for (uint32_t i = 0; i < 4; ++i) {
                const uint32_t x = c1(g[0], i * 8, 4);
                const uint32_t y = c1(g[0], i * 8 + 4, 4);
                const uint32_t z = c0(g[0], i * 4, 4);
                if (x || y || z) {
                    f += fmt("%s%u,%u,%u", i ? " " : "", x, y, z);
                }
            }
            break;
        }

        case (uint8_t)G_SETOTHERMODE_L:
            /* gfx_pc.cpp:2371 */
            f = fmt("shift=%u bits=%u val=%08x", c0(g[0], 8, 8), c0(g[0], 0, 8),
                    (uint32_t)g[0].words.w1);
            break;

        case (uint8_t)G_SETOTHERMODE_H:
            /* gfx_pc.cpp:2374: shift is biased by 32 into the high word. */
            f = fmt("shift=%u bits=%u val=%08x", c0(g[0], 8, 8) + 32,
                    c0(g[0], 0, 8), (uint32_t)g[0].words.w1);
            break;

        case (uint8_t)G_SETTIMG:
            /* gfx_pc.cpp:2381 */
            f = fmt("fmt=%u siz=%u width=%u img=%s", c0(g[0], 21, 3),
                    c0(g[0], 19, 2), c0(g[0], 0, 10) + 1,
                    addrName((uintptr_t)g[0].words.w1).c_str());
            break;

        case (uint8_t)G_LOADBLOCK:
            /* gfx_pc.cpp:2395 */
            f = fmt("tile=%u uls=%u ult=%u lrs=%u dxt=%u texels=%s",
                    c1(g[0], 24, 3), c0(g[0], 12, 12), c0(g[0], 0, 12),
                    c1(g[0], 12, 12), c1(g[0], 0, 12), payload(ref).c_str());
            break;

        case (uint8_t)G_LOADTILE:
            /* gfx_pc.cpp:2398 */
            f = fmt("tile=%u uls=%u ult=%u lrs=%u lrt=%u texels=%s",
                    c1(g[0], 24, 3), c0(g[0], 12, 12), c0(g[0], 0, 12),
                    c1(g[0], 12, 12), c1(g[0], 0, 12), payload(ref).c_str());
            break;

        case (uint8_t)G_LOADTLUT:
            /* gfx_pc.cpp:2407 */
            f = fmt("tile=%u uls=%u ult=%u lrs=%u lrt=%u tlut=%s",
                    c1(g[0], 24, 3), c0(g[0], 14, 10), c0(g[0], 2, 10),
                    c1(g[0], 14, 10), c1(g[0], 2, 10), payload(ref).c_str());
            break;

        case (uint8_t)G_SETTILE:
            /* gfx_pc.cpp:2401-2402 */
            f = fmt("fmt=%u siz=%u line=%u tmem=%u tile=%u pal=%u "
                    "cmt=%u maskt=%u shiftt=%u cms=%u masks=%u shifts=%u",
                    c0(g[0], 21, 3), c0(g[0], 19, 2), c0(g[0], 9, 9),
                    c0(g[0], 0, 9), c1(g[0], 24, 3), c1(g[0], 20, 4),
                    c1(g[0], 18, 2), c1(g[0], 14, 4), c1(g[0], 10, 4),
                    c1(g[0], 8, 2), c1(g[0], 4, 4), c1(g[0], 0, 4));
            break;

        case (uint8_t)G_SETTILESIZE:
            /* gfx_pc.cpp:2404 */
            f = fmt("tile=%u uls=%u ult=%u lrs=%u lrt=%u", c1(g[0], 24, 3),
                    c0(g[0], 12, 12), c0(g[0], 0, 12), c1(g[0], 12, 12),
                    c1(g[0], 0, 12));
            break;

        case (uint8_t)G_SETENVCOLOR:
        case (uint8_t)G_SETFOGCOLOR:
        case G_SETINTENSITY_EXT:
            /* gfx_pc.cpp:2410,2416,2419 */
            f = fmt("rgba=%u,%u,%u,%u", c1(g[0], 24, 8), c1(g[0], 16, 8),
                    c1(g[0], 8, 8), c1(g[0], 0, 8));
            break;

        case (uint8_t)G_SETPRIMCOLOR:
            /* gfx_pc.cpp:2413 */
            f = fmt("m=%u l=%u rgba=%u,%u,%u,%u", c0(g[0], 8, 8),
                    c0(g[0], 0, 8), c1(g[0], 24, 8), c1(g[0], 16, 8),
                    c1(g[0], 8, 8), c1(g[0], 0, 8));
            break;

        case (uint8_t)G_SETFILLCOLOR:
            f = fmt("color=%08x", (uint32_t)g[0].words.w1);
            break;

        case (uint8_t)G_SETCOMBINE:
            /* gfx_pc.cpp:2422-2426: the mux is 56 bits across both words. */
            f = fmt("mux=%06x%08x", c0(g[0], 0, 24), (uint32_t)g[0].words.w1);
            break;

        case (uint8_t)G_TEXRECT:
        case (uint8_t)G_TEXRECTFLIP:
            /* gfx_pc.cpp:2435-2450, three commands. */
            f = fmt("ul=%u,%u lr=%u,%u tile=%u st=%u,%u d=%u,%u",
                    c1(g[0], 12, 12), c1(g[0], 0, 12), c0(g[0], 12, 12),
                    c0(g[0], 0, 12), c1(g[0], 24, 3), c1(g[1], 16, 16),
                    c1(g[1], 0, 16), c1(g[2], 16, 16), c1(g[2], 0, 16));
            break;

        case (uint8_t)G_FILLRECT:
            /* gfx_pc.cpp:2453 */
            f = fmt("ul=%u,%u lr=%u,%u", c1(g[0], 12, 12), c1(g[0], 0, 12),
                    c0(g[0], 12, 12), c0(g[0], 0, 12));
            break;

        case G_FILLRECT_WIDE_EXT:
            /* gfx_pc.cpp:2455-2462: 24-bit signed coordinates, two commands. */
            f = fmt("ul=%d,%d lr=%d,%d", (int32_t)(c0(g[1], 0, 24) << 8) >> 8,
                    (int32_t)(c1(g[1], 0, 24) << 8) >> 8,
                    (int32_t)(c0(g[0], 0, 24) << 8) >> 8,
                    (int32_t)(c1(g[0], 0, 24) << 8) >> 8);
            break;

        case G_TEXRECT_WIDE_EXT:
            /* gfx_pc.cpp:2464-2481, three commands. */
            f = fmt("ul=%d,%d lr=%d,%d tile=%u flip=%u st=%u,%u d=%u,%u",
                    (int32_t)(c0(g[1], 0, 24) << 8) >> 8,
                    (int32_t)(c1(g[1], 0, 24) << 8) >> 8,
                    (int32_t)(c0(g[0], 0, 24) << 8) >> 8,
                    (int32_t)(c1(g[0], 0, 24) << 8) >> 8, c1(g[0], 24, 3),
                    c1(g[0], 27, 1), c0(g[2], 16, 16), c0(g[2], 0, 16),
                    c1(g[2], 16, 16), c1(g[2], 0, 16));
            break;

        case G_IMAGERECT_EXT:
            /* gfx_pc.cpp:2483-2501, three commands. */
            f = fmt("tile=%u img=%ux%u p0=%d,%d st0=%d,%d p1=%d,%d st1=%d,%d",
                    c0(g[0], 0, 3), c1(g[0], 16, 16), c1(g[0], 0, 16),
                    (int16_t)c0(g[1], 16, 16), (int16_t)c0(g[1], 0, 16),
                    (int16_t)c1(g[1], 16, 16), (int16_t)c1(g[1], 0, 16),
                    (int16_t)c0(g[2], 16, 16), (int16_t)c0(g[2], 0, 16),
                    (int16_t)c1(g[2], 16, 16), (int16_t)c1(g[2], 0, 16));
            break;

        case (uint8_t)G_SETSCISSOR:
            /* gfx_pc.cpp:2504 */
            f = fmt("mode=%u ul=%u,%u lr=%u,%u", c1(g[0], 24, 2),
                    c0(g[0], 12, 12), c0(g[0], 0, 12), c1(g[0], 12, 12),
                    c1(g[0], 0, 12));
            break;

        case (uint8_t)G_SETZIMG:
        case (uint8_t)G_SETCIMG:
            /* gfx_pc.cpp:2507,2510 */
            f = fmt("img=%s", addrName((uintptr_t)g[0].words.w1).c_str());
            break;

        case G_SETFB_EXT:
            /* gfx_pc.cpp:2513-2522: w1 is a framebuffer id, not an address. */
            f = fmt("fb=%u", (uint32_t)g[0].words.w1);
            break;

        case G_SETTIMG_FB_EXT:
            /* gfx_pc.cpp:2385-2390 */
            f = fmt("fb=%u", (uint32_t)g[0].words.w1);
            break;

        case G_COPYFB_EXT:
            /* gfx_pc.cpp:2524 */
            f = fmt("dst=%u src=%u x=%d y=%d flip=%u", c0(g[0], 11, 11),
                    c0(g[0], 0, 11), (int16_t)c1(g[0], 16, 16),
                    (int16_t)c1(g[0], 0, 16), c0(g[0], 22, 1));
            break;

        case G_INVALTEXCACHE_EXT:
            /* gfx_pc.cpp:2530-2535: null clears the whole cache. */
            f = g[0].words.w1 ? fmt("tex=%s", addrName((uintptr_t)g[0].words.w1).c_str())
                              : std::string("clear-all");
            break;

        case G_SETGRAYSCALE_EXT:
            f = fmt("on=%u", (uint32_t)g[0].words.w1);
            break;

        case G_SETSUBPIXELOFFSET_EXT:
            /* gfx_pc.cpp:2430 */
            f = fmt("x=%u y=%u", c0(g[0], 0, 16), c1(g[0], 0, 16));
            break;

        case (uint8_t)G_RDPSETOTHERMODE:
            /* gfx_pc.cpp:2527 */
            f = fmt("h=%06x l=%08x", c0(g[0], 0, 24), (uint32_t)g[0].words.w1);
            break;

        default:
            /* Syncs, RDPHALFs and G_NOOP carry nothing fast3d reads. Anything
             * genuinely unknown is fatal in fast3d (gfx_pc.cpp:2560), so an
             * unnamed opcode here means the walk has desynchronised - say so
             * rather than printing a plausible-looking line. */
            if (!gfxOpcodeName(opcode)) {
                line(depth, g[0], fmt("w1=%08x  <unknown opcode, walk may be "
                                      "desynchronised>",
                                      (uint32_t)g[0].words.w1));
                return;
            }
            break;
        }

        line(depth, g[0], f);

        if (opcode == (uint8_t)G_ENDDL) {
            return;
        }

        if (opcode == G_DL) {
            if (!ref.addr || ref.unbound) {
                /* fast3d skips a null call and would branch through an unbound
                 * segment into garbage; do neither, and say why in the dump. */
                out += "  <unresolved display list, not followed>\n";
                if (c0(g[0], 16, 1)) {
                    return;
                }
            } else if (c0(g[0], 16, 1)) {
                at = ref.addr;
                continue;
            } else if (opts.followCalls) {
                walk(ref.addr, depth + 1);
            }
        }

        at += sizeof(Gfx) * words;
    }
}

} // namespace

std::string disasmPortDl(uintptr_t rootDl, MemReader &mem,
                         const DisasmOptions &opts,
                         const uintptr_t *seedSegments)
{
    PortDisasm d(mem, opts);
    d.st.reset(seedSegments);
    d.walk(rootDl, 0);
    return d.out;
}

/*
 * === Canonical disassembly ===
 */

namespace {

/* Canonical commands are {uint32 w0, uint32 w1} at their natural offsets: the
 * emulator convention leaves 32-bit words readable as host uint32
 * (rt64_mem.h). */
inline uint32_t word32(const uint8_t *rdramBase, uint32_t off)
{
    uint32_t v;
    memcpy(&v, rdramBase + off, sizeof(v));
    return v;
}

inline uint32_t p(uint32_t w, uint32_t pos, uint32_t bits)
{
    return (w >> pos) & ((1u << bits) - 1);
}

} // namespace

bool decodeCanonicalCmd(const uint8_t *rdramBase, RdramAddr at, size_t rdramSize,
                        DecodedCmd *out)
{
    if (!rdramBase || !out) {
        return false;
    }

    const uint32_t off = at & ~kExtendedAddrBit;
    if ((off & 3u) || (size_t)off + 8u > rdramSize) {
        return false;
    }

    const uint32_t w0 = word32(rdramBase, off);
    const uint32_t w1 = word32(rdramBase, off + 4);
    const uint8_t opcode = (uint8_t)(w0 >> 24);

    *out = DecodedCmd{};
    out->opcode = opcode;
    out->words = 1;

    /* Field bags are per-opcode and exist only to be compared against the
     * translator's own reading of the same command. Each case cites the RT64
     * decoder it mirrors; the order of fields is this decoder's contract. */
    switch (opcode) {
    case 0x00: /* G_SPNOOP / RT64 extended-GBI hook, rt64_gbi_extended.cpp:334 */
        out->fields[0] = p(w0, 0, 24);  // magic
        out->fields[1] = p(w1, 28, 4);  // hook op
        out->fields[2] = p(w1, 0, 28);  // hook value
        out->fieldCount = 3;
        return true;

    case 0x01: /* G_MTX, rt64_gbi_f3d.cpp:17 */
        out->fields[0] = p(w0, 16, 8);  // params
        out->fields[1] = w1;            // address
        out->fieldCount = 2;
        return true;

    case 0x03: /* G_MOVEMEM, rt64_gbi_f3d.cpp:27 */
        out->fields[0] = p(w0, 16, 8);  // index
        out->fields[1] = w1;            // address
        out->fieldCount = 2;
        return true;

    case 0x04: /* G_VTX, rt64_gbi_f3dpd.cpp:14 - count is stored biased. */
        out->fields[0] = p(w0, 20, 4) + 1;  // vertex count
        out->fields[1] = p(w0, 16, 4);      // destination index
        out->fields[2] = w1;                // address
        out->fieldCount = 3;
        return true;

    case 0x06: /* G_DL, rt64_gbi_f3d.cpp:76 */
        out->fields[0] = p(w0, 16, 1);  // 0 = call, 1 = branch
        out->fields[1] = w1;            // address
        out->fieldCount = 2;
        return true;

    case 0x07: /* F3DPD_G_VTXCOLOR, rt64_gbi_f3dpd.cpp:18 - address only. */
        out->fields[0] = w1;
        out->fieldCount = 1;
        return true;

    case 0xB1: { /* F3DGOLDEN_G_TRIX, rt64_gbi_f3dgolden.cpp:12. RT64 stops at
                  * the first zero w1 remainder, so trailing triangles are
                  * dropped; decode exactly that many. */
        uint32_t a = w0, b = w1;
        uint8_t n = 0;
        while (b != 0 && n < 4) {
            const uint32_t v0 = b & 0xf;
            b >>= 4;
            const uint32_t v1 = b & 0xf;
            b >>= 4;
            const uint32_t v2 = a & 0xf;
            a >>= 4;
            out->fields[n * 2] = (v0 << 8) | (v1 << 4) | v2;
            ++n;
        }
        out->fields[7] = n;
        out->fieldCount = 8;
        return true;
    }

    case 0xB6: /* G_CLEARGEOMETRYMODE */
    case 0xB7: /* G_SETGEOMETRYMODE, rt64_gbi_f3d.cpp */
        out->fields[0] = w1;
        out->fieldCount = 1;
        return true;

    case 0xB8: /* G_ENDDL */
        out->fieldCount = 0;
        return true;

    case 0xB9: /* G_SETOTHERMODE_L */
    case 0xBA: /* G_SETOTHERMODE_H, rt64_gbi_f3d.cpp:152 */
        out->fields[0] = p(w0, 0, 8);  // bit count
        out->fields[1] = p(w0, 8, 8);  // shift
        out->fields[2] = w1;           // value
        out->fieldCount = 3;
        return true;

    case 0xBB: /* G_TEXTURE, rt64_gbi_f3d.cpp:143 */
        out->fields[0] = p(w0, 8, 3);    // tile
        out->fields[1] = p(w0, 11, 3);   // level
        out->fields[2] = p(w0, 0, 8);    // on
        out->fields[3] = p(w1, 16, 16);  // sc
        out->fields[4] = p(w1, 0, 16);   // tc
        out->fieldCount = 5;
        return true;

    case 0xBC: /* G_MOVEWORD, rt64_gbi_f3d.cpp:110 */
        out->fields[0] = p(w0, 0, 8);    // index
        out->fields[1] = p(w0, 8, 16);   // offset
        out->fields[2] = w1;             // value
        out->fieldCount = 3;
        return true;

    case 0xBD: /* G_POPMTX, rt64_gbi_f3d.cpp:21 - pops only when w1 is zero. */
        out->fields[0] = w1;
        out->fieldCount = 1;
        return true;

    case 0xBF: /* G_TRI1, rt64_gbi_f3d.cpp:93 - indices are stored times 10. */
        out->fields[0] = p(w1, 16, 8) / 10;
        out->fields[1] = p(w1, 8, 8) / 10;
        out->fields[2] = p(w1, 0, 8) / 10;
        out->fieldCount = 3;
        return true;

    case 0xC0: /* G_NOOP */
        out->fieldCount = 0;
        return true;

    default:
        break;
    }

    /* RDP commands pass through unchanged in both dialects; a raw word pair is
     * the honest normalisation for them. */
    if (opcode >= 0xE4) {
        out->fields[0] = p(w0, 0, 24);
        out->fields[1] = w1;
        out->fieldCount = 2;
        return true;
    }

    return false;
}

std::string disasmCanonicalDl(const uint8_t *rdramBase, RdramAddr start,
                              RdramAddr end, const DisasmOptions &opts)
{
    std::string out;
    const uint32_t from = start & ~kExtendedAddrBit;
    const uint32_t to = end & ~kExtendedAddrBit;

    if (!rdramBase || to < from) {
        return "<invalid canonical range>\n";
    }

    /* The extended opcode is whatever the stream's own enable hook registered
     * (rt64_gbi_extended.cpp:346-353); RT64 learns it the same way, so the
     * disassembler must not assume a constant. */
    uint8_t extendedOpcode = 0;
    size_t emitted = 0;

    for (uint32_t off = from; off + 8u <= to; off += 8) {
        if (emitted++ >= opts.maxCommands) {
            out += "<command limit reached>\n";
            break;
        }

        const uint32_t w0 = word32(rdramBase, off);
        const uint32_t w1 = word32(rdramBase, off + 4);
        const uint8_t opcode = (uint8_t)(w0 >> 24);

        std::string f;
        const char *nm = nullptr;

        if (extendedOpcode && opcode == extendedOpcode) {
            nm = "G_EX";
            f = fmt("op=%06x w1=%08x", p(w0, 0, 24), w1);
        } else if (opcode == 0x00 && p(w0, 0, 24) == 0x525464) {
            /* The enable hook, which is also how this disassembler discovers
             * the extended opcode for the rest of the stream. */
            nm = "RT64_HOOK";
            const uint32_t op = p(w1, 28, 4);
            const uint32_t val = p(w1, 0, 28);
            if (op == 0x1) {
                extendedOpcode = (uint8_t)(val & 0xff);
                f = fmt("ENABLE ext_opcode=%02x", extendedOpcode);
            } else {
                f = fmt("op=%u value=%08x", op, val);
            }
        } else {
            nm = canonicalOpcodeName(opcode);
            DecodedCmd d;
            if (decodeCanonicalCmd(rdramBase, off, (size_t)to, &d) && d.fieldCount) {
                f = "fields=";
                for (uint8_t i = 0; i < d.fieldCount; ++i) {
                    f += fmt("%s%08x", i ? "," : "", d.fields[i]);
                }
            } else {
                f = fmt("w1=%08x", w1);
            }
        }

        out += fmt("%08x  %-20s w0=%08x", off, nm ? nm : fmt("UNK_%02x", opcode).c_str(), w0);
        if (!f.empty()) {
            out += " | ";
            out += f;
        }
        out += '\n';

        if (opcode == 0xB8) { /* G_ENDDL */
            break;
        }
    }

    return out;
}

} // namespace pdrt64
