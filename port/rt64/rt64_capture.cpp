/*
 * Display-list capture.
 *
 * Walks each submitted display list exactly as gfx_run_dl does
 * (port/fast3d/gfx_pc.cpp:2286-2534) and records:
 *
 *   - every Gfx command word of the stream, including nested lists
 *   - the contents of every block the stream references: vertices, colour
 *     tables, matrices, movemem blocks, texture data and TLUTs
 *
 * Blocks are stored at their ORIGINAL host addresses so a replay can resolve
 * a display-list pointer by lookup. Sizes for texture data are derived the
 * same way fast3d derives them (gfx_dp_load_block at gfx_pc.cpp:1870,
 * gfx_dp_load_tile at :1897, gfx_dp_load_tlut at :1833), because the size of
 * a G_SETTIMG block is only knowable from the load command that follows it.
 *
 * File format (.pddl, little-endian, matching docs/interfaces/rt64_debug.h):
 *
 *   header  { char magic[4] = "PDDL"; u32 version = 2; u32 dlCount;
 *             u32 nativeW; u32 nativeH; u64 heapBase; u64 heapSize;
 *             u64 romBase; u64 romSize; }
 *   dlCount x { u64 rootPtr }
 *   rangeCount x { u64 addr; u32 len; u8 bytes[len] }
 *   trailer { u32 rangeCount }
 *
 * The trailer carries the range count because ranges are streamed as they are
 * discovered and the total is not known when the header is written.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <PR/gbi.h>
#include <PR/ultratypes.h>

#include "rt64_capture.h"

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

extern u8 *g_MempHeap;
extern u32 g_MempHeapSize;
extern u8 *g_RomFile;
extern u32 g_RomFileSize;

namespace {

constexpr uint32_t kPddlVersion = 2;

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
    int framesLeft = 0;
    int frameIndex = 0;
    std::string prefix;

    /* Per-frame accumulation. */
    std::vector<uintptr_t> roots;
    std::map<uintptr_t, std::vector<uint8_t>> ranges;
    bool frameOpen = false;

    /* Mirrors rdp.texture_to_load so load commands can size the block. */
    uintptr_t texAddr = 0;
    uint32_t texSiz = 0;
    uint32_t texWidth = 0;

    /* Golden image for this frame, if the backend supplied one. */
    std::vector<uint8_t> goldenRgb;
    int goldenW = 0;
    int goldenH = 0;
};

CaptureState g_cap;

/* Segment table mirror. The port flags segmented addresses with the low bit
 * set (gfx_pc.cpp:2262-2273); anything else is a raw host pointer. */
uintptr_t g_segments[16];

