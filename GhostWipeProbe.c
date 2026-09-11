/* How, exactly, the game wipes a Ghost Mode save on a full death.
 *
 * The symptom: in Ghost Mode, a complete death - killed outright, or
 * bleeding out after going down - ends the run, and returning to the
 * save list deletes that save. The goal is to keep the file, so that
 * the slot is still there afterwards.
 *
 * What that needs is not a guess but the answer to four questions, and
 * this plugin exists to get them:
 *
 *   1. which API the delete goes through: DeleteFile*, MoveFile*, the
 *      modern SetFileInformationByHandle(FileDispositionInfo), or a
 *      plain CreateFile that overwrites the file with nothing;
 *   2. which files are touched: just N.save, or its .save.upload
 *      companion too, or the whole slot folder;
 *   3. whether the game checks the save back afterwards - if it
 *      verifies existence, keeping the file is not enough on its own;
 *   4. whether the save list is read from disk when the screen comes
 *      back (a FindFirstFile* over the savegames folder) or kept in
 *      memory from start up. That is the whole difference between the
 *      slot reappearing right away and the slot reappearing after a
 *      restart.
 *
 * Every hook here records and then calls the original. Nothing is
 * blocked, no answer is changed, no file is preserved. That is the
 * other plugin's job, and it cannot be written sensibly until this one
 * has been read.
 *
 * What to keep in mind while reading the log:
 *   - only paths under "savegames\...\1771" are recorded, so the log
 *     stays short and to the point;
 *   - "FindFirstFileW" over that folder means the screen re-reads the
 *     disk, which is what makes a kept file reappear without a
 *     restart;
 *   - "GetFileAttributes"/"CreateFile" right after a delete means the
 *     game checks its work;
 *   - the stack frames name the module that asked, which matters
 *     because the save layer sits under Uplay's modules as much as
 *     under GRW.exe.
 *
 * The answers, measured 2026-09-11 on a region-locked build:
 *
 *   1. The wipe is a RENAME, not a delete:
 *          MoveFileExW("...\1771\18.save",
 *                      "...\1771\18.save.delete", 0xB)
 *      followed at once by an open of the new name for writing. Both
 *      slots of the pair are renamed, a few seconds apart.
 *   2. Only N.save is touched. The .save.upload companion keeps its
 *      timestamp throughout and is never opened for writing.
 *   3. The game does not check the save back within the run.
 *   4. The screen DOES re-read the disk. On the way back it issues
 *      FindFirstFileW over "...\1771\*.save", so a file that is still
 *      there is listed again as soon as the list is rebuilt. That is
 *      the whole reason the plugin after this one works.
 *
 * One further finding, which the log alone could not give - and which a
 * first reading got backwards, so read this part twice:
 *
 * The death DOES change the save, and that change is itself a record
 * the list reads. Decrypting with the algorithm the GRW save toolkit
 * uses - a 552 byte header, then an obfuscated seed, zero-key TEA, a
 * block shuffle and an xor stream - and diffing a save from before the
 * death against the one written at it shows 94% of the bytes different.
 * The format is positional, so that diff cannot locate the mark. What
 * settled it was substitution: put a pre-death file back and the slot
 * is listed; put the file written at the death there and it is not.
 *
 * The pair that first looked byte-identical was two post-death writes,
 * which is what made the content look untouched for a while.
 *
 * So the .delete is only one of the two records. Both have to be kept
 * out. And the write carrying the mark lands nine seconds BEFORE the
 * game over screen, so nothing can be decided from the screen itself.
 *
 * The caller stack names uplay_r164.dll and GRW.exe, so the wipe is
 * issued from inside the game process, under Uplay's save layer - which
 * is why the hooks here are on the functions themselves rather than on
 * any one module's import table.
 *
 * The save folder was backed up in full before this ran; the probe is
 * removed from the game folder once the questions are answered, and
 * the findings are written into the header of the plugin that follows.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "third_party/minhook/include/MinHook.h"

/* Frames of the calling stack to record per event. */
#define STACK_MAX 8

/* ---- framework state, bound by name at startup ------------------------ */

typedef int (*GetState_t)(void);
typedef int (*StateName_t)(char *buf, int len);

static GetState_t   g_getState;
static StateName_t  g_stateName;

static void ProbeLog(const char *fmt, ...);

typedef USHORT (WINAPI *CaptureStack_t)(ULONG framesToSkip,
                                        ULONG framesToCapture,
                                        PVOID *backTrace,
                                        PULONG backTraceHash);
