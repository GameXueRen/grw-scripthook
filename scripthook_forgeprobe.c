/* Forge Mod Loader, evidence round: how does the engine actually read a
 * .forge archive?
 *
 * The plan is to serve a modded .forge without touching the file on disk:
 * keep the game's own handle open on the vanilla archive and, at read
 * time, hand back mod bytes for the handful of byte ranges a mod
 * replaces. That is only possible if the engine reads through the
 * handle-based calls - ReadFile with a moving file pointer. If it maps
 * the whole archive with CreateFileMappingA + MapViewOfFile instead, the
 * bytes live in a view and the same trick has to be built another way
 * (a writable view of our own, or the synthetic-archive route), so this
 * single answer decides the design.
 *
 * This module therefore records, and changes nothing:
 *
 *   1. Which reading style is used for .forge files, and how the two
 *      split. ReadFile + SetFilePointer is the one an overlay needs.
 *   2. When the archives are opened, relative to this DLL's loader
 *      thread. If the opens are already over by the time the hooks go
 *      in, the log shows none at all and the hooks have to move earlier.
 *   3. The exact open parameters: path form, access, share, disposition
 *      and flags. FILE_FLAG_NO_BUFFERING or an overlapped open would
 *      change how an overlay has to answer.
 *   4. Whether the engine enumerates the folder for archives at all
 *      (FindFirstFile* on "*.forge") - i.e. whether a newly dropped
 *      archive can be discovered, or whether the set of archives is the
 *      fixed table that is in the exe.
 *
 * Every hook records and then calls the original. Nothing is blocked and
 * no answer is altered; the mod loader is written after this is read.
 * Output: <gamedir>\logs\forge_probe.log.
 *
 * This lives in dinput8.dll rather than in a plugins\ .asi, so it is not
 * gated by [loader] load_plugins and still runs with the other plugins
 * switched off - which is how it should be run, because GhostNoWipe
 * hooks CreateFileW too and MinHook keeps one hook per target address.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scripthook.h"
#include "log.h"

/* ---- limits kept small enough that the log stays readable ---------- */

#define FORGE_HANDLE_MAX 64      /* archives open at once; 21 exist     */
#define MAP_HANDLE_MAX   64
#define READS_PER_HANDLE 24      /* first N reads per archive, then a
                                  * running total only                   */
#define SEEK_PER_HANDLE  8

/* The one API this module still calls rather than watches: a synchronous
 * read's offset is not in the call, so the file pointer is asked where it
 * ended up and the transferred count is taken back off it. Resolved at
 * startup; the layer passes a callback's own calls straight through, so
 * this cannot come back into the probe. */
typedef BOOL (WINAPI *SetFilePointerEx_t)(HANDLE, LARGE_INTEGER,
                                          PLARGE_INTEGER, DWORD);
static SetFilePointerEx_t p_SeekEx;

/* log.h is included, so this translation unit gets its own static
 * g_logFile / LogInit / Log: the probe writes its own file and does not
 * disturb the loader's scripthook.log. */
#define P Log

/* ---- tracked handles ----------------------------------------------- */

typedef struct {
    HANDLE h;
    char   path[MAX_PATH];
    int    reads;
    int    seeks;
    int    sizeLogged;
    unsigned long long bytes;
} ForgeHandle;

typedef struct {
    HANDLE map;      /* the mapping object handle */
    HANDLE file;     /* the .forge handle it came from */
} MapHandle;

static ForgeHandle g_fh[FORGE_HANDLE_MAX];
static MapHandle   g_mh[MAP_HANDLE_MAX];
static CRITICAL_SECTION g_lock;

static int ContainsForgeA(const char *s) {
    if (!s) return 0;
    for (; s[0]; s++) {
        if (s[0] == '.' &&
            (s[1] == 'f' || s[1] == 'F') &&
            (s[2] == 'o' || s[2] == 'O') &&
            (s[3] == 'r' || s[3] == 'R') &&
            (s[4] == 'g' || s[4] == 'G') &&
            (s[5] == 'e' || s[5] == 'E'))
            return 1;
    }
    return 0;
}

