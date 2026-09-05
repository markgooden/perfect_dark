#pragma once

/*
 * Display-list translator: the port's widened 64-bit Gfx dialect in, the
 * 8-byte-per-command stream RT64's F3DPD + extended GBI understands out.
 *
 * Scope of this file today is SCAFFOLD tasks T5 (control flow and geometry)
 * and T6 (the RDP and texture path). The EXT dialect is T7. Everything outside
 * that is dropped and counted per opcode rather than guessed at, so a golden
 * shows exactly what is translated and what is still missing. See
 * dispositionOf in the .cpp.
 *
 * === Deferred texture sizing (T6) ===
 *
 * A G_SETTIMG names a texture image but carries no size; only the
 * G_LOADBLOCK/G_LOADTILE/G_LOADTLUT that follows says how much of it the
 * renderer reads. So the command is emitted immediately with a placeholder
 * address and patched when a load arrives - emitting it late instead would
 * reorder it past the G_SETTILE that normally sits between them, and RT64
 * needs the image bound before the load. Several loads may follow one
 * G_SETTIMG; the block grows to cover the largest, and since its base never
 * moves the earlier loads keep reading the right bytes.
 *
 * Texture and TLUT blocks are the only ones cached. Their extents are byte
 * counts and need not be a whole number of words, so a short final word is
 * zero-padded rather than read past - see PaddedReader in the .cpp.
 *
 * === What the output looks like ===
 *
 *   - Canonical 8-byte commands as {uint32 w0, uint32 w1} pairs, written into
 *     the arena's frame region.
 *   - Each stream is prefixed with the RT64 extended-GBI enable hook
 *     (SPNOOP + magic 0x525464, rt64_gbi_extended.cpp:334-353) followed by
 *     G_EX_SETRDRAMEXTENDED(1). The prefix is per stream, not per frame:
 *     every stream is handed to processDisplayLists on its own, and a stream
 *     that assumed a previous one had enabled extended addressing would break
 *     the moment the order changed.
 *   - Every emitted address is a tagged extended-RDRAM address: bit 31 set,
 *     4-byte aligned, no PD flag bits (rt64_mem.h, invariant 4). With
 *     G_EX_SETRDRAMEXTENDED on, RT64 passes such addresses through
 *     fromSegmented untouched and subtracts the tag in maskPhysicalAddress
 *     (rt64_rsp.cpp:97-127), skipping both segment lookup and the 8 MB
 *     physical mask.
 *   - Segment references and PD LSB flags are fully resolved here, so the
 *     output contains no G_MOVEWORD/G_MW_SEGMENT.
 *   - G_DL calls are flattened: nested lists are walked in place and the
 *     result is one linear stream ending in a single G_ENDDL. Flattening
 *     removes any need to place sub-lists at stable arena addresses, at the
 *     cost of defeating RT64-side DL caching. Revisit only if translation
 *     time exceeds budget (SCAFFOLD risk R1).
 *
 * === The dialects are not the same modulo width ===
 *
 * Each of these was verified against both sources and each would render
 * plausible-looking nonsense if assumed away:
 *
 *   G_VTX      port packs a BYTE TOTAL in w0[0:16] (gfx_pc.cpp:2340); RT64
 *              wants count-1 in w0[20:4] (rt64_gbi_f3dpd.cpp:15).
 *   G_COL      port packs a byte total too, but RT64's setVertexColorPD takes
 *              only an address (rt64_rsp.cpp:430) - the count is dropped.
 *              Vertices index this table by byte offset through their own ci
 *              field (rt64_rsp.cpp:390), so it must be one contiguous block.
 *   opcode 0   G_POPMTX to the port (gbi.h:113, gfx_pc.cpp:2325); G_SPNOOP -
 *              and therefore the extended-GBI hook - to RT64. Lowered to
 *              0xBD, whose w1 must be 0 or RT64 does not pop
 *              (rt64_gbi_f3d.cpp:21).
 *   G_TRI4     same bit layout as RT64's G_TRIX, different termination: RT64
 *              stops as soon as the remaining w1 is zero
 *              (rt64_gbi_f3dgolden.cpp:14) where the port tests each triple
 *              (gfx_pc.cpp:1618-1650). Only degenerate triangles differ, so
 *              both words pass through unchanged.
 *
 * === Swizzles differ per block, and not in the way the shapes suggest ===
 *
 * All four verified against how RT64 actually reads the block, which is the
 * only thing that settles it:
 *
 *   G_MTX          Swizzle::U32. Not U16, despite a matrix being 32 halfwords:
 *                  the port stores packed int32 N64 words (gfx_pc.cpp:1013-
 *                  1016) and RT64's FixedMatrix::toFloat indexes with j^1
 *                  (rt64_common.cpp:125-128), so the accessor's swap and the
 *                  convention's swap cancel and a straight copy is correct.
 *   G_MOVEMEM      Swizzle::U16 for G_MV_VIEWPORT: RT64 reads vscale[1] as x
 *   (viewport)     (rt64_rsp.cpp:906) where fast3d reads vscale[0]
 *                  (gfx_pc.cpp:1720), so the halves must swap.
 *   G_MOVEMEM      Swizzle::U8 for lights and lookat: RT64's DirLight is
 *   (lights)       declared byte-reversed within each word (rt64_rsp.h:98-109)
 *                  and reads bytes at swappedOffset(i) = i^3
 *                  (rt64_math.h:10).
 *   G_VTX          Swizzle::VtxPD. Word 1 mixes a native s16 z with two u8
 *                  fields, so it needs the u16 rule and the u8 rule at once;
 *                  a plain U16 would leave ci's bytes the wrong way round and
 *                  every vertex would take its colour from the flags byte.
 *   G_COL          Swizzle::U8: RT64 reads col[3],col[2],col[1],col[0] as
 *                  r,g,b,a (rt64_rsp.cpp:396-399).
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rt64_arena.h"
#include "rt64_debug.h"
#include "rt64_mem.h"

namespace pdrt64 {

enum class TranslateStatus : uint8_t {
    Ok,
    UnknownOpcode,      // opcode outside the port dialect: hard error
    CaptureMiss,        // MemReader could not supply referenced memory
    ValidationFailed,   // emitted stream failed its own decode check
};

struct TranslateStats {
    uint32_t commandsIn = 0;
    uint32_t commandsOut = 0;
    uint32_t trianglesIn = 0;       // from TRI1/TRI4 payloads
    uint32_t listsFlattened = 0;    // G_DL calls and branches followed
    uint32_t droppedCommands = 0;   // not handled at this task's scope
    uint32_t droppedPerOpcode[256] = {};  // counts whole commands, nothing else

    /* G_EXTRAGEOMETRYMODE_EXT commands that were acted on but carried flags
     * beyond invert-culling, which v1 drops (SCAFFOLD 3.4). Kept separate:
     * folding it into droppedPerOpcode would make one entry of that array mean
     * "flags ignored" while every other entry means "command dropped". */
    uint32_t droppedExtraGeometryFlags = 0;
    uint64_t marshalledBytes = 0;
    double translateMs = 0.0;

    /* Texture and TLUT blocks, which are the only ones cached (T6). A static
     * scene should be all hits from its second frame; a rising miss count
     * frame after frame means the cache key or its invalidation is wrong. */
    uint32_t textureBlocks = 0;
    uint32_t textureCacheHits = 0;
    uint32_t textureCacheMisses = 0;

    /* G_SETTIMG commands no load command ever sized. Their emitted address
     * stays a placeholder, which is harmless - RT64 only stores it - but a
     * nonzero count means the deferred-sizing assumption needs revisiting. */
    uint32_t unsizedImages = 0;
};

