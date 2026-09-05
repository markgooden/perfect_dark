#pragma once

/*
 * Reader for .pddl display-list captures, and the MemReader that serves a
 * capture's recorded memory to the translator.
 *
 * This is the offline half of the capture: rt64_capture.cpp writes these files
 * from inside the running game, and everything that consumes them - the unit
 * tests, tools/dlreplay - reads them through here. It is kept beside the
 * writer so the two halves of the format sit in one directory and share
 * kPddlVersion rather than drifting apart.
 *
 * The port's CMakeLists globs port/*.cpp (CMakeLists.txt:261), so this does
 * get compiled into the game even though the game never reads a capture back.
 * That is harmless - nothing calls it, and the linker drops it - but do not
 * write code here that assumes otherwise.
 *
 * Like rt64_mem.cpp, this file depends on nothing but the standard library, so
 * the tests compile it directly with no port, RT64 or SDL.
 *
 * === File format (little-endian) ===
 *
 *   header  { char magic[4] = "PDDL"; u32 version; u32 dlCount;
 *             u32 nativeW; u32 nativeH; u64 heapBase; u64 heapSize;
 *             u64 romBase; u64 romSize; }
 *   dlCount x { u64 rootPtr }
 *   rangeCount x { u64 addr; u32 len; u8 bytes[len] }
 *   trailer { u32 rangeCount }
 *
 * Ranges stream until the 4-byte trailer, which carries the count because it
 * is not known when the header is written. Blocks are stored at their ORIGINAL
 * host addresses, so a display-list pointer resolves by lookup.
 *
 * Version 1 had no rom window; version 2 added romBase/romSize. Both parse.
 *
 * KNOWN LIMITATION: the header carries heap and ROM windows only, so a capture
 * cannot say whether a non-heap block came from the executable image (linker
 * statics, immutable) or a sysMemAlloc block (room data, stable but
 * append-only). Both classify as Region::Malloc here. Nothing currently
 * depends on telling them apart - the arena's caching policy keys off data
 * type, not region - but recording a per-range region tag is the fix if
 * something ever does, and that is a format bump.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rt64_mem.h"

namespace pdrt64 {

/* Current writer version. rt64_capture.cpp writes this; the reader accepts
 * this and older. Bumping it is a deliberate format change - see the header
 * comment above and CLAUDE.md's stop-and-ask rule. */
constexpr uint32_t kPddlVersion = 2;

/*
 * One parsed capture file. Owns its bytes.
 *
 * Parsing is total: every failure returns false with a reason in `error`
 * rather than throwing or aborting, because these files come off disk and a
 * truncated one must not take the test runner or the replay tool down.
 */
class PddlFile {
public:
    /* Recorded block of source memory, at its original host address. */
    struct Range {
        uintptr_t addr;
        std::vector<uint8_t> bytes;
    };

    bool loadFile(const std::string &path, std::string *error = nullptr);
    bool loadBytes(const uint8_t *data, size_t len, std::string *error = nullptr);

    uint32_t version() const { return version_; }
    uint32_t nativeWidth() const { return nativeW_; }
    uint32_t nativeHeight() const { return nativeH_; }

    uintptr_t heapBase() const { return heapBase_; }
    size_t heapSize() const { return heapSize_; }
    uintptr_t romBase() const { return romBase_; }
    size_t romSize() const { return romSize_; }

    /* Root display lists, in submission order - what gfx_run was handed. */
    const std::vector<uintptr_t> &roots() const { return roots_; }

    /* Recorded memory as maximal blocks: sorted by address, never touching,
     * never overlapping. The file's ranges are coalesced on load (the writer
     * records one per reference and neighbours can abut or overlap), so this
     * is usually a shorter list than the file holds. */
    const std::vector<Range> &ranges() const { return ranges_; }

    /* Ranges as the file stored them, before coalescing - the number the
     * capture's own log line reports. */
    uint32_t fileRangeCount() const { return fileRangeCount_; }

    /* Total recorded bytes, for stats and sanity checks. */
    size_t totalBytes() const;

private:
    uint32_t version_ = 0;
    uint32_t nativeW_ = 0;
    uint32_t nativeH_ = 0;
    uintptr_t heapBase_ = 0;
    size_t heapSize_ = 0;
    uintptr_t romBase_ = 0;
    size_t romSize_ = 0;
    uint32_t fileRangeCount_ = 0;
    std::vector<uintptr_t> roots_;
    std::vector<Range> ranges_;
};

/*
 * Serves a capture's recorded memory as if it were the live game's.
 *
 * The translator does all source-memory access through MemReader, so running
 * it against one of these makes translation a pure function of (display list,
 * capture) - replayable, diffable, and testable with no game running.
 *
 * A read the capture does not cover returns false, which the translator
 * reports as TranslateStatus::CaptureMiss. That is a real signal, not noise:
 * it means the translator wants bytes the renderer never touched, so either
 * the capture is incomplete or the translator is walking somewhere fast3d did
 * not.
 */
class CaptureMemReader final : public MemReader {
public:
    explicit CaptureMemReader(const PddlFile &file);

    bool read(uintptr_t src, void *dst, size_t len) override;
    Region regionOf(uintptr_t src, size_t len) const override;

    /* Reads that failed, i.e. capture misses. Zero is the expected value for
     * a translation that stayed inside what the renderer touched. */
    uint32_t missCount() const { return misses_; }

private:
    const PddlFile &file_;
    uint32_t misses_ = 0;
};

} // namespace pdrt64
