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
 * A/W and CreateFile A/W) and answers "file not found" for the names
 * below, exactly as if they had been deleted. No game file is touched and
 * verification cannot undo it.
 *
 * ---- four names, and only four (disk check, 2026-09-13) ---------------
 *
 *   launch   videos\Nvidia.bk2, videos\Ubisoft_Logo.bk2
 *   legal    videos\TRC\<language>\Epilepsy.bk2, ...\WarningSaving.bk2
 *
 * videos\TRC\<language>\ holds exactly those two files, in every language
 * folder (English, SChinese and TChinese checked). NDA.bk2 is asked about
 * by the engine but does not exist anywhere on disk - hiding it was a
 * no-op - so it is gone. The three VIDEO_* names an earlier revision
 * carried (VIDEO_EXPERIENCE, VIDEO_GLOBA_000, VIDEO_INTRO_GAM) do exist -
 * 150 to 180 MB each - but the engine never asked for one of them in the
 * fifteen sessions measured, and they are not startup clips: keeping them
 * was a standing risk of eating a cutscene, so they are gone too.
 *
 * ---- one shot, then the hooks come back out ---------------------------
 *
 * The engine probes the names it wants in ONE burst during startup: over
 * fifteen sessions (logs\skipintro.log, 10116 lines) every process had
 * the same shape - four or five names asked for inside the same
 * millisecond, about 18 to 30 seconds after the hooks went in, and not
 * one further probe until that process exited. Once the burst is over,
 * intercepting buys nothing; it just sits in every file call the game
 * makes for the rest of the session.
 *
 * So the patches are taken back out, by a watcher thread, as soon as
 * either of these is true:
 *
 *   1. every name the ENABLED groups cover has been intercepted - 4/4
 *      with both groups on, 2/2 with one - the main way out;
 *   2. the main menu has been up once - the keeper, for a session where
 *      the count never fills because the engine never asks for a name.
 *
 * After that the plugin holds no hook at all for the rest of the
 * session, and the two switches can only take effect on the next launch -
 * the line under the menu says so. UnpatchAll and ReleaseThread below
 * carry the details, including what happens when a slot is no longer
 * ours.
 *
 * ---- configuration ----------------------------------------------------
 *
 * Both groups are live switches, available from the F4 menu under
 * "Skip intro videos" and persisted to this plugin's own ini
 * plugins\skipintro\skipintro.ini:
 *
 *   [Settings]
 *   skip_launch_videos=1   Nvidia.bk2, Ubisoft_Logo.bk2
 *   skip_legal_videos=1    Epilepsy.bk2, WarningSaving.bk2
 *
 * For compatibility the old scripthook.ini [loader] keys of the same
 * name are read as a fallback when the plugin ini is absent or lacks
 * a key. Missing everywhere defaults to 1 (skip everything); with both
 * off not one slot is touched. Disable the whole plugin with
 * [plugins] skipintro=0 in scripthook.ini.
 *
 * Why the main module's import table: GRW.exe statically imports
 * kernel32 (verified), so all its existence checks go through the IAT
 * slots we patch. No hooking library is needed, and BinkOpen is not
 * touched at all: hooking it cannot skip clips (see above) and the
 * existence layer alone turns the whole sequence off.
 *
 * ---- and there is nothing like it in the Forge mod loader ------------
 *
 * Checked 2026-09-13: Forge serves loose files from mods\ over entries
 * INSIDE .forge archives (it hooks ReadFile and friends) - it neither
 * hides nor replaces a loose file on disk, which is what these clips are.
 * It also deliberately does not hook CreateFile, because this plugin and
 * GhostNoWipe are already on it (MinHook keeps one hook per target; see
 * scripthook_forge_io.c). So there is no duplicate implementation of
 * "skip the intro" to factor out into a shared API.
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
};

static const char *g_legal[] = {
    "Epilepsy.bk2",
    "WarningSaving.bk2",
};

/* Pre-built wide copies of the blacklists, so the W hooks compare
 * without a conversion on every call. */
static wchar_t g_launchW[ARRAY_LEN(g_launch)][NAME_MAX];
static wchar_t g_legalW[ARRAY_LEN(g_legal)][NAME_MAX];

/* ---- the one-shot state ------------------------------------------------
 * One entry per IAT slot we wrote, so it can be written back: the slot
 * address is kept, not just the original value, because the way back has
 * to check that the slot still holds OUR detour before touching it -
 * somebody else may have patched GRW.exe's import in the meantime.
 */
