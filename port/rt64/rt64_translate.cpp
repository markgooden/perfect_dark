/*
 * Display-list translator, T5 scope: control flow and geometry.
 * Contract, dialect differences and swizzle reasoning are in rt64_translate.h.
 *
 * Every lowering below cites the RT64 decoder it must satisfy. That is not
 * decoration: the two dialects share opcode numbers while disagreeing about
 * what the fields mean, so "it has the same opcode" is never a reason to pass
 * a command through.
 */

#include "rt64_translate.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace pdrt64 {

namespace {

/* Values from rt64/include/rt64_extended_gbi.h, restated rather than included:
 * SCAFFOLD 3 makes rt64_host.h the only file that includes RT64 headers, so
 * that the translator stays compilable with nothing but the toolchain and the
 * unit tests keep working without RT64 on the include path. Each is pinned by
 * a test against the value RT64 actually checks for. */
constexpr uint32_t kRt64HookMagic = 0x525464;      // rt64_extended_gbi.h:23
constexpr uint32_t kRt64HookOpEnable = 0x1;        // rt64_extended_gbi.h:17
constexpr uint32_t kGexSetRdramExtendedV1 = 0x2C;  // rt64_extended_gbi.h:75

/* Canonical opcodes, from RT64's F3D/F3DPD maps. Named here rather than
 * included so the numbers sit next to the lowering that uses them. */
constexpr uint8_t kCanonSpNoop = 0x00;
constexpr uint8_t kCanonMtx = 0x01;
constexpr uint8_t kCanonMoveMem = 0x03;
constexpr uint8_t kCanonVtx = 0x04;
constexpr uint8_t kCanonVtxColorPD = 0x07;
constexpr uint8_t kCanonTriX = 0xB1;
constexpr uint8_t kCanonClearGeometryMode = 0xB6;
constexpr uint8_t kCanonSetGeometryMode = 0xB7;
constexpr uint8_t kCanonEndDl = 0xB8;
constexpr uint8_t kCanonSetOtherModeL = 0xB9;
constexpr uint8_t kCanonSetOtherModeH = 0xBA;
constexpr uint8_t kCanonTexture = 0xBB;
constexpr uint8_t kCanonMoveWord = 0xBC;
constexpr uint8_t kCanonPopMtx = 0xBD;
constexpr uint8_t kCanonTri1 = 0xBF;
constexpr uint8_t kCanonNoop = 0xC0;

/* RT64's setVertexPD packs the count into four bits as count-1
 * (rt64_gbi_f3dpd.cpp:15), so a single command can carry at most 16 vertices.
 * The port's byte total has no such limit, and a list that exceeded it would
 * silently wrap. */
constexpr uint32_t kMaxVerticesPerCommand = 16;

/* Guard rails for a corrupt or self-referential list. Neither should ever be
 * reached: the deepest real nesting measured is 2 and the busiest captured
 * frame is 10,468 commands (docs/census.md). */
constexpr int kMaxDepth = 16;
constexpr uint32_t kMaxCommands = 200000;

inline uint32_t fieldOf(const Gfx &g, uint32_t pos, uint32_t width)
{
    return (uint32_t)((g.words.w0 >> pos) & ((1ULL << width) - 1));
}

/* An address is only legal in the output if it is tagged, aligned and free of
 * the port's flag bits (invariant 4). */
bool addressIsWellFormed(RdramAddr a, size_t arenaSize)
{
    if ((a & kExtendedAddrBit) == 0) {
        return false;
    }
    const uint32_t off = a & ~kExtendedAddrBit;
    return (off & 3u) == 0 && off < arenaSize;
}

} // namespace

Translator::Translator(Arena &arena, FbRegistry &fbs, MemReader &mem)
    : arena_(arena), fbs_(fbs), mem_(mem)
{
    out_.reserve(16384);
}

void Translator::beginFrame()
{
    st_.reset();
    invertCulling_ = false;
    stats_ = TranslateStats{};
    error_.clear();
}

void Translator::emit(uint32_t w0, uint32_t w1)
{
    out_.push_back(w0);
    out_.push_back(w1);
    ++stats_.commandsOut;
}

