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

#include "log.h"
#include "third_party/minhook/include/MinHook.h"

/* ---- limits kept small enough that the log stays readable ---------- */

#define FORGE_HANDLE_MAX 64      /* archives open at once; 21 exist     */
#define MAP_HANDLE_MAX   64
#define READS_PER_HANDLE 24      /* first N reads per archive, then a
                                  * running total only                   */
#define SEEK_PER_HANDLE  8

#ifdef _MSC_VER
#define PROBE_TLS __declspec(thread)
#else
#define PROBE_TLS __thread
#endif

static PROBE_TLS int t_busy;      /* re-entrancy guard, per thread      */

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

static HANDLE WINAPI H_CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                   DWORD flags, HANDLE tmpl) {
    HANDLE h = r_CreateFileW(name, access, share, sa, disp, flags, tmpl);
    if (!t_busy && ContainsForgeW(name)) {
        char ansi[MAX_PATH];
        Wide2Ansi(name, ansi, sizeof(ansi));
        t_busy = 1;
        P("CreateFileW  ok=%d err=%lu access=0x%08lX share=0x%lX disp=%lu "
          "flags=0x%08lX\n             %s",
          h != INVALID_HANDLE_VALUE, (unsigned long)GetLastError(),
          (unsigned long)access, (unsigned long)share,
          (unsigned long)disp, (unsigned long)flags, ansi);
        t_busy = 0;
        TrackHandle(h, ansi);
    }
    return h;
}

static HANDLE WINAPI H_CreateFileA(LPCSTR name, DWORD access, DWORD share,
                                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                   DWORD flags, HANDLE tmpl) {
    HANDLE h = r_CreateFileA(name, access, share, sa, disp, flags, tmpl);
    if (!t_busy && ContainsForgeA(name)) {
        t_busy = 1;
        P("CreateFileA  ok=%d err=%lu access=0x%08lX disp=%lu flags=0x%08lX\n"
          "             %s",
          h != INVALID_HANDLE_VALUE, (unsigned long)GetLastError(),
          (unsigned long)access, (unsigned long)disp, (unsigned long)flags,
          name);
        t_busy = 0;
        TrackHandle(h, name);
    }
    return h;
}

static BOOL WINAPI H_ReadFile(HANDLE h, LPVOID buf, DWORD len, LPDWORD got,
                              LPOVERLAPPED ov) {
    ForgeHandle *fh = t_busy ? NULL : FindHandle(h);
    LARGE_INTEGER off;
    BOOL ok;

    off.QuadPart = -1;
    if (fh && r_SetFilePointerEx) {
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        r_SetFilePointerEx(h, zero, &off, FILE_CURRENT);
    }
    ok = r_ReadFile(h, buf, len, got, ov);
    if (fh) {
        DWORD n = (ok && got) ? *got : 0;
        fh->bytes += n;
        if (fh->reads < READS_PER_HANDLE) {
            t_busy = 1;
            P("ReadFile     %s\n             off=%lld len=%lu got=%lu ok=%d",
              fh->path, (long long)off.QuadPart, (unsigned long)len,
              (unsigned long)n, ok);
            t_busy = 0;
        } else if (fh->reads == READS_PER_HANDLE) {
            t_busy = 1;
            P("ReadFile     %s  (further reads counted only)", fh->path);
            t_busy = 0;
        }
        fh->reads++;
    }
    return ok;
}

static BOOL WINAPI H_SetFilePointerEx(HANDLE h, LARGE_INTEGER dist,
                                      PLARGE_INTEGER out, DWORD method) {
    BOOL ok = r_SetFilePointerEx(h, dist, out, method);
    ForgeHandle *fh = t_busy ? NULL : FindHandle(h);
    if (fh && fh->seeks < SEEK_PER_HANDLE) {
        t_busy = 1;
        P("SeekEx       %s  dist=%lld method=%lu -> off=%lld",
          fh->path, (long long)dist.QuadPart, (unsigned long)method,
          (long long)((out && ok) ? out->QuadPart : -1));
        t_busy = 0;
    }
    if (fh) fh->seeks++;
    return ok;
}

