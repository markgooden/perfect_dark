/*
 * Synthetic RDRAM arena. Layout and contract are in rt64_arena.h.
 *
 * Sizing follows docs/census.md: a frame references tens of kilobytes, not
 * megabytes, so there is no large immutable tier to cache and the per-block
 * cache is small by design. The cost that matters is per-command work, so
 * every operation here is O(1) bump allocation plus one marshalling pass.
 *
 * Like rt64_mem.cpp this has no dependency on the port, RT64 or SDL, so the
 * unit tests compile it standalone.
 */

#include "rt64_arena.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pdrt64 {

namespace {

[[noreturn]] void arenaFatal(const char *what, size_t need, size_t have)
{
    fprintf(stderr,
            "RT64 arena: %s exhausted (needed %zu bytes, %zu available).\n"
            "Raise Video.RT64RdramMB, but read SCAFFOLD.md risk R1 first:\n"
            "a frame should reference tens of KB, so overflow means something\n"
            "upstream is wrong rather than the arena being too small.\n",
            what, need, have);
    abort();
}

constexpr size_t align4(size_t n)
{
    return (n + 3u) & ~static_cast<size_t>(3u);
}

} // namespace

Arena::Arena(const ArenaConfig &cfg)
{
    /* Every offset must fit in the low 31 bits of an extended address. */
    assert(cfg.totalBytes < (size_t(1) << 31) && "arena must stay under 2 GB");
    assert(cfg.fbRegionBytes + cfg.frameRegionBytes <= cfg.totalBytes);

    buffer_.assign(cfg.totalBytes, 0u);
    fbEnd_ = align4(cfg.fbRegionBytes);
    frameEnd_ = fbEnd_ + align4(cfg.frameRegionBytes);
    framePos_ = fbEnd_;
    cachePos_ = frameEnd_;
    fbPos_ = 0;
}

Arena::~Arena() = default;

uint8_t *Arena::rdramBase()
{
    return buffer_.data();
}

size_t Arena::rdramSize() const
{
    return buffer_.size();
}

void Arena::beginFrame()
{
    framePos_ = fbEnd_;
    stats_.frameBytesUsed = 0;
    stats_.cacheHits = 0;
    stats_.cacheMisses = 0;
    stats_.outOfHeapBlocks = 0;
}

RdramAddr Arena::allocFrame(size_t len)
{
    const size_t need = align4(len);
    if (framePos_ + need > frameEnd_) {
        arenaFatal("frame region", need, frameEnd_ - framePos_);
    }
    const size_t at = framePos_;
    framePos_ += need;
    stats_.frameBytesUsed += need;
    return static_cast<RdramAddr>(at) | kExtendedAddrBit;
}

RdramAddr Arena::allocFramebuffer(size_t len)
{
    const size_t need = align4(len);
    if (fbPos_ + need > fbEnd_) {
        arenaFatal("framebuffer region", need, fbEnd_ - fbPos_);
    }
    const size_t at = fbPos_;
    fbPos_ += need;
    return static_cast<RdramAddr>(at) | kExtendedAddrBit;
}

RdramAddr Arena::marshalInto(RdramAddr at, MemReader &mem, uintptr_t src,
                             size_t len, Swizzle type)
{
    /* Read into scratch first: marshalCopy is not safe in place, and the
     * source is game memory we must not assume is readable without asking. */
    if (scratch_.size() < len) {
        scratch_.resize(len);
    }
    if (!mem.read(src, scratch_.data(), len)) {
        /* An unreadable source is a translator bug or a capture miss, never
         * something to paper over: emitting stale arena bytes would render
         * plausible-looking garbage. */
        fprintf(stderr, "RT64 arena: unreadable source 0x%llx (%zu bytes)\n",
                static_cast<unsigned long long>(src), len);
        abort();
    }

    if (mem.regionOf(src, len) != Region::Heap) {
        ++stats_.outOfHeapBlocks;
    }

    marshalCopy(buffer_.data() + (at & ~kExtendedAddrBit), scratch_.data(), len, type);
    return at;
}

RdramAddr Arena::pushFrameData(MemReader &mem, uintptr_t src, size_t len, Swizzle type)
{
    return marshalInto(allocFrame(len), mem, src, len, type);
}

