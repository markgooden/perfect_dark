/*
 * Region discovery. Contract is in rt64_regions.h.
 *
 * This was rt64_capture.cpp's private business until the live backend needed
 * the same answer; nothing about it is capture-specific.
 */

#include "rt64_regions.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace pdrt64 {

namespace {

uintptr_t g_imageBase = 0;
size_t g_imageSize = 0;

} // namespace

/*
 * Display lists reference static data compiled into the binary - light and
 * viewport blocks reached via G_MOVEMEM - which is neither heap nor ROM. The
 * image is mapped for the process lifetime, so reading it is safe; the bounds
 * come from the PE headers rather than a guess.
 */
void regionsInit()
{
    if (g_imageSize) {
        return;
    }

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

uintptr_t regionsImageBase()
{
    return g_imageBase;
}

size_t regionsImageSize()
{
    return g_imageSize;
}

bool regionsInImage(uintptr_t addr, size_t len)
{
    return g_imageSize && addr >= g_imageBase &&
           (addr + len) <= (g_imageBase + g_imageSize);
}

bool regionsInSpan(uintptr_t addr, size_t len, const void *base, size_t size)
{
    if (!base || !addr || !len) {
        return false;
    }
    const uintptr_t b = (uintptr_t)base;
    return addr >= b && (addr + len) <= (b + (uintptr_t)size);
}

} // namespace pdrt64