static int ContainsForgeW(const wchar_t *s) {
    if (!s) return 0;
    for (; s[0]; s++) {
        if (s[0] == L'.' &&
            (s[1] == L'f' || s[1] == L'F') &&
            (s[2] == L'o' || s[2] == L'O') &&
            (s[3] == L'r' || s[3] == L'R') &&
            (s[4] == L'g' || s[4] == L'G') &&
            (s[5] == L'e' || s[5] == L'E'))
            return 1;
    }
    return 0;
}

/* A path or a folder pattern: the callers decide which they log. */
static void Wide2Ansi(const wchar_t *w, char *out, int n) {
    out[0] = 0;
    if (w) WideCharToMultiByte(CP_ACP, 0, w, -1, out, n, NULL, NULL);
}

static ForgeHandle *FindHandle(HANDLE h) {
    int i;
    if (!h || h == INVALID_HANDLE_VALUE) return NULL;
    for (i = 0; i < FORGE_HANDLE_MAX; i++)
        if (g_fh[i].h == h) return &g_fh[i];
    return NULL;
}

static void TrackHandle(HANDLE h, const char *path) {
    int i;
    if (!h || h == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < FORGE_HANDLE_MAX; i++)
        if (g_fh[i].h == NULL) {
            g_fh[i].h = h;
            snprintf(g_fh[i].path, sizeof(g_fh[i].path), "%s", path);
            break;
        }
    LeaveCriticalSection(&g_lock);
}

static void TrackMap(HANDLE map, HANDLE file) {
    int i;
    if (!map || map == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < MAP_HANDLE_MAX; i++)
        if (g_mh[i].map == NULL) {
            g_mh[i].map = map;
            g_mh[i].file = file;
            break;
        }
    LeaveCriticalSection(&g_lock);
}

static ForgeHandle *MapFile(HANDLE map) {
    int i;
    if (!map) return NULL;
    for (i = 0; i < MAP_HANDLE_MAX; i++)
        if (g_mh[i].map == map) return FindHandle(g_mh[i].file);
    return NULL;
}

/* ---- the originals ------------------------------------------------- */

typedef HANDLE (WINAPI *CreateFileW_t)(LPCWSTR, DWORD, DWORD,
                                       LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                       HANDLE);
typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD,
                                       LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                       HANDLE);
typedef BOOL   (WINAPI *ReadFile_t)(HANDLE, LPVOID, DWORD, LPDWORD,
                                    LPOVERLAPPED);
typedef BOOL   (WINAPI *SetFilePointerEx_t)(HANDLE, LARGE_INTEGER,
                                            PLARGE_INTEGER, DWORD);
typedef DWORD  (WINAPI *SetFilePointer_t)(HANDLE, LONG, PLONG, DWORD);
typedef BOOL   (WINAPI *GetFileSizeEx_t)(HANDLE, PLARGE_INTEGER);
typedef DWORD  (WINAPI *GetFileSize_t)(HANDLE, LPDWORD);
typedef HANDLE (WINAPI *CreateFileMappingA_t)(HANDLE, LPSECURITY_ATTRIBUTES,
                                              DWORD, DWORD, DWORD, LPCSTR);
typedef HANDLE (WINAPI *CreateFileMappingW_t)(HANDLE, LPSECURITY_ATTRIBUTES,
                                              DWORD, DWORD, DWORD, LPCWSTR);
typedef LPVOID (WINAPI *MapViewOfFile_t)(HANDLE, DWORD, DWORD, DWORD,
                                         SIZE_T);
typedef BOOL   (WINAPI *UnmapViewOfFile_t)(LPCVOID);
typedef HANDLE (WINAPI *FindFirstFileW_t)(LPCWSTR, LPWIN32_FIND_DATAW);
typedef HANDLE (WINAPI *FindFirstFileExW_t)(LPCWSTR, FINDEX_INFO_LEVELS,
                                            LPVOID, FINDEX_SEARCH_OPS,
                                            LPVOID, DWORD);

