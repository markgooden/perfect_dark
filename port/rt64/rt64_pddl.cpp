/*
 * .pddl capture reader. Format and rationale are in rt64_pddl.h.
 *
 * Every bound is checked before it is used. These files come off disk, a
 * truncated or hostile one must fail as a parse error rather than a crash, and
 * the ranges drive memory reads afterwards - a bad length here would become a
 * bad memcpy later.
 */

#include "rt64_pddl.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace pdrt64 {

namespace {

/* Sequential little-endian cursor over the file bytes. Every read is bounds
 * checked and sets `ok` false on overrun rather than reading past the end. */
class Cursor {
public:
    Cursor(const uint8_t *data, size_t len) : p_(data), end_(data + len) {}

    bool ok() const { return ok_; }
    size_t remaining() const { return static_cast<size_t>(end_ - p_); }

    bool take(void *dst, size_t n)
    {
        if (!ok_ || remaining() < n) {
            ok_ = false;
            return false;
        }
        memcpy(dst, p_, n);
        p_ += n;
        return true;
    }

    uint32_t u32()
    {
        uint32_t v = 0;
        take(&v, sizeof(v));
        return v;
    }

    uint64_t u64()
    {
        uint64_t v = 0;
        take(&v, sizeof(v));
        return v;
    }

    /* Borrows n bytes in place, without copying. */
    const uint8_t *borrow(size_t n)
    {
        if (!ok_ || remaining() < n) {
            ok_ = false;
            return nullptr;
        }
        const uint8_t *at = p_;
        p_ += n;
        return at;
    }

private:
    const uint8_t *p_;
    const uint8_t *end_;
    bool ok_ = true;
};

bool fail(std::string *error, const std::string &what)
{
    if (error) {
        *error = what;
    }
    return false;
}

} // namespace

bool PddlFile::loadFile(const std::string &path, std::string *error)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        return fail(error, "cannot open " + path);
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return fail(error, "cannot seek " + path);
    }
    const long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return fail(error, "cannot size " + path);
    }
    rewind(f);

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    const size_t got = bytes.empty() ? 0 : fread(bytes.data(), 1, bytes.size(), f);
    fclose(f);

    if (got != bytes.size()) {
        return fail(error, "short read on " + path);
    }
    if (!loadBytes(bytes.data(), bytes.size(), error)) {
        if (error) {
            *error = path + ": " + *error;
        }
        return false;
    }
    return true;
}

bool PddlFile::loadBytes(const uint8_t *data, size_t len, std::string *error)
{
    roots_.clear();
    ranges_.clear();

    Cursor c(data, len);

    char magic[4] = {};
    if (!c.take(magic, sizeof(magic)) || memcmp(magic, "PDDL", 4) != 0) {
        return fail(error, "not a .pddl file (bad magic)");
    }

    version_ = c.u32();
    if (version_ == 0 || version_ > kPddlVersion) {
        char buf[96];
        snprintf(buf, sizeof(buf), "unsupported format version %u (reader knows up to %u)",
                 version_, kPddlVersion);
        return fail(error, buf);
    }

    const uint32_t dlCount = c.u32();
    nativeW_ = c.u32();
    nativeH_ = c.u32();
    heapBase_ = static_cast<uintptr_t>(c.u64());
    heapSize_ = static_cast<size_t>(c.u64());

    if (version_ >= 2) {
        romBase_ = static_cast<uintptr_t>(c.u64());
        romSize_ = static_cast<size_t>(c.u64());
    } else {
        romBase_ = 0;
        romSize_ = 0;
    }

    if (!c.ok()) {
        return fail(error, "truncated header");
    }

    /* Each root is 8 bytes and the trailer is 4, so a count that cannot fit is
     * a corrupt file, not a huge one - check before reserving. */
    if (static_cast<size_t>(dlCount) * 8u > c.remaining()) {
        return fail(error, "display-list count exceeds file size");
    }
    roots_.reserve(dlCount);
    for (uint32_t i = 0; i < dlCount; ++i) {
        roots_.push_back(static_cast<uintptr_t>(c.u64()));
    }
    if (!c.ok()) {
        return fail(error, "truncated display-list table");
    }

    /* Ranges stream until only the 4-byte trailer is left. */
    constexpr size_t kTrailerBytes = 4;
    while (c.ok() && c.remaining() > kTrailerBytes) {
        Range r;
        r.addr = static_cast<uintptr_t>(c.u64());
        const uint32_t rangeLen = c.u32();
        if (!c.ok()) {
            return fail(error, "truncated range header");
        }
        if (rangeLen > c.remaining() - std::min<size_t>(kTrailerBytes, c.remaining())) {
            return fail(error, "range length exceeds remaining file");
        }
        const uint8_t *at = c.borrow(rangeLen);
        if (!at) {
            return fail(error, "truncated range body");
        }
        r.bytes.assign(at, at + rangeLen);
        ranges_.push_back(std::move(r));
    }

    const uint32_t trailerCount = c.u32();
    if (!c.ok()) {
        return fail(error, "missing trailer");
    }
    if (trailerCount != ranges_.size()) {
        char buf[96];
        snprintf(buf, sizeof(buf), "trailer says %u ranges, read %zu",
                 trailerCount, ranges_.size());
        return fail(error, buf);
    }

    fileRangeCount_ = static_cast<uint32_t>(ranges_.size());

    /* The writer emits one range per reference, keyed by address, and does not
     * merge them - so ranges arrive sorted but can touch or overlap. Overlaps
     * are normal, not corruption: two G_MOVEMEM commands eight bytes apart
     * each pull a 16-byte window of the same light/colour array, and texture
     * blocks whose sizes are derived from their load commands can abut by a
     * byte or two. Measured on a menu capture: 3836 ranges, 5 overlapping
     * pairs, zero disagreeing bytes.
     *
     * Coalesce them into maximal blocks so a lookup is one containment test
     * with no ambiguity about which copy of a shared byte wins. Bytes that
     * disagree where two ranges overlap are a different matter - the capture
     * recorded one address twice with different contents, which no correct
     * writer does - so that is a parse error. */
    std::sort(ranges_.begin(), ranges_.end(),
              [](const Range &a, const Range &b) { return a.addr < b.addr; });

    std::vector<Range> merged;
    merged.reserve(ranges_.size());
    for (Range &r : ranges_) {
        if (!merged.empty()) {
            Range &into = merged.back();
            const uintptr_t intoEnd = into.addr + into.bytes.size();
            if (r.addr <= intoEnd) {
                const size_t offset = r.addr - into.addr;
                const size_t shared = intoEnd - r.addr < r.bytes.size()
                                          ? static_cast<size_t>(intoEnd - r.addr)
                                          : r.bytes.size();
                if (shared &&
                    memcmp(into.bytes.data() + offset, r.bytes.data(), shared) != 0) {
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                             "ranges at 0x%llx and 0x%llx overlap with differing bytes",
                             static_cast<unsigned long long>(into.addr),
                             static_cast<unsigned long long>(r.addr));
                    return fail(error, buf);
                }
                if (offset + r.bytes.size() > into.bytes.size()) {
                    into.bytes.insert(into.bytes.end(),
                                      r.bytes.begin() + static_cast<ptrdiff_t>(shared),
                                      r.bytes.end());
                }
                continue;
            }
        }
        merged.push_back(std::move(r));
    }
    ranges_ = std::move(merged);

    return true;
}