static CaptureStack_t g_captureStack;

/* The walker lives in ntdll and is forwarded by two other modules
 * depending on the Windows version; all three names are tried. */
static void BindStackWalk(void) {
    static const char *mods[] = { "ntdll.dll", "kernelbase.dll",
                                  "kernel32.dll" };
    static const char *fns[]  = { "RtlCaptureStackBackTrace",
                                  "CaptureStackBackTrace" };
    int i, j;

    for (i = 0; i < (int)(sizeof(mods) / sizeof(mods[0])) && !g_captureStack;
         i++) {
        HMODULE m = GetModuleHandleA(mods[i]);
        if (!m) continue;
        for (j = 0; j < (int)(sizeof(fns) / sizeof(fns[0])); j++) {
            g_captureStack = (CaptureStack_t)GetProcAddress(m, fns[j]);
            if (g_captureStack) {
                ProbeLog("stack walk: %s!%s", mods[i], fns[j]);
                return;
            }
        }
    }
    ProbeLog("stack walk: not found - callers will not be named");
}

/* ---- logging ---------------------------------------------------------- */

static FILE *g_log;
static LONG  g_logBusy;

/* Set while this plugin walks the save folder itself, so its own
 * enumeration is not mistaken for the game's. */
static volatile LONG g_selfScan;

static void ProbeLog(const char *fmt, ...) {
    va_list ap;
    char line[1024];
    SYSTEMTIME st;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (!g_log) return;

    while (InterlockedExchange(&g_logBusy, 1)) Sleep(1);
    if (g_log) {
        GetLocalTime(&st);
        fprintf(g_log, "%02u:%02u:%02u.%03u  [%lu] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                (unsigned long)GetCurrentProcessId(), line);
        fflush(g_log);
    }
    InterlockedExchange(&g_logBusy, 0);
}

static void OpenLog(void) {
    char path[MAX_PATH];
    char *slash;
    int len;

    if (g_log) return;
    if (!GetModuleFileNameA(NULL, path, MAX_PATH)) return;
    slash = strrchr(path, '\\');
    if (!slash) return;
    slash[1] = 0;
    len = (int)strlen(path);
    if (len + 5 >= (int)sizeof(path)) return;
    strcpy(path + len, "logs");
    CreateDirectoryA(path, NULL);
    if (len + 24 < (int)sizeof(path))
        strcpy(path + len, "logs\\GhostWipeProbe.log");
    else
        strcpy(path + len, "GhostWipeProbe.log");
    g_log = fopen(path, "a");
}

/* ---- naming an address and a state ------------------------------------ */

static void DescribeAddr(void *addr, char *out, size_t n) {
    HMODULE mod = NULL;
    char    file[MAX_PATH];
    const char *base;

    out[0] = 0;
    if (!addr) { snprintf(out, n, "(null)"); return; }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) && mod) {
        if (!GetModuleFileNameA(mod, file, sizeof(file)))
            snprintf(file, sizeof(file), "?");
        base = strrchr(file, '\\');
        base = base ? base + 1 : file;
        snprintf(out, n, "%s+0x%llX", base,
                 (unsigned long long)((uintptr_t)addr - (uintptr_t)mod));
        return;
    }
    snprintf(out, n, "0x%p", addr);
}

static const char *StateText(void) {
    static char buf[64];
    buf[0] = 0;
    if (g_stateName) g_stateName(buf, (int)sizeof(buf));
    if (!buf[0]) snprintf(buf, sizeof(buf), "state#%d",
                          g_getState ? g_getState() : -1);
    return buf;
}

/* ---- what is worth recording ------------------------------------------ */

/* Only the save folder. "savegames" alone appears in plenty of Ubisoft
 * paths, so the game id has to follow it. */
static int IsSavePathW(const wchar_t *path) {
    const wchar_t *s;

    if (!path) return 0;
    s = wcsstr(path, L"savegames");
    return s && wcsstr(s, L"1771");
}

static int IsSavePathA(const char *path) {
    const char *s;

    if (!path) return 0;
    s = strstr(path, "savegames");
    return s && strstr(s, "1771");
}

/* One line per event, with the caller's stack under it. This is the
 * whole record: what was asked, to which file, whether it worked, and
 * who asked. */
