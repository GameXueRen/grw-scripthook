/* The crash on entering the "Last Rite" DLC (August 2026 update), and
 * nothing else.
 *
 * Symptom: from the main menu, entering the new DLC starts loading and
 * the process dies. The community workaround is a proxy loader plus a
 * third-party unlocker whose only business hook is UPLAY_USER_IsOwned,
 * answered "owned" for everything. That points at where the fault is:
 * the game asks Uplay whether this content is owned, a region-locked
 * build is told no, and the load that follows walks a branch that
 * dereferences a null pointer. The global build is told yes and never
 * takes that branch.
 *
 * So this plugin answers that one call for that one id, and has
 * exactly one setting - on or off, off by default:
 *
 *   [Settings]
 *   enabled = 0   off: not one call is hooked, no log is written and
 *                      the game runs exactly as it would without this
 *                      plugin - the menu row is the one thing left,
 *                      because that row is where the switch is
 *   enabled = 1   on:  id 3718 alone is answered owned
 *
 * The id is a compile-time constant on purpose. There is no target to
 * configure and no list to edit, so this plugin cannot be pointed at
 * paid content, and every id but this one is answered with whatever
 * the game would have got anyway.
 *
 * What was measured on a region-locked build, 2026-09-11:
 *   - the game asks about forty-some ids at start up and caches the
 *     answers; entering the DLC asks nothing further. The answer has
 *     to be in force from the start, which is why the switch is read
 *     at launch and takes effect on the next one;
 *   - 3718 was answered 0 while its neighbour 3717 was owned, and the
 *     load that follows the 0 dies reading null + 0x340 at
 *     GRW.exe+0xE09DA51, on the same instruction every time;
 *   - answering 3718 owned makes the DLC load, and no other id is
 *     touched.
 * If another DLC ever needs the same treatment, that is the method:
 * log every query and its real answer, and the id answered 0 just
 * before the fault is the one to build in here.
 *
 * Where the call is intercepted: at the function itself.
 *
 * The first version of this plugin hooked the game's GetProcAddress
 * lookup instead. That worked - until the plugin folder was renamed.
 * The old name sorted early under plugins\, the new one sorts late,
 * and by then the game had already loaded uplay_r1_loader64.dll and
 * taken the pointer it wanted: the log said "already loaded" and never
 * showed a lookup, and the DLC crashed again. A fix that only works
 * when it is loaded before the game asks is a race, and that one lost
 * it the moment its name changed.
 *
 * So MinHook (which the framework already carries) rewrites the
 * function's own entry point instead. The module is waited for and
 * hooked whenever it appears - if the game already holds a pointer to
 * it, that pointer is to the entry point that was rewritten, so every
 * call lands here either way. Nothing about the game's timing matters.
 *
 * Boundaries: single player only. Do not use it online - the game
 * carries EasyAntiCheat. This is a workaround for a bug in the game's
 * own load path, not a licence for anything else: turn it off and
 * delete the plugin folder once the game itself is patched.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "third_party/minhook/include/MinHook.h"

/* The module and the one export that answers "is this owned". */
#define TARGET_DLL "uplay_r1_loader64.dll"
#define TARGET_FN  "UPLAY_USER_IsOwned"

/* The one id this plugin knows about: the "Last Rite" update, a free
 * one, measured on 2026-09-11. Built in rather than configured so that
 * nothing can point this plugin at paid content. */
#define TARGET_ID 3718

/* Live state, written by the menu on the API's thread and read on
 * every query, so both are volatile LONG via Interlocked*. */
static volatile LONG g_enabled;    /* the one setting, 0 by default */
static volatile LONG g_answered;   /* times the id was answered owned */

static int Enabled(void) {
    return InterlockedCompareExchange(&g_enabled, 0, 0) ? 1 : 0;
}

/* ---- logging ---------------------------------------------------------- */

static FILE *g_log;
static LONG  g_logBusy;

