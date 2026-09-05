#ifndef GFX_GRAPHICS_API_H
#define GFX_GRAPHICS_API_H

/*
 * Frontend graphics-backend vtable.
 *
 * gfx_api.h declares free functions (gfx_init, gfx_run, ...) that used to be
 * implemented directly by fast3d's gfx_pc.cpp. To make the whole renderer
 * (interpreter + rasterizer) selectable at runtime, those free functions are
 * now thin dispatchers in gfx_api_dispatch.c that call through this table.
 * Two implementations exist:
 *
 *   - gfx_fast3d_api  (gfx_pc.cpp; the fast3d/OpenGL path, functions renamed
 *                      f3d_*, behaviour unchanged)
 *   - gfx_rt64_api    (port/rt64/gfx_rt64.cpp; the RT64 path)
 *
 * Selection happens once in videoInit() before gfx_init and never changes for
 * the lifetime of the process.
 *
 * The eight globals declared by gfx_api.h are shared state, not backend state:
 * they are defined in gfx_api_dispatch.c and every backend is responsible for
 * keeping the ones it owns up to date. video.c reads seven of them directly.
 *
 * NOTE: this header includes gfx_api.h inside its own extern "C" block, the
 * same trick gfx_pc.h uses, so it is safe to include from either C or C++.
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __cplusplus
#include <stdint.h>
#include <stdbool.h>
#endif

#include <PR/gbi.h>

#include "gfx_api.h"

struct GfxGraphicsAPI {
    /* Human-readable backend name for logs ("fast3d", "rt64"). Never NULL. */
    const char *name;

    /* Create window, device and all renderer state. Called exactly once,
     * before any other entry. Must not return on failure; call sysFatalError
     * like the existing paths do. */
    void (*init)(const struct GfxInitSettings *settings);

    /* Tear down renderer state. Called at most once, at shutdown. */
    void (*destroy)(void);

    /* Only meaningful for backends layered on a GfxRenderingAPI (fast3d).
     * RT64 returns NULL. */
    struct GfxRenderingAPI *(*get_current_rendering_api)(void);

    /* Frame lifecycle. run() may be called more than once per frame, each
     * with a root display list in the port's 64-bit Gfx format. */
    void (*start_frame)(void);
    void (*run)(Gfx *commands);
    void (*end_frame)(void);

    void (*set_target_fps)(int fps);

    void (*set_texture_filter)(enum FilteringMode mode);
    void (*set_mipmap_filter)(enum MipmapFilteringMode mode);

    /* Texture cache invalidation; addresses are host pointers into game
     * memory, same semantics as the gfx_texture_cache_* free functions. */
    void (*texture_cache_clear)(void);
    void (*texture_cache_delete)(const uint8_t *addr);
    void (*texture_cache_delete_range)(const uint8_t *start, const uint8_t *end);

    /* Offscreen framebuffers addressed by small integer handles. */
    int  (*create_framebuffer)(uint32_t width, uint32_t height, int upscale, int autoresize);
    void (*resize_framebuffer)(int fb, uint32_t width, uint32_t height, int upscale, int autoresize);
    void (*set_framebuffer)(int fb, float noise_scale);
    void (*reset_framebuffer)(void);
    void (*copy_framebuffer)(int fb_dst, int fb_src, int left, int top, int use_back);

    /* Anisotropy, previously reached from video.c via GfxRenderingAPI. */
    int  (*get_max_anisotropy_level)(void);
    void (*set_anisotropy_level)(int level);
};

extern const struct GfxGraphicsAPI gfx_fast3d_api;
extern const struct GfxGraphicsAPI gfx_rt64_api;

/* Selects the active backend. Must be called before gfx_init and never after.
 * Passing NULL is a no-op. */
void gfx_select_backend(const struct GfxGraphicsAPI *api);

/* The active backend; defaults to gfx_fast3d_api, never NULL. */
const struct GfxGraphicsAPI *gfx_get_backend(void);

#ifdef __cplusplus
}
#endif

#endif /* GFX_GRAPHICS_API_H */
