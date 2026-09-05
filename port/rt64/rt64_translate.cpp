/*
 * Display-list translator, T5 and T6 scope: control flow, geometry, and the
 * RDP and texture path.
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
constexpr uint32_t kGexTexRectV1 = 0x02;           // rt64_extended_gbi.h:33
constexpr uint32_t kGexFillRectV1 = 0x03;          // rt64_extended_gbi.h:34
constexpr uint32_t kGexOriginNone = 0x800;         // rt64_extended_gbi.h:84
constexpr uint32_t kGexSetRenderToRamV1 = 0x12;     // rt64_extended_gbi.h:51

/* Canonical opcodes, from RT64's F3D/F3DPD maps. Named here rather than
 * included so the numbers sit next to the lowering that uses them. */
constexpr uint8_t kCanonSpNoop = 0x00;
constexpr uint8_t kCanonMtx = 0x01;
constexpr uint8_t kCanonMoveMem = 0x03;
constexpr uint8_t kCanonVtx = 0x04;
constexpr uint8_t kCanonVtxColorPD = 0x07;
constexpr uint8_t kCanonTriX = 0xB1;
constexpr uint8_t kCanonRdpHalf2 = 0xB3;
constexpr uint8_t kCanonRdpHalf1 = 0xB4;
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

/* fast3d's C0/C1 (gfx_pc.cpp:91-92), for the multi-word EXT rectangles. */
inline uint32_t c0(const Gfx &g, uint32_t pos, uint32_t width)
{
    return (uint32_t)((g.words.w0 >> pos) & ((1ULL << width) - 1));
}

inline uint32_t c1(const Gfx &g, uint32_t pos, uint32_t width)
{
    return (uint32_t)((g.words.w1 >> pos) & ((1ULL << width) - 1));
}

/*
 * Serves a source block, zero-filling past its true end.
 *
 * Texture and TLUT extents are byte counts derived from a load command and are
 * not always a multiple of four - `((lrs + 1) << siz) >> 1` is odd for plenty
 * of real textures. marshalCopy needs a whole number of words, because BE32
 * reverses bytes within each word and a partial trailing word has no defined
 * mapping (rt64_mem.h).
 *
 * Rounding the read up instead would be wrong twice over: the extra bytes may
 * lie outside what the capture recorded, and in the running game they may lie
 * outside the allocation, which is a genuine overread. So the length is rounded
 * up and the bytes past the end are supplied as zero. They belong to a texel
 * outside the loaded region, which the renderer does not sample.
 */
class PaddedReader final : public MemReader {
public:
    PaddedReader(MemReader &inner, uintptr_t base, size_t trueLen)
        : inner_(inner), base_(base), trueLen_(trueLen)
    {
    }

    bool read(uintptr_t src, void *dst, size_t len) override
    {
        const size_t offset = src - base_;
        const size_t real = offset < trueLen_ ? trueLen_ - offset : 0;
        const size_t take = len < real ? len : real;
        if (take && !inner_.read(src, dst, take)) {
            return false;
        }
        if (take < len) {
            memset((uint8_t *)dst + take, 0, len - take);
        }
        return true;
    }

    Region regionOf(uintptr_t src, size_t len) const override
    {
        return inner_.regionOf(src, len < trueLen_ ? len : trueLen_);
    }

private:
    MemReader &inner_;
    uintptr_t base_;
    size_t trueLen_;
};

constexpr size_t roundUp4(size_t n)
{
    return (n + 3u) & ~(size_t)3u;
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
    pendingImage_ = PendingImage{};
    /* Until the game binds its own, the main image is the arena's, at the size
     * it was allocated. A depth clear can arrive before the frame's first
     * G_SETCIMG - it does in the menu capture - and restoring to a command
     * with no width would hand RT64 an image one pixel across. */
    uint32_t nativeW = 0, nativeH = 0;
    fbs_.fbSize(0, &nativeW, &nativeH);
    mainCimgW0_ = ((uint32_t)(uint8_t)G_SETCIMG << 24) | (2u << 19) |
                  ((nativeW ? nativeW - 1 : 0) & 0xfff);
    mainCimg_ = currentMainColorImage();
    boundCimgW0_ = mainCimgW0_;
    boundCimg_ = mainCimg_;
    otherModeH_ = 0;
    stats_ = TranslateStats{};
    error_.clear();
}