static void DlcLog(const char *fmt, ...) {
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

static void OpenLog(void) {
    char path[MAX_PATH];
    char *slash;
    int len;

    if (g_log) return;

    /* The main module is GRW.exe, so its folder is the game dir. */
    if (!GetModuleFileNameA(NULL, path, MAX_PATH)) return;
    slash = strrchr(path, '\\');
    if (!slash) return;
    slash[1] = 0;

    len = (int)strlen(path);
    if (len + 5 >= (int)sizeof(path)) return;
    strcpy(path + len, "logs");
    CreateDirectoryA(path, NULL);
    if (len + 27 < (int)sizeof(path))
        strcpy(path + len, "logs\\LastRites_dlcfix.log");
    else
        strcpy(path + len, "LastRites_dlcfix.log");
    g_log = fopen(path, "a");
}

/* ---- plugin ini -------------------------------------------------------- */

static HINSTANCE g_inst = NULL;
static char      g_iniPath[MAX_PATH];

/* plugins\LastRites_dlcfix\LastRites_dlcfix.ini, from our module name. */
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

/* A missing key, a missing file and junk all read as off: the switch
 * is only on when it says so. */
static void LoadConfig(void) {
    int on = 0;

    if (g_iniPath[0])
        on = GetPrivateProfileIntA("Settings", "enabled", 0, g_iniPath);
    InterlockedExchange(&g_enabled, on ? 1 : 0);
}

static void SaveIni(void) {
    char buf[8];

    if (!g_iniPath[0]) return;
    snprintf(buf, sizeof(buf), "%d", Enabled());
    WritePrivateProfileStringA("Settings", "enabled", buf, g_iniPath);
}

/* ---- the intercepted call ---------------------------------------------- */

/* Same signature as the export: the caller tests eax against 1, so it
 * answers a uint32 and not a BOOL. */
typedef uint32_t (*IsOwned_t)(const int aUplayId);

static IsOwned_t g_realIsOwned;

static uint32_t HookIsOwned(const int aUplayId) {
    uint32_t real = g_realIsOwned ? g_realIsOwned(aUplayId) : 0;

    /* The one id, and only while the switch is on. Everything else,
     * and everything while it is off, is the answer the game would
     * have got without this plugin. */
    if (aUplayId != TARGET_ID || !Enabled())
        return real;

    if (InterlockedIncrement(&g_answered) == 1)
        DlcLog("answer id=%d -> owned  (the game's own answer was %u)",
               aUplayId, real);
    return 1u;
}

/* ---- installation ------------------------------------------------------ */

/* The module is waited for rather than demanded, because it arrives
 * whenever the game gets round to loading it - sometimes before this
 * plugin runs, sometimes after. Either way the hook lands on the
 * function's entry point, which is the address any pointer taken
 * earlier already holds. */
static void InstallHook(void) {
    MH_STATUS st;
    int i;

    st = MH_Initialize();
    if (st != MH_OK) {
        DlcLog("install: MH_Initialize failed (%d) - not one call is "
               "touched, the game keeps its own answers", (int)st);
        return;
    }

    for (i = 0; i < 480; i++) {
        HMODULE up = GetModuleHandleA(TARGET_DLL);

        if (up) {
            LPVOID fn = (LPVOID)GetProcAddress(up, TARGET_FN);

            if (!fn) {
                DlcLog("install: %s has no %s export - not one call is "
                       "touched, the game keeps its own answers",
                       TARGET_DLL, TARGET_FN);
                return;
            }
            st = MH_CreateHook(fn, (LPVOID)HookIsOwned,
                               (LPVOID *)&g_realIsOwned);
            if (st != MH_OK) {
                DlcLog("install: MH_CreateHook failed (%d) - not one call is "
                       "touched, the game keeps its own answers", (int)st);
                return;
            }
            st = MH_EnableHook(fn);
            if (st != MH_OK) {
                DlcLog("install: MH_EnableHook failed (%d) - not one call is "
                       "touched, the game keeps its own answers", (int)st);
                return;
            }
            DlcLog("install: %s!%s hooked at %p (original %p)",
                   TARGET_DLL, TARGET_FN, fn, (void *)g_realIsOwned);
            DlcLog("install: id %d alone will be answered owned",
                   TARGET_ID);
            return;
        }
        if (i == 0)
            DlcLog("waiting for %s to be loaded...", TARGET_DLL);
        Sleep(i < 240 ? 250 : 1000);
    }
    DlcLog("gave up waiting for %s - not one call is touched", TARGET_DLL);
}

/* ---- menu -------------------------------------------------------------- */

/* The signature the framework's menu callbacks use (ShMenuFn). */
typedef void (*MenuFn)(uint32_t menu, uint32_t item, int value, void *user);

static uint32_t g_menu;

static void OnFix(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    InterlockedExchange(&g_enabled, value ? 1 : 0);
    /* A session that began with the switch off has no log open; turning
     * it on opens one, so the flip itself is on record. */
    if (Enabled()) OpenLog();
    DlcLog("menu: fix=%s (takes effect on the next launch)",
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

    g_menu = menuCreate("DLC crash fix");
    menuToggle(g_menu, "Fix the crash (takes effect after a restart)",
               Enabled(), OnFix, NULL);
    if (menuHint)
        menuHint(g_menu,
                 "A temporary fix. Turn it off and remove this plugin once "
                 "the game itself is patched.");
    DlcLog("menu created");
}

/* ---- startup ----------------------------------------------------------- */

static DWORD WINAPI InitThread(LPVOID p) {
    (void)p;
    ResolveIniPath();
    LoadConfig();

    /* The switch is read here, before anything else is done, because
     * the game reads and caches its answers at start up - which is why
     * the menu says the change lands on the next launch.
     *
     * Off means untouched, and that includes the log: a run with the
     * switch off installs no hook, changes no answer and leaves no
     * file behind. The menu row is still built, because that row is
     * where the switch lives. */
    if (Enabled()) {
        OpenLog();
        DlcLog("--- LastRites_dlcfix: the \"Last Rite\" load crash ---");
        DlcLog("build " __DATE__ " " __TIME__);
        DlcLog("config: fix=on, id %d (built in), ini=%s",
               TARGET_ID, g_iniPath[0] ? g_iniPath : "(none)");

        InstallHook();
    }

    {
        HMODULE di = GetModuleHandleA("dinput8.dll");
        if (di) BuildMenu(di);
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
