#pragma once

/*
 * Memory access + marshalling rules for the RT64 backend.
 *
 * === Why marshalling exists (read this before touching anything) ===
 *
 * RT64 consumes RDRAM in the *emulator convention*: the buffer is an array
 * of little-endian uint32 words whose 32-bit VALUES equal the big-endian
 * N64 words. Consequences, all verified against RT64 source:
 *
 *   - uint32 data (matrices, packed RGBA words, Gfx w0/w1) is read at its
 *     natural offset with its natural value.
 *   - uint16 data at N64 offset A is read by RT64 as a host-LE uint16 at
 *     offset A^2. Evidence: RSP::VertexPD is declared {y,x, ci,z, t,s}
 *     (rt64/src/hle/rt64_rsp.h:63-68) - the halves of every word are
 *     swapped relative to the game's natural {x,y, flag:ci, z, s,t}.
 *   - uint8 data at N64 offset A is read at host offset A^3. Evidence:
 *     setVertexPD reads the color table as col[3],col[2],col[1],col[0]
 *     for r,g,b,a (rt64_rsp.cpp:396-399).
 *
 * === Source order is NOT uniform. Check the table before marshalling. ===
 *
 * An earlier draft of this header assumed all port memory is natural
 * little-endian C data. That is false. The port keeps SOME data in native
 * LE (structs it dereferences directly) and SOME in raw N64 big-endian
 * byte order (texel data it converts on the fly). Getting this wrong is
 * survivable-looking: geometry renders correctly while every texture is
 * garbage.
 *
 * Verified source order, by data type (all citations are fast3d, which is
 * ground truth: it renders correctly today, so whatever it does to read a
 * block tells you how that block is stored):
 *
 *   Vtx (12 bytes)      NATIVE LE. gfx_pc.cpp:1062-1070 reads v->v[0..2],
 *                       v->s, v->t, v->colour as plain struct fields with
 *                       no conversion.                    -> Swizzle::VtxPD
 *   Mtx (s15.16)        NATIVE LE. gfx_pc.cpp:988-993 reads int32 integer
 *                       and fraction halves directly.     -> Swizzle::U32
 *   Colour table        NATIVE LE, byte-addressed r,g,b,a.
 *   (NormalColor)       gfx_pc.cpp:69-74 declares {u8 r,g,b,a}; read at
 *                       :967 and :1092-1094. The U8 rule lands r at dst
 *                       byte 3, which is what RT64 expects when it reads
 *                       col[3],col[2],col[1],col[0] as r,g,b,a
 *                       (rt64_rsp.cpp:396-399).            -> Swizzle::U8
 *   Texture data,       RAW N64 BIG-ENDIAN, every format.
 *   all formats         gfx_pc.cpp:623 assembles rgba16 texels from BE byte
 *                       pairs, (addr[2*i] << 8) | addr[2*i+1]; :653 applies
 *                       PD_BE32 to rgba32; :854 indexes CI data bytewise.
 *                                                          -> Swizzle::BE32
 *   TLUT / palette      RAW N64 BIG-ENDIAN. gfx_pc.cpp:1864 applies PD_BE16
 *                       to every entry.                    -> Swizzle::BE32
 *
 * PD_BE16/PD_BE32 are byte swaps on little-endian hosts and no-ops on
 * big-endian ones (src/include/platform.h:61-71), so their presence in a
 * read path is proof the stored bytes are big-endian.
 *
 * Rules, given the source order:
 *
 *   NATIVE LE source (dst_offset transform, value copied verbatim):
 *     u32 element:  dst_offset = src_offset
 *     u16 element:  dst_offset = src_offset ^ 2
 *     u8  element:  dst_offset = src_offset ^ 3
 *
 *   RAW BE source:
 *     reverse the bytes of every 32-bit word (bswap32 per word)
 *
 * One rule covers every texel format because the BE transform is purely
 * positional - byte i of a word moves to byte 3-i - regardless of whether
 * the block is later interpreted as 4-, 8-, 16- or 32-bit texels.
 *
 * IMPLEMENTATION NOTE: BE32 and U8 are the same byte permutation (both map
 * i -> 3-i within each word). They are separate enum values because their
 * REASONS differ, and conflating them invites someone to "simplify" a call
 * site by picking the wrong one for the wrong data. One implementation may
 * back both; the enum documents intent.
 *
 * All marshalled blocks must start 4-byte aligned in both source
 * interpretation and destination placement.
 *
 * === Address form emitted by the translator ===
 *
 * All emitted addresses use RT64's extended-RDRAM form: bit 31 set, low 31
 * bits = byte offset into the synthetic RDRAM buffer. With
 * G_EX_SETRDRAMEXTENDED enabled, RT64 resolves these by subtracting
 * 0x80000000 and skipping both segment lookup and the 8MB physical mask
 * (rt64_rsp.cpp:97-127). Emitted addresses must be 4-byte aligned with no
 * PD LSB flag bits; the translator resolves the port's segment/LSB
 * convention (gfx_pc.cpp:2262-2273) before emission.
 */