static void Note(const char *api, const wchar_t *path, const char *extra,
                 int ok, DWORD err) {
    void *frames[STACK_MAX];
    char  name[80];
    USHORT n = 0, i;

    if (InterlockedCompareExchange(&g_selfScan, 0, 0)) return;

    ProbeLog("%-24s ok=%d err=%lu %s %ls", api, ok, (unsigned long)err,
             extra ? extra : "", path ? path : L"(null)");
    ProbeLog("    state=%s", StateText());

    if (g_captureStack)
        n = g_captureStack(1, STACK_MAX, frames, NULL);
    for (i = 0; i < n; i++) {
        DescribeAddr(frames[i], name, sizeof(name));
        ProbeLog("    frame[%u] %s", (unsigned)i, name);
    }
}

/* ---- the hooks -------------------------------------------------------- */

typedef BOOL  (WINAPI *DeleteFileW_t)(LPCWSTR);
typedef BOOL  (WINAPI *DeleteFileA_t)(LPCSTR);
typedef BOOL  (WINAPI *MoveFileExW_t)(LPCWSTR, LPCWSTR, DWORD);
typedef BOOL  (WINAPI *MoveFileW_t)(LPCWSTR, LPCWSTR);
typedef BOOL  (WINAPI *SetFileInformationByHandle_t)(HANDLE,
                                                     FILE_INFO_BY_HANDLE_CLASS,
                                                     LPVOID, DWORD);
typedef HANDLE(WINAPI *CreateFileW_t)(LPCWSTR, DWORD, DWORD,
                                      LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                      HANDLE);
typedef BOOL  (WINAPI *RemoveDirectoryW_t)(LPCWSTR);
typedef BOOL  (WINAPI *CopyFileW_t)(LPCWSTR, LPCWSTR, BOOL);
typedef HANDLE(WINAPI *FindFirstFileW_t)(LPCWSTR, LPWIN32_FIND_DATAW);
typedef HANDLE(WINAPI *FindFirstFileExW_t)(LPCWSTR, FINDEX_INFO_LEVELS,
                                           LPVOID, FINDEX_SEARCH_OPS,
                                           LPVOID, DWORD);
typedef BOOL  (WINAPI *FindNextFileW_t)(HANDLE, LPWIN32_FIND_DATAW);
typedef DWORD (WINAPI *GetFileAttributesW_t)(LPCWSTR);

static DeleteFileW_t      g_realDeleteFileW;
static DeleteFileA_t      g_realDeleteFileA;
static MoveFileExW_t      g_realMoveFileExW;
static MoveFileW_t        g_realMoveFileW;
static SetFileInformationByHandle_t g_realSetFileInformationByHandle;
static CreateFileW_t      g_realCreateFileW;
static RemoveDirectoryW_t g_realRemoveDirectoryW;
static CopyFileW_t        g_realCopyFileW;
static FindFirstFileW_t   g_realFindFirstFileW;
static FindFirstFileExW_t g_realFindFirstFileExW;
static FindNextFileW_t    g_realFindNextFileW;
static GetFileAttributesW_t g_realGetFileAttributesW;

static BOOL WINAPI HookDeleteFileW(LPCWSTR name) {
    BOOL ok = g_realDeleteFileW(name);
    if (IsSavePathW(name)) Note("DeleteFileW", name, "", ok, GetLastError());
    return ok;
}

static BOOL WINAPI HookDeleteFileA(LPCSTR name) {
    BOOL ok = g_realDeleteFileA(name);
    if (IsSavePathA(name)) {
        wchar_t w[512];
        MultiByteToWideChar(CP_ACP, 0, name, -1, w, 512);
        Note("DeleteFileA", w, "", ok, GetLastError());
    }
    return ok;
}

static BOOL WINAPI HookMoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
    BOOL ok = g_realMoveFileExW(from, to, flags);
    if (IsSavePathW(from) || IsSavePathW(to)) {
        char extra[600];
        snprintf(extra, sizeof(extra), "-> %ls flags=0x%lX",
                 to ? to : L"(null)", (unsigned long)flags);
        Note("MoveFileExW", from, extra, ok, GetLastError());
    }
    return ok;
}

static BOOL WINAPI HookMoveFileW(LPCWSTR from, LPCWSTR to) {
    BOOL ok = g_realMoveFileW(from, to);
    if (IsSavePathW(from) || IsSavePathW(to)) {
        char extra[600];
        snprintf(extra, sizeof(extra), "-> %ls", to ? to : L"(null)");
        Note("MoveFileW", from, extra, ok, GetLastError());
    }
    return ok;
}

