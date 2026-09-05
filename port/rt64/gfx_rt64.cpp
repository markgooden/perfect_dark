/*
 * RT64 backend — stub.
 *
 * T2 only establishes that a second backend can be selected at runtime and
 * that the seam is complete. The real implementation composes the arena,
 * translator, register block and RT64 host wrapper described in
 * docs/interfaces/; it arrives in later tasks.
 *
 * Note the link mode decided in T1 (docs/BUILD-RT64.md): rt64 builds only
 * under MSVC while the port builds only under MinGW, so this file will never
 * include RT64 headers directly. It will call an MSVC-built C-ABI shim DLL
 * through docs/interfaces/rt64_host.h.
 */

#include <stddef.h>

extern "C" {
#include "platform.h"
#include "system.h"
#include "../fast3d/gfx_graphics_api.h"
}

namespace {

void rt64Unimplemented(const char *what)
{
    sysFatalError("RT64 backend: %s is not implemented yet.\n"
                  "This build can only run the OpenGL path; set Video.Renderer=0\n"
                  "or omit --renderer rt64.", what);
}

void rt64Init(const struct GfxInitSettings *settings)
{
    (void)settings;
    sysLogPrintf(LOG_NOTE, "rt64 backend selected");
    rt64Unimplemented("init");
}

void rt64Destroy(void) {}

struct GfxRenderingAPI *rt64GetRenderingApi(void)
{
    /* RT64 is not layered on a GfxRenderingAPI. */
    return NULL;
}

void rt64StartFrame(void) { rt64Unimplemented("start_frame"); }
void rt64Run(Gfx *commands) { (void)commands; rt64Unimplemented("run"); }
void rt64EndFrame(void) { rt64Unimplemented("end_frame"); }
void rt64SetTargetFps(int fps) { (void)fps; }

void rt64SetTextureFilter(enum FilteringMode mode) { (void)mode; }
void rt64SetMipmapFilter(enum MipmapFilteringMode mode) { (void)mode; }

void rt64TextureCacheClear(void) {}
void rt64TextureCacheDelete(const uint8_t *addr) { (void)addr; }
void rt64TextureCacheDeleteRange(const uint8_t *start, const uint8_t *end)
{
    (void)start;
    (void)end;
}

int rt64CreateFramebuffer(uint32_t width, uint32_t height, int upscale, int autoresize)
{
    (void)width; (void)height; (void)upscale; (void)autoresize;
    return 0;
}

void rt64ResizeFramebuffer(int fb, uint32_t width, uint32_t height, int upscale, int autoresize)
{
    (void)fb; (void)width; (void)height; (void)upscale; (void)autoresize;
}

void rt64SetFramebuffer(int fb, float noiseScale) { (void)fb; (void)noiseScale; }
void rt64ResetFramebuffer(void) {}
void rt64CopyFramebuffer(int dst, int src, int left, int top, int useBack)
{
    (void)dst; (void)src; (void)left; (void)top; (void)useBack;
}

int rt64GetMaxAnisotropyLevel(void) { return 1; }
void rt64SetAnisotropyLevel(int level) { (void)level; }

} // namespace

extern "C" const struct GfxGraphicsAPI gfx_rt64_api = {
    "rt64",
    rt64Init,
    rt64Destroy,
    rt64GetRenderingApi,
    rt64StartFrame,
    rt64Run,
    rt64EndFrame,
    rt64SetTargetFps,
    rt64SetTextureFilter,
    rt64SetMipmapFilter,
    rt64TextureCacheClear,
    rt64TextureCacheDelete,
    rt64TextureCacheDeleteRange,
    rt64CreateFramebuffer,
    rt64ResizeFramebuffer,
    rt64SetFramebuffer,
    rt64ResetFramebuffer,
    rt64CopyFramebuffer,
    rt64GetMaxAnisotropyLevel,
    rt64SetAnisotropyLevel,
};