typedef struct {
    void       **slot;      /* the import slot we wrote              */
    void        *detour;    /* what we wrote there                   */
    void        *orig;      /* what it held before - the way back    */
    char         name[64];  /* "kernel32!CreateFileW", for the log   */
} SkipPatch;

#define PATCH_MAX 8

static SkipPatch      g_patch[PATCH_MAX];
static volatile LONG  g_npatch;     /* entries of g_patch that are live */
static volatile LONG  g_released;   /* 1 = the hooks are out again      */
static volatile LONG  g_calls;      /* file calls seen while intercepting */
static DWORD          g_started;    /* when the patches went in (for ms) */

/* The menu, and the one framework call the status line needs. Bound by
 * name in BuildMenu; declared up here because the release thread updates
 * the line as well. */
static uint32_t g_menu;
typedef int (*MenuStatusF_t)(uint32_t menu, const char *fmt, ...);
static MenuStatusF_t g_statusF;

/* The game state, late bound like everything else; the main menu is state
 * 1 (SH_STATE_MENU in scripthook.h). */
#define SH_STATE_MENU_LOCAL 1
typedef int (*GameState_t)(void);
static GameState_t g_getGameState;

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

/* 1 once the patches have been taken back out. Checked on the hook path so
 * a call that was already inside a detour when the release ran passes the
 * file straight through, like the rest of the session will. */
static int Released(void) {
    return InterlockedCompareExchange(&g_released, 0, 0) ? 1 : 0;
}

static int ShouldHideW(const wchar_t *name) {
    const wchar_t *f = WFilePart(name);

    if (Released()) return 0;
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

    if (Released()) return 0;
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

/* One log line per hidden name, de-duplicated. The list is also what the
 * release condition counts (HaveCount below), so it is guarded: two
 * engine threads can ask about two clips at the same moment. */
static char          g_hid[64][NAME_MAX];
static volatile LONG g_nhid;          /* how many entries are filled */
static LONG          g_hidBusy;

static void LogHideW(const wchar_t *name) {
    char  buf[NAME_MAX];
    int   i, n, fresh = 0;
    const wchar_t *f = WFilePart(name);

    n = WideCharToMultiByte(CP_ACP, 0, f, -1, buf, sizeof(buf),
                            NULL, NULL);
    if (n <= 0) return;

    while (InterlockedExchange(&g_hidBusy, 1)) Sleep(1);
    n = (int)InterlockedCompareExchange(&g_nhid, 0, 0);
    for (i = 0; i < n; i++)
        if (!strcmp(g_hid[i], buf)) break;
    if (i == n) {
        if (n < (int)ARRAY_LEN(g_hid)) {
            snprintf(g_hid[n], sizeof(g_hid[n]), "%s", buf);
            InterlockedIncrement(&g_nhid);
        }
        fresh = 1;
    }
    InterlockedExchange(&g_hidBusy, 0);

    if (fresh) SkipLog("hide  %s", buf);
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
    InterlockedIncrement(&g_calls);
    if (name && ShouldHideA(name)) {
        LogHideW_FromA(name);
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_FILE_ATTRIBUTES;
    }
    return g_realGFA(name);
}

static DWORD WINAPI HookGetFileAttributesW(LPCWSTR name) {
    InterlockedIncrement(&g_calls);
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
    InterlockedIncrement(&g_calls);
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
    InterlockedIncrement(&g_calls);
    if (name && ShouldHideW(name)) {
        LogHideW(name);
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    return g_realCFW(name, access, share, sa, disp, flags, tmpl);
}

/* ---- import table patch ------------------------------------------------- */

/* Write one detour into GRW.exe's import slot for kernel32!fnName, and
 * remember the slot so it can be written back later. realOut gets what the
 * slot held - what the detours call through to. 1 when a slot is live, 0
 * when there was nothing to patch. */
static int PatchImport(const char *dllName, const char *fnName, void *detour,
                       void **realOut) {
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    SkipPatch *p;
    LONG n;
    DWORD old;

    if (!base) return 0;
    dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS *)(base + (uintptr_t)dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

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

            n = InterlockedCompareExchange(&g_npatch, 0, 0);
            if (n >= PATCH_MAX) {
                SkipLog("patch %s!%s: no room left in the slot table",
                        dllName, fnName);
                return 0;
            }
            p = &g_patch[n];
            memset(p, 0, sizeof(*p));
            p->slot   = (void **)&ft->u1.Function;
            p->detour = detour;
            p->orig   = (void *)ft->u1.Function;
            if (realOut) *realOut = p->orig;
            snprintf(p->name, sizeof(p->name), "%s!%s", dllName, fnName);

            if (!VirtualProtect(p->slot, sizeof(void *),
                                PAGE_READWRITE, &old)) {
                SkipLog("patch %s: VirtualProtect failed (%lu)",
                        p->name, GetLastError());
                return 0;
            }
            InterlockedExchangePointer((void *volatile *)&ft->u1.Function,
                                       detour);
            VirtualProtect(p->slot, sizeof(void *), old, &old);
            InterlockedIncrement(&g_npatch);      /* publish it */
            SkipLog("hooked %-38s (was %p)", p->name, p->orig);
            return 1;
        }
    }
    SkipLog("patch %s!%s: import slot not found", dllName, fnName);
    return 0;
}