#include <cstdint>
#include <cstddef>

namespace pdrt64 {

/* Extended-RDRAM address: bit 31 set, low 31 bits = arena byte offset. */
using RdramAddr = uint32_t;

constexpr uint32_t kExtendedAddrBit = 0x80000000u;

/* Element type of a marshalled block; selects the swizzle rule above.
 * Pick by SOURCE ORDER first (native LE vs raw BE), then by element width.
 * When in doubt, find how fast3d reads the block and follow the table. */
enum class Swizzle : uint8_t {
    // --- native little-endian source ---
    U32,     // matrices (s15.16 int32 pairs), raw 32-bit words
    U16,     // generic native s16/u16 arrays
    U8,      // native byte-addressed data: colour tables (NormalColor/Col)
    VtxPD,   // PD 12-byte vertices: words 0,2 use U16 rule on both halves,
             // word 1 is {s16 z, u8 flags, u8 colour} -> U16 rule for z,
             // U8 rule for flags/colour. Provided as a fast path; the
             // generic rules produce the same bytes.

    // --- raw N64 big-endian source ---
    BE32,    // ALL texel data (rgba16/rgba32/ia/i/ci, any bit depth) and
             // TLUT/palette entries. bswap32 per word. NOT U16 - assigning
             // U16 here is the specific bug this enum split exists to
             // prevent; it half-swaps 16-bit texels and produces coloured
             // noise while geometry still looks fine.
};

/*
 * Where a referenced block lives.
 *
 * Display lists reach more than one region, and anything assuming otherwise
 * silently drops data. Measured in docs/census.md:
 *
 *   Heap   the bulk of it - display lists, vertices, matrices, colours.
 *   Image  statics compiled into the executable: light and viewport blocks
 *          reached via G_MOVEMEM, static display lists reached via G_DL.
 *          Small (~1 KB/frame) but load-bearing, and it is neither heap nor
 *          ROM, which is what made it invisible until it was hunted down.
 *   Rom    the loaded ROM image. MEASURED ZERO in every frame captured so
 *          far. An earlier draft of this header claimed the menu referenced
 *          it ~12 times per frame; that was an inference and it was wrong.
 *          Kept because classifying it costs nothing and proves the absence,
 *          but do NOT design a large immutable-ROM cache tier around it.
 *   Malloc room graphics data, 25-35 KB/frame in gameplay. bgLoadRoom uses
 *          plain sysMemAlloc on this platform (src/game/bg.c:2839), so it is
 *          in none of the spans above. It is not one contiguous range, so it
 *          is classified by asking the port's allocation registry
 *          (sysMemIsTracked) rather than by a bounds test.
 *
 * Immutability, which is what the arena's caching policy turns on: Image and
 * Rom never change after load, so blocks from them can be marshalled once and
 * kept. Heap blocks are rewritten between frames and cannot. Malloc blocks
 * measured byte-stable across every captured frame (docs/census.md) but the
 * referenced set grows as rooms come into view, so cache them append-only and
 * drop the lot on stage load.
 */
enum class Region : uint8_t {
    None,       // not in any region we can prove is mapped: never dereference
    Heap,       // g_MempHeap: display lists, vertices, matrices, colours
    Rom,        // g_RomFile: the loaded ROM image; immutable
    Image,      // the executable's own image: linker-placed statics; immutable
    Malloc,     // a live sysMemAlloc block, proven mapped by the registry:
                // room graphics data in practice
};

/*
 * Predicate for Region::Malloc: "does the port have a live allocation covering
 * [addr, addr+len)?". Backed by sysMemIsTracked in port/src/system.c.
 *
 * Injected as a plain function pointer rather than called directly so this
 * translation unit keeps its no-dependency property - the unit tests compile
 * rt64_mem.cpp with nothing but the toolchain, and pass their own fake. A null
 * predicate means "no registry", and such pointers then classify as None and
 * are refused, which is the safe failure.
 */
using TrackedAllocFn = bool (*)(uintptr_t addr, size_t len);

/*
 * Abstract source-memory reader. Two implementations:
 *   - LiveMemReader: direct dereference of game memory (in-process).
 *   - CaptureMemReader: lookup into a capture file's recorded ranges
 *     (tools/dlreplay and unit tests). Returns false for addresses the
 *     capture does not contain, which the translator reports as an error.
 *
 * The translator performs ALL source-memory access through this interface
 * so that translation is a pure, replayable function of (DL, memory).
 */
class MemReader {
public:
    virtual ~MemReader() = default;