RdramAddr Translator::currentMainColorImage() const
{
    /* Defaults to the first image, so a caller that never sets one still
     * renders somewhere sensible rather than to address zero. */
    return mainColorImage_ ? mainColorImage_ : fbs_.mainColorImage(0);
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

    if (renderToRam_) {
        /* rt64_gbi_extended.cpp:183-186 reads the flag from w1 bit 0. */
        emit(((uint32_t)kExtendedOpcode << 24) | kGexSetRenderToRamV1, 1u);
    }
}

bool Translator::pushBlock(const GfxRef &ref, Swizzle type, bool cached, RdramAddr *out)
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

    if (cached) {
        /* Texture extents are byte counts and need not be a whole number of
         * words; PaddedReader supplies the shortfall as zero rather than
         * reading past the block. The cache key becomes (src, padded length,
         * type), which is still an exact identity for the marshalled result. */
        const size_t padded = roundUp4(len);
        PaddedReader reader(mem_, ref.addr, len);

        /* Ask the arena whether this was already marshalled by comparing its
         * hit counter across the call. Arena has no "contains" query and this
         * needs no new API surface to answer the question T6's acceptance
         * asks: is a static scene all cache hits by its second frame. */
        const uint32_t hitsBefore = arena_.stats().cacheHits;
        *out = arena_.pushCachedData(reader, ref.addr, padded, type);
        ++stats_.textureBlocks;
        if (arena_.stats().cacheHits != hitsBefore) {
            ++stats_.textureCacheHits;
        } else {
            ++stats_.textureCacheMisses;
            stats_.marshalledBytes += padded;
        }
        return true;
    }

    *out = arena_.pushFrameData(mem_, ref.addr, len, type);
    stats_.marshalledBytes += len;
    return true;
}

/*
 * === EXT lowerings that expand to more than one command (SCAFFOLD 3.4) ===
 *
 * These are emitted straight rather than through the validator's expect/got
 * path: they are sequences this file composes rather than translations of one
 * source command, so there is no source command to check them against. Every
 * address they emit comes from FbRegistry, which only ever hands out arena
 * allocations, so invariant 4 holds by construction.
 */

void Translator::emitSetColorImage(uint32_t w0, RdramAddr addr)
{
    emit(w0, addr);
    boundCimgW0_ = w0;
    boundCimg_ = addr;
}

void Translator::emitDepthClear()
{
    /* The canonical N64 depth clear, which RT64 recognises: point the colour
     * image at the depth buffer, fill it with the maximum depth value, and put
     * the colour image back. fast3d instead calls clear_framebuffer directly
     * (gfx_pc.cpp:2545-2548), which has no equivalent in a command stream.
     *
     * Fill cycle is not optional - a G_FILLRECT outside it does not write the
     * packed value - so the cycle type is switched and restored around it. */
    const uint32_t savedW0 = boundCimgW0_;
    const RdramAddr savedAddr = boundCimg_;

    uint32_t w = 0, h = 0;
    fbs_.fbSize(0, &w, &h);

    /* Cycle type to fill. G_MDSFT_CYCLETYPE is 20 and the field is 2 bits
     * (gbi.h:502,514); RT64 masks with the same shift and width
     * (rt64_rsp.cpp:1033-1038). */
    emit(((uint32_t)kCanonSetOtherModeH << 24) | (20u << 8) | 2u, (uint32_t)G_CYC_FILL);

    /* RGBA16 depth image at the arena's depth allocation. */
    emitSetColorImage(((uint32_t)(uint8_t)G_SETCIMG << 24) | (2u << 19) |
                          ((w ? w - 1 : 0) & 0xfff),
                      fbs_.depthImage());

    /* Two 16-bit maximum-depth values packed into the fill word. */
    emit(((uint32_t)(uint8_t)G_SETFILLCOLOR << 24), 0xFFFCFFFCu);

    /* Full frame, in the 2.2 fixed point the RDP rectangles use. */
    const uint32_t lrx = w ? (w - 1) << 2 : 0;
    const uint32_t lry = h ? (h - 1) << 2 : 0;
    emit(((uint32_t)(uint8_t)G_FILLRECT << 24) | (lrx << 12) | lry, 0);

    /* Put back what was there. */
    emitSetColorImage(savedW0, savedAddr);
    emit(((uint32_t)kCanonSetOtherModeH << 24) | (20u << 8) | 2u,
         otherModeH_ & (3u << 20));
}