static DWORD WINAPI H_SetFilePointer(HANDLE h, LONG dist, PLONG outHigh,
                                     DWORD method) {
    DWORD r = r_SetFilePointer(h, dist, outHigh, method);
    ForgeHandle *fh = t_busy ? NULL : FindHandle(h);
    if (fh && fh->seeks < SEEK_PER_HANDLE) {
        t_busy = 1;
        P("Seek         %s  dist=%ld method=%lu -> %lu",
          fh->path, (long)dist, (unsigned long)method, (unsigned long)r);
        t_busy = 0;
    }
    if (fh) fh->seeks++;
    return r;
}

static BOOL WINAPI H_GetFileSizeEx(HANDLE h, PLARGE_INTEGER out) {
    BOOL ok = r_GetFileSizeEx(h, out);
    ForgeHandle *fh = t_busy ? NULL : FindHandle(h);
    if (fh && !fh->sizeLogged) {
        t_busy = 1;
        P("GetFileSizeEx %s  size=%lld", fh->path,
          (long long)((ok && out) ? out->QuadPart : -1));
        t_busy = 0;
        fh->sizeLogged = 1;
    }
    return ok;
}

static DWORD WINAPI H_GetFileSize(HANDLE h, LPDWORD outHigh) {
    DWORD r = r_GetFileSize(h, outHigh);
    ForgeHandle *fh = t_busy ? NULL : FindHandle(h);
    if (fh && !fh->sizeLogged) {
        t_busy = 1;
        P("GetFileSize   %s  size=%lu hi=%lu", fh->path,
          (unsigned long)r,
          (unsigned long)(outHigh ? *outHigh : 0));
        t_busy = 0;
        fh->sizeLogged = 1;
    }
    return r;
}

static HANDLE WINAPI H_CreateFileMappingA(HANDLE file,
                                          LPSECURITY_ATTRIBUTES sa,
                                          DWORD protect, DWORD hi, DWORD lo,
                                          LPCSTR name) {
    HANDLE m = r_CreateFileMappingA(file, sa, protect, hi, lo, name);
    ForgeHandle *fh = t_busy ? NULL : FindHandle(file);
    if (fh) {
        t_busy = 1;
        P("MapCreateA   %s  protect=0x%08lX size=%08lX%08lX -> %p",
          fh->path, (unsigned long)protect, (unsigned)hi, (unsigned)lo,
          (void *)m);
        t_busy = 0;
        TrackMap(m, file);
    }
    return m;
}

static HANDLE WINAPI H_CreateFileMappingW(HANDLE file,
                                          LPSECURITY_ATTRIBUTES sa,
                                          DWORD protect, DWORD hi, DWORD lo,
                                          LPCWSTR name) {
    HANDLE m = r_CreateFileMappingW(file, sa, protect, hi, lo, name);
    ForgeHandle *fh = t_busy ? NULL : FindHandle(file);
    if (fh) {
        t_busy = 1;
        P("MapCreateW   %s  protect=0x%08lX size=%08lX%08lX -> %p",
          fh->path, (unsigned long)protect, (unsigned)hi, (unsigned)lo,
          (void *)m);
        t_busy = 0;
        TrackMap(m, file);
    }
    return m;
}

static LPVOID WINAPI H_MapViewOfFile(HANDLE map, DWORD access, DWORD hi,
                                     DWORD lo, SIZE_T size) {
    LPVOID p = r_MapViewOfFile(map, access, hi, lo, size);
    ForgeHandle *fh = t_busy ? NULL : MapFile(map);
    if (fh) {
        t_busy = 1;
        P("MapView      %s  access=0x%08lX off=%08lX%08lX size=%llu -> %p",
          fh->path, (unsigned long)access, (unsigned)hi, (unsigned)lo,
          (unsigned long long)size, p);
        t_busy = 0;
    }
    return p;
}