void Translator::emitStreamPrefix()
{
    /* Hook: SPNOOP carrying the magic in w0[0:24], the op in w1[28:4] and the
     * opcode to register in w1[0:28] (rt64_gbi_extended.cpp:334-353). */
    emit(((uint32_t)kCanonSpNoop << 24) | kRt64HookMagic,
         (kRt64HookOpEnable << 28) | kExtendedOpcode);

    /* Extended addressing on, so the tagged addresses below resolve
     * (rt64_gbi_extended.cpp:292-295). */
    emit(((uint32_t)kExtendedOpcode << 24) | kGexSetRdramExtendedV1, 1u);
}

bool Translator::pushBlock(const GfxRef &ref, Swizzle type, RdramAddr *out)
{
    const size_t len = (size_t)ref.startOffset + ref.bytes;
    if (!ref.addr || !len || ref.unbound) {
        error_ = "reference has no resolvable source address";
        return false;
    }

    /* Read first, purely to find out whether the source is available.
     * Arena::pushFrameData aborts on an unreadable source (rt64_arena.cpp:108-
     * 115), which is right inside the running game but would take the test
     * runner down when replaying a capture, and a capture miss is a result the
     * caller is meant to be able to handle. The redundant copy costs a memcpy
     * of ~100 KB per frame at census-measured volumes. */
    if (scratch_.size() < len) {
        scratch_.resize(len);
    }
    if (!mem_.read(ref.addr, scratch_.data(), len)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "capture miss reading %zu bytes at 0x%llx", len,
                 (unsigned long long)ref.addr);
        error_ = buf;
        return false;
    }

    /* Nothing is cached at this scope. The arena's cache has no invalidation
     * beyond the texture-cache hooks and clearCache, while the heap blocks
     * these commands point at are rewritten between frames (docs/census.md),
     * so a cache keyed on source address would serve stale geometry. Textures
     * and TLUTs are the blocks with a real caching case and they arrive in
     * T6, together with the hooks that invalidate them. */
    *out = arena_.pushFrameData(mem_, ref.addr, len, type);
    stats_.marshalledBytes += len;
    return true;
}

bool Translator::validateEmitted(size_t pairIndex, const DecodedCmd &expect)
{
    /* Decode the bytes we are about to hand RT64, using rt64_debug's decoder -
     * written from RT64's headers and sharing no code with this file. It
     * cannot prove the lowering is semantically right, but it does catch the
     * failure this stage actually produces: a field packed at the wrong offset
     * or width, which is invisible until something renders wrong. */
    uint8_t bytes[8];
    memcpy(bytes, &out_[pairIndex], 4);
    memcpy(bytes + 4, &out_[pairIndex + 1], 4);

    DecodedCmd got;
    if (!decodeCanonicalCmd(bytes, 0, sizeof(bytes), &got)) {
        char buf[96];
        snprintf(buf, sizeof(buf), "emitted opcode 0x%02x is not one RT64 decodes",
                 (unsigned)(out_[pairIndex] >> 24));
        error_ = buf;
        return false;
    }

    if (got.opcode != expect.opcode || got.fieldCount != expect.fieldCount) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "emitted 0x%02x/%u fields, meant 0x%02x/%u", got.opcode,
                 got.fieldCount, expect.opcode, expect.fieldCount);
        error_ = buf;
        return false;
    }

    for (uint8_t i = 0; i < expect.fieldCount; ++i) {
        if (got.fields[i] != expect.fields[i]) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "opcode 0x%02x field %u decoded as 0x%08x, meant 0x%08x",
                     got.opcode, i, got.fields[i], expect.fields[i]);
            error_ = buf;
            return false;
        }
    }
    return true;
}

/*
 * Disposition of every opcode fast3d executes. Anything not Translated is
 * counted rather than guessed at, so a golden shows the gap.
 */
