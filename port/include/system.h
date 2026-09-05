#ifndef _IN_SYSTEM_H
#define _IN_SYSTEM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <PR/ultratypes.h>

enum LogLevel {
  LOG_NOTE,
  LOG_WARNING,
  LOG_ERROR,
};

void sysInitArgs(s32 argc, const char **argv);
void sysInit(void);

s32 sysArgCheck(const char *arg);
const char *sysArgGetString(const char *arg);
s32 sysArgGetInt(const char *arg, s32 defval);

u64 sysGetMicroseconds(void);

void sysFatalError(const char *fmt, ...) __attribute__((noreturn));

s32 sysLogIsOpen(void);
void sysLogPrintf(s32 level, const char *fmt, ...);

void sysGetExecutablePath(char *outPath, const u32 outLen);
void sysGetHomePath(char *outPath, const u32 outLen);

void *sysMemAlloc(const u32 size);
void *sysMemZeroAlloc(const u32 size);
void *sysMemRealloc(void *ptr, const u32 newSize);
void sysMemFree(void *ptr);

// True if [addr, addr+len) lies entirely inside one live sysMem allocation.
// The RT64 backend uses this to prove a pointer is mapped before reading it:
// room graphics data is sysMemAlloc'd (src/game/bg.c:2839) and so falls
// outside every memory region that backend models. See system.c for why the
// registry needs no locking.
s32 sysMemIsTracked(const void *addr, const u32 len);

// hns is specified in 100ns units
void sysSleep(const s64 hns);

// yield CPU if supported (e.g. during a busy loop)
void sysCpuRelax(void);

void crashInit(void);
void crashShutdown(void);

#ifdef __cplusplus
}
#endif

#endif
