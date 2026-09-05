#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/time.h>
#include <SDL.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "system.h"

#ifdef PLATFORM_WIN32

#include <windows.h>

// on win32 we use waitable timers instead of nanosleep
typedef HANDLE WINAPI (*CREATEWAITABLETIMEREXAFN)(LPSECURITY_ATTRIBUTES, LPCSTR, DWORD, DWORD);
static HANDLE timer;
static CREATEWAITABLETIMEREXAFN pfnCreateWaitableTimerExA;

// winapi also provides a yield macro
#define DO_YIELD() YieldProcessor()

// ask system for high performance GPU, if any
__attribute__((dllexport)) u32 NvOptimusEnablement = 1;
__attribute__((dllexport)) u32 AmdPowerXpressRequestHighPerformance = 1;

#else

#include <unistd.h>

// figure out how to yield
#if defined(PLATFORM_X86) || defined(PLATFORM_X86_64)
// this should work even if the code is not built with SSE enabled, at least on gcc and clang,
// but if it doesn't we'll have to use  __builtin_ia32_pause() or something
#include <immintrin.h>
#define DO_YIELD() _mm_pause()
#elif defined(PLATFORM_ARM) && (defined(PLATFORM_64BIT) || PLATFORM_ARM == 7 || PLATFORM_ARM == 8)
// same as YieldProcessor() on ARM Windows
#define DO_YIELD() __asm__ volatile("dmb ishst\n\tyield":::"memory")
#else
// fuck it
#define DO_YIELD() do { } while (0)
#endif

#endif

#define LOG_FNAME "pd.log"
#define CRASHLOG_FNAME "pd.crash.log"
#define USEC_IN_SEC 1000000ULL

static u64 startTick = 0;
static char logPath[2048];

static s32 sysArgc;
static const char **sysArgv;

static inline void sysLogSetPath(const char *fname)
{
	// figure out where the log is and clear it
	// try working dir first
	snprintf(logPath, sizeof(logPath), "./%s", fname);
	FILE *f = fopen(logPath, "wb");
	if (!f) {
		// try home dir
		sysGetHomePath(logPath, sizeof(logPath) - 1);
		strncat(logPath, "/", sizeof(logPath) - 1);
		strncat(logPath, fname, sizeof(logPath) - 1);
		f = fopen(logPath, "wb");
	}
	if (f) {
		fclose(f);
	}
}

void sysInitArgs(s32 argc, const char **argv)
{
	sysArgc = argc;
	sysArgv = argv;
}

