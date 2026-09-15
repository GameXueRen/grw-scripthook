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
 * Which handle is which comes from the interception layer
 * (scripthook_files.c), which owns CreateFileW for the whole process:
 * this module registers an open rule and is told the path at the moment
 * the file is opened, so a .forge handle never has to be asked who it is.
 * GetFinalPathNameByHandleW stays as the fallback for a handle opened
 * before these rules went in - the engine opens some of its archives
 * during start up, ahead of the loader thread - and the answer, found or
 * not, is cached either way, so each handle costs one resolve for the
 * session. Reading is what matters anyway: an archive that is never read
 * is never a problem.
 *
 * The overlay is still built at the first read of an archive rather than
 * at open, which is where the tables (a few MB) are parsed and the mod
 * payloads are read into memory.
 *
 * The hooks this module used to install are gone. It asks the layer for
 * two groups - the reads and the completion path - and the callbacks
 * below run from the layer's dispatch, on the engine's own thread, at the
 * point in the call where they used to run. Reentrancy is the layer's
 * (its own dispatch never comes back here), so the thread-local busy flag
 * this module used to keep is gone with the hooks.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* SH_BUILD before scripthook.h, like every other framework source:
 * without it SH_API expands to nothing and the read ledger's exports
 * are not in the DLL at all. */
#define SH_BUILD 1
#include "scripthook.h"
#include "forge.h"
#include "log.h"

#define IO_CACHE    256    /* direct mapped by handle; handles are few */
#define PENDING_MAX 64     /* asynchronous reads in flight that matter */

typedef BOOL   (WINAPI *SetFilePointerEx_t)(HANDLE, LARGE_INTEGER,
                                            PLARGE_INTEGER, DWORD);
typedef DWORD  (WINAPI *GetFinalPathNameByHandleW_t)(HANDLE, LPWSTR, DWORD,
                                                     DWORD);

/* The two this module still calls rather than hooks: SetFilePointerEx to
 * ask a synchronous read where it starts, GetFinalPathNameByHandleW to
 * name a handle that was opened before the layer's rules went in. Both
 * are resolved at startup and called from inside a callback, where the
 * layer passes its own calls straight through. */
static SetFilePointerEx_t            p_SetFilePointerEx;
static GetFinalPathNameByHandleW_t   p_FinalPath;

/* Handles by what they are. `g_pathH/g_pathP` is the layer's open rule
 * saying "this handle is this .forge", written at open; `g_cacheH/g_cacheO`
 * is the overlay resolved at the first read, which is the expensive part
 * and stays lazy. A handle that is in neither is resolved the old way, by
 * asking it. */
static HANDLE          g_cacheH[IO_CACHE];
static ShForgeOverlay *g_cacheO[IO_CACHE];
static HANDLE          g_pathH[IO_CACHE];
static char            g_pathP[IO_CACHE][SH_FORGE_PATH_MAX + 8];
static volatile LONG   g_fixups;
static volatile LONG   g_gorCalls;

/* ---- the read ledger ------------------------------------------------
 *
 * Every .forge the engine reads has to be resolved here - that is how a
 * handle is matched to an archive - so this is the one place that can
 * answer "which archives did this session actually load" without
 * scanning gigabytes of memory or trusting a name to be in it.
 *
 * It is worth having because it is mode evidence: a mode that mounts an
 * archive of its own reads a file the other modes never touch, and a
 * session that read it cannot have been in the other mode. Read, not
 * opened: an archive the engine opens but never reads is not loaded, and
 * the handle cache means this is written once per handle, not per read.
 *
 * Measured on a retail install, the engine reads rather than maps its
 * archives (47 .forge opens, 646 reads, not one CreateFileMapping or
 * MapViewOfFile on a .forge - docs/forge-mod-loader.md), so a mapped
 * file name would have missed all of them.
 */
#define LEDGER_MAX  64
#define LEDGER_NAME 96

static char               g_ledger[LEDGER_MAX][LEDGER_NAME];
static volatile LONG      g_ledgerCount;
static CRITICAL_SECTION   g_ledgerLock;