/* ---- the release -------------------------------------------------------
 * What has been intercepted, and taking the hooks back out.
 */

/* Membership in the same two lists the matcher uses, so the count can
 * never disagree with what is actually being hidden. */
static int ListHas(const char *name, const char *const *list, int n) {
    int i;

    for (i = 0; i < n; i++)
        if (!_stricmp(name, list[i])) return 1;
    return 0;
}

/* How many names the ENABLED groups cover right now: 4 with both on, 2
 * with one, 0 with neither. The switches stay live until the release. */
static int NeedCount(void) {
    int n = 0;

    if (WantLaunch()) n += (int)ARRAY_LEN(g_launch);
    if (WantLegal())  n += (int)ARRAY_LEN(g_legal);
    return n;
}

/* Of those, how many have actually been hidden this session. */
static int HaveCount(void) {
    int i, n, c = 0;

    n = (int)InterlockedCompareExchange(&g_nhid, 0, 0);
    if (n > (int)ARRAY_LEN(g_hid)) n = (int)ARRAY_LEN(g_hid);
    for (i = 0; i < n; i++) {
        if (WantLaunch() &&
            ListHas(g_hid[i], g_launch, (int)ARRAY_LEN(g_launch))) c++;
        else if (WantLegal() &&
                 ListHas(g_hid[i], g_legal, (int)ARRAY_LEN(g_legal))) c++;
    }
    return c;
}

/* The line under the menu. One sentence, and it is true in every state the
 * plugin can be in: the switches are honoured while the hooks are in, but
 * for the rest of the session - which is most of it - changing one can only
 * shape the next launch. English template; ShMenuStatusF translates it in
 * this menu's own scope. */
static void UpdateStatus(void) {
    if (!g_menu || !g_statusF) return;
    g_statusF(g_menu, "Changing a switch takes effect after a restart");
}

/* Take the patches back out. This runs on the watcher thread, never inside
 * a hook - a memory write and a log line have no business in a file API a
 * thread of the engine is sitting in. Two rules hold here:
 *
 *   - the flag goes up first, so a call already inside a detour stops
 *     hiding anything and the game sees the truth from then on;
 *   - a slot is only written when it still holds OUR detour. Another
 *     component may have patched over it, and the only honest move then is
 *     to leave it exactly as it is and say so in the log.
 */
static void UnpatchAll(const char *why) {
    LONG i, n;

    if (InterlockedExchange(&g_released, 1)) return;   /* once, ever */
    n = InterlockedCompareExchange(&g_npatch, 0, 0);

    for (i = 0; i < n; i++) {
        SkipPatch *p = &g_patch[i];
        DWORD old;

        if (!p->slot) continue;
        if (*(p->slot) != p->detour) {
            SkipLog("left %s alone - the slot is not ours any more (%p)",
                    p->name, *(p->slot));
            continue;
        }
        if (!VirtualProtect(p->slot, sizeof(void *), PAGE_READWRITE,
                            &old)) {
            SkipLog("restore %s: VirtualProtect failed (%lu)",
                    p->name, GetLastError());
            continue;
        }
        *(p->slot) = p->orig;
        VirtualProtect(p->slot, sizeof(void *), old, &old);
        SkipLog("restored %-38s (was ours, now %p)", p->name, p->orig);
    }
    SkipLog("released (%s) after %lu ms; %ld file call(s) had gone through "
            "the four detours by then",
            why, (unsigned long)(GetTickCount() - g_started),
            (long)InterlockedCompareExchange(&g_calls, 0, 0));
}