RdramAddr Arena::pushCachedData(MemReader &mem, uintptr_t src, size_t len, Swizzle type)
{
    const CacheKey key{src, len, type};
    const auto it = cache_.find(key);
    if (it != cache_.end()) {
        ++stats_.cacheHits;
        return it->second;
    }
    ++stats_.cacheMisses;

    const size_t need = align4(len);
    if (cachePos_ + need > buffer_.size()) {
        arenaFatal("cache region", need, buffer_.size() - cachePos_);
    }
    const RdramAddr at = static_cast<RdramAddr>(cachePos_) | kExtendedAddrBit;
    cachePos_ += need;
    stats_.cacheBytesUsed += need;

    cache_[key] = at;
    return marshalInto(at, mem, src, len, type);
}

void Arena::invalidateCache(uintptr_t addr)
{
    /* Drop every entry whose source range covers the address. The bytes stay
     * allocated - the cache region is a bump allocator, so partial
     * invalidation leaks until clearCache. At census-measured volumes (tens
     * of KB per frame) that is cheaper than maintaining a free list; revisit
     * if cacheBytesUsed ever approaches the region size. */
    for (auto it = cache_.begin(); it != cache_.end();) {
        const bool covers = addr >= it->first.src && addr < it->first.src + it->first.len;
        it = covers ? cache_.erase(it) : std::next(it);
    }
}

void Arena::invalidateCacheRange(uintptr_t start, uintptr_t end)
{
    for (auto it = cache_.begin(); it != cache_.end();) {
        const uintptr_t blockEnd = it->first.src + it->first.len;
        const bool overlaps = it->first.src < end && start < blockEnd;
        it = overlaps ? cache_.erase(it) : std::next(it);
    }
}

void Arena::clearCache()
{
    cache_.clear();
    cachePos_ = frameEnd_;
    stats_.cacheBytesUsed = 0;
}

const ArenaStats &Arena::stats() const
{
    return stats_;
}

/* ---- FbRegistry ---- */

namespace {

/* Native-resolution images are RGBA16. The port's native mode is dynamic
 * (pdsched.c:213 can switch to hi-res), so the fixed images are sized for the
 * largest mode rather than reallocated on a mode change. */
constexpr uint32_t kMaxNativeWidth = 640;
constexpr uint32_t kMaxNativeHeight = 480;

size_t imageBytes(uint32_t width, uint32_t height)
{
    return static_cast<size_t>(width) * static_cast<size_t>(height) * 2u;
}

} // namespace

FbRegistry::FbRegistry(Arena &arena) : arena_(arena)
{
    const size_t maxImage = imageBytes(kMaxNativeWidth, kMaxNativeHeight);
    mainColor_[0] = arena_.allocFramebuffer(maxImage);
    mainColor_[1] = arena_.allocFramebuffer(maxImage);
    depth_ = arena_.allocFramebuffer(maxImage);
    fbs_.push_back(Fb{0, 0, 0}); // handle 0 is "the current main image"
}

RdramAddr FbRegistry::mainColorImage(int index) const
{
    assert(index == 0 || index == 1);
    return mainColor_[index & 1];
}

RdramAddr FbRegistry::depthImage() const
{
    return depth_;
}

int FbRegistry::createFb(uint32_t width, uint32_t height)
{
    const RdramAddr addr = arena_.allocFramebuffer(imageBytes(width, height));
    fbs_.push_back(Fb{addr, width, height});
    return static_cast<int>(fbs_.size()) - 1;
}

void FbRegistry::resizeFb(int fb, uint32_t width, uint32_t height)
{
    if (fb <= 0 || static_cast<size_t>(fb) >= fbs_.size()) {
        return;
    }
    Fb &slot = fbs_[static_cast<size_t>(fb)];
    if (slot.width == width && slot.height == height) {
        return;
    }
    /* Reallocate rather than resize in place; the old bytes leak until the
     * arena is rebuilt. Framebuffer resizes happen on mode changes, not per
     * frame, so the leak is bounded by how often the user toggles video
     * settings. */
    slot.addr = arena_.allocFramebuffer(imageBytes(width, height));
    slot.width = width;
    slot.height = height;
}

RdramAddr FbRegistry::fbAddress(int fb) const
{
    if (fb <= 0 || static_cast<size_t>(fb) >= fbs_.size()) {
        return mainColor_[0];
    }
    return fbs_[static_cast<size_t>(fb)].addr;
}

void FbRegistry::fbSize(int fb, uint32_t *width, uint32_t *height) const
{
    uint32_t w = kMaxNativeWidth;
    uint32_t h = kMaxNativeHeight;
    if (fb > 0 && static_cast<size_t>(fb) < fbs_.size()) {
        w = fbs_[static_cast<size_t>(fb)].width;
        h = fbs_[static_cast<size_t>(fb)].height;
    }
    if (width) {
        *width = w;
    }
    if (height) {
        *height = h;
    }
}

} // namespace pdrt64