static int LedgerEqI(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

/* Case insensitive substring, for "does any read archive name hold
 * this": callers pass a fragment like "GhostRoom", not a full name. */
static int LedgerHasI(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    const char *p;

    if (!n) return 0;
    for (p = hay; *p; p++) {
        size_t i;
        for (i = 0; i < n; i++) {
            char c = p[i];
            char d = needle[i];
            if (!c) return 0;
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (d >= 'A' && d <= 'Z') d += 'a' - 'A';
            if (c != d) break;
        }
        if (i == n) return 1;
    }
    return 0;
}

static void LedgerAdd(const char *path) {
    const char *b = path, *s;
    char name[LEDGER_NAME];
    LONG count, i;
    size_t n;

    for (s = path; *s; s++) {
        if (*s == '\\' || *s == '/') b = s + 1;
    }
    n = strlen(b);
    if (!n || n >= LEDGER_NAME) return;
    memcpy(name, b, n);
    name[n] = 0;

    EnterCriticalSection(&g_ledgerLock);
    count = g_ledgerCount;
    for (i = 0; i < count; i++) {
        if (LedgerEqI(g_ledger[i], name)) {
            LeaveCriticalSection(&g_ledgerLock);
            return;
        }
    }
    if (count < LEDGER_MAX) {
        memcpy(g_ledger[count], name, n + 1);
        InterlockedIncrement(&g_ledgerCount);
    }
    LeaveCriticalSection(&g_ledgerLock);
}

SH_API int ShForgeReadCount(void) {
    return (int)g_ledgerCount;
}

SH_API const char *ShForgeReadName(int index) {
    if (index < 0 || index >= (int)g_ledgerCount) return "";
    return g_ledger[index];
}

SH_API int ShForgeReadSeen(const char *name) {
    LONG count, i;
    int found = 0;

    if (!name || !name[0]) return 0;
    EnterCriticalSection(&g_ledgerLock);
    count = g_ledgerCount;
    for (i = 0; i < count; i++) {
        if (LedgerHasI(g_ledger[i], name)) { found = 1; break; }
    }
    LeaveCriticalSection(&g_ledgerLock);
    return found;
}

/* The loader's own archive I/O: the FileDataID index pass and the mod
 * resolve read every archive on disk, and those reads must stay out of
 * the ledger (which is what the ENGINE read) and out of this module's own
 * rules while an overlay is being built. That mark is the layer's now
 * (ShFileOwn); this is the same flag, under the name the loader calls it
 * by. */
void ShForgeIoOwn(int on) {
    ShFileOwn(on);
}

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

/* ---- the mode gate --------------------------------------------------
 * Ghost War (4v4) and Mercenaries (the eight player PvPvE mode) are the
 * two places where serving a swapped archive entry could hand someone an
 * edge: Forge replaces entries in the game's OWN archives, weapon data
 * among them. So while either is the selected mode nothing is served -
 * the read goes back to the engine untouched.
 *
 * The declaration is in the source, deliberately: like a plugin's
 * blacklist bitmask, no ini can widen or narrow it. The menu is left
 * alone too - the page is not hidden, only its effect is held; a player
 * wondering why a mod stopped showing up gets the log line below.
 *
 * The mode is asked only where a patch would actually land (see
 * CountFixup) plus once per read that could have built an overlay, so the
 * cost is nothing next to the reads themselves. A mode the framework has
 * not read yet (SH_PLAYMODE_NONE, which is also what the front end
 * reports) blocks nobody - the same rule the plugin blacklist uses.
 */
#define FORGE_BLOCKED_MODES (SH_MODE_BLACKLIST_GHOST_WAR | \
                             SH_MODE_BLACKLIST_MERCENARIES)

/* The last answer, so the transition is logged once. A race can at worst
 * print the line twice, which is why a plain volatile read is enough. */
static volatile LONG g_gateOn = 0;