void *segAddr(uintptr_t w1)
{
    if (w1 & 1) {
        const uintptr_t seg = (w1 & 0x0f000000) >> 24;
        if (seg && g_segments[seg]) {
            return (void *)(g_segments[seg] + (w1 & 0x00fffffe));
        }
    }
    return (void *)w1;
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

/* The memp heap is not the only region display lists point into: the menu
 * references the loaded ROM image directly (12 commands per frame at
 * STAGE_CITRAINING). Both regions must be captured, and the translator will
 * have to marshal from both. */
bool inCapturableMemory(uintptr_t addr, size_t len)
{
    return inHeap(addr, len) || inRegion(addr, len, g_RomFile, g_RomFileSize);
}

/* Records [addr, addr+len). Refuses anything outside the memp heap: the
 * capture must never dereference a pointer we cannot prove is mapped, and
 * out-of-heap references are exactly what task T3 exists to census. */
void recordRange(uintptr_t addr, size_t len)
{
    if (!len || !inCapturableMemory(addr, len)) {
        return;
    }
    auto it = g_cap.ranges.find(addr);
    if (it != g_cap.ranges.end() && it->second.size() >= len) {
        return;
    }
    std::vector<uint8_t> bytes(len);
    memcpy(bytes.data(), (const void *)addr, len);
    g_cap.ranges[addr] = std::move(bytes);
}

/* Number of Gfx words each opcode consumes, mirroring gfx_run_dl's ++cmd
 * behaviour for the multi-word commands. */
uint32_t commandWords(uint8_t opcode)
{
    switch (opcode) {
    case G_TEXRECT:
    case G_TEXRECTFLIP:
    case G_TEXRECT_WIDE_EXT:
    case G_IMAGERECT_EXT:
        return 3;
    case G_FILLRECT_WIDE_EXT:
        return 2;
    default:
        return 1;
    }
}

void walkDl(const Gfx *cmd, int depth);

void recordTextureLoad(uint32_t sizeBytes, uint32_t startOffset)
{
    if (g_cap.texAddr) {
        recordRange(g_cap.texAddr, (size_t)startOffset + sizeBytes);
    }
}

void walkCommand(const Gfx *cmd, int depth, bool *stop, const Gfx **jumpTo)
{
    const uint8_t opcode = (uint8_t)(cmd->words.w0 >> 24);

    switch (opcode) {
    case G_MTX:
        recordRange((uintptr_t)segAddr(cmd->words.w1), sizeof(Mtx));
        break;

    case G_MOVEMEM:
        /* Lights, viewport and lookat blocks. Sizes vary by index and are not
         * worth decoding here; one cache line covers every PD use. */
        recordRange((uintptr_t)segAddr(cmd->words.w1), 64);
        break;

    case G_VTX: {
        const uint32_t bytes = C0(cmd, 0, 16);
        recordRange((uintptr_t)segAddr(cmd->words.w1), bytes);
        break;
    }

    case G_COL: {
        /* gSPColor packs sizeof(Col)*n into w0[0:16] (gbiex.h:15-16). */
        const uint32_t bytes = C0(cmd, 0, 16);
        recordRange((uintptr_t)segAddr(cmd->words.w1), bytes);
        break;
    }

    case G_MOVEWORD:
        /* Track segment assignments so later segmented addresses resolve. */
        if (C0(cmd, 0, 8) == G_MW_SEGMENT) {
            const uint32_t index = (C0(cmd, 8, 16) >> 2) & 0x0f;
            g_segments[index] = (uintptr_t)cmd->words.w1;
        }
        break;

    case G_SETTIMG:
        g_cap.texSiz = C0(cmd, 19, 2);
        g_cap.texWidth = C0(cmd, 0, 10);
        g_cap.texAddr = (uintptr_t)segAddr(cmd->words.w1);
        break;

    case G_LOADBLOCK: {
        /* gfx_pc.cpp:1876 - lrs is a texel count. */
        const uint32_t lrs = C1(cmd, 12, 12);
        recordTextureLoad(((lrs + 1) << g_cap.texSiz) >> 1, 0);
        break;
    }

    case G_LOADTILE: {
        /* gfx_pc.cpp:1900-1912 */
        const uint32_t uls = C0(cmd, 12, 12), ult = C0(cmd, 0, 12);
        const uint32_t lrs = C1(cmd, 12, 12), lrt = C1(cmd, 0, 12);
        const uint32_t offsetX = uls >> G_TEXTURE_IMAGE_FRAC;
        const uint32_t offsetY = ult >> G_TEXTURE_IMAGE_FRAC;
        const uint32_t tileW = ((lrs - uls) >> G_TEXTURE_IMAGE_FRAC) + 1;
        const uint32_t tileH = ((lrt - ult) >> G_TEXTURE_IMAGE_FRAC) + 1;
        const uint32_t fullW = g_cap.texWidth + 1;
        const uint32_t offsetXBytes = (offsetX << g_cap.texSiz) >> 1;
        const uint32_t tileLine = (tileW << g_cap.texSiz) >> 1;
        const uint32_t fullLine = (fullW << g_cap.texSiz) >> 1;
        recordTextureLoad(tileLine * tileH, fullLine * offsetY + offsetXBytes);
        break;
    }

    case G_LOADTLUT: {
        /* gfx_pc.cpp:1843-1847; entries are 16-bit. */
        const uint32_t uls = C0(cmd, 14, 10), ult = C0(cmd, 2, 10);
        const uint32_t lrs = C1(cmd, 14, 10), lrt = C1(cmd, 2, 10);
        const uint32_t width = lrs - uls + 1;
        const uint32_t height = lrt - ult + 1;
        const uint32_t pitch = g_cap.texWidth + 1;
        const uint32_t entries = pitch * ult + uls + width * height;
        recordTextureLoad(entries * 2, 0);
        break;
    }

    case G_DL: {
        const Gfx *sub = (const Gfx *)segAddr(cmd->words.w1);
        if (!sub) {
            break;
        }
        if (C0(cmd, 16, 1) == 0) {
            walkDl(sub, depth + 1);
        } else {
            *jumpTo = sub;
        }
        break;
    }

    case (uint8_t)G_ENDDL:
        *stop = true;
        break;

    default:
        break;
    }
}

void walkDl(const Gfx *cmd, int depth)
{
    if (!cmd || depth > 32) {
        return;
    }

    for (;;) {
        if (!inCapturableMemory((uintptr_t)cmd, sizeof(Gfx))) {
            /* Display lists in static data or elsewhere outside the heap are
             * recorded as a reference only; T3 censuses how often this
             * happens. Stop walking rather than risk a bad dereference. */
            return;
        }

        const uint8_t opcode = (uint8_t)(cmd->words.w0 >> 24);
        const uint32_t words = commandWords(opcode);
        recordRange((uintptr_t)cmd, sizeof(Gfx) * words);

        bool stop = false;
        const Gfx *jumpTo = nullptr;
        walkCommand(cmd, depth, &stop, &jumpTo);

        if (stop) {
            return;
        }
        if (jumpTo) {
            cmd = jumpTo;
            continue;
        }
        cmd += words;
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
    }

    if (!g_cap.goldenRgb.empty()) {
        char pngPath[512];
        snprintf(pngPath, sizeof(pngPath), "%s.%04d.png", g_cap.prefix.c_str(), g_cap.frameIndex);
        writePng(pngPath, g_cap.goldenRgb.data(), g_cap.goldenW, g_cap.goldenH);
    }

    g_cap.roots.clear();
    g_cap.ranges.clear();
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

extern "C" void pdCaptureArm(const char *pathPrefix, int frames)
{
    if (!pathPrefix || frames <= 0) {
        return;
    }
    g_cap.quitWhenDone = sysArgCheck("--capture-quit") != 0;
    g_cap.prefix = pathPrefix;
    g_cap.framesLeft = frames;
    g_cap.frameIndex = 0;
    g_cap.armed = true;
    memset(g_segments, 0, sizeof(g_segments));
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
    g_cap.frameOpen = true;
    g_cap.roots.push_back((uintptr_t)rootDl);
    walkDl(rootDl, 0);
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

extern "C" void pdCaptureEndFrame(void)
{
    if (!pdCaptureActive()) {
        return;
    }

    /* Only frames that actually carried a display list count against the
     * budget. The game submits none during loading, and letting those consume
     * frames silently produced short captures. */
    if (!g_cap.frameOpen) {
        return;
    }

    flushFrame();
    if (--g_cap.framesLeft <= 0) {
        g_cap.armed = false;
        sysLogPrintf(LOG_NOTE, "capture: finished, %d frames written", g_cap.frameIndex);
        if (g_cap.quitWhenDone) {
            /* Clean exit so stdio buffers flush and the run length is
             * deterministic; verification scripts rely on both. */
            fflush(NULL);
            exit(0);
        }
    }
}