void Translator::emitFramebufferCopy(const Gfx &cmd)
{
    /* f3d_copy_framebuffer(dst, src, x, y, flip) (gfx_pc.cpp:2524). Lowered to
     * the sequence that copies one image into another through the texture
     * unit: bind the destination, bind the source as a texture, describe it as
     * a tile, and draw a rectangle over it. */
    const uint32_t dst = (uint32_t)((cmd.words.w0 >> 11) & 0x7ff);
    const uint32_t src = (uint32_t)(cmd.words.w0 & 0x7ff);
    const int16_t x = (int16_t)((cmd.words.w1 >> 16) & 0xffff);
    const int16_t y = (int16_t)(cmd.words.w1 & 0xffff);
    const uint32_t flip = (uint32_t)((cmd.words.w0 >> 22) & 1);

    const uint32_t savedW0 = boundCimgW0_;
    const RdramAddr savedAddr = boundCimg_;

    uint32_t sw = 0, sh = 0;
    fbs_.fbSize((int)src, &sw, &sh);
    uint32_t dw = 0, dh = 0;
    fbs_.fbSize((int)dst, &dw, &dh);

    emitSetColorImage(((uint32_t)(uint8_t)G_SETCIMG << 24) | (2u << 19) |
                          ((dw ? dw - 1 : 0) & 0xfff),
                      fbs_.fbAddress((int)dst));

    emit(((uint32_t)(uint8_t)G_SETTIMG << 24) | (2u << 19) |
             ((sw ? sw - 1 : 0) & 0xfff),
         fbs_.fbAddress((int)src));

    /* Tile 0, RGBA16. `line` counts 64-bit words per row, which for 16-bit
     * texels is width/4. */
    emit(((uint32_t)(uint8_t)G_SETTILE << 24) | (2u << 19) | (((sw >> 2) & 0x1ff) << 9), 0);
    emit(((uint32_t)(uint8_t)G_SETTILESIZE << 24),
         ((sw ? (sw - 1) << 2 : 0) << 12) | (sh ? (sh - 1) << 2 : 0));

    /* One rectangle over the destination region, sampled 1:1 - dsdx and dtdy
     * are 1.0 in the 5.10 fixed point the RDP uses for them. */
    const uint32_t ulx = (uint32_t)((x < 0 ? 0 : x) << 2) & 0xfff;
    const uint32_t uly = (uint32_t)((y < 0 ? 0 : y) << 2) & 0xfff;
    const uint32_t lrx = (ulx + (sw ? (sw - 1) << 2 : 0)) & 0xfff;
    const uint32_t lry = (uly + (sh ? (sh - 1) << 2 : 0)) & 0xfff;
    const uint8_t rectOp = flip ? (uint8_t)G_TEXRECTFLIP : (uint8_t)G_TEXRECT;
    emit(((uint32_t)rectOp << 24) | (lrx << 12) | lry, (ulx << 12) | uly);
    emit((uint32_t)kCanonRdpHalf1 << 24, 0);
    emit((uint32_t)kCanonRdpHalf2 << 24, (0x0400u << 16) | 0x0400u);

    emitSetColorImage(savedW0, savedAddr);
}