static BOOL WINAPI H_UnmapViewOfFile(LPCVOID base) {
    t_busy = 1;
    P("UnmapView    %p", (const void *)base);
    t_busy = 0;
    return r_UnmapViewOfFile(base);
}

static HANDLE WINAPI H_FindFirstFileW(LPCWSTR pat, LPWIN32_FIND_DATAW fd) {
    HANDLE h = r_FindFirstFileW(pat, fd);
    if (!t_busy && ContainsForgeW(pat)) {
        char ansi[MAX_PATH];
        Wide2Ansi(pat, ansi, sizeof(ansi));
        t_busy = 1;
        P("FindFirstW   %s  -> first=%ls", ansi,
          (h != INVALID_HANDLE_VALUE && fd) ? fd->cFileName : L"(none)");
        t_busy = 0;
    }
    return h;
}

static HANDLE WINAPI H_FindFirstFileExW(LPCWSTR pat, FINDEX_INFO_LEVELS lvl,
                                        LPVOID data, FINDEX_SEARCH_OPS ops,
                                        LPVOID filter, DWORD flags) {
    HANDLE h = r_FindFirstFileExW(pat, lvl, data, ops, filter, flags);
    if (!t_busy && ContainsForgeW(pat)) {
        char ansi[MAX_PATH];
        Wide2Ansi(pat, ansi, sizeof(ansi));
        t_busy = 1;
        P("FindFirstExW %s  -> %s", ansi,
          h != INVALID_HANDLE_VALUE ? "ok" : "none");
        t_busy = 0;
    }
    return h;
}

/* ---- install ------------------------------------------------------- */

#define HOOK(fn, det, real)                                              \
    do {                                                                 \
        MH_STATUS s_ = MH_CreateHookApi(L"kernel32.dll", fn,             \
                                        (LPVOID)(det), (LPVOID *)(real));\
        Log("  %-20s %s", fn, s_ == MH_OK ? "ok" : MH_StatusToString(s_));\
    } while (0)

void ShForgeProbeStartup(void) {
    static LONG started = 0;

    if (InterlockedExchange(&started, 1)) return;

    LogInit("forge_probe.log");
    InitializeCriticalSection(&g_lock);

    Log("forge probe starting (built " __DATE__ " " __TIME__ ")");
    /* MinHook is per-DLL, and scripthook_corefix.c already initialises it
     * from DllMain, so "already initialized" is the normal case here and
     * not a failure. */
    {
        MH_STATUS s = MH_Initialize();
        if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
            Log("MH_Initialize failed (%s); probe not installed",
                MH_StatusToString(s));
            return;
        }
    }

    HOOK("CreateFileW",         H_CreateFileW,         &r_CreateFileW);
    HOOK("CreateFileA",         H_CreateFileA,         &r_CreateFileA);
    HOOK("ReadFile",            H_ReadFile,            &r_ReadFile);
    HOOK("SetFilePointerEx",    H_SetFilePointerEx,    &r_SetFilePointerEx);
    HOOK("SetFilePointer",      H_SetFilePointer,      &r_SetFilePointer);
    HOOK("GetFileSizeEx",       H_GetFileSizeEx,       &r_GetFileSizeEx);
    HOOK("GetFileSize",         H_GetFileSize,         &r_GetFileSize);
    HOOK("CreateFileMappingA",  H_CreateFileMappingA,  &r_CreateFileMappingA);
    HOOK("CreateFileMappingW",  H_CreateFileMappingW,  &r_CreateFileMappingW);
    HOOK("MapViewOfFile",       H_MapViewOfFile,       &r_MapViewOfFile);
    HOOK("UnmapViewOfFile",     H_UnmapViewOfFile,     &r_UnmapViewOfFile);
    HOOK("FindFirstFileW",      H_FindFirstFileW,      &r_FindFirstFileW);
    HOOK("FindFirstFileExW",    H_FindFirstFileExW,    &r_FindFirstFileExW);

    MH_EnableHook(MH_ALL_HOOKS);
    Log("forge probe installed");
}