/* The modern delete: hand the file a disposition of "delete on
 * close". Reached through a handle, so the path has to be read back
 * from it. */
static BOOL WINAPI HookSetFileInformationByHandle(HANDLE h,
                                                  FILE_INFO_BY_HANDLE_CLASS cls,
                                                  LPVOID info, DWORD size) {
    BOOL ok = g_realSetFileInformationByHandle(h, cls, info, size);
    const char *what = NULL;
    wchar_t path[MAX_PATH * 2];

    switch (cls) {
    case FileDispositionInfo:   what = "FileDispositionInfo";   break;
    case FileDispositionInfoEx: what = "FileDispositionInfoEx"; break;
    case FileRenameInfo:        what = "FileRenameInfo";        break;
    case FileRenameInfoEx:      what = "FileRenameInfoEx";      break;
    default:                    return ok;
    }
    path[0] = 0;
    if (GetFinalPathNameByHandleW(h, path, MAX_PATH * 2, 0) && IsSavePathW(path))
        Note("SetFileInformationByHandle", path, what, ok, GetLastError());
    return ok;
}

static HANDLE WINAPI HookCreateFileW(LPCWSTR name, DWORD access, DWORD share,
                                     LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                     DWORD flags, HANDLE tmpl) {
    HANDLE h = g_realCreateFileW(name, access, share, sa, disp, flags, tmpl);

    /* Only the calls that can change a save are worth a line: an
     * overwrite, a truncate, a delete-on-close, or an open for write.
     * Plain reads of the folder are the game's own business. */
    if (IsSavePathW(name) &&
        ((access & (GENERIC_WRITE | DELETE)) ||
         disp == CREATE_ALWAYS || disp == TRUNCATE_EXISTING)) {
        char extra[160];
        snprintf(extra, sizeof(extra), "access=0x%lX disp=%lu flags=0x%lX",
                 (unsigned long)access, (unsigned long)disp,
                 (unsigned long)flags);
        Note("CreateFileW", name, extra, h != INVALID_HANDLE_VALUE,
             GetLastError());
    }
    return h;
}

static BOOL WINAPI HookRemoveDirectoryW(LPCWSTR name) {
    BOOL ok = g_realRemoveDirectoryW(name);
    if (IsSavePathW(name)) Note("RemoveDirectoryW", name, "", ok, GetLastError());
    return ok;
}

static BOOL WINAPI HookCopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists) {
    BOOL ok = g_realCopyFileW(from, to, failIfExists);
    if (IsSavePathW(from) || IsSavePathW(to)) {
        char extra[600];
        snprintf(extra, sizeof(extra), "-> %ls", to ? to : L"(null)");
        Note("CopyFileW", from, extra, ok, GetLastError());
    }
    return ok;
}

/* The decisive one for "does the slot come back without a restart":
 * if the save list is built by walking the folder, a kept file shows
 * up again on the spot. */
static HANDLE WINAPI HookFindFirstFileW(LPCWSTR name, LPWIN32_FIND_DATAW fd) {
    HANDLE h = g_realFindFirstFileW(name, fd);
    if (IsSavePathW(name)) {
        char extra[320];
        snprintf(extra, sizeof(extra), "first=%ls",
                 (h != INVALID_HANDLE_VALUE && fd) ? fd->cFileName : L"(none)");
        Note("FindFirstFileW", name, extra, h != INVALID_HANDLE_VALUE,
             GetLastError());
    }
    return h;
}

static HANDLE WINAPI HookFindFirstFileExW(LPCWSTR name,
                                          FINDEX_INFO_LEVELS lvl,
                                          LPVOID data, FINDEX_SEARCH_OPS op,
                                          LPVOID filter, DWORD flags) {
    HANDLE h = g_realFindFirstFileExW(name, lvl, data, op, filter, flags);
    if (IsSavePathW(name))
        Note("FindFirstFileExW", name, "", h != INVALID_HANDLE_VALUE,
             GetLastError());
    return h;
}

static BOOL WINAPI HookFindNextFileW(HANDLE h, LPWIN32_FIND_DATAW fd) {
    BOOL ok = g_realFindNextFileW(h, fd);
    if (ok && fd && IsSavePathW(fd->cFileName)) {
        char extra[320];
        snprintf(extra, sizeof(extra), "next=%ls", fd->cFileName);
        Note("FindNextFileW", fd->cFileName, extra, ok, GetLastError());
    }
    return ok;
}

