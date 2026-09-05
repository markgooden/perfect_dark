#pragma once

/*
 * Disassembly and hashing for the RT64 backend.
 *
 * Two dialects, two disassemblers, deliberately not sharing a decoder:
 *
 *   disasmPortDl       the port's widened 64-bit Gfx stream (16-byte commands)
 *                      as fast3d executes it (port/fast3d/gfx_pc.cpp:2300-2560)
 *   disasmCanonicalDl  the 8-byte stream the translator emits, decoded the way
 *                      RT64 decodes it (vendor/rt64/src/gbi/rt64_gbi_f3d.cpp,
 *                      rt64_gbi_f3dpd.cpp, rt64_gbi_extended.cpp)
 *
 * They must stay independent. The canonical side is what the translator's
 * validation mode checks its own output against (invariant 6), and a shared
 * decoder would happily agree with a translator bug. The dialects are NOT the
 * same modulo width - G_VTX alone packs its count as a byte total in w0[0:16]
 * on the port side (gfx_pc.cpp:2340) and as count-1 in w0[20:4] on RT64's
 * (rt64_gbi_f3dpd.cpp:15) - so a decoder written once for both would have to
 * be told which rules to apply, which is the bug it is meant to catch.
 *
 * === Output is a committed golden, so it must be reproducible ===
 *
 * CLAUDE.md invariant 7: .disasm goldens are committed, .pddl captures never
 * are. Two properties follow, and both are enforced here rather than left to
 * whoever writes a golden:
 *
 *   No payload bytes.  Anything reached through a pointer - vertices, texels,
 *                      TLUTs, matrices, lights, colour tables - is printed as
 *                      `name size hash`, never dumped. The hash still pins the
 *                      bytes, so a payload regression fails the diff.
 *   No raw addresses.  Host pointers move between runs (ASLR, allocation
 *                      order). Every address prints as a block ordinal plus an
 *                      offset, numbered in first-reference order along the
 *                      walk, which depends only on the display list.
 *
 * === Sizing lives here, not in the capture ===
 *
 * gfxCommandWords/gfxStep are the single description of what a port command
 * spans and what it references. rt64_capture.cpp uses them to decide which
 * bytes to record; the disassembler uses them to decide which bytes to hash.
 * The two MUST agree: if the disassembler hashed a range the capture did not
 * record, replaying a golden would report capture misses that mean nothing.
 * That is why this is one function and not two similar ones.
 */

#include <cstddef>
#include <cstdint>
#include <string>

#include "rt64_mem.h"

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

namespace pdrt64 {

/*
 * === Command shape and references ===
 */

/* Gfx words the command at `opcode` occupies. Commands carrying more operands
 * than two words are stored as consecutive Gfx and fast3d advances over them
 * (gfx_pc.cpp:2435-2503); a walker that assumes 1 desynchronises and decodes
 * operand words as opcodes. */
uint32_t gfxCommandWords(uint8_t opcode);

/* What kind of block a command points at. Determines the swizzle a marshaller
 * must use and how a disassembler labels the reference. */
enum class RefKind : uint8_t {
    None,        // command references no memory
    DisplayList, // G_DL: a nested command stream, walked rather than hashed
    Matrix,      // G_MTX: one Mtx, native LE s15.16          -> Swizzle::U32
    MoveMem,     // G_MOVEMEM: light/viewport/lookat, 16 bytes -> Swizzle::U32
    Vertices,    // G_VTX: n * sizeof(Vtx), native LE          -> Swizzle::VtxPD
    Colours,     // G_COL: n * sizeof(Col), native LE bytes    -> Swizzle::U8
    Texels,      // G_LOADBLOCK/G_LOADTILE: raw N64 BE texels  -> Swizzle::BE32
    Tlut,        // G_LOADTLUT: raw N64 BE palette entries     -> Swizzle::BE32
};

/* A command's reference to source memory. */
struct GfxRef {
    RefKind kind = RefKind::None;

    /* Segment-resolved host address, or 0 when `segmented` is set and the
     * segment is unbound. Zero with kind != None means "referenced but not
     * resolvable": read nothing from it. */
    uintptr_t addr = 0;

    /* Bytes the renderer reads at `addr + startOffset`. Texture loads read a
     * sub-rectangle of a larger image, so the recorded block starts at `addr`
     * and runs startOffset + bytes (rt64_capture.cpp:310-314). */
    uint32_t bytes = 0;
    uint32_t startOffset = 0;