/*
 * Per-frame translation context. Create once at backend init, call
 * beginFrame() then translate() for each root list.
 *
 * The context owns the mutable RSP-mirror state needed to resolve addresses,
 * which is currently the segment table and the invert-culling flag.
 */
class Translator {
public:
    Translator(Arena &arena, FbRegistry &fbs, MemReader &mem);

    /* Resets per-frame state. Must be called after Arena::beginFrame.
     *
     * The segment table resets with the frame. Every gSPSegment in the game
     * writes into the list being built (src/lib/model.c:3161-3293,
     * src/game/bg.c:2197,3225,3243), so a list carries the bindings it needs
     * and nothing depends on them surviving a frame boundary - confirmed on a
     * real capture, which resolves every reference from a zeroed table. */
    void beginFrame();

    /* Translates one root display list, as handed to gfx_run. On success
     * writes the tagged start/end of the emitted stream, to be passed to RT64
     * processDisplayLists. May be called several times per frame; each call
     * appends an independent stream. */
    TranslateStatus translate(uintptr_t rootDl, RdramAddr *outStart, RdramAddr *outEnd);

    const TranslateStats &stats() const { return stats_; }

    /* Human-readable reason for the last non-Ok status. */
    const std::string &lastError() const { return error_; }

    /* When enabled, every emitted command is decoded back by rt64_debug's
     * independent decoder and checked against what this file meant to emit,
     * and every emitted address is checked for the invariant-4 form. Debug
     * builds and all tests default on (invariant 6). */
    void setValidationEnabled(bool enabled) { validate_ = enabled; }
    bool validationEnabled() const { return validate_; }

private:
    void emit(uint32_t w0, uint32_t w1);
    void emitStreamPrefix();

