/*
 * Display-list capture.
 *
 * Records, from inside fast3d's interpreter (pdCaptureCommandHook, called by
 * gfx_run_dl for every command it executes):
 *
 *   - every Gfx command word of the stream, including nested lists
 *   - the contents of every block the stream references: vertices, colour
 *     tables, matrices, movemem blocks, texture data and TLUTs
 *
 * Segmented addresses are resolved with the table fast3d hands us at each
 * command, so the capture cannot disagree with the renderer about what a
 * frame contains. The independent walker that used to drive capture is kept
 * as a self-check (verifyWalk): after each frame it re-walks the list from
 * the same starting state and reports the first command it reaches that
 * fast3d did not execute. The translator needs exactly such a walk, so the
 * check is the proof that one can be written to match.
 *
 * Blocks are stored at their ORIGINAL host addresses so a replay can resolve
 * a display-list pointer by lookup. Sizes for texture data are derived the
 * same way fast3d derives them (gfx_dp_load_block at gfx_pc.cpp:1870,
 * gfx_dp_load_tile at :1897, gfx_dp_load_tlut at :1833), because the size of
 * a G_SETTIMG block is only knowable from the load command that follows it.
 *
 * The .pddl file format is documented once, in rt64_pddl.h, which also holds
 * the reader and the shared kPddlVersion. Do not restate the layout here: the
 * writer below and that reader are the two halves of one format and they drift
 * the moment there are two descriptions of it.
 */

#ifdef _WIN32
#include <windows.h>
#endif

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <PR/gbi.h>
#include <PR/ultratypes.h>

#include "rt64_capture.h"
#include "rt64_debug.h"
#include "rt64_pddl.h"

extern "C" {
#include "platform.h"
#include "system.h"
#include "video.h"
}

/* gbiex.h defines the port-private opcodes; it needs PLATFORM_N64 to be
 * undefined, which it is for the port build. */
extern "C" {
#include "gbiex.h"
}

extern "C" const uintptr_t *f3d_get_segment_table(void);

extern u8 *g_MempHeap;
extern u32 g_MempHeapSize;
extern u8 *g_RomFile;
extern u32 g_RomFileSize;

namespace {

/* The writer's version is the reader's version by construction: it comes from
 * rt64_pddl.h, so the two halves of the format cannot drift apart. */
using pdrt64::kPddlVersion;

/* Same accessors gfx_run_dl uses. */
static inline uint32_t C0(const Gfx *cmd, uint32_t pos, uint32_t width)
{
    return (uint32_t)((cmd->words.w0 >> pos) & ((1U << width) - 1));
}

static inline uint32_t C1(const Gfx *cmd, uint32_t pos, uint32_t width)
{
    return (uint32_t)((cmd->words.w1 >> pos) & ((1U << width) - 1));
}

struct CaptureState {
    bool armed = false;
    bool quitWhenDone = false;
    int skipLeft = 0;      /* rendered frames to discard before recording */
    bool sawDl = false;    /* a display list arrived this frame */
    int framesLeft = 0;
    int frameIndex = 0;
    std::string prefix;

    /* Per-frame accumulation. */
    std::vector<uintptr_t> roots;
    std::map<uintptr_t, std::vector<uint8_t>> ranges;
    bool frameOpen = false;

    /* Every command address fast3d executed this frame, and the segment table
     * as it stood when the root was submitted. Together they let verifyWalk
     * replay the frame independently and be checked against the truth. */
    std::set<uintptr_t> executed;
    uint32_t executedCount = 0;
    uintptr_t seedSegments[16] = {};

    /* Census (T3-lite): per-region accounting of what the frame referenced.
     * "malloc" is the port's tracked allocations (room graphics data);
     * "untracked" is anything left over, which should be zero and is a gap
     * in the translator's memory model if it is not. */
    uint32_t heapBlocks = 0, romBlocks = 0, imageBlocks = 0;
    uint32_t mallocBlocks = 0, untrackedRefs = 0;
    uint64_t heapBytes = 0, romBytes = 0, imageBytes = 0;
    uint64_t mallocBytes = 0, untrackedBytes = 0;