static int ForgeBlocked(void) {
    const char *name;
    int mode = ShSelectedPlayMode();
    int blocked;

    if (mode == SH_PLAYMODE_NONE) return 0;
    blocked = (ShPlayModeBit(mode) & FORGE_BLOCKED_MODES) != 0;

    if (blocked != (int)g_gateOn) {
        g_gateOn = blocked;
        name = ShPlayModeName(mode);
        Log("mode gate: %s - %s", name && name[0] ? name : "unknown mode",
            blocked ? "mods\\ is not served" : "mods\\ is served again");
    }
    return blocked;
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
    int c;

    /* Every patch path comes through here - synchronous, completed in
     * place, and the pending sweep - so this one check also covers a read
     * that was already in flight when the mode changed. */
    if (ForgeBlocked()) return;

    c = (int)InterlockedIncrement(&g_fixups);
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
    if (!g_lockReady || g_pendingCount == 0) return;

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
        if (n) CountFixup(pr.o, pr.off, pr.buf, n, "async-done");
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

    /* The layer's open rule said where this handle came from, so the usual
     * case is a table lookup and the archive is resolved from the name it
     * was opened with. Asking the handle is what is left for one opened
     * before those rules went in - the engine opens archives during start
     * up, ahead of the loader thread. */
    if (g_pathH[slot] == h && g_pathP[slot][0]) {
        LedgerAdd(g_pathP[slot]);
        o = ShForgeDryRun() ? NULL : ShForgeOverlayFor(g_pathP[slot]);
        if (o) g_cacheO[slot] = o;
        return o;
    }

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
        /* Resolved here and nowhere else, so this is also where the read
         * ledger is written. A dry run still wants the ledger, so the
         * overlay is what gets dropped, not the resolve. */
        LedgerAdd(p);
        o = ShForgeDryRun() ? NULL : ShForgeOverlayFor(p);
    }

    if (o) g_cacheO[slot] = o;
    /* No per-handle log here: the cache is small enough that ordinary
     * traffic evicts and re-resolves, and the overlay build already
     * announces itself once. */
    return o;
}

/* ---- what the layer runs -------------------------------------------
 *
 * Three rules, registered at startup (ShForgeIoStartup below), and the
 * callbacks they name. Each one runs on the engine's own thread, inside
 * the call it is about, which is exactly where the old detours ran - so
 * the logic below is that logic with the call already made and its result
 * in the context. Reentrancy is the layer's: anything these callbacks do
 * themselves - resolving an overlay reads the mod files - passes straight
 * through without coming back here.
 */

/* The path of an open call in ANSI, with the \\?\ form folded away. 1 when
 * it is a .forge, and then `out` holds the name the overlay is keyed by. */
static int ForgePathOf(const ShFileCall *c, char *out, int n) {
    char tmp[SH_FORGE_PATH_MAX + 8];
    const char *p;

    if (c->pathA) {
        snprintf(tmp, sizeof(tmp), "%s", c->pathA);
    } else if (c->path) {
        if (WideCharToMultiByte(CP_ACP, 0, c->path, -1, tmp, sizeof(tmp),
                                NULL, NULL) <= 0)
            return 0;
    } else {
        return 0;
    }

    p = tmp;
    if (p[0] == '\\' && p[1] == '\\' && p[2] == '?' && p[3] == '\\') p += 4;
    if (!EndsWithForge(p)) return 0;
    snprintf(out, n, "%s", p);
    return 1;
}

/* Where a .forge handle came from, remembered at the moment it is opened.
 * The ledger is NOT written here: it records what the engine READ, and an
 * archive that is opened but never read is not loaded. */
static void ForgeOpenAfter(ShFileCall *c, void *user) {
    unsigned slot;
    HANDLE   h = (HANDLE)c->result;

    (void)user;
    if (!h || h == INVALID_HANDLE_VALUE) return;

    slot = SlotOf(h);
    if (!ForgePathOf(c, g_pathP[slot], (int)sizeof(g_pathP[slot]))) {
        /* Not an archive this module serves - and a recycled handle value
         * must not inherit an older mapping. */
        if (g_pathH[slot]) {
            g_pathH[slot] = NULL;
            g_pathP[slot][0] = 0;
        }
        return;
    }
    g_pathH[slot] = h;
}

/* A read: patch the buffer when it covers a byte a mod replaced. The
 * asynchronous half is mechanism-agnostic on purpose - a read that came
 * back pending is remembered and patched by whichever completion the
 * engine uses (see ForgeWaitAfter). */