void sysInit(void)
{
	startTick = sysGetMicroseconds();

	if (sysArgCheck("--log")) {
		sysLogSetPath(LOG_FNAME);
	}

#ifdef VERSION_HASH
	sysLogPrintf(LOG_NOTE, "version: " VERSION_BRANCH " " VERSION_HASH " (" VERSION_TARGET ")");
#endif

	char timestr[256];
	const time_t curtime = time(NULL);
	strftime(timestr, sizeof(timestr), "%d %b %Y %H:%M:%S", localtime(&curtime));
	sysLogPrintf(LOG_NOTE, "startup date: %s", timestr);

#ifdef PLATFORM_WIN32
	// this function is only present on Vista+, so try to import it from kernel32 by hand
	pfnCreateWaitableTimerExA = (CREATEWAITABLETIMEREXAFN)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateWaitableTimerExA");
	if (pfnCreateWaitableTimerExA) {
		// function exists, try to create a hires timer
		timer = pfnCreateWaitableTimerExA(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	}
	if (!timer) {
		// no function or hires timers not supported, fallback to lower resolution timer
		sysLogPrintf(LOG_WARNING, "SYS: hires waitable timers not available");
		timer = CreateWaitableTimerA(NULL, FALSE, NULL);
	}
#endif
}

s32 sysArgCheck(const char *arg)
{
	for (s32 i = 1; i < sysArgc; ++i) {
		if (!strcasecmp(sysArgv[i], arg)) {
			return 1;
		}
	}
	return 0;
}

const char *sysArgGetString(const char *arg)
{
	for (s32 i = 1; i < sysArgc; ++i) {
		if (!strcasecmp(sysArgv[i], arg)) {
			if (i < sysArgc - 1) {
				return sysArgv[i + 1];
			}
		}
	}
	return NULL;
}

s32 sysArgGetInt(const char *arg, s32 defval)
{
	for (s32 i = 1; i < sysArgc; ++i) {
		if (!strcasecmp(sysArgv[i], arg)) {
			if (i < sysArgc - 1) {
				return strtol(sysArgv[i + 1], NULL, 0);
			}
		}
	}
	return defval;
}

u64 sysGetMicroseconds(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return ((u64)tv.tv_sec * USEC_IN_SEC + (u64)tv.tv_usec) - startTick;
}

s32 sysLogIsOpen(void)
{
	return (logPath[0] != '\0');
}

void sysLogPrintf(s32 level, const char *fmt, ...)
{
	static const char *prefix[3] = {
		"", "WARNING: ", "ERROR: "
	};

	char logmsg[2048];

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(logmsg, sizeof(logmsg), fmt, ap);
	va_end(ap);

	if (logPath[0]) {
		FILE *f = fopen(logPath, "ab");
		if (f) {
			fprintf(f, "%s%s\n", prefix[level], logmsg);
			fclose(f);
		}
	}

	FILE *fout = (level == LOG_NOTE) ? stdout : stderr;
	fprintf(fout, "%s%s\n", prefix[level], logmsg);
}

void sysFatalError(const char *fmt, ...)
{
	static s32 alreadyCrashed = 0;

	if (alreadyCrashed) {
		abort();
	}

	char errmsg[2048] = { 0 };

	alreadyCrashed = 1;

	va_list ap;
	va_start(ap, fmt);
	vsnprintf(errmsg, sizeof(errmsg), fmt, ap);
	va_end(ap);

	sysLogPrintf(LOG_ERROR, "FATAL: %s", errmsg);

	fflush(stdout);
	fflush(stderr);

	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Fatal error", errmsg, NULL);

	exit(1);
}

void sysGetExecutablePath(char *outPath, const u32 outLen)
{
	// try asking SDL
	char *sdlPath = SDL_GetBasePath();

	if (sdlPath && *sdlPath) {
		// -1 to trim trailing slash
		const u32 len = strlen(sdlPath) - 1;
		if (len < outLen) {
			memcpy(outPath, sdlPath, len);
			outPath[len] = '\0';
		}
	} else if (sysArgc && sysArgv[0] && sysArgv[0][0]) {
		// get exe path from argv[0]
		strncpy(outPath, sysArgv[0], outLen - 1);
		outPath[outLen - 1] = '\0';
	} else if (outLen > 1) {
		// give up, use working directory instead
		outPath[0] = '.';
		outPath[1] = '\0';
	}

#ifdef PLATFORM_WIN32
	// replace all backslashes with forward slashes, windows supports both
	for (u32 i = 0; i < outLen && outPath[i]; ++i) {
		if (outPath[i] == '\\') {
			outPath[i] = '/';
		}
	}
#endif

	SDL_free(sdlPath);
}

void sysGetHomePath(char *outPath, const u32 outLen)
{
	// try asking SDL
	char *sdlPath = SDL_GetPrefPath("", "perfectdark");

	if (sdlPath && *sdlPath) {
		// -1 to trim trailing slash
		const u32 len = strlen(sdlPath) - 1;
		if (len < outLen) {
			memcpy(outPath, sdlPath, len);
			outPath[len] = '\0';
		}
	} else if (outLen > 1) {
		// give up, use working directory instead
		outPath[0] = '.';
		outPath[1] = '\0';
	}

#ifdef PLATFORM_WIN32
	// replace all backslashes with forward slashes, windows supports both
	for (u32 i = 0; i < outLen && outPath[i]; ++i) {
		if (outPath[i] == '\\') {
			outPath[i] = '/';
		}
	}
#endif

	SDL_free(sdlPath);
}

/*
 * Live-allocation registry.
 *
 * The RT64 backend must never dereference a pointer it cannot prove is mapped.
 * Room graphics data is plain malloc on this platform (bgLoadRoom,
 * src/game/bg.c:2839, "allocate room data from heap to not take up mema
 * space"), so it lies outside every region the backend models - heap, ROM
 * image and executable image - and the backend has no choice but to refuse it.
 * Recording what sysMemAlloc hands out lets it classify those pointers
 * instead. Display lists point into the MIDDLE of a room block, so the
 * question is "which allocation contains this address", not "is this a base
 * pointer": the table is kept sorted and searched by containment.
 *
 * Cost: insertions are O(n) memmove but happen only at load time - every call
 * site is romdata, preprocess or bgLoadRoom, none per-frame - while lookups
 * are per-display-list-command and O(log n).
 *
 * No locking, because the port allocates only on the main thread: it creates
 * no threads (no SDL_CreateThread or pthread_create anywhere in port/src or
 * src/lib) and uses SDL's queue audio API rather than a callback thread
 * (port/src/audio.c:29 sets want.callback = NULL).
 *
 * The table itself uses malloc/realloc directly rather than the sysMem
 * wrappers, so tracking never recurses into itself.
 */
struct sysAllocEntry {
	uintptr_t base;
	u32 size;
};

static struct sysAllocEntry *g_SysAllocs = NULL;
static u32 g_SysAllocCount = 0;
static u32 g_SysAllocCap = 0;

/* Index of the first entry whose base is >= addr. */
static u32 sysAllocLowerBound(const uintptr_t addr)
{
	u32 lo = 0;
	u32 hi = g_SysAllocCount;

	while (lo < hi) {
		const u32 mid = lo + (hi - lo) / 2;
		if (g_SysAllocs[mid].base < addr) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}

	return lo;
}

static void sysAllocTrack(void *ptr, const u32 size)
{
	const uintptr_t base = (uintptr_t)ptr;
	u32 at;

	if (!ptr || !size) {
		return;
	}

	if (g_SysAllocCount == g_SysAllocCap) {
		const u32 cap = g_SysAllocCap ? g_SysAllocCap * 2 : 128;
		struct sysAllocEntry *grown = realloc(g_SysAllocs, cap * sizeof(struct sysAllocEntry));
		if (!grown) {
			/* Out of memory for the registry alone. Dropping the entry
			 * costs the backend a classification, not correctness: an
			 * unclassified pointer is refused, never blindly read. */
			return;
		}
		g_SysAllocs = grown;
		g_SysAllocCap = cap;
	}

	at = sysAllocLowerBound(base);
	memmove(&g_SysAllocs[at + 1], &g_SysAllocs[at],
			(g_SysAllocCount - at) * sizeof(struct sysAllocEntry));
	g_SysAllocs[at].base = base;
	g_SysAllocs[at].size = size;
	++g_SysAllocCount;
}

static void sysAllocUntrack(void *ptr)
{
	const uintptr_t base = (uintptr_t)ptr;
	u32 at;

	if (!ptr || !g_SysAllocCount) {
		return;
	}

	at = sysAllocLowerBound(base);
	if (at >= g_SysAllocCount || g_SysAllocs[at].base != base) {
		return;
	}

	memmove(&g_SysAllocs[at], &g_SysAllocs[at + 1],
			(g_SysAllocCount - at - 1) * sizeof(struct sysAllocEntry));
	--g_SysAllocCount;
}

s32 sysMemIsTracked(const void *addr, const u32 len)
{
	const uintptr_t a = (uintptr_t)addr;
	u32 at;

	if (!addr || !len || !g_SysAllocCount) {
		return 0;
	}

	/* The containing allocation is the last one starting at or before addr. */
	at = sysAllocLowerBound(a);
	if (at >= g_SysAllocCount || g_SysAllocs[at].base != a) {
		if (at == 0) {
			return 0;
		}
		--at;
	}

	/* Check the length against the block before subtracting it, or the u32
	 * arithmetic underflows and every oversized read looks contained. */
	if (a < g_SysAllocs[at].base || len > g_SysAllocs[at].size) {
		return 0;
	}

	return (a - g_SysAllocs[at].base) <= (uintptr_t)(g_SysAllocs[at].size - len);
}

void *sysMemAlloc(const u32 size)
{
	void *ptr = malloc(size);
	sysAllocTrack(ptr, size);
	return ptr;
}

void *sysMemZeroAlloc(const u32 size)
{
	void *ptr = calloc(1, size);
	sysAllocTrack(ptr, size);
	return ptr;
}

void *sysMemRealloc(void *ptr, const u32 newSize)
{
	void *out;
	sysAllocUntrack(ptr);
	out = realloc(ptr, newSize);
	sysAllocTrack(out, newSize);
	return out;
}

void sysMemFree(void *ptr)
{
	sysAllocUntrack(ptr);
	free(ptr);
}

void sysSleep(const s64 hns)
{
#ifdef PLATFORM_WIN32
	static LARGE_INTEGER li;
	li.QuadPart = -hns;
	SetWaitableTimer(timer, &li, 0, NULL, NULL, FALSE);
	WaitForSingleObject(timer, INFINITE);
#else
	const struct timespec spec = { 0, hns * 100 };
	nanosleep(&spec, NULL);
#endif
}

void sysCpuRelax(void)
{
	DO_YIELD();
}