static CreateFileW_t        r_CreateFileW;
static CreateFileA_t        r_CreateFileA;
static ReadFile_t           r_ReadFile;
static SetFilePointerEx_t   r_SetFilePointerEx;
static SetFilePointer_t     r_SetFilePointer;
static GetFileSizeEx_t      r_GetFileSizeEx;
static GetFileSize_t        r_GetFileSize;
static CreateFileMappingA_t r_CreateFileMappingA;
static CreateFileMappingW_t r_CreateFileMappingW;
static MapViewOfFile_t      r_MapViewOfFile;
static UnmapViewOfFile_t    r_UnmapViewOfFile;
static FindFirstFileW_t     r_FindFirstFileW;
static FindFirstFileExW_t   r_FindFirstFileExW;

/* ---- hooks --------------------------------------------------------- */

/* ---- what the layer runs -------------------------------------------
 *
 * One watcher, registered at startup, and this callback. It runs inside
 * the call, on the engine's own thread, so it is the same evidence the
 * old per-API detours recorded - with one reduction: the arguments the
 * layer's context does not carry are no longer in the log (a CreateFile's
 * access/share/disp/flags, a view's access mask). Those were colour. What
 * this probe was built to answer - which calls the engine makes against a
 * .forge, and whether it reads them or maps them - is all still here, one
 * line per call, first few per archive.
 */

/* The path of a call in ANSI, for the log and for the handle table. */
static int PathAnsiOf(const ShFileCall *c, char *out, int n) {
    if (c->pathA) {
        snprintf(out, n, "%s", c->pathA);
        return 1;
    }
    if (c->path) {
        Wide2Ansi(c->path, out, n);
        return out[0] != 0;
    }
    return 0;
}