    /* Copies `len` bytes of source memory at host address `src` into
     * `dst`. Returns false if the range is unavailable (capture miss).
     * `src` is the value that appeared in the display list after segment
     * resolution, i.e. a host pointer in the game process's terms. */
    virtual bool read(uintptr_t src, void *dst, size_t len) = 0;

    /* Classifies where [src, src+len) lives. Marshalling itself does not
     * care, but the census does, and the arena's caching policy does: ROM
     * data is immutable and can be cached for the process lifetime, while
     * heap data can be rewritten between frames. */
    virtual Region regionOf(uintptr_t src, size_t len) const = 0;
};

/* In-process reader over live game memory. Regions come from the port:
 * g_MempHeap/g_MempHeapSize, g_RomFile/g_RomFileSize, and the executable
 * image (PE SizeOfImage on Windows; see rt64_capture.cpp for how the capture
 * obtains it). The image range may be left null on platforms where it has not
 * been wired up - those references then classify as Untracked and read()
 * refuses them, which is the safe failure. */
class LiveMemReader final : public MemReader {
public:
    LiveMemReader(const uint8_t *heapBase, size_t heapSize,
                  const uint8_t *romBase, size_t romSize,
                  const uint8_t *imageBase = nullptr, size_t imageSize = 0,
                  TrackedAllocFn tracked = nullptr);
    bool read(uintptr_t src, void *dst, size_t len) override;
    Region regionOf(uintptr_t src, size_t len) const override;

private:
    struct Span { uintptr_t base; size_t size; };
    Span heap_, rom_, image_;
    TrackedAllocFn tracked_;
};

/*
 * Applies the swizzle rule for `type` while copying `len` bytes from
 * `src` (game memory, in the source order the table above gives for that
 * data type) to `dst` (emulator-convention RDRAM bytes). `len` and both
 * pointers must be 4-byte aligned, except Swizzle::VtxPD which requires
 * 12-byte multiples.
 * This is the single place the marshalling rules are implemented; unit
 * tests pin its output byte-for-byte (see SCAFFOLD.md task T4), and must
 * cover BE32 against a hand-computed big-endian texel vector, not only
 * the native-LE cases.
 */
void marshalCopy(void *dst, const void *src, size_t len, Swizzle type);

} // namespace pdrt64