    /* Set when w1 carried the port's segmented form (LSB set,
     * gfx_pc.cpp:2285-2296). Kept even after successful resolution so a
     * disassembly can name the segment instead of a moving host address. */
    bool segmented = false;
    uint8_t segment = 0;
    uint32_t segOffset = 0;
};

/*
 * Interpreter state a walker needs to resolve and size references.
 *
 * Segments are bound by G_MOVEWORD/G_MW_SEGMENT commands inside the stream
 * itself - every gSPSegment in the game writes into the display list being
 * built (src/lib/model.c:3161-3293, src/game/bg.c:2197,3225,3243) - so a walk
 * that starts from a zeroed table resolves everything a submitted list uses.
 * `seed` exists for the in-process case, where fast3d's live table is
 * available and costs nothing to pass.
 *
 * Texture loads are sized from the G_SETTIMG that preceded them, which is why
 * this is a state machine and not a pure function of one command.
 */
struct GfxWalkState {
    uintptr_t segments[16] = {};

    /* Pending G_SETTIMG: address, size code and width-1 of the source image. */
    uintptr_t texAddr = 0;
    uint32_t texSiz = 0;
    uint32_t texWidth = 0;

    void reset(const uintptr_t *seed = nullptr);
};

/*
 * Advances `st` for `cmd` and returns what `cmd` references.
 *
 * One call per executed command, in execution order - the state machine only
 * makes sense over a stream. Control flow is the caller's business: G_DL
 * returns RefKind::DisplayList with the resolved target and the caller decides
 * whether to follow it.
 */
GfxRef gfxStep(GfxWalkState &st, const Gfx *cmd);

/* fast3d's seg_addr (gfx_pc.cpp:2285-2296): the port flags segmented
 * addresses with the low bit set, everything else is a raw host pointer.
 * Returns 0 for a segmented reference whose segment is unbound. */
uintptr_t gfxSegResolve(uintptr_t w1, const uintptr_t segments[16]);

/*
 * === Hashing ===
 */

/* FNV-1a, 64-bit (offset basis 0xcbf29ce484222325, prime 0x100000001b3).
 * Chosen for being short enough to reimplement in a scratch script when a
 * golden diff needs explaining, not for collision resistance - nothing here
 * is adversarial. */
uint64_t hashBytes(const void *data, size_t len);

/* Same hash over a range of the synthetic RDRAM buffer. */
uint64_t hashRdramRange(const uint8_t *rdramBase, RdramAddr start, size_t len);

/*
 * === Disassembly ===
 */

struct DisasmOptions {
    /* Stop after this many commands. A runaway or circular list must not hang
     * the tool that is trying to show you why it is broken. */
    size_t maxCommands = 200000;

    /* Follow G_DL into nested lists (indented). Off prints the call site
     * only, which is what you want when diffing one list in isolation. */
    bool followCalls = true;

    /* Print `payload=<name> <size>B h=<hash>` for referenced blocks. Requires
     * a MemReader that can supply them; blocks it cannot are printed as
     * `<unreadable>`, which is itself a stable and informative golden line. */
    bool hashPayloads = true;
};

/*
 * Disassembles the port's dialect starting at `rootDl`.
 *
 * All memory access, including the command words themselves, goes through
 * `mem` - so this runs identically against live game memory and against a
 * capture, which is what makes a golden meaningful.
 */
std::string disasmPortDl(uintptr_t rootDl, MemReader &mem,
                         const DisasmOptions &opts = {},
                         const uintptr_t *seedSegments = nullptr);

/* Disassembles a translated canonical stream in arena memory. Addresses are
 * arena offsets, already stable, and print as `rdram+0x...`. */
std::string disasmCanonicalDl(const uint8_t *rdramBase, RdramAddr start,
                              RdramAddr end, const DisasmOptions &opts = {});

/*
 * Independent decoder for the translator's validation mode.
 *
 * Decodes one canonical command into a normalised field bag WITHOUT reusing
 * translator code, so the validator is comparing two independent readings of
 * the same bytes. Field meanings are per-opcode and documented at the
 * implementation's switch; the validator compares bags, it does not interpret
 * them.
 */
struct DecodedCmd {
    uint8_t opcode = 0;
    uint32_t fields[8] = {};
    uint8_t fieldCount = 0;

    /* Words this command occupies in the canonical stream. */
    uint8_t words = 1;
};

/* Returns false if `at` is not a readable, aligned address inside the arena,
 * or if the opcode is not one the canonical dialect defines. */
bool decodeCanonicalCmd(const uint8_t *rdramBase, RdramAddr at, size_t rdramSize,
                        DecodedCmd *out);

/* Opcode mnemonic in the port dialect ("G_VTX"), or "G_UNK_xx". */
const char *gfxOpcodeName(uint8_t opcode);

/* Opcode mnemonic in the canonical dialect. These differ: 0xB1 is G_TRI4 to
 * the port and F3DGOLDEN_G_TRIX to RT64, 0x07 is G_COL and F3DPD_G_VTXCOLOR. */
const char *canonicalOpcodeName(uint8_t opcode);

} // namespace pdrt64
