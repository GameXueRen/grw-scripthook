/* Skip GRW startup and legal videos, UPlay build.
 *
 * The UPlay GRW.exe has no -nointro branch (Steam build only), so the
 * intro clips cannot be skipped by an engine flag. The wiki method is
 * deleting videos\*.bk2 + videos\TRC. It works for a specific reason:
 *
 *   the engine checks whether each clip EXISTS first (GetFileAttributes
 *   and friends) and only calls BinkOpen when the file is there. When
 *   the check fails the whole intro sequence is skipped and the main
 *   menu comes up. Deleting the files makes every check fail.
 *
 * Returning NULL from BinkOpen does NOT reproduce that: the file is
 * still there, so the engine starts the player and then sits on a NULL
 * handle -> permanent black screen (observed). The correct hook is the
 * existence check itself.
 *
 * This plugin patches the file APIs GRW.exe imports (GetFileAttributes
 * A/W and CreateFile A/W) and answers "file not found" for the target
 * clip names, exactly as if they had been deleted. No game file is
 * touched and verification cannot undo it.
 *
 * Both groups are live switches, available from the F4 menu under
 * "Skip intro videos" and persisted to this plugin's own ini
 * scripts\skipintro\skipintro.ini:
 *
 *   [Settings]
 *   skip_launch_videos=1   Nvidia / Ubisoft_Logo / VIDEO_* intros
 *   skip_legal_videos=1    TRC\*.bk2 (Epilepsy, WarningSaving, NDA)
 *
 * For compatibility the old scripthook.ini [loader] keys of the same
 * name are read as a fallback when the plugin ini is absent or lacks
 * a key. Missing everywhere defaults to 1 (skip everything). Disable
 * the whole plugin with [plugins] skipintro=0 in scripthook.ini.
 *
 * Why the main module's import table: GRW.exe statically imports
 * kernel32 (verified), so all its existence checks go through the IAT
 * slots we patch. No hooking library is needed, and BinkOpen is not
 * touched at all: hooking it cannot skip clips (see above) and the
 * existence layer alone turns the whole sequence off.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define NAME_MAX     64

/* ---- configuration ---------------------------------------------------- */

/* Live switches. Hooks run on arbitrary threads, menu callbacks on the
 * API's own thread, so both are volatile LONG accessed with
 * Interlocked* and never cached in a plain local. */
static volatile LONG g_skipLaunch = 1;  /* logo + startup intros */
static volatile LONG g_skipLegal  = 1;  /* TRC legal/health clips */

static const char *g_launch[] = {
    "Nvidia.bk2",
    "Ubisoft_Logo.bk2",
    "VIDEO_EXPERIENCE.bk2",
    "VIDEO_GLOBA_000.bk2",
    "VIDEO_INTRO_GAM.bk2",
};

static const char *g_legal[] = {
    "Epilepsy.bk2",
    "WarningSaving.bk2",
    "NDA.bk2",
};

/* Pre-built wide copies of the blacklists, so the W hooks compare
 * without a conversion on every call. */
static wchar_t g_launchW[ARRAY_LEN(g_launch)][NAME_MAX];
static wchar_t g_legalW[ARRAY_LEN(g_legal)][NAME_MAX];

/* ---- logging ---------------------------------------------------------- */

static FILE *g_log;
static LONG  g_logBusy;

