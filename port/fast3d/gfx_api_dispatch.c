/*
 * Backend dispatch for the gfx_api.h contract.
 *
 * Every gfx_* free function declared by gfx_api.h is implemented here as a
 * one-line call through the active GfxGraphicsAPI. The fast3d implementations
 * live in gfx_pc.cpp under f3d_* names; the RT64 ones in port/rt64/.
 *
 * This file also OWNS the eight shared globals gfx_api.h declares. They used
 * to be defined in gfx_pc.cpp, which made them fast3d state; video.c reads
 * seven of them directly (dimensions, native viewport/aspect, msaa level,
 * framebuffer/detail-texture toggles), so with a second backend selected
 * nothing would have kept them current. They are shared state now, and each
 * backend updates the ones it owns.
 */

#include <stddef.h>

#include "gfx_graphics_api.h"
#include "../rt64/rt64_capture.h"

/* ---- shared state (moved here from gfx_pc.cpp) ---- */

struct GfxDimensions gfx_current_window_dimensions;
struct GfxDimensions gfx_current_dimensions;
struct XYWidthHeight gfx_current_game_window_viewport;
struct XYWidthHeight gfx_current_native_viewport;
float gfx_current_native_aspect = 4.f / 3.f;
bool gfx_framebuffers_enabled = true;
bool gfx_detail_textures_enabled = true;
uint32_t gfx_msaa_level = 1;

/* ---- backend selection ---- */

static const struct GfxGraphicsAPI *g_backend = &gfx_fast3d_api;

void gfx_select_backend(const struct GfxGraphicsAPI *api)
{
    if (api) {
        g_backend = api;
    }
}

const struct GfxGraphicsAPI *gfx_get_backend(void)
{
    return g_backend;
}

/* ---- dispatch ---- */

void gfx_init(const struct GfxInitSettings *settings)
{
    g_backend->init(settings);
}

void gfx_destroy(void)
{
    g_backend->destroy();
}

struct GfxRenderingAPI *gfx_get_current_rendering_api(void)
{
    return g_backend->get_current_rendering_api();
}

void gfx_start_frame(void)
{
    g_backend->start_frame();
}

void gfx_run(Gfx *commands)
{
    /* Capture sees the display list exactly as the game submitted it, before
     * any backend touches it. No-op unless armed. */
    pdCaptureOnRun(commands);
    g_backend->run(commands);
}

void gfx_end_frame(void)
{
    g_backend->end_frame();
    pdCaptureEndFrame();
}

void gfx_set_target_fps(int fps)
{
    g_backend->set_target_fps(fps);
}

void gfx_set_texture_filter(enum FilteringMode mode)
{
    g_backend->set_texture_filter(mode);
}

void gfx_set_mipmap_filter(enum MipmapFilteringMode mode)
{
    g_backend->set_mipmap_filter(mode);
}

void gfx_texture_cache_clear(void)
{
    g_backend->texture_cache_clear();
}

void gfx_texture_cache_delete(const uint8_t *orig_addr)
{
    g_backend->texture_cache_delete(orig_addr);
}

void gfx_texture_cache_delete_range(const uint8_t *start, const uint8_t *end)
{
    g_backend->texture_cache_delete_range(start, end);
}

int gfx_create_framebuffer(uint32_t width, uint32_t height, int upscale, int autoresize)
{
    return g_backend->create_framebuffer(width, height, upscale, autoresize);
}

void gfx_resize_framebuffer(int fb, uint32_t width, uint32_t height, int upscale, int autoresize)
{
    g_backend->resize_framebuffer(fb, width, height, upscale, autoresize);
}

void gfx_set_framebuffer(int fb, float noise_scale)
{
    g_backend->set_framebuffer(fb, noise_scale);
}

void gfx_reset_framebuffer(void)
{
    g_backend->reset_framebuffer();
}

void gfx_copy_framebuffer(int fb_dst, int fb_src, int left, int top, int use_back)
{
    g_backend->copy_framebuffer(fb_dst, fb_src, left, top, use_back);
}