    /* Hotkey arming: --capture-key sets these up, F9 fires them. */
    bool hotkeyEnabled = false;
    bool hotkeyWasDown = false;
    int hotkeyFrames = 0;
    std::string hotkeyPrefix;

    /* Golden image for this frame, if the backend supplied one. */
    std::vector<uint8_t> goldenRgb;
    int goldenW = 0;
    int goldenH = 0;
};

CaptureState g_cap;

/* fast3d's seg_addr, including its fallthrough for an unbound segment
 * (gfx_pc.cpp:2285-2296). Lives in rt64_debug.cpp so the disassembler and this
 * file cannot resolve an address two different ways. */
void *segAddr(uintptr_t w1, const uintptr_t *segments)
{
    return (void *)pdrt64::gfxSegResolve(w1, segments);
}

/*
 * The executable's own image. Display lists reference static data compiled
 * into the binary - light and viewport blocks reached via G_MOVEMEM - which
 * is neither heap nor ROM. The image is mapped for the process lifetime, so
 * reading it is safe; bounds come from the PE headers rather than a guess.
 */
uintptr_t g_imageBase = 0;
size_t g_imageSize = 0;

void initImageRegion(void)
{
#ifdef _WIN32
    HMODULE mod = GetModuleHandleW(NULL);
    if (!mod) {
        return;
    }
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    const IMAGE_NT_HEADERS *nt =
        (const IMAGE_NT_HEADERS *)((const uint8_t *)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }
    g_imageBase = (uintptr_t)mod;
    g_imageSize = (size_t)nt->OptionalHeader.SizeOfImage;
#endif
}

bool inRegion(uintptr_t addr, size_t len, const u8 *base, u32 size)
{
    if (!base || !addr || !len) {
        return false;
    }
    const uintptr_t b = (uintptr_t)base;
    return addr >= b && (addr + len) <= (b + (uintptr_t)size);
}

bool inHeap(uintptr_t addr, size_t len)
{
    return inRegion(addr, len, g_MempHeap, g_MempHeapSize);
}

/* The memp heap is not the only region display lists point into; statics in
 * the executable image are the other one that matters (docs/census.md). */
bool inImage(uintptr_t addr, size_t len)
{
    return g_imageSize && addr >= g_imageBase &&
           (addr + len) <= (g_imageBase + g_imageSize);
}

/* Records [addr, addr+len). Every range comes from a command fast3d is about
 * to execute, at a size derived the way fast3d derives it, so fast3d itself
 * is about to read exactly these bytes: they are mapped or the game is
 * already dead. That is what makes reading outside the modelled regions safe
 * here, and it is the only reason it is safe - an independent walk has no
 * such guarantee. */
void recordRange(uintptr_t addr, size_t len)
{
    if (!addr || !len) {
        return;
    }

    if (inHeap(addr, len)) {
        ++g_cap.heapBlocks;
        g_cap.heapBytes += len;
    } else if (inImage(addr, len)) {
        ++g_cap.imageBlocks;
        g_cap.imageBytes += len;
    } else if (inRegion(addr, len, g_RomFile, g_RomFileSize)) {
        ++g_cap.romBlocks;
        g_cap.romBytes += len;
    } else if (sysMemIsTracked((const void *)addr, (u32)len)) {
        /* Room graphics data: bgLoadRoom allocates it with plain sysMemAlloc
         * on the port (src/game/bg.c:2839), so it is in none of the spans
         * above and the allocation registry is what identifies it. */
        ++g_cap.mallocBlocks;
        g_cap.mallocBytes += len;
    } else {
        /* Reached by fast3d, so mapped, but in memory nothing accounts for.
         * The translator cannot read this - it has no "fast3d is about to
         * read it" guarantee - so a nonzero count here is a real gap and
         * wants chasing, not tolerating. */
        ++g_cap.untrackedRefs;
        g_cap.untrackedBytes += len;
        if (g_cap.untrackedRefs <= 4) {
            sysLogPrintf(LOG_WARNING, "census: unaccounted ref %p (%u bytes)",
                         (void *)addr, (unsigned)len);
        }
    }

    auto it = g_cap.ranges.find(addr);
    if (it != g_cap.ranges.end() && it->second.size() >= len) {
        return;
    }
    std::vector<uint8_t> bytes(len);
    memcpy(bytes.data(), (const void *)addr, len);
    g_cap.ranges[addr] = std::move(bytes);
}

/*
 * Every opcode gfx_run_dl accepts (gfx_pc.cpp:2286-2534). fast3d calls
 * sysFatalError on anything else, so the hook never sees an unknown opcode;
 * this exists for verifyWalk, where an unknown opcode means the independent
 * walk has drifted and is reading data as commands.
 */
bool isKnownOpcode(uint8_t op)
{
    switch (op) {
    /* RSP */
    /* Opcode 0 is G_POPMTX here: the port's gbi.h defines G_SPNOOP, G_POPMTX
     * and G_CULLDL all as 0, and fast3d's switch treats 0 as G_POPMTX. Mirror
     * that rather than inventing a second meaning for the value. */
    case G_MTX: case G_MOVEMEM: case G_VTX: case G_DL:
    case G_COL: case (uint8_t)G_POPMTX: case (uint8_t)G_MOVEWORD:
    case (uint8_t)G_TEXTURE: case (uint8_t)G_SETOTHERMODE_H:
    case (uint8_t)G_SETOTHERMODE_L: case (uint8_t)G_ENDDL:
    case (uint8_t)G_SETGEOMETRYMODE: case (uint8_t)G_CLEARGEOMETRYMODE:
    case (uint8_t)G_TRI1: case (uint8_t)G_TRI4:
    case (uint8_t)G_RDPHALF_1: case (uint8_t)G_RDPHALF_2:
    case (uint8_t)G_RDPHALF_CONT: case G_NOOP:
    /* RDP */
    case G_SETTIMG: case G_SETCIMG: case G_SETZIMG: case G_SETCOMBINE:
    case G_SETTILE: case G_SETTILESIZE: case G_LOADTILE: case G_LOADBLOCK:
    case G_LOADTLUT: case G_SETENVCOLOR: case G_SETPRIMCOLOR:
    case G_SETFOGCOLOR: case G_SETFILLCOLOR: case G_SETSCISSOR:
    case G_RDPSETOTHERMODE: case G_TEXRECT: case G_TEXRECTFLIP:
    case G_FILLRECT: case G_RDPLOADSYNC: case G_RDPPIPESYNC:
    case G_RDPTILESYNC: case G_RDPFULLSYNC:
    /* port-private (src/include/gbiex.h:187-199) */
    case G_SETFB_EXT: case G_SETTIMG_FB_EXT: case G_INVALTEXCACHE_EXT:
    case G_TEXRECT_WIDE_EXT: case G_FILLRECT_WIDE_EXT: case G_SETGRAYSCALE_EXT:
    case G_EXTRAGEOMETRYMODE_EXT: case G_SETINTENSITY_EXT: case G_COPYFB_EXT:
    case G_IMAGERECT_EXT: case G_RDPFLUSH_EXT: case G_CLEAR_DEPTH_EXT:
    case G_SETSUBPIXELOFFSET_EXT:
        return true;
    default:
        return false;
    }
}

/*
 * Interpreter state gfxStep needs: the pending G_SETTIMG that sizes the next
 * texture load. Segments live here too but are overwritten from fast3d's live
 * table at every command - see recordCommand.
 */
pdrt64::GfxWalkState g_walk;

/*
 * Records one command and the data it references. Called from fast3d for
 * every command it executes; control flow (G_DL, G_ENDDL) and segment
 * bindings (G_MOVEWORD) are the interpreter's business and need nothing here
 * beyond the command bytes themselves.
 *
 * What a command spans and what it references are gfxStep's to say
 * (rt64_debug.h), not this file's. The disassembler hashes exactly the ranges
 * recorded here, so if these two ever sized a block differently, a golden
 * replayed from a capture would read bytes the capture never stored and
 * report misses that mean nothing. One function, no second opinion.
 */
void recordCommand(const Gfx *cmd, const uintptr_t *segments)
{
    const uint8_t opcode = (uint8_t)(cmd->words.w0 >> 24);
    recordRange((uintptr_t)cmd, sizeof(Gfx) * pdrt64::gfxCommandWords(opcode));

    /* fast3d hands us the table it is about to resolve against, so the capture
     * cannot disagree with the renderer about where a reference points. That
     * is stronger than the bindings gfxStep would infer from the stream, so it
     * wins. */
    memcpy(g_walk.segments, segments, sizeof(g_walk.segments));

    const pdrt64::GfxRef ref = pdrt64::gfxStep(g_walk, cmd);
    if (ref.kind != pdrt64::RefKind::None && ref.kind != pdrt64::RefKind::DisplayList) {
        recordRange(ref.addr, (size_t)ref.startOffset + ref.bytes);
    }
}

void captureCommand(const Gfx *cmd, const uintptr_t *segments)
{
    if (!g_cap.frameOpen) {
        return;
    }
    ++g_cap.executedCount;
    g_cap.executed.insert((uintptr_t)cmd);
    recordCommand(cmd, segments);
}

/*
 * Independent re-walk of the frame, structured like gfx_run_dl, starting from
 * the segment table as it stood at submission. Records nothing; it only
 * checks that every command it reaches is one fast3d executed. The first
 * miss is logged with the chain of G_DL commands that led there, which is
 * the information needed to see *why* the two disagree.
 */
struct WalkLink {
    const Gfx *dlCmd;   /* the G_DL that entered this list, null for the root */
    uintptr_t target;   /* where it resolved to */
};

struct VerifyState {
    uintptr_t segments[16];
    uint32_t visited = 0;
    bool diverged = false;
    std::vector<WalkLink> chain;
};

/* `cmd` is only dereferenced if fast3d executed it; otherwise we have no
 * proof the address is mapped, which is the very thing being reported. */
void verifyReport(VerifyState &vs, const Gfx *cmd, const char *why)
{
    vs.diverged = true;
    if (g_cap.executed.count((uintptr_t)cmd)) {
        sysLogPrintf(LOG_WARNING, "verify: %s at %p (op %02x w0 %08llx w1 %016llx) after %u commands",
                     why, (const void *)cmd, (unsigned)(cmd->words.w0 >> 24),
                     (unsigned long long)cmd->words.w0, (unsigned long long)cmd->words.w1,
                     vs.visited);
    } else {
        sysLogPrintf(LOG_WARNING, "verify: %s at %p after %u commands",
                     why, (const void *)cmd, vs.visited);
    }
    for (size_t i = vs.chain.size(); i-- > 0;) {
        const WalkLink &l = vs.chain[i];
        if (!l.dlCmd) {
            sysLogPrintf(LOG_WARNING, "verify:   root %p", (const void *)l.target);
            continue;
        }
        const uintptr_t w1 = l.dlCmd->words.w1;
        const unsigned seg = (w1 & 1) ? (unsigned)((w1 & 0x0f000000) >> 24) : 0;
        sysLogPrintf(LOG_WARNING, "verify:   via G_DL at %p w1 %016llx -> %p (seg %u base %016llx, %s)",
                     (const void *)l.dlCmd, (unsigned long long)w1, (const void *)l.target,
                     seg, (unsigned long long)(seg ? vs.segments[seg] : 0),
                     C0(l.dlCmd, 16, 1) ? "branch" : "push");
    }
    sysLogPrintf(LOG_WARNING, "verify:   segments at failure:");
    for (unsigned s = 1; s < 16; ++s) {
        if (vs.segments[s]) {
            sysLogPrintf(LOG_WARNING, "verify:     [%2u] %016llx", s, (unsigned long long)vs.segments[s]);
        }
    }
}

void verifyWalkDl(VerifyState &vs, const Gfx *cmd, int depth)
{
    if (!cmd || vs.diverged) {
        return;
    }
    if (depth > 32) {
        verifyReport(vs, cmd, "nesting deeper than 32");
        return;
    }

    for (;;) {
        if (vs.diverged) {
            return;
        }
        /* Membership in the executed set is the only gate, and it is also
         * the safety check: nothing is dereferenced until fast3d has. Room
         * data is malloc'd and outside every modelled region, so a region
         * test here would reject correct walks. */
        if (!g_cap.executed.count((uintptr_t)cmd)) {
            verifyReport(vs, cmd, "command fast3d did not execute");
            return;
        }
        const uint8_t opcode = (uint8_t)(cmd->words.w0 >> 24);
        if (!isKnownOpcode(opcode)) {
            verifyReport(vs, cmd, "unknown opcode");
            return;
        }
        ++vs.visited;

        switch (opcode) {
        case (uint8_t)G_MOVEWORD:
            if (C0(cmd, 0, 8) == G_MW_SEGMENT) {
                vs.segments[(C0(cmd, 8, 16) >> 2) & 0x0f] = (uintptr_t)cmd->words.w1;
            }
            break;

        case G_DL: {
            const Gfx *sub = (const Gfx *)segAddr(cmd->words.w1, vs.segments);
            if (!sub) {
                break;
            }
            vs.chain.push_back({cmd, (uintptr_t)sub});
            if (C0(cmd, 16, 1) == 0) {
                verifyWalkDl(vs, sub, depth + 1);
                vs.chain.pop_back();
                break;
            }
            /* Branch: this list continues at the target. Keep the link so the
             * report shows how we got here. */
            cmd = sub;
            continue;
        }

        case (uint8_t)G_ENDDL:
            return;

        default:
            break;
        }

        cmd += pdrt64::gfxCommandWords(opcode);
    }
}

void verifyWalk(void)
{
    VerifyState vs;
    memcpy(vs.segments, g_cap.seedSegments, sizeof(vs.segments));

    for (uintptr_t root : g_cap.roots) {
        vs.chain.clear();
        vs.chain.push_back({nullptr, root});
        verifyWalkDl(vs, (const Gfx *)root, 0);
        if (vs.diverged) {
            break;
        }
    }

    if (vs.diverged) {
        sysLogPrintf(LOG_WARNING, "verify: independent walk DIVERGED after %u of %u commands",
                     vs.visited, g_cap.executedCount);
    } else if (vs.visited != g_cap.executedCount) {
        sysLogPrintf(LOG_WARNING, "verify: independent walk visited %u commands, fast3d executed %u",
                     vs.visited, g_cap.executedCount);
    } else {
        sysLogPrintf(LOG_NOTE, "verify: independent walk matches fast3d (%u commands)", vs.visited);
    }
}

void writeU32(FILE *f, uint32_t v) { fwrite(&v, sizeof(v), 1, f); }
void writeU64(FILE *f, uint64_t v) { fwrite(&v, sizeof(v), 1, f); }

/* Minimal PNG writer (RGB8, one IDAT, filter type 0). zlib is already linked
 * by the port, so this needs no new dependency. */
void writePng(const std::string &path, const uint8_t *rgbBottomUp, int w, int h);

void flushFrame(void)
{
    if (!g_cap.frameOpen) {
        return;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s.%04d.pddl", g_cap.prefix.c_str(), g_cap.frameIndex);

    FILE *f = fopen(path, "wb");
    if (!f) {
        sysLogPrintf(LOG_ERROR, "capture: could not open %s", path);
    } else {
        fwrite("PDDL", 1, 4, f);
        writeU32(f, kPddlVersion);
        writeU32(f, (uint32_t)g_cap.roots.size());
        writeU32(f, (uint32_t)videoGetNativeWidth());
        writeU32(f, (uint32_t)videoGetNativeHeight());
        writeU64(f, (uint64_t)(uintptr_t)g_MempHeap);
        writeU64(f, (uint64_t)g_MempHeapSize);
        writeU64(f, (uint64_t)(uintptr_t)g_RomFile);
        writeU64(f, (uint64_t)g_RomFileSize);

        for (uintptr_t root : g_cap.roots) {
            writeU64(f, (uint64_t)root);
        }

        uint32_t rangeCount = 0;
        uint64_t rangeBytes = 0;
        for (const auto &kv : g_cap.ranges) {
            writeU64(f, (uint64_t)kv.first);
            writeU32(f, (uint32_t)kv.second.size());
            fwrite(kv.second.data(), 1, kv.second.size(), f);
            ++rangeCount;
            rangeBytes += kv.second.size();
        }
        writeU32(f, rangeCount);
        fclose(f);

        sysLogPrintf(LOG_NOTE, "capture: wrote %s (%u dls, %u ranges, %llu bytes)",
                     path, (unsigned)g_cap.roots.size(), rangeCount,
                     (unsigned long long)rangeBytes);
        sysLogPrintf(LOG_NOTE, "census: %u commands | heap %u/%lluB | rom %u/%lluB | image %u/%lluB "
                     "| malloc %u/%lluB | unaccounted %u/%lluB",
                     g_cap.executedCount,
                     g_cap.heapBlocks, (unsigned long long)g_cap.heapBytes,
                     g_cap.romBlocks, (unsigned long long)g_cap.romBytes,
                     g_cap.imageBlocks, (unsigned long long)g_cap.imageBytes,
                     g_cap.mallocBlocks, (unsigned long long)g_cap.mallocBytes,
                     g_cap.untrackedRefs, (unsigned long long)g_cap.untrackedBytes);
    }

    verifyWalk();

    if (!g_cap.goldenRgb.empty()) {
        char pngPath[512];
        snprintf(pngPath, sizeof(pngPath), "%s.%04d.png", g_cap.prefix.c_str(), g_cap.frameIndex);
        writePng(pngPath, g_cap.goldenRgb.data(), g_cap.goldenW, g_cap.goldenH);
    }

    g_cap.roots.clear();
    g_cap.ranges.clear();
    g_cap.executed.clear();
    g_cap.executedCount = 0;
    g_cap.heapBlocks = g_cap.romBlocks = g_cap.imageBlocks = 0;
    g_cap.mallocBlocks = g_cap.untrackedRefs = 0;
    g_cap.heapBytes = g_cap.romBytes = g_cap.imageBytes = 0;
    g_cap.mallocBytes = g_cap.untrackedBytes = 0;
    g_cap.goldenRgb.clear();
    g_cap.frameOpen = false;
    ++g_cap.frameIndex;
}

} // namespace

/* ---- PNG ---- */

#include <zlib.h>

namespace {

void pngChunk(FILE *f, const char *type, const uint8_t *data, size_t len)
{
    uint8_t lenBe[4] = { (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len };
    fwrite(lenBe, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (len) {
        fwrite(data, 1, len, f);
    }
    uLong c = crc32(0L, (const Bytef *)type, 4);
    if (len) {
        c = crc32(c, (const Bytef *)data, (uInt)len);
    }
    uint8_t crcBe[4] = { (uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
    fwrite(crcBe, 1, 4, f);
}

void writePng(const std::string &path, const uint8_t *rgbBottomUp, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }

    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        sysLogPrintf(LOG_ERROR, "capture: could not open %s", path.c_str());
        return;
    }

    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', '\r', '\n', 26, '\n' };
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16);
    ihdr[2] = (uint8_t)(w >> 8);  ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16);
    ihdr[6] = (uint8_t)(h >> 8);  ihdr[7] = (uint8_t)h;
    ihdr[8] = 8;    /* bit depth */
    ihdr[9] = 2;    /* colour type: truecolour */
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    pngChunk(f, "IHDR", ihdr, sizeof(ihdr));

    /* Raw scanlines, top-down, each prefixed with filter byte 0. */
    const size_t stride = (size_t)w * 3;
    std::vector<uint8_t> raw((stride + 1) * (size_t)h);
    for (int y = 0; y < h; ++y) {
        const uint8_t *src = rgbBottomUp + (size_t)(h - 1 - y) * stride;
        uint8_t *dst = raw.data() + (size_t)y * (stride + 1);
        dst[0] = 0;
        memcpy(dst + 1, src, stride);
    }

    uLongf compLen = compressBound((uLong)raw.size());
    std::vector<uint8_t> comp(compLen);
    if (compress2(comp.data(), &compLen, raw.data(), (uLong)raw.size(), Z_DEFAULT_COMPRESSION) == Z_OK) {
        pngChunk(f, "IDAT", comp.data(), compLen);
    }
    pngChunk(f, "IEND", nullptr, 0);
    fclose(f);
}

} // namespace

/* ---- C entry points ---- */

void (*pdCaptureCommandHook)(const Gfx *cmd, const uintptr_t *segments) = nullptr;

extern "C" void pdCaptureSetupHotkey(const char *pathPrefix, int frames)
{
    if (!pathPrefix || frames <= 0) {
        return;
    }
    g_cap.hotkeyEnabled = true;
    g_cap.hotkeyPrefix = pathPrefix;
    g_cap.hotkeyFrames = frames;
    initImageRegion();
    sysLogPrintf(LOG_NOTE, "capture: press F9 to capture %d frames to '%s'",
                 frames, pathPrefix);
}

extern "C" void pdCaptureArm(const char *pathPrefix, int frames)
{
    if (!pathPrefix || frames <= 0) {
        return;
    }
    initImageRegion();
    g_cap.quitWhenDone = sysArgCheck("--capture-quit") != 0;
    /* Levels spend their first frames on loading screens, which are not
     * representative. --capture-skip discards that many rendered frames. */
    g_cap.skipLeft = sysArgGetInt("--capture-skip", 0);
    g_cap.prefix = pathPrefix;
    g_cap.framesLeft = frames;
    g_cap.frameIndex = 0;
    g_cap.armed = true;
    pdCaptureCommandHook = captureCommand;
    sysLogPrintf(LOG_NOTE, "capture: armed for %d frames, prefix '%s'", frames, pathPrefix);
}

extern "C" int pdCaptureActive(void)
{
    return (g_cap.armed && g_cap.framesLeft > 0) ? 1 : 0;
}

extern "C" void pdCaptureOnRun(const Gfx *rootDl)
{
    if (!pdCaptureActive() || !rootDl) {
        return;
    }
    g_cap.sawDl = true;
    if (g_cap.skipLeft > 0) {
        return;
    }

    if (g_cap.roots.empty()) {
        /* The table fast3d will start this frame with. Its bindings persist
         * across frames (gfx_pc.cpp:2590 is the only reset), so this is the
         * correct initial state for an independent walk, not a stale one. */
        memcpy(g_cap.seedSegments, f3d_get_segment_table(), sizeof(g_cap.seedSegments));
    }
    g_cap.frameOpen = true;
    g_cap.roots.push_back((uintptr_t)rootDl);
    /* Recording happens in captureCommand as fast3d executes the list. */
}

extern "C" void pdCaptureGoldenImage(const unsigned char *pixels, int width, int height)
{
    if (!pdCaptureActive() || !pixels || width <= 0 || height <= 0) {
        return;
    }
    const size_t len = (size_t)width * (size_t)height * 3;
    g_cap.goldenRgb.assign(pixels, pixels + len);
    g_cap.goldenW = width;
    g_cap.goldenH = height;
}

/* Edge-triggered so holding the key does not re-arm every frame. */
static void pdCapturePollHotkey(void)
{
    if (!g_cap.hotkeyEnabled || pdCaptureActive()) {
        return;
    }

    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    if (!keys) {
        return;
    }

    const bool down = keys[SDL_SCANCODE_F9] != 0;
    if (down && !g_cap.hotkeyWasDown) {
        pdCaptureArm(g_cap.hotkeyPrefix.c_str(), g_cap.hotkeyFrames);
    }
    g_cap.hotkeyWasDown = down;
}

extern "C" void pdCaptureEndFrame(void)
{
    pdCapturePollHotkey();

    if (!pdCaptureActive()) {
        return;
    }

    /* Only frames that actually carried a display list count against the
     * budget. The game submits none during loading, and letting those consume
     * frames silently produced short captures. */
    if (!g_cap.sawDl) {
        return;
    }
    g_cap.sawDl = false;

    if (g_cap.skipLeft > 0) {
        --g_cap.skipLeft;
        return;
    }

    if (!g_cap.frameOpen) {
        return;
    }

    if (g_cap.executedCount == 0) {
        /* fast3d dropped the frame for pacing (f3d_run returns before
         * gfx_run_dl when start_frame fails), so nothing was executed and
         * there is nothing truthful to write. Try again next frame. */
        g_cap.roots.clear();
        g_cap.frameOpen = false;
        return;
    }

    flushFrame();
    if (--g_cap.framesLeft <= 0) {
        g_cap.armed = false;
        pdCaptureCommandHook = nullptr;
        sysLogPrintf(LOG_NOTE, "capture: finished, %d frames written", g_cap.frameIndex);
        if (g_cap.quitWhenDone) {
            /* Clean exit so stdio buffers flush and the run length is
             * deterministic; verification scripts rely on both. */
            fflush(NULL);
            exit(0);
        }
    }
}
