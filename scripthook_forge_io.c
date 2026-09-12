/* Forge Mod Loader: the file I/O half.
 *
 * The engine reads its archives through the ordinary Win32 calls, so the
 * cheapest way to serve a modded archive without copying 19.89 GB or
 * writing anything is to leave the game's own handle on the vanilla file
 * alone and only change the bytes that a mod replaces, at the moment
 * they are read.
 *
 * ----- what was measured, 2026-09-12, retail install, probe round -----
 *
 *   - archives are read, never mapped: 47 .forge opens and 646 reads in
 *     one session, and not one CreateFileMappingA/W or MapViewOfFile on
 *     a .forge. (The handful of UnmapViewOfFile calls belong to other
 *     files.) The overlay idea rests on this.
 *   - each archive is opened TWICE and the pair behaves differently:
 *
 *       share=0x7  flags=0x10000000 (FILE_FLAG_RANDOM_ACCESS)
 *           buffered. ReadFile completes on the spot, at the position a
 *           preceding SetFilePointer set.
 *
 *       share=0x1  flags=0x60000000 (NO_BUFFERING | OVERLAPPED)
 *           asynchronous. ReadFile returns ERROR_IO_PENDING for 391 of
 *           the reads, the file pointer never moves, and the offset
 *           exists only in the OVERLAPPED structure. Payload streaming
 *           goes through this one.
 *
 *   - the hooks go in from the loader thread about 3.3 seconds before
 *     the first archive is opened, so DllMain is not needed.
 *
 * ----- what is hooked, and why exactly this much ---------------------
 *
 *   ReadFile              - patch the caller's buffer for the ranges a
 *                           mod replaces. A synchronous read is patched
 *                           on return. An asynchronous one is recorded
 *                           and patched later (see below).
 *   GetOverlappedResult   - the completion point for those asynchronous
 *   GetOverlappedResultEx   reads: by the time either returns, the data
 *                           IS in the buffer, and only then is it safe
 *                           to patch. Waiting for completion inside
 *                           ReadFile instead would consume the signal
 *                           on the engine's own event, and an engine
 *                           that waits on that event itself would then
 *                           wait forever. So nothing here ever waits.
 *   CloseHandle           - drop the handle from the cache, so a later
 *                           file that reuses the same handle value
 *                           cannot inherit an archive's patches. That
 *                           mistake would corrupt data silently.
 *
 * A read that covers no patch is passed through completely untouched, so
 * ordinary loading behaviour and its asynchronous pattern are unchanged.
 * If an engine never calls GetOverlappedResult for some read, the patch
 * for that one read simply does not land - a missing mod, not a crash.
 *
 * Which handle is which is not learned from CreateFile: hooks on
 * CreateFileW would collide with GhostNoWipe and skipintro, which both
 * want it, and MinHook keeps one hook per target address. Instead a
 * handle is resolved LATE, the first time it is read, with
 * GetFinalPathNameByHandleW - and the answer, found or not, is cached,
 * so each handle costs one lookup for the whole session. Reading is what
 * matters anyway: an archive that is never read is never a problem.
 *
 * This also means the overlay is built at the first read of an archive
 * rather than at open, which is where the tables (a few MB) are parsed
 * and the mod payloads are read into memory.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "forge.h"
#include "log.h"
#include "third_party/minhook/include/MinHook.h"

#define IO_CACHE    256    /* direct mapped by handle; handles are few */
#define PENDING_MAX 64     /* asynchronous reads in flight that matter */

#ifdef _MSC_VER
#define IO_TLS __declspec(thread)
#else
#define IO_TLS __thread
#endif

typedef BOOL   (WINAPI *ReadFile_t)(HANDLE, LPVOID, DWORD, LPDWORD,
                                    LPOVERLAPPED);
typedef BOOL   (WINAPI *CloseHandle_t)(HANDLE);
typedef BOOL   (WINAPI *GetOverlappedResult_t)(HANDLE, LPOVERLAPPED, LPDWORD,
                                               BOOL);