static DWORD WINAPI HookGetFileAttributesW(LPCWSTR name) {
    DWORD r = g_realGetFileAttributesW(name);

    /* Only the failures and the successes on a save file matter: a
     * check right after a delete would say the game verifies its
     * work. */
    if (IsSavePathW(name) && wcsstr(name, L".save"))
        Note("GetFileAttributesW", name, "", r != INVALID_FILE_ATTRIBUTES,
             GetLastError());
    return r;
}

/* ---- listing the folder once, at startup ------------------------------ */

static void ListSaveFolder(void) {
    wchar_t pattern[MAX_PATH];
    const wchar_t *base = L"C:\\Program Files (x86)\\Ubisoft\\"
                          L"Ubisoft Game Launcher\\savegames\\";
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;

    /* The user id is not known here, so walk the top level for the
     * game folder and list what is in it. */
    swprintf(pattern, MAX_PATH, L"%ls*", base);
    InterlockedExchange(&g_selfScan, 1);
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 ||
                wcscmp(fd.cFileName, L"..") == 0) continue;
            {
                wchar_t inner[MAX_PATH];
                WIN32_FIND_DATAW ifd;
                HANDLE ih;

                swprintf(inner, MAX_PATH, L"%ls%ls\\1771\\*.save*", base,
                         fd.cFileName);
                ih = FindFirstFileW(inner, &ifd);
                if (ih != INVALID_HANDLE_VALUE) {
                    ProbeLog("on disk: user %ls", fd.cFileName);
                    do {
                        ProbeLog("  save %ls  %lu bytes",
                                 ifd.cFileName,
                                 (unsigned long)(((ULONGLONG)ifd.nFileSizeHigh
                                                  << 32) | ifd.nFileSizeLow));
                        n++;
                    } while (FindNextFileW(ih, &ifd));
                    FindClose(ih);
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    InterlockedExchange(&g_selfScan, 0);
    ProbeLog("on disk: %d save files before the run", n);
}

/* ---- plugin ini -------------------------------------------------------- */

static HINSTANCE g_inst = NULL;
static char      g_iniPath[MAX_PATH];
static volatile LONG g_enabled = 1;

static int Enabled(void) {
    return InterlockedCompareExchange(&g_enabled, 0, 0) ? 1 : 0;
}

static void ResolveIniPath(void) {
    char mod[MAX_PATH];
    const char *dot;
    size_t n;

    g_iniPath[0] = 0;
    if (!g_inst || !GetModuleFileNameA(g_inst, mod, sizeof(mod))) return;
    dot = strrchr(mod, '.');
    n = dot ? (size_t)(dot - mod) : strlen(mod);
    if (n >= sizeof(g_iniPath)) n = sizeof(g_iniPath) - 1;
    memcpy(g_iniPath, mod, n);
    g_iniPath[n] = 0;
    strncat(g_iniPath, ".ini", sizeof(g_iniPath) - n - 1);
}

static void LoadConfig(void) {
    int on = 1;

    if (g_iniPath[0])
        on = GetPrivateProfileIntA("Settings", "enabled", 1, g_iniPath);
    InterlockedExchange(&g_enabled, on ? 1 : 0);
}

static void SaveIni(void) {
    char buf[8];

    if (!g_iniPath[0]) return;
    snprintf(buf, sizeof(buf), "%d", Enabled());
    WritePrivateProfileStringA("Settings", "enabled", buf, g_iniPath);
}

/* ---- menu -------------------------------------------------------------- */

typedef void (*MenuFn)(uint32_t menu, uint32_t item, int value, void *user);

static void OnEnable(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    InterlockedExchange(&g_enabled, value ? 1 : 0);
    ProbeLog("menu: probe=%s (takes effect on the next launch)",
             Enabled() ? "on" : "off");
    SaveIni();
}

static void BuildMenu(HMODULE m) {
    uint32_t (*menuCreate)(const char *) = NULL;
    int (*menuToggle)(uint32_t, const char *, int, MenuFn, void *) = NULL;
    int (*menuHint)(uint32_t, const char *) = NULL;

    *(FARPROC *)&menuCreate = GetProcAddress(m, "ShMenuCreate");
    *(FARPROC *)&menuToggle = GetProcAddress(m, "ShMenuToggle");
    *(FARPROC *)&menuHint   = GetProcAddress(m, "ShMenuHint");
    if (!menuCreate || !menuToggle) return;

    {
        uint32_t menu = menuCreate("Ghost wipe probe");
        menuToggle(menu, "Log save-file operations", Enabled(),
                   OnEnable, NULL);
        if (menuHint)
            menuHint(menu,
                     "Records every file operation on the save folder, so the "
                     "wipe can be read off the log. Nothing is changed and no "
                     "save is protected.");
    }
    ProbeLog("menu created");
}

/* ---- startup ----------------------------------------------------------- */

static void InstallHooks(void) {
    MH_STATUS st;

    st = MH_Initialize();
    if (st != MH_OK) {
        ProbeLog("install: MH_Initialize failed (%d) - nothing is recorded",
                 (int)st);
        return;
    }

    MH_CreateHookApi(L"kernel32.dll", "DeleteFileW",
                     (LPVOID)HookDeleteFileW, (LPVOID *)&g_realDeleteFileW);
    MH_CreateHookApi(L"kernel32.dll", "DeleteFileA",
                     (LPVOID)HookDeleteFileA, (LPVOID *)&g_realDeleteFileA);
    MH_CreateHookApi(L"kernel32.dll", "MoveFileExW",
                     (LPVOID)HookMoveFileExW, (LPVOID *)&g_realMoveFileExW);
    MH_CreateHookApi(L"kernel32.dll", "MoveFileW",
                     (LPVOID)HookMoveFileW, (LPVOID *)&g_realMoveFileW);
    MH_CreateHookApi(L"kernel32.dll", "SetFileInformationByHandle",
                     (LPVOID)HookSetFileInformationByHandle,
                     (LPVOID *)&g_realSetFileInformationByHandle);
    MH_CreateHookApi(L"kernel32.dll", "CreateFileW",
                     (LPVOID)HookCreateFileW, (LPVOID *)&g_realCreateFileW);
    MH_CreateHookApi(L"kernel32.dll", "RemoveDirectoryW",
                     (LPVOID)HookRemoveDirectoryW,
                     (LPVOID *)&g_realRemoveDirectoryW);
    MH_CreateHookApi(L"kernel32.dll", "CopyFileW",
                     (LPVOID)HookCopyFileW, (LPVOID *)&g_realCopyFileW);
    MH_CreateHookApi(L"kernel32.dll", "FindFirstFileW",
                     (LPVOID)HookFindFirstFileW,
                     (LPVOID *)&g_realFindFirstFileW);
    MH_CreateHookApi(L"kernel32.dll", "FindFirstFileExW",
                     (LPVOID)HookFindFirstFileExW,
                     (LPVOID *)&g_realFindFirstFileExW);
    MH_CreateHookApi(L"kernel32.dll", "FindNextFileW",
                     (LPVOID)HookFindNextFileW,
                     (LPVOID *)&g_realFindNextFileW);
    MH_CreateHookApi(L"kernel32.dll", "GetFileAttributesW",
                     (LPVOID)HookGetFileAttributesW,
                     (LPVOID *)&g_realGetFileAttributesW);

    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK) {
        ProbeLog("install: MH_EnableHook failed (%d)", (int)st);
        return;
    }
    ProbeLog("install: twelve hooks in place, recording savegames only");
    ProbeLog("install: walk the save folder now, then die in Ghost Mode");
}

static DWORD WINAPI InitThread(LPVOID p) {
    (void)p;
    OpenLog();
    ResolveIniPath();
    LoadConfig();

    ProbeLog("--- GhostWipeProbe: how a Ghost Mode save is wiped ---");
    ProbeLog("build " __DATE__ " " __TIME__);
    ProbeLog("start: cmdline=%s", GetCommandLineA());
    ProbeLog("config: probe=%s, ini=%s", Enabled() ? "on" : "off",
             g_iniPath[0] ? g_iniPath : "(none)");

    {
        HMODULE di = GetModuleHandleA("dinput8.dll");
        if (di) {
            *(FARPROC *)&g_getState = GetProcAddress(di, "ShGetGameState");
            *(FARPROC *)&g_stateName =
                GetProcAddress(di, "ShGetGameStateName");
            BuildMenu(di);
        }
    }
    BindStackWalk();

    if (Enabled()) {
        ListSaveFolder();
        InstallHooks();
    } else {
        ProbeLog("probe is off: nothing is hooked, no folder is listed");
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = inst;
        DisableThreadLibraryCalls(inst);
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
    }
    return TRUE;
}