static void SkipLog(const char *fmt, ...) {
    va_list ap;
    char line[512];
    SYSTEMTIME st;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (!g_log) return;

    while (InterlockedExchange(&g_logBusy, 1)) Sleep(1);
    if (g_log) {
        GetLocalTime(&st);
        fprintf(g_log, "%02u:%02u:%02u.%03u  %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
        fflush(g_log);
    }
    InterlockedExchange(&g_logBusy, 0);
}

/* ---- name matching ----------------------------------------------------- */

/* File name after the last '\' or '/'. */
static const wchar_t *WFilePart(const wchar_t *path) {
    const wchar_t *p = path, *f = path;
    for (; *p; p++)
        if (*p == L'\\' || *p == L'/') f = p + 1;
    return f;
}

static int WMmatch(const wchar_t *file, const wchar_t (*list)[NAME_MAX],
                   int n) {
    int i;
    for (i = 0; i < n; i++)
        if (!_wcsicmp(file, list[i])) return 1;
    return 0;
}

/* Fast reject before the string compares: every target ends in
 * ".bk2", and almost nothing else the game touches does, so this one
 * check skips the whole blacklist for every normal file call. */
static int IsBinkNameW(const wchar_t *f) {
    size_t n = wcslen(f);
    return n > 4 && f[n - 4] == L'.' &&
           (f[n - 3] | 0x20) == L'b' &&
           (f[n - 2] | 0x20) == L'k' &&
           (f[n - 1] | 0x20) == L'2';
}

static int IsBinkNameA(const char *f) {
    size_t n = strlen(f);
    return n > 4 && f[n - 4] == '.' &&
           (f[n - 3] | 0x20) == 'b' &&
           (f[n - 2] | 0x20) == 'k' &&
           (f[n - 1] | 0x20) == '2';
}

static int WantLaunch(void) {
    return InterlockedCompareExchange(&g_skipLaunch, 0, 0) ? 1 : 0;
}

static int WantLegal(void) {
    return InterlockedCompareExchange(&g_skipLegal, 0, 0) ? 1 : 0;
}

static int ShouldHideW(const wchar_t *name) {
    const wchar_t *f = WFilePart(name);

    if (!*f || !IsBinkNameW(f)) return 0;
    if (WantLaunch() && WMmatch(f, g_launchW, (int)ARRAY_LEN(g_launch)))
        return 1;
    if (WantLegal() && WMmatch(f, g_legalW, (int)ARRAY_LEN(g_legal)))
        return 1;
    return 0;
}

static int ShouldHideA(const char *name) {
    const char *f;
    const char *b = strrchr(name, '\\');
    const char *s = strrchr(name, '/');
    int i;

    f = name;
    if (b && b > f) f = b + 1;
    if (s && s > f) f = s + 1;
    if (!*f || !IsBinkNameA(f)) return 0;

    if (WantLaunch())
        for (i = 0; i < (int)ARRAY_LEN(g_launch); i++)
            if (!_stricmp(f, g_launch[i])) return 1;
    if (WantLegal())
        for (i = 0; i < (int)ARRAY_LEN(g_legal); i++)
            if (!_stricmp(f, g_legal[i])) return 1;
    return 0;
}

/* One log line per hidden name, de-duplicated. */
static char  g_hid[64][NAME_MAX];
static int   g_nhid = 0;

static void LogHideW(const wchar_t *name) {
    char  buf[NAME_MAX];
    int   i, n;
    const wchar_t *f = WFilePart(name);

    n = WideCharToMultiByte(CP_ACP, 0, f, -1, buf, sizeof(buf),
                            NULL, NULL);
    if (n <= 0) return;
    for (i = 0; i < g_nhid; i++)
        if (!strcmp(g_hid[i], buf)) return;
    if (g_nhid < (int)ARRAY_LEN(g_hid))
        snprintf(g_hid[g_nhid], sizeof(g_hid[g_nhid]), "%s", buf);
    SkipLog("hide  %s", buf);
}

static void LogHideW_FromA(const char *name) {
    wchar_t wide[NAME_MAX];

    if (MultiByteToWideChar(CP_ACP, 0, name, -1, wide, NAME_MAX) > 0)
        LogHideW(wide);
}

/* ---- kernel32 file API detours ----------------------------------------- */

typedef DWORD  (WINAPI *GetFileAttributesA_t)(LPCSTR);
typedef DWORD  (WINAPI *GetFileAttributesW_t)(LPCWSTR);
typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD,
                                       LPSECURITY_ATTRIBUTES, DWORD,
                                       DWORD, HANDLE);
typedef HANDLE (WINAPI *CreateFileW_t)(LPCWSTR, DWORD, DWORD,
                                       LPSECURITY_ATTRIBUTES, DWORD,
                                       DWORD, HANDLE);

static GetFileAttributesA_t g_realGFA;
static GetFileAttributesW_t g_realGFW;
static CreateFileA_t        g_realCFA;
static CreateFileW_t        g_realCFW;

static DWORD WINAPI HookGetFileAttributesA(LPCSTR name) {
    if (name && ShouldHideA(name)) {
        LogHideW_FromA(name);
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_FILE_ATTRIBUTES;
    }
    return g_realGFA(name);
}

static DWORD WINAPI HookGetFileAttributesW(LPCWSTR name) {
    if (name && ShouldHideW(name)) {
        LogHideW(name);
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_FILE_ATTRIBUTES;
    }
    return g_realGFW(name);
}