/* The watcher. 250 ms, the same order of magnitude as the framework's own
 * polling threads. The conditions are asked in the order of preference:
 *
 *   1. every name the enabled groups cover has been intercepted. The
 *      engine asks for them in one burst (measured over fifteen
 *      sessions), so this is the normal way out - and because the burst
 *      lands inside a single millisecond and this runs 250 ms later, it
 *      always covers the whole burst before letting go;
 *   2. the main menu has been up once - the keeper, for a session whose
 *      count never fills because a name was never asked for;
 *   3. both groups turned off while the patches were still in.
 */
static DWORD WINAPI ReleaseThread(LPVOID p) {
    char why[64];

    (void)p;
    for (;;) {
        int need, have;

        Sleep(250);
        if (Released()) return 0;

        need = NeedCount();
        have = HaveCount();
        if (need > 0 && have >= need) {
            snprintf(why, sizeof(why), "already intercepted %d/%d",
                     have, need);
            UnpatchAll(why);
            return 0;
        }
        if (need == 0) {
            UnpatchAll("both groups were switched off");
            return 0;
        }
        if (g_getGameState && g_getGameState() == SH_STATE_MENU_LOCAL) {
            UnpatchAll("the main menu was up");
            return 0;
        }
    }
    return 0;
}

/* The target names, in the log, so a session can be lined up with the
 * list it was filtered by. */
static void LogList(const char *what, const char *const *list, int n) {
    char buf[192];
    int i;
    size_t at = 0;

    buf[0] = 0;
    for (i = 0; i < n && at + 24 < sizeof(buf); i++)
        at += (size_t)snprintf(buf + at, sizeof(buf) - at, "%s%s",
                               i ? " " : "", list[i]);
    SkipLog("%s (%d): %s", what, n, buf);
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

/* g_menu and the status call live at the top of the file: the release
 * thread writes that line too. */

/* ---- plugin ini ---------------------------------------------------------- */

static HINSTANCE g_inst = NULL;
static char      g_iniPath[MAX_PATH];

/* plugins\skipintro\skipintro.ini, from our own module file name. */
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
    SkipLog("menu: skip_launch_videos=%d%s", WantLaunch(),
            Released() ? " (the hooks are already out - this applies to the "
                         "next launch)" : "");
    SaveIni();
}

static void OnLegal(int v) {
    InterlockedExchange(&g_skipLegal, v ? 1 : 0);
    SkipLog("menu: skip_legal_videos=%d%s", WantLegal(),
            Released() ? " (the hooks are already out - this applies to the "
                         "next launch)" : "");
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
    *(FARPROC *)&g_statusF  = GetProcAddress(m, "ShMenuStatusF");
    if (!menuCreate || !menuToggle) return;

    g_menu = menuCreate("Skip intro videos");
    menuToggle(g_menu, "Skip legal videos", WantLegal(),
               MenuLegal, NULL);
    menuToggle(g_menu, "Skip launch videos", WantLaunch(),
               MenuLaunch, NULL);
    UpdateStatus();
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
    HMODULE di;

    (void)p;
    OpenLog();
    SkipLog("--- skipintro plugin, file-level hide ---");
    BuildWideLists();
    ResolveIniPath();
    LoadConfig();
    LogList("launch targets", g_launch, (int)ARRAY_LEN(g_launch));
    LogList("legal targets", g_legal, (int)ARRAY_LEN(g_legal));

    di = GetModuleHandleA("dinput8.dll");
    if (di) {
        *(FARPROC *)&g_getGameState = GetProcAddress(di, "ShGetGameState");
        BuildMenu(di);
    }

    /* Nothing enabled means nothing to take back out: an install with both
     * groups off costs the game not one detour, all session long. */
    if (!WantLaunch() && !WantLegal()) {
        SkipLog("both groups are off - not one file call is intercepted");
        return 0;
    }

    g_started = GetTickCount();
    PatchImport("kernel32.dll", "GetFileAttributesA",
                HookGetFileAttributesA, (void **)&g_realGFA);
    PatchImport("kernel32.dll", "GetFileAttributesW",
                HookGetFileAttributesW, (void **)&g_realGFW);
    PatchImport("kernel32.dll", "CreateFileA",
                HookCreateFileA, (void **)&g_realCFA);
    PatchImport("kernel32.dll", "CreateFileW",
                HookCreateFileW, (void **)&g_realCFW);
    UpdateStatus();
    SkipLog("ready - the hooks come back out as soon as the enabled names "
            "have been intercepted");
    CreateThread(NULL, 0, ReleaseThread, NULL, 0, NULL);
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