static void ProbeAfter(ShFileCall *c, void *user) {
    const char *api = c->api ? c->api : "?";
    char        ansi[MAX_PATH];
    ForgeHandle *fh;

    (void)user;

    /* An open: only the archives are interesting, and the handle has to be
     * remembered whatever else happens. */
    if (_stricmp(api, "CreateFileW") == 0 || _stricmp(api, "CreateFileA") == 0) {
        HANDLE h = (HANDLE)c->result;

        if (!PathAnsiOf(c, ansi, sizeof(ansi))) return;
        if (!ContainsForgeA(ansi)) return;
        P("%-12s ok=%d err=%lu\n             %s", api,
          h && h != INVALID_HANDLE_VALUE, (unsigned long)c->error, ansi);
        if (h && h != INVALID_HANDLE_VALUE) TrackHandle(h, ansi);
        return;
    }

    if (_stricmp(api, "ReadFile") == 0) {
        fh = FindHandle(c->handle);
        if (!fh) return;
        fh->bytes += c->done;
        if (fh->reads < READS_PER_HANDLE) {
            /* A synchronous read has no OVERLAPPED, so its offset is not in
             * the call: the file pointer has already moved by the time this
             * runs, which is exactly enough to recover where it started. */
            long long off = (long long)c->offset;
            if (!c->overlapped && c->result && p_SeekEx) {
                LARGE_INTEGER cur, zero;
                zero.QuadPart = 0;
                if (p_SeekEx(c->handle, zero, &cur, FILE_CURRENT))
                    off = (long long)cur.QuadPart - (long long)c->done;
            }
            P("ReadFile     %s\n             off=%lld len=%lu got=%lu ok=%d",
              fh->path, off, (unsigned long)c->bytes,
              (unsigned long)c->done, c->result ? 1 : 0);
        } else if (fh->reads == READS_PER_HANDLE) {
            P("ReadFile     %s  (further reads counted only)", fh->path);
        }
        fh->reads++;
        return;
    }

    if (_stricmp(api, "SetFilePointerEx") == 0) {
        fh = FindHandle(c->handle);
        if (fh && fh->seeks < SEEK_PER_HANDLE)
            P("SeekEx       %s  -> off=%lld ok=%d", fh->path,
              (long long)c->offset, c->result ? 1 : 0);
        if (fh) fh->seeks++;
        return;
    }

    if (_stricmp(api, "SetFilePointer") == 0) {
        fh = FindHandle(c->handle);
        if (fh && fh->seeks < SEEK_PER_HANDLE)
            P("Seek         %s  method=%lu -> %lu", fh->path,
              (unsigned long)c->bytes, (unsigned long)(uintptr_t)c->result);
        if (fh) fh->seeks++;
        return;
    }

    if (_stricmp(api, "GetFileSizeEx") == 0 ||
        _stricmp(api, "GetFileSize") == 0) {
        fh = FindHandle(c->handle);
        if (fh && !fh->sizeLogged) {
            P("GetFileSize  %s  size=%lld ok=%d", fh->path,
              (long long)c->offset, c->result ? 1 : 0);
            fh->sizeLogged = 1;
        }
        return;
    }

    if (_stricmp(api, "CreateFileMappingA") == 0 ||
        _stricmp(api, "CreateFileMappingW") == 0) {
        HANDLE m = (HANDLE)c->result;

        fh = FindHandle(c->handle);
        if (!fh) return;
        P("MapCreate    %s  protect=0x%08lX size=%08llX -> %p", fh->path,
          (unsigned long)c->bytes, (unsigned long long)c->offset, (void *)m);
        if (m) TrackMap(m, c->handle);
        return;
    }

    if (_stricmp(api, "MapViewOfFile") == 0) {
        fh = MapFile(c->handle);
        if (fh)
            P("MapView      %s  off=%08llX size=%lu -> %p", fh->path,
              (unsigned long long)c->offset, (unsigned long)c->bytes,
              c->result);
        return;
    }

    if (_stricmp(api, "UnmapViewOfFile") == 0) {
        P("UnmapView    %p", c->buffer);
        return;
    }

    if (_stricmp(api, "FindFirstFileW") == 0 ||
        _stricmp(api, "FindFirstFileExW") == 0) {
        HANDLE h = (HANDLE)c->result;

        if (!PathAnsiOf(c, ansi, sizeof(ansi))) return;
        if (!ContainsForgeA(ansi)) return;
        if (h && h != INVALID_HANDLE_VALUE && c->buffer &&
            _stricmp(api, "FindFirstFileW") == 0)
            P("FindFirstW   %s  -> first=%ls", ansi,
              ((LPWIN32_FIND_DATAW)c->buffer)->cFileName);
        else
            P("FindFirstW   %s  -> %s", ansi,
              h != INVALID_HANDLE_VALUE ? "ok" : "none");
        return;
    }

    (void)fh;
}



/* ---- install ------------------------------------------------------- */

/* One watcher, over the three groups this probe used to hook by hand: the
 * opens, the reads (the read itself, the position it starts from, the size
 * and the mapping calls) and the finds. A DECIDE rule with only an after
 * callback changes nothing - it looks - and the layer owns the hooks, so
 * this fires whatever the rest of the process is doing and costs a
 * trampoline nothing when the probe is switched off in scripthook.ini. */
void ShForgeProbeStartup(void) {
    static LONG started = 0;
    ShFileRuleDesc d;

    if (InterlockedExchange(&started, 1)) return;

    LogInit("forge_probe.log");
    InitializeCriticalSection(&g_lock);

    Log("forge probe starting (built " __DATE__ " " __TIME__ ")");

    p_SeekEx = (SetFilePointerEx_t)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "SetFilePointerEx");

    memset(&d, 0, sizeof(d));
    d.group  = SH_FILE_OPEN | SH_FILE_READ | SH_FILE_FIND;
    d.action = SH_FILE_DECIDE;
    d.after  = ProbeAfter;
    if (!ShFileRuleAdd(&d)) {
        Log("forge probe: the layer refused the rule - not installed");
        return;
    }
    Log("forge probe installed (watching through the framework's layer)");
}