typedef BOOL   (WINAPI *GetOverlappedResultEx_t)(HANDLE, LPOVERLAPPED,
                                                 LPDWORD, DWORD, BOOL);
typedef BOOL   (WINAPI *SetFilePointerEx_t)(HANDLE, LARGE_INTEGER,
                                            PLARGE_INTEGER, DWORD);
typedef DWORD  (WINAPI *GetFinalPathNameByHandleW_t)(HANDLE, LPWSTR, DWORD,
                                                     DWORD);
typedef DWORD  (WINAPI *WaitForSingleObject_t)(HANDLE, DWORD);
typedef DWORD  (WINAPI *WaitForSingleObjectEx_t)(HANDLE, DWORD, BOOL);
typedef DWORD  (WINAPI *WaitForMultipleObjects_t)(DWORD, const HANDLE *, BOOL,
                                                  DWORD);
typedef DWORD  (WINAPI *WaitForMultipleObjectsEx_t)(DWORD, const HANDLE *,
                                                    BOOL, DWORD, BOOL);

static ReadFile_t            r_ReadFile;
static CloseHandle_t         r_CloseHandle;
static GetOverlappedResult_t r_GetOverlappedResult;
static GetOverlappedResultEx_t r_GetOverlappedResultEx;
static WaitForSingleObject_t      r_WaitForSingleObject;
static WaitForSingleObjectEx_t    r_WaitForSingleObjectEx;
static WaitForMultipleObjects_t   r_WaitForMultipleObjects;
static WaitForMultipleObjectsEx_t r_WaitForMultipleObjectsEx;
static SetFilePointerEx_t            p_SetFilePointerEx;
static GetFinalPathNameByHandleW_t   p_FinalPath;

static HANDLE          g_cacheH[IO_CACHE];
static ShForgeOverlay *g_cacheO[IO_CACHE];
static IO_TLS int      t_busy;
static volatile LONG   g_fixups;
static volatile LONG   g_gorCalls;

/* An asynchronous read that covers a patch: remembered at ReadFile time
 * so the buffer can be patched once the transfer is known to be done. */
typedef struct {
    HANDLE        h;
    LPOVERLAPPED  ov;
    uint8_t      *buf;
    uint32_t      len;
    uint64_t      off;
    ShForgeOverlay *o;
} PendingRead;

static PendingRead g_pending[PENDING_MAX];
static CRITICAL_SECTION g_lock;
static int g_lockReady;

/* What the OS leaves in OVERLAPPED::Internal while a transfer is still
 * running. Anything else means it has finished. */
#define IO_STATUS_PENDING 0x103u

/* How many entries are live. Waits are extremely common, so the sweep
 * below has to cost nothing when there is nothing outstanding. */
static volatile LONG g_pendingCount;

static unsigned SlotOf(HANDLE h) {
    return (unsigned)(((uintptr_t)h >> 4) & (IO_CACHE - 1));
}

static int EndsWithForge(const char *p) {
    size_t n = p ? strlen(p) : 0;
    if (n < 6) return 0;
    return _stricmp(p + n - 6, ".forge") == 0;
}

/* Does a read of [off, off+len) cover any patched byte? Reads that do
 * not are left exactly as they were. */
static int TouchesOverlay(const ShForgeOverlay *o, uint64_t off,
                          uint32_t len) {
    int i;
    for (i = 0; i < o->patchCount; i++) {
        const ShForgePatch *p = &o->patches[i];
        if (p->off < off + len && off < p->off + p->len) return 1;
    }
    return 0;
}

static void CountFixup(ShForgeOverlay *o, uint64_t off, const uint8_t *buf,
                       uint32_t len, const char *what) {
    int c = (int)InterlockedIncrement(&g_fixups);
    ShForgeOverlayFixup(o, off, (uint8_t *)buf, len);
    if (c <= 4 || (c % 4096) == 0)
        Log("read/%s: %s off=%llu len=%lu (fixup %d)", what, o->path,
            (unsigned long long)off, (unsigned long)len, c);
}