void Translator::emitImageRect(const Gfx *cmd)
{
    /* gfx_dp_image_rectangle(tile, iw, ih, x0,y0,s0,t0, x1,y1,s1,t1)
     * (gfx_pc.cpp:2483-2501): a textured quad given by two corners with their
     * texture coordinates. Lowered to a tile description plus one rectangle;
     * the rate of change is derived from the two corners rather than assumed,
     * because an image rectangle is how the menus scale artwork. */
    const uint32_t tile = (uint32_t)(cmd[0].words.w0 & 0x7);
    const uint32_t iw = (uint32_t)((cmd[0].words.w1 >> 16) & 0xffff);
    const uint32_t ih = (uint32_t)(cmd[0].words.w1 & 0xffff);
    const int16_t x0 = (int16_t)((cmd[1].words.w0 >> 16) & 0xffff);
    const int16_t y0 = (int16_t)(cmd[1].words.w0 & 0xffff);
    const int16_t s0 = (int16_t)((cmd[1].words.w1 >> 16) & 0xffff);
    const int16_t t0 = (int16_t)(cmd[1].words.w1 & 0xffff);
    const int16_t x1 = (int16_t)((cmd[2].words.w0 >> 16) & 0xffff);
    const int16_t y1 = (int16_t)(cmd[2].words.w0 & 0xffff);
    const int16_t s1 = (int16_t)((cmd[2].words.w1 >> 16) & 0xffff);
    const int16_t t1 = (int16_t)(cmd[2].words.w1 & 0xffff);

    emit(((uint32_t)(uint8_t)G_SETTILESIZE << 24),
         ((uint32_t)tile << 24) | ((iw ? (iw - 1) << 2 : 0) << 12) |
             (ih ? (ih - 1) << 2 : 0));

    /* dsdx and dtdy are texels per pixel in 5.10 fixed point. A zero-width
     * rectangle would divide by zero, so it is emitted at 1:1 and left for the
     * rasteriser to reject. */
    const int32_t dx = (int32_t)x1 - (int32_t)x0;
    const int32_t dy = (int32_t)y1 - (int32_t)y0;
    const uint32_t dsdx =
        dx ? (uint32_t)((((int32_t)s1 - (int32_t)s0) << 10) / dx) & 0xffff : 0x0400u;
    const uint32_t dtdy =
        dy ? (uint32_t)((((int32_t)t1 - (int32_t)t0) << 10) / dy) & 0xffff : 0x0400u;

    const uint32_t ulx = ((uint32_t)(x0 < 0 ? 0 : x0) << 2) & 0xfff;
    const uint32_t uly = ((uint32_t)(y0 < 0 ? 0 : y0) << 2) & 0xfff;
    const uint32_t lrx = ((uint32_t)(x1 < 0 ? 0 : x1) << 2) & 0xfff;
    const uint32_t lry = ((uint32_t)(y1 < 0 ? 0 : y1) << 2) & 0xfff;

    emit(((uint32_t)(uint8_t)G_TEXRECT << 24) | (lrx << 12) | lry,
         (tile << 24) | (ulx << 12) | uly);
    emit((uint32_t)kCanonRdpHalf1 << 24,
         ((uint32_t)(uint16_t)s0 << 16) | (uint32_t)(uint16_t)t0);
    emit((uint32_t)kCanonRdpHalf2 << 24, (dsdx << 16) | dtdy);
}