size_t PddlFile::totalBytes() const
{
    size_t n = 0;
    for (const Range &r : ranges_) {
        n += r.bytes.size();
    }
    return n;
}

CaptureMemReader::CaptureMemReader(const PddlFile &file) : file_(file)
{
}

bool CaptureMemReader::read(uintptr_t src, void *dst, size_t len)
{
    if (!src || !len) {
        ++misses_;
        return false;
    }

    const std::vector<PddlFile::Range> &ranges = file_.ranges();

    /* First range starting after src; the one that can contain src is the one
     * before it. */
    auto it = std::upper_bound(ranges.begin(), ranges.end(), src,
                               [](uintptr_t a, const PddlFile::Range &r) { return a < r.addr; });
    if (it == ranges.begin()) {
        ++misses_;
        return false;
    }
    --it;

    if (src < it->addr || src >= it->addr + it->bytes.size()) {
        ++misses_;
        return false;
    }

    /* Walk forward across contiguous ranges. The capture records a block per
     * reference and does not merge neighbours, so a read the translator sizes
     * differently from the recorder can legitimately span two of them. Only a
     * genuine hole is a miss. */
    uint8_t *out = static_cast<uint8_t *>(dst);
    uintptr_t want = src;
    size_t left = len;

    while (left) {
        if (it == ranges.end() || want < it->addr ||
            want >= it->addr + it->bytes.size()) {
            ++misses_;
            return false;
        }
        const size_t offset = want - it->addr;
        const size_t avail = it->bytes.size() - offset;
        const size_t take = left < avail ? left : avail;
        memcpy(out, it->bytes.data() + offset, take);
        out += take;
        want += take;
        left -= take;
        ++it;
    }

    return true;
}

Region CaptureMemReader::regionOf(uintptr_t src, size_t len) const
{
    if (!src || !len) {
        return Region::None;
    }

    auto within = [&](uintptr_t base, size_t size) {
        return base && size && src >= base && len <= size && (src - base) <= (size - len);
    };

    if (within(file_.heapBase(), file_.heapSize())) {
        return Region::Heap;
    }
    if (within(file_.romBase(), file_.romSize())) {
        return Region::Rom;
    }

    /* Anything else recorded is readable but the format cannot say which
     * non-heap region it came from (see rt64_pddl.h). Report Malloc for
     * recorded bytes and None for bytes the capture never saw, so the
     * distinction that matters - can this be read at all - stays exact. */
    const std::vector<PddlFile::Range> &ranges = file_.ranges();
    auto it = std::upper_bound(ranges.begin(), ranges.end(), src,
                               [](uintptr_t a, const PddlFile::Range &r) { return a < r.addr; });
    if (it == ranges.begin()) {
        return Region::None;
    }
    --it;
    if (src >= it->addr && src < it->addr + it->bytes.size()) {
        return Region::Malloc;
    }
    return Region::None;
}

} // namespace pdrt64