static HANDLE WINAPI HookCreateFileA(LPCSTR name, DWORD access,
                                     DWORD share,
                                     LPSECURITY_ATTRIBUTES sa,
                                     DWORD disp, DWORD flags,
                                     HANDLE tmpl) {
    if (name && ShouldHideA(name)) {
        LogHideW_FromA(name);
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    return g_realCFA(name, access, share, sa, disp, flags, tmpl);
}

static HANDLE WINAPI HookCreateFileW(LPCWSTR name, DWORD access,
                                     DWORD share,
                                     LPSECURITY_ATTRIBUTES sa,
                                     DWORD disp, DWORD flags,
                                     HANDLE tmpl) {
    if (name && ShouldHideW(name)) {
        LogHideW(name);
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    return g_realCFW(name, access, share, sa, disp, flags, tmpl);
}

/* ---- import table patch ------------------------------------------------- */

static void PatchImport(const char *dllName, const char *fnName,
                        void *detour, void **realOut) {
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    DWORD old;

    if (!base) return;
    dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    nt = (IMAGE_NT_HEADERS *)(base + (uintptr_t)dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    imp = (IMAGE_IMPORT_DESCRIPTOR *)(
        base + nt->OptionalHeader.DataDirectory[
            IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

    for (; imp->Name; imp++) {
        const char *dll = (const char *)(base + imp->Name);
        IMAGE_THUNK_DATA *oft;
        IMAGE_THUNK_DATA *ft;

        if (_stricmp(dll, dllName)) continue;

        oft = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        ft  = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);

        for (; oft->u1.AddressOfData; oft++, ft++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            const char *fname;

            /* skip ordinal imports */
            if (oft->u1.AddressOfData & IMAGE_ORDINAL_FLAG64) continue;
            if (!ft->u1.Function) continue;

            ibn = (IMAGE_IMPORT_BY_NAME *)(
                base + oft->u1.AddressOfData);
            fname = (const char *)ibn->Name;
            if (strcmp(fname, fnName)) continue;

            if (!VirtualProtect(&ft->u1.Function, sizeof(ft->u1.Function),
                                PAGE_READWRITE, &old)) {
                SkipLog("patch %s!%s: VirtualProtect failed (%lu)",
                        dllName, fnName, GetLastError());
                return;
            }
            *realOut = (void *)ft->u1.Function;
            InterlockedExchangePointer(
                (void *volatile *)&ft->u1.Function, detour);
            VirtualProtect(&ft->u1.Function, sizeof(ft->u1.Function),
                           old, &old);
            SkipLog("hooked %s!%s  (was %p)", dllName, fnName, *realOut);
            return;
        }
    }
    SkipLog("patch %s!%s: import slot not found", dllName, fnName);
}

/* ---- menu ---------------------------------------------------------------- */

/* Binds the menu exports off dinput8.dll by name, the same way the
 * other GetProcAddress plugins do. The menu is created as soon as the
 * module is there, which is long before the world loads. */
typedef void (*MenuFn_t)(uint32_t menu, uint32_t item, int value,
                         void *user);
typedef uint32_t (*MenuCreate_t)(const char *title);
typedef int  (*MenuToggle_t)(uint32_t menu, const char *label,
                             int initial, MenuFn_t fn, void *user);

static uint32_t g_menu = 0;

/* ---- plugin ini ---------------------------------------------------------- */

static HINSTANCE g_inst = NULL;
static char      g_iniPath[MAX_PATH];

/* scripts\skipintro\skipintro.ini, from our own module file name. */
static void ResolveIniPath(void) {
    char mod[MAX_PATH];
    const char *dot;
    size_t n;

    g_iniPath[0] = 0;
    if (!g_inst || !GetModuleFileNameA(g_inst, mod, sizeof(mod)))
        return;
    dot = strrchr(mod, '.');
    n = dot ? (size_t)(dot - mod) : strlen(mod);
    if (n >= sizeof(g_iniPath)) n = sizeof(g_iniPath) - 1;
    memcpy(g_iniPath, mod, n);
    g_iniPath[n] = 0;
    strncat(g_iniPath, ".ini", sizeof(g_iniPath) - n - 1);
}

/* The setting, from the plugin ini first. When the key is absent
 * there, fallback (the legacy scripthook.ini [loader] value, already
 * read by the caller) is used. */
static int IniBool(const char *key, int fallback) {
    char buf[8];

    if (g_iniPath[0] &&
        GetPrivateProfileStringA("Settings", key, "", buf, sizeof(buf),
                                 g_iniPath) > 0)
        return !_stricmp(buf, "1") || !_stricmp(buf, "true") ||
               !_stricmp(buf, "yes") || !_stricmp(buf, "on");
    return fallback;
}

static void LoadConfig(void) {
    HMODULE di = GetModuleHandleA("dinput8.dll");
    int (*getBool)(const char *, const char *, int) = NULL;
    int launchFallback = 1;
    int legalFallback  = 1;

    if (di) {
        *(FARPROC *)&getBool =
            GetProcAddress(di, "ShConfigGetBool");
        if (getBool) {
            launchFallback =
                getBool("loader", "skip_launch_videos", 1);
            legalFallback =
                getBool("loader", "skip_legal_videos", 1);
        }
    }
    InterlockedExchange(&g_skipLaunch,
                        IniBool("skip_launch_videos", launchFallback));
    InterlockedExchange(&g_skipLegal,
                        IniBool("skip_legal_videos", legalFallback));
    SkipLog("skip_launch_videos=%d skip_legal_videos=%d",
            WantLaunch(), WantLegal());
}

static void SaveIni(void) {
    char buf[8];

    if (!g_iniPath[0]) return;
    snprintf(buf, sizeof(buf), "%d", WantLaunch());
    WritePrivateProfileStringA("Settings", "skip_launch_videos", buf,
                               g_iniPath);
    snprintf(buf, sizeof(buf), "%d", WantLegal());
    WritePrivateProfileStringA("Settings", "skip_legal_videos", buf,
                               g_iniPath);
}

/* ---- menu callbacks ------------------------------------------------------ */

static void OnLaunch(int v) {
    InterlockedExchange(&g_skipLaunch, v ? 1 : 0);
    SkipLog("menu: skip_launch_videos=%d", WantLaunch());
    SaveIni();
}

static void OnLegal(int v) {
    InterlockedExchange(&g_skipLegal, v ? 1 : 0);
    SkipLog("menu: skip_legal_videos=%d", WantLegal());
    SaveIni();
}

static void MenuLaunch(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    OnLaunch(v);
}

static void MenuLegal(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    OnLegal(v);
}

static void BuildMenu(HMODULE m) {
    MenuCreate_t menuCreate = NULL;
    MenuToggle_t menuToggle = NULL;

    *(FARPROC *)&menuCreate = GetProcAddress(m, "ShMenuCreate");
    *(FARPROC *)&menuToggle = GetProcAddress(m, "ShMenuToggle");
    if (!menuCreate || !menuToggle) return;

    g_menu = menuCreate("Skip intro videos");
    menuToggle(g_menu, "Skip legal videos", WantLegal(),
               MenuLegal, NULL);
    menuToggle(g_menu, "Skip launch videos", WantLaunch(),
               MenuLaunch, NULL);
    SkipLog("menu created");
}

/* ---- startup ------------------------------------------------------------- */

static void BuildWideLists(void) {
    size_t i;

    for (i = 0; i < ARRAY_LEN(g_launch); i++)
        MultiByteToWideChar(CP_ACP, 0, g_launch[i], -1,
                            g_launchW[i], NAME_MAX);
    for (i = 0; i < ARRAY_LEN(g_legal); i++)
        MultiByteToWideChar(CP_ACP, 0, g_legal[i], -1,
                            g_legalW[i], NAME_MAX);
}

static void OpenLog(void) {
    char path[MAX_PATH];
    char *slash;
    int len;

    /* The main module is GRW.exe, so its folder is the game dir. */
    if (!GetModuleFileNameA(NULL, path, MAX_PATH)) return;
    slash = strrchr(path, '\\');
    if (!slash) return;
    slash[1] = 0;                         /* keep the trailing backslash */

    len = (int)strlen(path);
    if (len + 5 >= (int)sizeof(path)) return;
    strcpy(path + len, "logs");           /* <gamedir>\logs */
    CreateDirectoryA(path, NULL);
    if (len + 22 < (int)sizeof(path))
        strcpy(path + len, "logs\\skipintro.log");
    else
        strcpy(path + len, "skipintro.log");
    g_log = fopen(path, "a");
}

static DWORD WINAPI InitThread(LPVOID p) {
    (void)p;
    OpenLog();
    SkipLog("--- skipintro plugin, file-level hide ---");
    BuildWideLists();
    ResolveIniPath();
    LoadConfig();
    PatchImport("kernel32.dll", "GetFileAttributesA",
                HookGetFileAttributesA, (void **)&g_realGFA);
    PatchImport("kernel32.dll", "GetFileAttributesW",
                HookGetFileAttributesW, (void **)&g_realGFW);
    PatchImport("kernel32.dll", "CreateFileA",
                HookCreateFileA, (void **)&g_realCFA);
    PatchImport("kernel32.dll", "CreateFileW",
                HookCreateFileW, (void **)&g_realCFW);
    {
        HMODULE di = GetModuleHandleA("dinput8.dll");
        if (di) BuildMenu(di);
    }
    SkipLog("ready");
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