/* ---- the pending table ---------------------------------------------- */

static void PendingAdd(HANDLE h, LPOVERLAPPED ov, void *buf, uint32_t len,
                       uint64_t off, ShForgeOverlay *o) {
    int i;
    if (!g_lockReady) return;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < PENDING_MAX; i++)
        if (g_pending[i].ov == NULL) {
            g_pending[i].h = h;
            g_pending[i].ov = ov;
            g_pending[i].buf = (uint8_t *)buf;
            g_pending[i].len = len;
            g_pending[i].off = off;
            g_pending[i].o = o;
            InterlockedIncrement(&g_pendingCount);
            break;
        }
    LeaveCriticalSection(&g_lock);
}

/* Look up an operation by its OVERLAPPED alone: that pointer is what
 * identifies the request, and matching the handle as well would miss a
 * caller that collects the result on a duplicated handle. */
static int PendingTake(LPOVERLAPPED ov, PendingRead *out) {
    int i, found = 0;
    if (!g_lockReady) return 0;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < PENDING_MAX; i++)
        if (g_pending[i].ov == ov && ov != NULL) {
            *out = g_pending[i];
            g_pending[i].ov = NULL;
            InterlockedDecrement(&g_pendingCount);
            found = 1;
            break;
        }
    LeaveCriticalSection(&g_lock);
    return found;
}

static void PendingDrop(LPOVERLAPPED ov) {
    int i;
    if (!g_lockReady) return;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < PENDING_MAX; i++)
        if (g_pending[i].ov == ov) {
            g_pending[i].ov = NULL;
            InterlockedDecrement(&g_pendingCount);
        }
    LeaveCriticalSection(&g_lock);
}

/* Patch every recorded read whose transfer has finished. This is what
 * makes the asynchronous half mechanism-agnostic: it does not matter
 * whether the caller collects the result with GetOverlappedResult, waits
 * on the OVERLAPPED's event, waits on the file handle, or polls
 * HasOverlappedIoCompleted - by the time any of those report progress,
 * OVERLAPPED::Internal is no longer "pending", and the bytes are in the
 * caller's buffer. Called after every wait and on every read, and it
 * costs one compare when nothing is outstanding. */
static void PendingSweep(void) {
    int i;
    if (!g_lockReady || t_busy || g_pendingCount == 0) return;

    for (i = 0; i < PENDING_MAX; i++) {
        PendingRead pr;
        DWORD n;

        EnterCriticalSection(&g_lock);
        if (g_pending[i].ov == NULL ||
            g_pending[i].ov->Internal == IO_STATUS_PENDING) {
            LeaveCriticalSection(&g_lock);
            continue;
        }
        pr = g_pending[i];
        g_pending[i].ov = NULL;
        InterlockedDecrement(&g_pendingCount);
        LeaveCriticalSection(&g_lock);

        /* A failed transfer leaves its status in Internal, not zero. Only
         * the successful one has bytes to patch. */
        if (pr.ov->Internal != 0) continue;
        n = (DWORD)pr.ov->InternalHigh;
        if (n > pr.len) n = pr.len;
        if (n) {
            t_busy = 1;
            CountFixup(pr.o, pr.off, pr.buf, n, "async-done");
            t_busy = 0;
        }
    }
}

/* ---- resolving a handle -------------------------------------------- */

/* Resolve a handle once; NULL means "not a modded archive" (cached too,
 * so the lookup is not repeated for the life of that handle). */