namespace {

enum class Disposition : uint8_t {
    Translated,  // handled below
    DeferredRdp, // RDP and texture path: SCAFFOLD T6
    DeferredExt, // port-private EXT opcodes: SCAFFOLD T6/T7
};

Disposition dispositionOf(uint8_t opcode)
{
    switch (opcode) {
    case (uint8_t)G_POPMTX:
    case G_MTX:
    case G_MOVEMEM:
    case G_VTX:
    case G_DL:
    case G_COL:
    case (uint8_t)G_MOVEWORD:
    case (uint8_t)G_TEXTURE:
    case (uint8_t)G_ENDDL:
    case (uint8_t)G_SETGEOMETRYMODE:
    case (uint8_t)G_CLEARGEOMETRYMODE:
    case (uint8_t)G_TRI1:
    case (uint8_t)G_TRI4:
    case (uint8_t)G_SETOTHERMODE_L:
    case (uint8_t)G_SETOTHERMODE_H:
    case (uint8_t)G_NOOP:
    case G_EXTRAGEOMETRYMODE_EXT:
        return Disposition::Translated;

    case G_SETFB_EXT:
    case G_SETTIMG_FB_EXT:
    case G_INVALTEXCACHE_EXT:
    case G_TEXRECT_WIDE_EXT:
    case G_FILLRECT_WIDE_EXT:
    case G_SETGRAYSCALE_EXT:
    case G_SETINTENSITY_EXT:
    case G_COPYFB_EXT:
    case G_IMAGERECT_EXT:
    case G_RDPFLUSH_EXT:
    case G_CLEAR_DEPTH_EXT:
    case G_SETSUBPIXELOFFSET_EXT:
        return Disposition::DeferredExt;

    default:
        break;
    }

    /* The rest of the port dialect is RDP state and rectangles. gfxOpcodeName
     * knows the full set, so an opcode it cannot name is not in the dialect at
     * all and the caller reports UnknownOpcode. */
    return Disposition::DeferredRdp;
}

} // namespace