bool Translator::sizePendingImage(const GfxRef &ref)
{
    /* A load command reached us with no G_SETTIMG in front of it, or with one
     * whose address never resolved. fast3d would read from a stale image
     * pointer here; there is nothing sensible to marshal, so leave the
     * placeholder and let the unsizedImages count show it. */
    if (!pendingImage_.valid || !ref.addr) {
        return true;
    }

    const size_t needed = (size_t)ref.startOffset + ref.bytes;
    if (needed <= pendingImage_.marshalledLen) {
        /* An earlier load already covered this extent. The block base has not
         * moved, so the address already patched in is still right. */
        return true;
    }

    RdramAddr addr = 0;
    if (!pushBlock(ref, Swizzle::BE32, true, &addr)) {
        return false;
    }

    pendingImage_.marshalledLen = needed;
    out_[pendingImage_.pairIndex + 1] = addr;

    /* Re-check the patched command: the address went in behind the validator's
     * back, and an unchecked address in the stream is exactly what invariant 4
     * exists to prevent. */
    if (validate_ && !addressIsWellFormed(addr, arena_.rdramSize())) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "G_SETTIMG patched with 0x%08x, which is not a tagged aligned "
                 "arena address",
                 addr);
        error_ = buf;
        return false;
    }
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
    /* T6, the RDP and texture path. Field layouts were checked one by one
     * against GBI_RDP's decoders (rt64_gbi_rdp.cpp:17-241) and match fast3d's
     * exactly, so these pass through with only the opcode and any address
     * rewritten. G_LOADTLUT is the one that looks like it differs - fast3d
     * reads 10-bit coordinates at bits 14 and 2 (gfx_pc.cpp:2407) where RT64
     * reads 12 bits at 12 and 0 (rt64_gbi_rdp.cpp:87-92) - but those are the
     * same field with and without its two fractional bits, so handing over the
     * raw words serves both. */
    case (uint8_t)G_SETTIMG:
    case (uint8_t)G_SETCIMG:
    case (uint8_t)G_SETZIMG:
    case (uint8_t)G_SETTILE:
    case (uint8_t)G_SETTILESIZE:
    case (uint8_t)G_LOADBLOCK:
    case (uint8_t)G_LOADTILE:
    case (uint8_t)G_LOADTLUT:
    case (uint8_t)G_SETCOMBINE:
    case (uint8_t)G_SETENVCOLOR:
    case (uint8_t)G_SETPRIMCOLOR:
    case (uint8_t)G_SETFOGCOLOR:
    case (uint8_t)G_SETFILLCOLOR:
    case (uint8_t)G_SETSCISSOR:
    case (uint8_t)G_RDPSETOTHERMODE:
    case (uint8_t)G_FILLRECT:
    case (uint8_t)G_TEXRECT:
    case (uint8_t)G_TEXRECTFLIP:
    case (uint8_t)G_RDPLOADSYNC:
    case (uint8_t)G_RDPPIPESYNC:
    case (uint8_t)G_RDPTILESYNC:
    case (uint8_t)G_RDPFULLSYNC:
    /* Not the EXT lowering (that is T7), only the cache invalidation half,
     * which has to land with the caching it makes sound rather than a task
     * later. The lowering is "emit nothing, forward to the arena" either way,
     * so nothing of T7's design is being pre-empted. */
    case G_INVALTEXCACHE_EXT:
    /* T7, the EXT dialect. Every one of these is now handled - lowered,
     * expanded, or deliberately dropped with a counter - so none reaches the
     * unknown path. */
    case G_SETFB_EXT:
    case G_SETTIMG_FB_EXT:
    case G_TEXRECT_WIDE_EXT:
    case G_FILLRECT_WIDE_EXT:
    case G_SETGRAYSCALE_EXT:
    case G_SETINTENSITY_EXT:
    case G_COPYFB_EXT:
    case G_IMAGERECT_EXT:
    case G_RDPFLUSH_EXT:
    case G_CLEAR_DEPTH_EXT:
    case G_SETSUBPIXELOFFSET_EXT:
        return Disposition::Translated;

    default:
        break;
    }

    /* What is left is G_RDPHALF_1/2/CONT. On N64 the sky path issues low-level
     * ucode triangles through them; the port renders sky differently and
     * fast3d ignores them (gfx_pc.cpp:2537-2542). Forwarding them would be
     * worse than dropping: RT64's rdpHalf1/rdpHalf2 store them as state that a
     * following command can consume, so emitting words the reference renderer
     * ignores could change what gets drawn. */
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
            if (!pushBlock(ref, Swizzle::U32, false, &addr)) {
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
            if (!pushBlock(ref, type, false, &addr)) {
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
            if (!pushBlock(ref, Swizzle::VtxPD, false, &addr)) {
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
            if (!pushBlock(ref, Swizzle::U8, false, &addr)) {
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
            if (canon == kCanonSetOtherModeH) {
                /* Shadow it the way RT64 will apply it (rt64_rsp.cpp:1033), so
                 * the depth clear has something to restore. */
                const uint32_t mask = (uint32_t)(((1ULL << bits) - 1) << shift);
                otherModeH_ = (otherModeH_ & ~mask) | (value & mask);
            }
            emit(((uint32_t)canon << 24) | (shift << 8) | bits, value);
            expect.opcode = canon;
            expect.fields[0] = bits;
            expect.fields[1] = shift;
            expect.fields[2] = value;
            expect.fieldCount = 3;
            emitted = true;
            break;
        }

        /*
         * === T6: the RDP and texture path ===
         *
         * These share their opcode numbers AND their field layouts with RT64
         * (checked against rt64_gbi_rdp.cpp:17-241), so the words pass through
         * unchanged. Only the three that carry an address are rewritten.
         */
        case (uint8_t)G_SETTILE:
        case (uint8_t)G_SETTILESIZE:
        case (uint8_t)G_LOADBLOCK:
        case (uint8_t)G_LOADTILE:
        case (uint8_t)G_LOADTLUT:
        case (uint8_t)G_SETCOMBINE:
        case (uint8_t)G_SETENVCOLOR:
        case (uint8_t)G_SETPRIMCOLOR:
        case (uint8_t)G_SETFOGCOLOR:
        case (uint8_t)G_SETFILLCOLOR:
        case (uint8_t)G_SETSCISSOR:
        case (uint8_t)G_RDPSETOTHERMODE:
        case (uint8_t)G_FILLRECT:
        case (uint8_t)G_RDPLOADSYNC:
        case (uint8_t)G_RDPPIPESYNC:
        case (uint8_t)G_RDPTILESYNC:
        case (uint8_t)G_RDPFULLSYNC: {
            /* A load command is where a pending G_SETTIMG finally learns its
             * size. gfxStep has already computed the extent fast3d would read;
             * marshal it and patch the address back into the emitted command. */
            if (ref.kind == RefKind::Texels || ref.kind == RefKind::Tlut) {
                if (!sizePendingImage(ref)) {
                    return TranslateStatus::CaptureMiss;
                }
            }
            const uint32_t w0 = (uint32_t)g[0].words.w0;
            const uint32_t w1 = (uint32_t)g[0].words.w1;
            if (opcode == (uint8_t)G_RDPSETOTHERMODE) {
                otherModeH_ = w0 & 0x00ffffff;
            }
            emit(w0, w1);
            expect.opcode = opcode;
            expect.fields[0] = w0 & 0x00ffffff;
            expect.fields[1] = w1;
            expect.fieldCount = 2;
            emitted = true;
            break;
        }

        case (uint8_t)G_SETTIMG: {
            /* Emitted now with a placeholder address, patched when a load
             * command gives the image a size. See PendingImage. */
            /* The one being replaced was never sized if no load command ever
             * grew it. Testing `valid` alone would count every image after the
             * first, since a sized one stays pending until the next G_SETTIMG
             * displaces it. */
            if (pendingImage_.valid && pendingImage_.marshalledLen == 0) {
                ++stats_.unsizedImages;
            }
            const uint32_t w0 = (uint32_t)g[0].words.w0;
            pendingImage_ = PendingImage{};
            pendingImage_.pairIndex = out_.size();
            pendingImage_.valid = true;
            pendingImage_.src = st_.texAddr;
            emit(w0, 0);
            expect.opcode = opcode;
            expect.fields[0] = w0 & 0x00ffffff;
            expect.fields[1] = 0;
            expect.fieldCount = 2;
            emitted = true;
            break;
        }

        case (uint8_t)G_SETCIMG:
        case (uint8_t)G_SETZIMG: {
            /* The game names its own framebuffers; we render into synthetic
             * RDRAM, so these are redirected to the arena's images rather than
             * marshalled. Which main colour image is current comes from the
             * caller, which sets it in step with the VI scanout - rendering
             * every frame into image 0 while the VI alternated meant half the
             * presented frames came from a buffer nothing had drawn into. */
            const uint32_t w0 = (uint32_t)g[0].words.w0;
            const RdramAddr img = (opcode == (uint8_t)G_SETZIMG)
                                      ? fbs_.depthImage()
                                      : currentMainColorImage();
            if (opcode == (uint8_t)G_SETCIMG) {
                mainCimgW0_ = w0;
                mainCimg_ = img;
                boundCimgW0_ = w0;
                boundCimg_ = img;
            }
            emit(w0, img);
            expect.opcode = opcode;
            expect.fields[0] = w0 & 0x00ffffff;
            expect.fields[1] = img;
            expect.fieldCount = 2;
            emitted = true;
            break;
        }

        case (uint8_t)G_TEXRECT:
        case (uint8_t)G_TEXRECTFLIP: {
            /* Three commands on both sides, and the same field positions - the
             * port just spreads them over three 16-byte Gfx where RT64 reads
             * three 8-byte ones, consuming the trailing two itself
             * (rt64_gbi_rdp.cpp:171-186) rather than dispatching them.
             *
             * Their w0 is therefore never read. It is filled with the N64
             * convention's G_RDPHALF_1 and G_RDPHALF_2 so a disassembly of the
             * stream is legible; a bare zero would decode as G_SPNOOP and read
             * like a stray extended-GBI hook. */
            emit((uint32_t)g[0].words.w0, (uint32_t)g[0].words.w1);
            expect.opcode = opcode;
            expect.fields[0] = (uint32_t)g[0].words.w0 & 0x00ffffff;
            expect.fields[1] = (uint32_t)g[0].words.w1;
            expect.fieldCount = 2;

            if (validate_ && !validateEmitted(pairIndex, expect)) {
                return TranslateStatus::ValidationFailed;
            }

            const uint8_t halfOpcodes[2] = {kCanonRdpHalf1, kCanonRdpHalf2};
            const uint32_t halfWords[2] = {(uint32_t)g[1].words.w1,
                                           (uint32_t)g[2].words.w1};
            for (int h = 0; h < 2; ++h) {
                const size_t halfIndex = out_.size();
                emit((uint32_t)halfOpcodes[h] << 24, halfWords[h]);
                if (validate_) {
                    DecodedCmd halfExpect;
                    halfExpect.opcode = halfOpcodes[h];
                    halfExpect.fields[0] = halfWords[h];
                    halfExpect.fieldCount = 1;
                    if (!validateEmitted(halfIndex, halfExpect)) {
                        return TranslateStatus::ValidationFailed;
                    }
                }
            }

            at += sizeof(Gfx) * words;
            continue;
        }

        /*
         * === T7: the EXT dialect (SCAFFOLD 3.4) ===
         */

        case G_SETFB_EXT: {
            /* Bind one of the game's offscreen framebuffers as the colour
             * image. Handle 0 means "back to the main image", which is why the
             * current one is tracked rather than assumed
             * (gfx_pc.cpp:2513-2522). */
            const uint32_t fb = (uint32_t)g[0].words.w1;
            if (fb == 0) {
                emitSetColorImage(mainCimgW0_, mainCimg_);
            } else {
                uint32_t fw = 0, fh = 0;
                fbs_.fbSize((int)fb, &fw, &fh);
                emitSetColorImage(((uint32_t)(uint8_t)G_SETCIMG << 24) | (2u << 19) |
                                      ((fw ? fw - 1 : 0) & 0xfff),
                                  fbs_.fbAddress((int)fb));
            }
            at += sizeof(Gfx) * words;
            continue;
        }

        case G_SETTIMG_FB_EXT: {
            /* Bind a framebuffer as the texture source. No marshalling: the
             * image lives in the arena already and RT64's framebuffer manager
             * detects the overlap and copies on the GPU. Any pending image is
             * dropped, so a later load cannot patch an address into a command
             * that no longer points at marshalled memory. */
            const uint32_t fb = (uint32_t)g[0].words.w1;
            uint32_t fw = 0, fh = 0;
            fbs_.fbSize((int)fb, &fw, &fh);
            emit(((uint32_t)(uint8_t)G_SETTIMG << 24) | (2u << 19) |
                     ((fw ? fw - 1 : 0) & 0xfff),
                 fbs_.fbAddress((int)fb));
            pendingImage_ = PendingImage{};
            at += sizeof(Gfx) * words;
            continue;
        }

        case G_COPYFB_EXT:
            emitFramebufferCopy(g[0]);
            at += sizeof(Gfx) * words;
            continue;

        case G_CLEAR_DEPTH_EXT:
            emitDepthClear();
            at += sizeof(Gfx) * words;
            continue;

        case G_IMAGERECT_EXT:
            emitImageRect(&g[0]);
            at += sizeof(Gfx) * words;
            continue;

        case G_FILLRECT_WIDE_EXT: {
            /* G_EX_FILLRECT_V1: two commands, the first carrying the alignment
             * origins and the second the corners as int16
             * (rt64_gbi_extended.cpp:43-54). The port's coordinates are
             * sign-extended 24-bit in the same 2.2 fixed point
             * (gfx_pc.cpp:2455-2462); at the widest supported resolution they
             * are far inside int16, so the narrowing is safe.
             *
             * Both origins are G_EX_ORIGIN_NONE, which is RT64's own default
             * (rt64_rdp.h:49) and means no alignment adjustment - matching the
             * decision to drop the aspect flags in v1. */
            const int32_t lrx = (int32_t)(c0(g[0], 0, 24) << 8) >> 8;
            const int32_t lry = (int32_t)(c1(g[0], 0, 24) << 8) >> 8;
            const int32_t ulx = (int32_t)(c0(g[1], 0, 24) << 8) >> 8;
            const int32_t uly = (int32_t)(c1(g[1], 0, 24) << 8) >> 8;
            emit(((uint32_t)kExtendedOpcode << 24) | kGexFillRectV1,
                 (uint32_t)kGexOriginNone | ((uint32_t)kGexOriginNone << 12));
            emit(((uint32_t)(uint16_t)ulx << 16) | (uint32_t)(uint16_t)uly,
                 ((uint32_t)(uint16_t)lrx << 16) | (uint32_t)(uint16_t)lry);
            at += sizeof(Gfx) * words;
            continue;
        }

        case G_TEXRECT_WIDE_EXT: {
            /* G_EX_TEXRECT_V1: three commands (rt64_gbi_extended.cpp:22-41).
             * Nothing in this build emits the source opcode - the macros exist
             * but have no callers - so this lowering is exercised only by its
             * unit test and has never met real data. */
            const int32_t lrx = (int32_t)(c0(g[0], 0, 24) << 8) >> 8;
            const int32_t lry = (int32_t)(c1(g[0], 0, 24) << 8) >> 8;
            const uint32_t tile = c1(g[0], 24, 3);
            const uint32_t flip = c1(g[0], 27, 1);
            const int32_t ulx = (int32_t)(c0(g[1], 0, 24) << 8) >> 8;
            const int32_t uly = (int32_t)(c1(g[1], 0, 24) << 8) >> 8;
            const uint32_t uls = c0(g[2], 16, 16), ult = c0(g[2], 0, 16);
            const uint32_t dsdx = c1(g[2], 16, 16), dtdy = c1(g[2], 0, 16);

            emit(((uint32_t)kExtendedOpcode << 24) | kGexTexRectV1,
                 tile | (flip << 7) | ((uint32_t)kGexOriginNone << 3) |
                     ((uint32_t)kGexOriginNone << 15));
            emit(((uint32_t)(uint16_t)ulx << 16) | (uint32_t)(uint16_t)uly,
                 ((uint32_t)(uint16_t)lrx << 16) | (uint32_t)(uint16_t)lry);
            emit((uls << 16) | ult, (dsdx << 16) | dtdy);
            at += sizeof(Gfx) * words;
            continue;
        }

        case G_RDPFLUSH_EXT:
            /* A batching artefact of fast3d (gfx_pc.cpp:2544), with nothing to
             * express in a command stream. Not counted as dropped: no output
             * is the correct translation, not a gap. */
            at += sizeof(Gfx) * words;
            continue;

        case G_SETGRAYSCALE_EXT:
        case G_SETINTENSITY_EXT:
        case G_SETSUBPIXELOFFSET_EXT:
            /* Dropped and counted in v1 (SCAFFOLD 3.4). Grayscale and intensity
             * drive the IR scanner and cloak, which degrade visually rather
             * than break, and the effects phase decides their real mapping;
             * RT64 renders subpixel-correct at high resolution, so the offset
             * has nothing to apply to. */
            ++stats_.droppedCommands;
            ++stats_.droppedPerOpcode[opcode];
            at += sizeof(Gfx) * words;
            continue;

        case G_INVALTEXCACHE_EXT: {
            /* Emits nothing: this is the game telling the renderer that texture
             * data at an address has changed, which is what makes caching those
             * blocks sound. fast3d does the same thing to its own cache
             * (gfx_pc.cpp:2530-2535). A null argument clears everything. */
            const uintptr_t tex = gfxSegResolve((uintptr_t)g[0].words.w1, st_.segments);
            if (tex) {
                arena_.invalidateCache(tex);
            } else {
                arena_.clearCache();
            }
            at += sizeof(Gfx) * words;
            continue;
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
                expect.opcode == kCanonVtx || expect.opcode == kCanonVtxColorPD ||
                expect.opcode == (uint8_t)G_SETCIMG || expect.opcode == (uint8_t)G_SETZIMG;
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

    /* The last image in the stream has no following G_SETTIMG to displace it,
     * so it is checked here rather than being missed. */
    if (pendingImage_.valid && pendingImage_.marshalledLen == 0) {
        ++stats_.unsizedImages;
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
