#pragma once

/*
 * Synthetic RDRAM arena for the RT64 backend.
 *
 * One contiguous host allocation handed to RT64 as its `RDRAM` base.
 * Layout (offsets are RdramAddr values without the bit-31 tag):
 *
 *   [0x00000000, fbRegionEnd)   Framebuffer region. Color/depth images for
 *                               the game's offscreen framebuffer handles
 *                               plus the two main color buffers and the
 *                               depth buffer. Managed by Rt64FbRegistry.
 *   [fbRegionEnd, frameEnd)     Per-frame bump region. Reset every frame.
 *                               Holds the translated command stream and
 *                               all non-cached marshalled data.
 *   [frameEnd, cacheEnd)        Persistent cache region. Marshalled copies
 *                               of stable game data (background geometry,
 *                               textures) keyed by source pointer.
 *                               Invalidated by the texture-cache calls and
 *                               wiped wholesale on stage load.
 *
 * Total size is configurable (Video.RT64RdramMB, default 128, max 1024;
 * must stay below 2048 so every offset fits in 31 bits).
 */

#include <cstdint>
#include <cstddef>
#include <map>
#include <vector>

#include "rt64_mem.h"

namespace pdrt64 {

struct ArenaConfig {
    size_t totalBytes;      // whole synthetic RDRAM size
    size_t fbRegionBytes;   // reserved framebuffer region
    size_t frameRegionBytes;// per-frame bump region
};

struct ArenaStats {
    size_t frameBytesUsed;      // bump bytes consumed this frame
    size_t cacheBytesUsed;      // persistent bytes currently live
    uint32_t cacheHits;         // this frame
    uint32_t cacheMisses;       // this frame
    uint32_t outOfHeapBlocks;   // marshalled blocks whose source was
                                // outside g_MempHeap (census signal)
};

class Arena {
public:
    /* Allocates the RDRAM buffer. Fatal on allocation failure. */
    explicit Arena(const ArenaConfig &cfg);
    ~Arena();

    /* Host pointer to the start of the synthetic RDRAM. Handed to RT64 as
     * Application::Core::RDRAM. Stable for the arena's lifetime. */
    uint8_t *rdramBase();
    size_t rdramSize() const;

    /* Resets the per-frame bump region. Call once per game frame before
     * translation begins. Does not touch fb or cache regions. */
    void beginFrame();

    /* Bump-allocates `len` bytes (4-byte aligned) in the frame region and
     * returns its tagged address. Fatal if the frame region overflows
     * (see SCAFFOLD.md risk R1 before raising the region size). */
    RdramAddr allocFrame(size_t len);

    /* Bump-allocates in the framebuffer region. Never reclaimed; FbRegistry
     * owns the handles. Fatal on overflow. */
    RdramAddr allocFramebuffer(size_t len);

    /* Marshals `len` source bytes at `src` into the frame region using
     * `type` (see rt64_mem.h) and returns the tagged address. */
    RdramAddr pushFrameData(MemReader &mem, uintptr_t src, size_t len, Swizzle type);

    /* Marshals into the persistent cache region, keyed by (src, len,
     * type). On a key hit returns the existing address without copying.
     * The caller decides cacheability (translator policy: vertices and
     * DL-referenced matrices are frame data; textures, TLUTs and color
     * tables are cached). */
    RdramAddr pushCachedData(MemReader &mem, uintptr_t src, size_t len, Swizzle type);

    /* Cache invalidation, wired to the gfx texture-cache entry points.
     * `addr`/`start`/`end` are source host pointers. */
    void invalidateCache(uintptr_t addr);
    void invalidateCacheRange(uintptr_t start, uintptr_t end);
    void clearCache();

    /* Stats for the census/perf overlay; reset by beginFrame. */
    const ArenaStats &stats() const;

private:
    /* Cache key. Two requests for the same source bytes under different
     * swizzles are different marshalled results, so the type is part of the
     * identity. */
    struct CacheKey {
        uintptr_t src;
        size_t len;
        Swizzle type;
        bool operator<(const CacheKey &o) const
        {
            if (src != o.src) return src < o.src;
            if (len != o.len) return len < o.len;
            return type < o.type;
        }
    };

    RdramAddr marshalInto(RdramAddr at, MemReader &mem, uintptr_t src,
                          size_t len, Swizzle type);

    std::vector<uint8_t> buffer_;
    size_t fbEnd_ = 0;      // frame region starts here
    size_t frameEnd_ = 0;   // cache region starts here
    size_t framePos_ = 0;   // bump cursor, reset every frame
    size_t cachePos_ = 0;   // bump cursor, reset only by clearCache
    size_t fbPos_ = 0;      // bump cursor for framebuffer allocations
    std::map<CacheKey, RdramAddr> cache_;
    std::vector<uint8_t> scratch_;
    ArenaStats stats_{};
};

/*
 * Registry mapping the port's integer framebuffer handles (see
 * gfx_create_framebuffer in gfx_api.h) to color-image allocations in the
 * arena's framebuffer region, plus the fixed allocations for the main
 * double-buffered color images and the depth image.
 *
 * All images are RGBA16 at native resolution (matching what the game
 * believes it renders); RT64 upscales internally.
 */
class FbRegistry {
public:
    explicit FbRegistry(Arena &arena);

    /* Fixed allocations, created up front. cimg(0)/cimg(1) are the two
     * main color buffers the register block alternates VI_ORIGIN between;
     * zimg() is the shared depth image. */
    RdramAddr mainColorImage(int index) const; // index in {0,1}
    RdramAddr depthImage() const;

    /* Handle management mirroring the gfx_api framebuffer calls. Returns
     * a small positive handle. Resizing reallocates in the fb region. */
    int  createFb(uint32_t width, uint32_t height);
    void resizeFb(int fb, uint32_t width, uint32_t height);

    /* Address + dimensions for a handle. Used by the translator to turn
     * G_SETFB_EXT / G_SETTIMG_FB_EXT / G_COPYFB_EXT into canonical
     * G_SETCIMG / G_SETTIMG sequences. fb==0 refers to the current main
     * color image. */
    RdramAddr fbAddress(int fb) const;
    void fbSize(int fb, uint32_t *width, uint32_t *height) const;

private:
    struct Fb {
        RdramAddr addr;
        uint32_t width;
        uint32_t height;
    };

    Arena &arena_;
    RdramAddr mainColor_[2] = {0, 0};
    RdramAddr depth_ = 0;
    std::vector<Fb> fbs_;   // index 0 unused; handles are 1-based
};

} // namespace pdrt64