static ShForgeOverlay *LookupOrResolve(HANDLE h) {
    unsigned slot = SlotOf(h);
    wchar_t wpath[SH_FORGE_PATH_MAX + 8];
    char path[SH_FORGE_PATH_MAX + 8];
    ShForgeOverlay *o;

    if (g_cacheH[slot] == h) return g_cacheO[slot];

    g_cacheH[slot] = h;
    g_cacheO[slot] = NULL;

    if (!p_FinalPath || !h || h == INVALID_HANDLE_VALUE) return NULL;
    if (!p_FinalPath(h, wpath, SH_FORGE_PATH_MAX + 4, 0)) return NULL;
    if (!WideCharToMultiByte(CP_ACP, 0, wpath, -1, path, sizeof(path), NULL, NULL))
        return NULL;

    /* The API answers with a \\?\ form; the archive folder name is all
     * the overlay cares about, so the prefix is only folded away. */
    {
        char *p = path;
        if (p[0] == '\\' && p[1] == '\\' && p[2] == '?' && p[3] == '\\') p += 4;
        if (!EndsWithForge(p)) return NULL;
        o = ShForgeOverlayFor(p);
    }

    if (o) g_cacheO[slot] = o;
    /* No per-handle log here: the cache is small enough that ordinary
     * traffic evicts and re-resolves, and the overlay build already
     * announces itself once. */
    return o;
}

/* ---- hooks --------------------------------------------------------- */

static BOOL WINAPI H_ReadFile(HANDLE h, LPVOID buf, DWORD len, LPDWORD got,
                              LPOVERLAPPED ov) {
    int was = t_busy;
    ShForgeOverlay *o = NULL;
    uint64_t off = 0;
    int haveOff = 0;
    BOOL ok;
    DWORD err;

    /* Our own table and payload reads run through here too; they must
     * pass straight through or the overlay build would recurse. */
    if (was) return r_ReadFile(h, buf, len, got, ov);

    /* Also covers a caller that polls instead of waiting. */
    PendingSweep();

    t_busy = 1;
    o = LookupOrResolve(h);
    if (o) {
        if (ov) {
            /* The asynchronous handle: its file pointer never moves, so
             * the OVERLAPPED is the only place the offset exists. */
            off = ((uint64_t)ov->OffsetHigh << 32) | ov->Offset;
            haveOff = 1;
        } else if (p_SetFilePointerEx) {
            /* A zero distance at FILE_CURRENT only asks. It is the only
             * way to know where this read starts. */
            LARGE_INTEGER cur, zero;
            zero.QuadPart = 0;
            if (p_SetFilePointerEx(h, zero, &cur, FILE_CURRENT)) {
                off = (uint64_t)cur.QuadPart;
                haveOff = 1;
            }
        }
    }

    if (o && haveOff && ShForgeLogReads())
        Log("read: off=%llu len=%lu %s hit=%d",
            (unsigned long long)off, (unsigned long)len,
            ov ? "async" : "sync", TouchesOverlay(o, off, len));

    if (!o || !haveOff || !TouchesOverlay(o, off, len)) {
        /* Nothing to patch in this read; leave it entirely alone, its
         * asynchronous behaviour included. */
        ok = r_ReadFile(h, buf, len, got, ov);
        err = GetLastError();
        SetLastError(err);
        t_busy = was;
        return ok;
    }

    if (ov) {
        /* Asynchronous by construction: remember it and patch once the
         * engine asks for the result - unless it completed right here,
         * which small reads often do. The byte count then comes from the
         * count parameter if the caller passed one, and otherwise from
         * the OVERLAPPED itself, because passing none is the usual thing
         * for overlapped I/O and reading that as "no bytes" would drop
         * the patch on the floor. */
        PendingAdd(h, ov, buf, len, off, o);
        ok = r_ReadFile(h, buf, len, got, ov);
        err = GetLastError();
        if (ok) {
            DWORD n = got ? *got : (DWORD)ov->InternalHigh;
            PendingDrop(ov);
            if (n) CountFixup(o, off, (uint8_t *)buf, n, "async-now");
            else if (ShForgeLogReads())
                Log("read/async-now: no byte count (got=%p intHigh=%lu)",
                    (void *)got, (unsigned long)ov->InternalHigh);
        } else if (err != ERROR_IO_PENDING) {
            PendingDrop(ov);   /* failed outright; nothing will complete */
        } else if (ShForgeLogReads()) {
            Log("read/async-pending: recorded off=%llu len=%lu",
                (unsigned long long)off, (unsigned long)len);
        }
        SetLastError(err);
        t_busy = was;
        return ok;
    }

    /* Synchronous, and it covers a patch: call it, then patch. */
    ok = r_ReadFile(h, buf, len, got, ov);
    err = GetLastError();
    if (ok && got && *got) CountFixup(o, off, (uint8_t *)buf, *got, "sync");
    SetLastError(err);
    t_busy = was;
    return ok;
}