    /* Marshals a referenced block into the arena and returns its tagged
     * address. Returns false and sets the error on a capture miss.
     *
     * `cached` selects the persistent region. Only texture and TLUT blocks
     * set it: their source is immutable until the game says otherwise, and it
     * says so through the texture-cache calls that drive
     * Arena::invalidateCache. That is the same contract fast3d's own texture
     * cache runs on - it keys on the source pointer too (gfx_pc.h:23-28) and
     * is invalidated by the same calls - so this is no more of an assumption
     * than the renderer we are matching already makes. Vertices, matrices,
     * colours and movemem blocks are NOT cached: they live in heap the game
     * rewrites between frames with no notification (docs/census.md). */
    bool pushBlock(const GfxRef &ref, Swizzle type, bool cached, RdramAddr *out);

    /* Marshals a texture or TLUT block and patches the address into the
     * G_SETTIMG that named it. Returns false only on a capture miss. */
    bool sizePendingImage(const GfxRef &ref);

    /* EXT lowerings that expand to several canonical commands (SCAFFOLD 3.4).
     * Each leaves the RDP state it found: whatever colour image or cycle type
     * they change, they change back. */
    void emitSetColorImage(uint32_t w0, RdramAddr addr);
    void emitDepthClear();
    void emitFramebufferCopy(const Gfx &cmd);
    void emitImageRect(const Gfx *cmd);

    TranslateStatus walk(uintptr_t at, int depth);
    bool validateEmitted(size_t pairIndex, const DecodedCmd &expect);

    Arena &arena_;
    FbRegistry &fbs_;
    MemReader &mem_;

    /*
     * A G_SETTIMG whose block size is not yet known.
     *
     * The command names a texture image but nothing in it says how big that
     * image is; only the G_LOADBLOCK/G_LOADTILE/G_LOADTLUT that follows gives
     * a size (rt64_capture.cpp derives it the same way). So the command is
     * emitted straight away with a placeholder address and patched once a load
     * arrives - emitting it late instead would reorder it past the G_SETTILE
     * that normally sits between the two, and RT64 needs the image set before
     * the load.
     *
     * Several loads can follow one G_SETTIMG. Each is checked against the
     * extent already marshalled and the block is grown if needed, so the final
     * address covers every load - and because the base never moves, the
     * earlier loads still read the right bytes.
     */
    struct PendingImage {
        size_t pairIndex = 0;    // index of the SETTIMG w0 within out_
        bool valid = false;
        uintptr_t src = 0;
        size_t marshalledLen = 0;
    };
    PendingImage pendingImage_;

    /* Two different colour images, and conflating them is a bug the tests
     * caught: `bound` is whatever the stream last pointed at, which the
     * multi-command lowerings save and restore around their own work, while
     * `main` is the last one the GAME chose with a G_SETCIMG, which is what
     * G_SETFB_EXT(0) means by "back to the main image". Binding an offscreen
     * framebuffer must not overwrite the second, or the restore returns to the
     * framebuffer it was meant to leave.
     *
     * Both keep the whole emitted command, because restoring an address
     * without the format and width the game chose rebinds the image wrongly. */
    uint32_t boundCimgW0_ = 0;
    RdramAddr boundCimg_ = 0;
    uint32_t mainCimgW0_ = 0;
    RdramAddr mainCimg_ = 0;

    /* Shadow of the high othermode word, tracked from the commands that set
     * it. The depth-clear idiom has to switch to fill cycle and switch back,
     * and there is nothing to switch back to unless we know what it was. */
    uint32_t otherModeH_ = 0;

    GfxWalkState st_;
    std::vector<uint32_t> out_;   // emitted words, flushed to the arena at end
    std::vector<uint8_t> scratch_;
    TranslateStats stats_;
    std::string error_;
    bool validate_ = true;
    bool invertCulling_ = false;
};

/* The extended opcode this translator registers with RT64. Must be nonzero and
 * must differ from the hook's own opcode, which is 0 (rt64_gbi_extended.cpp:
 * 348); 0x64 is RT64's own default and collides with nothing in F3D. */
constexpr uint8_t kExtendedOpcode = 0x64;

} // namespace pdrt64