TranslateStatus Translator::walk(uintptr_t at, int depth)
{
    if (depth > kMaxDepth) {
        error_ = "display list nesting too deep";
        return TranslateStatus::UnknownOpcode;
    }

    for (;;) {
        if (stats_.commandsIn >= kMaxCommands) {
            error_ = "command limit reached; list is probably circular";
            return TranslateStatus::UnknownOpcode;
        }

        Gfx g[3] = {};
        if (!mem_.read(at, &g[0], sizeof(Gfx))) {
            char buf[96];
            snprintf(buf, sizeof(buf), "capture miss reading command at 0x%llx",
                     (unsigned long long)at);
            error_ = buf;
            return TranslateStatus::CaptureMiss;
        }

        const uint8_t opcode = (uint8_t)(g[0].words.w0 >> 24);
        const uint32_t words = gfxCommandWords(opcode);
        if (words > 1 && !mem_.read(at, &g[0], sizeof(Gfx) * words)) {
            error_ = "capture miss reading multi-word command operands";
            return TranslateStatus::CaptureMiss;
        }

        if (!gfxOpcodeName(opcode)) {
            /* fast3d treats this as fatal (gfx_pc.cpp:2560); so must we,
             * because the walk is now at an unknown offset and everything
             * after it would be invented. */
            char buf[96];
            snprintf(buf, sizeof(buf), "unknown opcode 0x%02x at 0x%llx", opcode,
                     (unsigned long long)at);
            error_ = buf;
            return TranslateStatus::UnknownOpcode;
        }

        const GfxRef ref = gfxStep(st_, &g[0]);
        ++stats_.commandsIn;

        const Disposition how = dispositionOf(opcode);
        if (how != Disposition::Translated) {
            ++stats_.droppedCommands;
            ++stats_.droppedPerOpcode[opcode];
            at += sizeof(Gfx) * words;
            continue;
        }

        /* What we mean to emit, for the validator to check the bytes against. */
        DecodedCmd expect;
        const size_t pairIndex = out_.size();
        bool emitted = false;

        switch (opcode) {
        case (uint8_t)G_NOOP:
            emit((uint32_t)kCanonNoop << 24, 0);
            expect.opcode = kCanonNoop;
            expect.fieldCount = 0;
            emitted = true;
            break;

        case (uint8_t)G_POPMTX:
            /* The port pops one unconditionally (gfx_pc.cpp:2325-2327); RT64
             * pops only when w1 is zero (rt64_gbi_f3d.cpp:21-25), so w1 must
             * be zero and not the port's ignored operand. */
            emit((uint32_t)kCanonPopMtx << 24, 0);
            expect.opcode = kCanonPopMtx;
            expect.fields[0] = 0;
            expect.fieldCount = 1;
            emitted = true;
            break;

        case G_MTX: {
            RdramAddr addr = 0;
            if (!pushBlock(ref, Swizzle::U32, &addr)) {
                return TranslateStatus::CaptureMiss;
            }
            const uint32_t params = fieldOf(g[0], 16, 8);
            emit(((uint32_t)kCanonMtx << 24) | (params << 16), addr);
            expect.opcode = kCanonMtx;
            expect.fields[0] = params;
            expect.fields[1] = addr;
            expect.fieldCount = 2;
            emitted = true;
            break;
        }

        case G_MOVEMEM: {
            /* The swizzle depends on the index: the viewport is an array of
             * native s16 while lights are byte-addressed. See rt64_translate.h
             * for how each was established. */
            const uint32_t index = fieldOf(g[0], 16, 8);
            const Swizzle type = (index == G_MV_VIEWPORT) ? Swizzle::U16 : Swizzle::U8;
            RdramAddr addr = 0;
            if (!pushBlock(ref, type, &addr)) {
                return TranslateStatus::CaptureMiss;
            }
            emit(((uint32_t)kCanonMoveMem << 24) | (index << 16), addr);
            expect.opcode = kCanonMoveMem;
            expect.fields[0] = index;
            expect.fields[1] = addr;
            expect.fieldCount = 2;
            emitted = true;
            break;
        }

        case G_VTX: {
            /* Port: byte total in w0[0:16], destination index in w0[16:4]
             * (gfx_pc.cpp:2340). RT64: count-1 in w0[20:4], index in w0[16:4]
             * (rt64_gbi_f3dpd.cpp:15). */
            const uint32_t count = ref.bytes / (uint32_t)sizeof(Vtx);
            const uint32_t dstIndex = fieldOf(g[0], 16, 4);
            if (count == 0 || count > kMaxVerticesPerCommand) {
                char buf[96];
                snprintf(buf, sizeof(buf),
                         "G_VTX carries %u vertices; RT64 encodes at most %u",
                         count, kMaxVerticesPerCommand);
                error_ = buf;
                return TranslateStatus::UnknownOpcode;
            }
            RdramAddr addr = 0;
            if (!pushBlock(ref, Swizzle::VtxPD, &addr)) {
                return TranslateStatus::CaptureMiss;
            }
            emit(((uint32_t)kCanonVtx << 24) | ((count - 1) << 20) | (dstIndex << 16),
                 addr);
            expect.opcode = kCanonVtx;
            expect.fields[0] = count;
            expect.fields[1] = dstIndex;
            expect.fields[2] = addr;
            expect.fieldCount = 3;
            emitted = true;
            break;
        }

        case G_COL: {
            /* RT64 keeps only the base address (rt64_rsp.cpp:430-432); each
             * vertex indexes the table by byte offset through its ci field
             * (rt64_rsp.cpp:390), so the whole table must be one block. */
            RdramAddr addr = 0;
            if (!pushBlock(ref, Swizzle::U8, &addr)) {
                return TranslateStatus::CaptureMiss;
            }
            emit((uint32_t)kCanonVtxColorPD << 24, addr);
            expect.opcode = kCanonVtxColorPD;
            expect.fields[0] = addr;
            expect.fieldCount = 1;
            emitted = true;
            break;
        }

        case (uint8_t)G_MOVEWORD: {
            /* Segment bindings are resolved here and must not reach RT64
             * (rt64_translate.h); gfxStep has already applied this one. */
            if (fieldOf(g[0], 0, 8) == G_MW_SEGMENT) {
                at += sizeof(Gfx) * words;
                continue;
            }
            /* Everything else passes through. G_MW_NUMLIGHT looks like it
             * needs adjusting - the port computes (w1-0x80000000)/32 and RT64
             * ((w1-0x80000000)>>5)-1 - but each is self-consistent with its
             * own light indexing, so the word is handed over unchanged. */
            const uint32_t index = fieldOf(g[0], 0, 8);
            const uint32_t offset = fieldOf(g[0], 8, 16);
            const uint32_t value = (uint32_t)g[0].words.w1;
            emit(((uint32_t)kCanonMoveWord << 24) | (offset << 8) | index, value);
            expect.opcode = kCanonMoveWord;
            expect.fields[0] = index;
            expect.fields[1] = offset;
            expect.fields[2] = value;
            expect.fieldCount = 3;
            emitted = true;
            break;
        }

        case (uint8_t)G_TEXTURE: {
            /* Identical field layout on both sides (gfx_pc.cpp:2337,
             * rt64_gbi_f3d.cpp:143-150). */
            const uint32_t on = fieldOf(g[0], 0, 8);
            const uint32_t tile = fieldOf(g[0], 8, 3);
            const uint32_t level = fieldOf(g[0], 11, 3);
            const uint32_t sc = (uint32_t)((g[0].words.w1 >> 16) & 0xffff);
            const uint32_t tc = (uint32_t)(g[0].words.w1 & 0xffff);
            emit(((uint32_t)kCanonTexture << 24) | (level << 11) | (tile << 8) | on,
                 (sc << 16) | tc);
            expect.opcode = kCanonTexture;
            expect.fields[0] = tile;
            expect.fields[1] = level;
            expect.fields[2] = on;
            expect.fields[3] = sc;
            expect.fields[4] = tc;
            expect.fieldCount = 5;
            emitted = true;
            break;
        }

        case (uint8_t)G_TRI1: {
            const uint32_t w1 = (uint32_t)g[0].words.w1 & 0x00ffffff;
            emit((uint32_t)kCanonTri1 << 24, w1);
            expect.opcode = kCanonTri1;
            expect.fields[0] = ((w1 >> 16) & 0xff) / 10;
            expect.fields[1] = ((w1 >> 8) & 0xff) / 10;
            expect.fields[2] = (w1 & 0xff) / 10;
            expect.fieldCount = 3;
            ++stats_.trianglesIn;
            emitted = true;
            break;
        }

        case (uint8_t)G_TRI4: {
            /* Same packing as RT64's G_TRIX, so both words pass through. The
             * two differ only in when they stop: RT64 halts once the remaining
             * w1 is zero, the port skips triples whose three indices are all
             * zero. Every case that diverges is a triangle with two equal
             * vertices, which has no area. */
            const uint32_t w0 = (uint32_t)g[0].words.w0 & 0x0000ffff;
            const uint32_t w1 = (uint32_t)g[0].words.w1;
            for (uint32_t i = 0; i < 4; ++i) {
                if (((w1 >> (i * 8)) & 0xff) || ((w0 >> (i * 4)) & 0xf)) {
                    ++stats_.trianglesIn;
                }
            }
            emit(((uint32_t)kCanonTriX << 24) | w0, w1);
            /* decodeCanonicalCmd reports RT64's reading, triangle by triangle;
             * reproduce it rather than restate the words. */
            uint32_t a = ((uint32_t)kCanonTriX << 24) | w0, b = w1;
            uint8_t n = 0;
            while (b != 0 && n < 4) {
                const uint32_t v0 = b & 0xf;
                b >>= 4;
                const uint32_t v1 = b & 0xf;
                b >>= 4;
                const uint32_t v2 = a & 0xf;
                a >>= 4;
                expect.fields[n * 2] = (v0 << 8) | (v1 << 4) | v2;
                ++n;
            }
            expect.opcode = kCanonTriX;
            expect.fields[7] = n;
            expect.fieldCount = 8;
            emitted = true;
            break;
        }

        case (uint8_t)G_SETGEOMETRYMODE:
        case (uint8_t)G_CLEARGEOMETRYMODE: {
            /* G_EXTRAGEOMETRYMODE_EXT's invert-culling flag is applied here,
             * where the culling bits are actually emitted - the port applies
             * it inside its own geometry-mode state (gfx_pc.cpp:1652). */
            uint32_t mode = (uint32_t)g[0].words.w1;
            if (invertCulling_) {
                const uint32_t cull = mode & (uint32_t)G_CULL_BOTH;
                if (cull == (uint32_t)G_CULL_FRONT || cull == (uint32_t)G_CULL_BACK) {
                    mode ^= (uint32_t)G_CULL_BOTH;
                }
            }
            const uint8_t canon = (opcode == (uint8_t)G_SETGEOMETRYMODE)
                                      ? kCanonSetGeometryMode
                                      : kCanonClearGeometryMode;
            emit((uint32_t)canon << 24, mode);
            expect.opcode = canon;
            expect.fields[0] = mode;
            expect.fieldCount = 1;
            emitted = true;
            break;
        }

        case G_EXTRAGEOMETRYMODE_EXT: {
            /* Emits nothing. Only invert-culling is honoured in v1; the aspect
             * and no-clipping flags are dropped and counted (SCAFFOLD 3.4).
             * The port's clear mask is the inverse of w0[0:24]
             * (gfx_pc.cpp:2365). */
            const uint32_t clear = ~fieldOf(g[0], 0, 24);
            const uint32_t set = (uint32_t)g[0].words.w1;
            if (clear & (uint32_t)G_INVERT_CULLING_EXT) {
                invertCulling_ = false;
            }
            if (set & (uint32_t)G_INVERT_CULLING_EXT) {
                invertCulling_ = true;
            }
            if ((set | clear) & ~(uint32_t)G_INVERT_CULLING_EXT) {
                ++stats_.droppedExtraGeometryFlags;
            }
            at += sizeof(Gfx) * words;
            continue;
        }

        case (uint8_t)G_SETOTHERMODE_L:
        case (uint8_t)G_SETOTHERMODE_H: {
            /* Same encoding on both sides: bit count in w0[0:8], shift in
             * w0[8:8] (gfx_pc.cpp:2371,2374; rt64_gbi_f3d.cpp:152-156). The
             * port's +32 bias on the H shift is its own way of folding both
             * halves into one 64-bit word and does not belong in the stream. */
            const uint32_t bits = fieldOf(g[0], 0, 8);
            const uint32_t shift = fieldOf(g[0], 8, 8);
            const uint32_t value = (uint32_t)g[0].words.w1;
            const uint8_t canon = (opcode == (uint8_t)G_SETOTHERMODE_L)
                                      ? kCanonSetOtherModeL
                                      : kCanonSetOtherModeH;
            emit(((uint32_t)canon << 24) | (shift << 8) | bits, value);
            expect.opcode = canon;
            expect.fields[0] = bits;
            expect.fields[1] = shift;
            expect.fields[2] = value;
            expect.fieldCount = 3;
            emitted = true;
            break;
        }

        case (uint8_t)G_ENDDL:
            /* The flattened stream gets exactly one G_ENDDL, appended by
             * translate() once the whole walk is done. */
            return TranslateStatus::Ok;

        case G_DL: {
            if (!ref.addr || ref.unbound) {
                /* fast3d skips a null call and would branch into an unbound
                 * segment; neither is something to reproduce. */
                at += sizeof(Gfx) * words;
                continue;
            }
            ++stats_.listsFlattened;
            if (fieldOf(g[0], 16, 1)) {
                at = ref.addr;   // branch: this list continues there
                continue;
            }
            const TranslateStatus sub = walk(ref.addr, depth + 1);
            if (sub != TranslateStatus::Ok) {
                return sub;
            }
            at += sizeof(Gfx) * words;
            continue;
        }

        default:
            /* dispositionOf said Translated, so a case is missing. Fail rather
             * than silently drop it. */
            error_ = "opcode marked translated but not handled";
            return TranslateStatus::UnknownOpcode;
        }

        if (validate_ && emitted) {
            if (!validateEmitted(pairIndex, expect)) {
                return TranslateStatus::ValidationFailed;
            }
            /* Invariant 4: every emitted address is tagged, aligned and inside
             * the arena. Checked on the bytes, not on the variable. */
            const uint32_t w1 = out_[pairIndex + 1];
            const bool carriesAddress =
                expect.opcode == kCanonMtx || expect.opcode == kCanonMoveMem ||
                expect.opcode == kCanonVtx || expect.opcode == kCanonVtxColorPD;
            if (carriesAddress && !addressIsWellFormed(w1, arena_.rdramSize())) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "opcode 0x%02x emitted address 0x%08x, which is not a "
                         "tagged aligned arena address",
                         expect.opcode, w1);
                error_ = buf;
                return TranslateStatus::ValidationFailed;
            }
        }

        at += sizeof(Gfx) * words;
    }
}

TranslateStatus Translator::translate(uintptr_t rootDl, RdramAddr *outStart,
                                      RdramAddr *outEnd)
{
    const auto began = std::chrono::steady_clock::now();

    out_.clear();
    error_.clear();
    emitStreamPrefix();

    const TranslateStatus status = walk(rootDl, 0);
    if (status != TranslateStatus::Ok) {
        return status;
    }

    /* One terminator for the whole flattened stream. */
    emit((uint32_t)kCanonEndDl << 24, 0);

    /* The stream is built host-side and copied in once. Emitting straight into
     * the arena would interleave command words with the payload blocks that
     * marshalling allocates from the same bump region, and the stream has to
     * be contiguous. */
    const size_t bytes = out_.size() * sizeof(uint32_t);
    const RdramAddr start = arena_.allocFrame(bytes);
    memcpy(arena_.rdramBase() + (start & ~kExtendedAddrBit), out_.data(), bytes);

    if (outStart) {
        *outStart = start;
    }
    if (outEnd) {
        *outEnd = start + (RdramAddr)bytes;
    }

    stats_.translateMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began)
            .count();
    return TranslateStatus::Ok;
}

} // namespace pdrt64