static void ForgeReadAfter(ShFileCall *c, void *user) {
    ShForgeOverlay *o = NULL;
    uint64_t off = 0;
    int haveOff = 0;
    HANDLE h;

    (void)user;
    if (!c->api || _stricmp(c->api, "ReadFile") != 0) return;
    if (!c->buffer) return;
    /* A call that failed outright, and is not in flight, read nothing: it
     * has no patch, and it is not a read of an archive either. */
    if (!c->result && c->error != ERROR_IO_PENDING) return;

    h = c->handle;

    /* Also covers a caller that polls instead of waiting. */
    PendingSweep();

    /* The mode gate comes first: while a PvP mode is selected nothing is
     * served, and an overlay must not even be built - that is what reads
     * the mod payloads and parses the archive's tables. The sweep above
     * still ran, so the pending table stays clean, and CountFixup refuses
     * any patch it finds there. */
    if (ForgeBlocked()) return;

    o = LookupOrResolve(h);
    if (o) {
        if (c->overlapped) {
            /* The asynchronous handle: its file pointer never moves, so
             * the OVERLAPPED is the only place the offset exists, and the
             * layer has already read it out for us. */
            off = c->offset;
            haveOff = 1;
        } else if (p_SetFilePointerEx) {
            /* A zero distance at FILE_CURRENT only asks where the read
             * ENDED: this is the after callback, so the file pointer has
             * already moved by c->done. The read's start is that minus the
             * transfer - the same arithmetic as the evidence probe in
             * scripthook_forgeprobe.c. Using the pointer as-is shifted
             * every patch by the number of bytes just read, which made
             * TouchesOverlay miss (or patch the wrong range). */
            LARGE_INTEGER cur, zero;
            zero.QuadPart = 0;
            if (p_SetFilePointerEx(h, zero, &cur, FILE_CURRENT)) {
                off = (uint64_t)cur.QuadPart - (uint64_t)c->done;
                haveOff = 1;
            }
        }
    }

    if (o && haveOff && ShForgeLogReads())
        Log("read: off=%llu len=%lu %s hit=%d",
            (unsigned long long)off, (unsigned long)c->bytes,
            c->overlapped ? "async" : "sync", TouchesOverlay(o, off, c->bytes));

    if (!o || !haveOff || !TouchesOverlay(o, off, c->bytes)) {
        /* Nothing to patch in this read; leave it entirely alone, its
         * asynchronous behaviour included. */
        return;
    }

    if (c->overlapped) {
        LPOVERLAPPED ov = (LPOVERLAPPED)c->overlapped;

        if (c->result) {
            /* It completed right here, which small reads often do. The
             * byte count comes from the count parameter when the caller
             * passed one and from the OVERLAPPED otherwise, because
             * passing none is the usual thing for overlapped I/O and
             * reading that as "no bytes" would drop the patch. */
            DWORD n = c->done ? c->done : (DWORD)ov->InternalHigh;
            if (n) {
                CountFixup(o, off, (uint8_t *)c->buffer, n, "async-now");
            } else if (ShForgeLogReads()) {
                Log("read/async-now: no byte count (intHigh=%lu)",
                    (unsigned long)ov->InternalHigh);
            }
        } else if (c->error == ERROR_IO_PENDING) {
            /* Still in flight: remembered, and patched once the engine
             * asks for the result or waits on it. */
            PendingAdd(h, ov, c->buffer, c->bytes, off, o);
            if (ShForgeLogReads())
                Log("read/async-pending: recorded off=%llu len=%lu",
                    (unsigned long long)off, (unsigned long)c->bytes);
        }
        return;
    }

    /* Synchronous, and it covers a patch: patch what it read. */
    if (c->result && c->done)
        CountFixup(o, off, (uint8_t *)c->buffer, c->done, "sync");
}

/* The completion path. Two of these calls name the OVERLAPPED, so the
 * read they finish can be matched exactly; the waits cannot (they are
 * given a handle, an event or an array of them), so they get the sweep,
 * which patches everything whose transfer has finished. CloseHandle is
 * here for a different reason: the handle caches have to forget it, or a
 * later file that reuses the handle value would inherit an archive's
 * patches - a mistake that would corrupt data silently. */