static BOOL WINAPI H_GetOverlappedResult(HANDLE h, LPOVERLAPPED ov,
                                         LPDWORD got, BOOL wait) {
    PendingRead pr;
    BOOL ok, matched = FALSE;
    DWORD err;

    ok = r_GetOverlappedResult(h, ov, got, wait);
    err = GetLastError();

    /* Only consume the record once the transfer really finished. A
     * caller that polls with bWait = FALSE reports "not yet" many times
     * before it reports success, and treating the first of those as the
     * end would throw the patch away. */
    if (!t_busy && ok) matched = PendingTake(ov, &pr) ? TRUE : FALSE;
    if (ShForgeLogReads() && (matched || InterlockedIncrement(&g_gorCalls) <= 8))
        Log("gor: matched=%d ok=%d", matched, ok);

    if (matched) {
        DWORD n = (ok && got) ? *got : 0;
        if (n) {
            if (n > pr.len) n = pr.len;
            t_busy = 1;
            CountFixup(pr.o, pr.off, pr.buf, n, "async-done");
            t_busy = 0;
        }
    }

    SetLastError(err);
    return ok;
}

static BOOL WINAPI H_GetOverlappedResultEx(HANDLE h, LPOVERLAPPED ov,
                                           LPDWORD got, DWORD ms,
                                           BOOL alertable) {
    PendingRead pr;
    BOOL ok, matched = FALSE;
    DWORD err;

    ok = r_GetOverlappedResultEx(h, ov, got, ms, alertable);
    err = GetLastError();

    if (!t_busy && ok) matched = PendingTake(ov, &pr) ? TRUE : FALSE;
    if (ShForgeLogReads() && (matched || InterlockedIncrement(&g_gorCalls) <= 8))
        Log("gor-ex: matched=%d ok=%d", matched, ok);

    if (matched) {
        DWORD n = (ok && got) ? *got : 0;
        if (n) {
            if (n > pr.len) n = pr.len;
            t_busy = 1;
            CountFixup(pr.o, pr.off, pr.buf, n, "async-done");
            t_busy = 0;
        }
    }

    SetLastError(err);
    return ok;
}

static BOOL WINAPI H_CloseHandle(HANDLE h) {
    unsigned slot = SlotOf(h);
    if (g_cacheH[slot] == h) {
        g_cacheH[slot] = NULL;
        g_cacheO[slot] = NULL;
    }
    return r_CloseHandle(h);
}

/* Waits are the other way an overlapped read gets noticed, and the one
 * this engine actually uses. The patch still lands before the caller can
 * look at the buffer, because the sweep runs here, inside the wait, on
 * the way back out. */
static DWORD WINAPI H_WaitForSingleObject(HANDLE h, DWORD ms) {
    DWORD r = r_WaitForSingleObject(h, ms);
    if (r != WAIT_TIMEOUT && r != WAIT_FAILED) PendingSweep();
    return r;
}

static DWORD WINAPI H_WaitForSingleObjectEx(HANDLE h, DWORD ms,
                                            BOOL alertable) {
    DWORD r = r_WaitForSingleObjectEx(h, ms, alertable);
    if (r != WAIT_TIMEOUT && r != WAIT_FAILED) PendingSweep();
    return r;
}

static DWORD WINAPI H_WaitForMultipleObjects(DWORD n, const HANDLE *hs,
                                             BOOL all, DWORD ms) {
    DWORD r = r_WaitForMultipleObjects(n, hs, all, ms);
    if (r != WAIT_TIMEOUT && r != WAIT_FAILED) PendingSweep();
    return r;
}