static void ForgeWaitAfter(ShFileCall *c, void *user) {
    PendingRead pr;

    (void)user;
    if (!c->api) return;

    if (_stricmp(c->api, "CloseHandle") == 0) {
        unsigned slot = SlotOf(c->handle);

        if (g_cacheH[slot] == c->handle) {
            g_cacheH[slot] = NULL;
            g_cacheO[slot] = NULL;
        }
        if (g_pathH[slot] == c->handle) {
            g_pathH[slot] = NULL;
            g_pathP[slot][0] = 0;
        }
        return;
    }

    if (_stricmp(c->api, "GetOverlappedResult") == 0 ||
        _stricmp(c->api, "GetOverlappedResultEx") == 0) {
        /* Only consume the record once the transfer really finished: a
         * caller that polls with bWait = FALSE reports "not yet" many
         * times before it reports success, and treating the first of those
         * as the end would throw the patch away. */
        if (c->result && PendingTake((LPOVERLAPPED)c->overlapped, &pr)) {
            DWORD n = c->done;

            if (ShForgeLogReads()) Log("gor: matched=1 ok=1");
            if (n) {
                if (n > pr.len) n = pr.len;
                CountFixup(pr.o, pr.off, pr.buf, n, "async-done");
            }
        } else if (ShForgeLogReads() &&
                   InterlockedIncrement(&g_gorCalls) <= 8) {
            Log("gor: matched=0 ok=%d", c->result ? 1 : 0);
        }
        return;
    }

    /* The waits - and this runs after the real wait has returned, which is
     * the point: the patch lands before the caller can look at the buffer,
     * on the way back out of the call that told it the read was done. */
    PendingSweep();
}

/* ---- install ------------------------------------------------------- */

/* Three rules, registered once, when the forge module comes up. The layer
 * installs its hooks with the first of them and keeps them for as long as
 * any rule lives, so this module owns no hook of its own - which is what
 * removes the old CreateFile collision with GhostNoWipe and skipintro,
 * and why forgeprobe and GhostWipeProbe can now run beside it. */
void ShForgeIoStartup(void) {
    static LONG started = 0;
    ShFileRuleDesc d;
    HMODULE k32;

    if (InterlockedExchange(&started, 1)) return;

    LogInit("scripthook_forge_io.log");

    InitializeCriticalSection(&g_lock);
    g_lockReady = 1;
    InitializeCriticalSection(&g_ledgerLock);

    /* Called, never hooked: SetFilePointerEx asks a synchronous read where
     * it starts, and GetFinalPathNameByHandleW is the fallback for a
     * handle opened before the layer's rules went in. */
    k32 = GetModuleHandleA("kernel32.dll");
    p_SetFilePointerEx = (SetFilePointerEx_t)GetProcAddress(
        k32, "SetFilePointerEx");
    p_FinalPath = (GetFinalPathNameByHandleW_t)GetProcAddress(
        k32, "GetFinalPathNameByHandleW");

    Log("forge io installing");

    /* Where a .forge handle came from: the open, watched. */
    memset(&d, 0, sizeof(d));
    d.group  = SH_FILE_OPEN;
    d.action = SH_FILE_DECIDE;          /* no before: watch, never answer */
    d.after  = ForgeOpenAfter;
    if (!ShFileRuleAdd(&d)) Log("  open rule        REFUSED");

    /* The reads themselves. */
    memset(&d, 0, sizeof(d));
    d.group  = SH_FILE_READ;
    d.action = SH_FILE_DECIDE;
    d.after  = ForgeReadAfter;
    if (!ShFileRuleAdd(&d)) Log("  read rule        REFUSED");

    /* And the completion path, which is what closes the asynchronous
     * half: close, the results, and the waits. */
    memset(&d, 0, sizeof(d));
    d.group  = SH_FILE_WAIT;
    d.action = SH_FILE_DECIDE;
    d.after  = ForgeWaitAfter;
    if (!ShFileRuleAdd(&d)) Log("  completion rule  REFUSED");

    Log("forge io installed (SetFilePointerEx=%p GetFinalPathNameByHandleW=%p)",
        (void *)p_SetFilePointerEx, (void *)p_FinalPath);
}

int ShForgeIoFixups(void) {
    return (int)g_fixups;
}