static DWORD WINAPI H_WaitForMultipleObjectsEx(DWORD n, const HANDLE *hs,
                                               BOOL all, DWORD ms,
                                               BOOL alertable) {
    DWORD r = r_WaitForMultipleObjectsEx(n, hs, all, ms, alertable);
    if (r != WAIT_TIMEOUT && r != WAIT_FAILED) PendingSweep();
    return r;
}

/* ---- install ------------------------------------------------------- */

#define HOOK(fn, det, real)                                                \
    do {                                                                   \
        MH_STATUS s_ = MH_CreateHookApi(L"kernel32.dll", fn,               \
                                        (LPVOID)(det), (LPVOID *)(real));  \
        Log("  %-22s %s", fn, s_ == MH_OK ? "ok" : MH_StatusToString(s_)); \
    } while (0)

void ShForgeIoStartup(void) {
    static LONG started = 0;
    MH_STATUS s;

    if (InterlockedExchange(&started, 1)) return;

    LogInit("scripthook_forge_io.log");

    /* MinHook is per-DLL and scripthook_corefix.c already initialises it
     * from DllMain, so "already initialized" is the normal case. */
    s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        Log("MH_Initialize failed (%s); forge I/O not installed",
            MH_StatusToString(s));
        return;
    }

    InitializeCriticalSection(&g_lock);
    g_lockReady = 1;

    /* Resolved, not hooked: used to ask a handle where the read is and
     * what it is, without intercepting either call. */
    p_SetFilePointerEx = (SetFilePointerEx_t)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "SetFilePointerEx");
    p_FinalPath = (GetFinalPathNameByHandleW_t)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "GetFinalPathNameByHandleW");

    Log("forge io installing");
    HOOK("ReadFile",              H_ReadFile,              &r_ReadFile);
    HOOK("GetOverlappedResult",   H_GetOverlappedResult,   &r_GetOverlappedResult);
    HOOK("WaitForSingleObject",   H_WaitForSingleObject,   &r_WaitForSingleObject);
    HOOK("WaitForMultipleObjects", H_WaitForMultipleObjects, &r_WaitForMultipleObjects);
    HOOK("CloseHandle",           H_CloseHandle,           &r_CloseHandle);
    {
        MH_STATUS s3 = MH_CreateHookApi(L"kernel32.dll", "WaitForSingleObjectEx",
                                        (LPVOID)H_WaitForSingleObjectEx,
                                        (LPVOID *)&r_WaitForSingleObjectEx);
        Log("  %-22s %s", "WaitForSingleObjectEx",
            s3 == MH_OK ? "ok" : MH_StatusToString(s3));
        s3 = MH_CreateHookApi(L"kernel32.dll", "WaitForMultipleObjectsEx",
                              (LPVOID)H_WaitForMultipleObjectsEx,
                              (LPVOID *)&r_WaitForMultipleObjectsEx);
        Log("  %-22s %s", "WaitForMultipleObjectsEx",
            s3 == MH_OK ? "ok" : MH_StatusToString(s3));
    }
    {
        /* Present since Windows 8; absent means the plain one is used. */
        MH_STATUS s2 = MH_CreateHookApi(L"kernel32.dll", "GetOverlappedResultEx",
                                        (LPVOID)H_GetOverlappedResultEx,
                                        (LPVOID *)&r_GetOverlappedResultEx);
        Log("  %-22s %s", "GetOverlappedResultEx",
            s2 == MH_OK ? "ok" : MH_StatusToString(s2));
    }
    MH_EnableHook(MH_ALL_HOOKS);
    Log("forge io installed (SetFilePointerEx=%p GetFinalPathNameByHandleW=%p)",
        (void *)p_SetFilePointerEx, (void *)p_FinalPath);
}

int ShForgeIoFixups(void) {
    return (int)g_fixups;
}
