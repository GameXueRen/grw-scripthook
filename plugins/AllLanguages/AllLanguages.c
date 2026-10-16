/* The RU/CN build's language lock, and nothing else.
 *
 * Symptom: the text language list of a RU/CN build offers Chinese and
 * Russian, and nothing else - the players' ask is to be able to pick any
 * of the languages the game carries.
 *
 * The restriction is not missing files and not a different build: it is
 * applied at run time from the ownership answers the game reads and caches
 * at start up. One of those answers - the id below - is 0 on this build,
 * and answering it 1 is what removes the restriction. The languages are in
 * the install all along: with the answer at 1 the list is everything the
 * install carries.
 *
 * What was measured, 2026-09-21, on a RU/CN build:
 *   - with the switch off the list is Chinese and Russian;
 *   - with the switch on the list is every language the install carries;
 *   - the engine's own answer for the id was 0 while this plugin answered
 *     1, and no other id was touched.
 *
 * Where the id came from: it was found while fixing the crash on entering
 * the "Last Rite" DLC that the 2026-08 game update introduced on this
 * build - the load that follows the 0 faulted reading null + 0x340 at
 * GRW.exe+0xE09DA51, on the same instruction every time. The same 0 is what
 * shortens the language list. The official update of 2026-09 fixed the
 * crash; the restriction is still there, which is what this plugin is for
 * now.
 *
 * The id is a compile-time constant on purpose. There is no target to
 * configure and no list to edit, so this plugin cannot be pointed at paid
 * content, and every id but this one is answered with whatever the game
 * would have got anyway.
 *
 * The answers are read and cached at start up - entering a menu asks
 * nothing further - so the answer has to be in force from the start. That
 * is why the switch is read at launch and takes effect on the next one.
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
 * So the hook goes on the function's own entry point instead, through
 * the framework's shared hook layer (ShHookAddLate): the module is waited
 * for and hooked whenever it appears - if the game already holds a pointer
 * to it, that pointer is to the entry point that was rewritten, so every
 * call lands here either way. Nothing about the game's timing matters, and
 * this plugin no longer carries MinHook of its own.
 *
 * Settings, in AllLanguages.ini beside the .asi:
 *
 *   [Settings]
 *   enabled = 0   off: not one call is hooked, no log is written and the
 *                      game runs exactly as it would without this plugin -
 *                      the menu row is the one thing left, because that row
 *                      is where the switch is
 *   enabled = 1   on:  the id below alone is answered owned
 *
 * Boundaries: single player only. Do not use it online - the game
 * carries EasyAntiCheat.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "scripthook.h"

/* What this plugin needs of the framework: the shared hook layer (the
 * ShHook* group), which this line carries under version 2 - see the
 * exception in the header's version note. The header is included for that
 * declaration and for the shared types; this plugin still binds every entry
 * point it calls by name at run time, so including it imports nothing, and
 * a missing ShHookAddLate reads as null rather than as an error. */
SH_REQUIRES_API(2);

/* The module and the one export that answers "is this owned". */
#define TARGET_DLL "uplay_r1_loader64.dll"
#define TARGET_FN  "UPLAY_USER_IsOwned"

/* This plugin's name for its one hook: the shared layer's registry, its log
 * and ShHookRemove all take it. */
#define HOOK_TAG "AllLanguages:uplay_owned"

/* The two entry points of the shared hook layer this plugin uses, bound by
 * name like the rest of the framework calls here: the plugin links nothing,
 * so an older framework without them reads as null and the install is
 * skipped rather than crashing. */
typedef int (*HookAddLate_t)(const char *, const char *, const char *,
                             void *, void **);
typedef int (*HookRemove_t)(const char *);
static HookAddLate_t pHookAddLate;
static HookRemove_t pHookRemove;

/* The one id this plugin knows about - the entry this build answers 0 to,
 * measured on 2026-09-11 and again on 2026-09-21. Built in rather than
 * configured so that nothing can point this plugin at paid content. */
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

static void Log(const char *fmt, ...) {
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
    if (len + 23 < (int)sizeof(path))
        strcpy(path + len, "logs\\AllLanguages.log");
    else
        strcpy(path + len, "AllLanguages.log");
    /* "w", not "a": the framework's own logs are per session, and a
     * diagnostic that only ever grows is a file that grows on the player's
     * disk forever. */
    g_log = fopen(path, "w");
}

/* ---- plugin ini -------------------------------------------------------- */

static HINSTANCE g_inst = NULL;
static char      g_iniPath[MAX_PATH];

/* plugins\AllLanguages\AllLanguages.ini, from our module name. */
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

static IsOwned_t g_realIsOwned;   /* the trampoline, to call through */

static uint32_t HookIsOwned(const int aUplayId) {
    uint32_t real = g_realIsOwned ? g_realIsOwned(aUplayId) : 0;

    /* The one id, and only while the switch is on. Everything else,
     * and everything while it is off, is the answer the game would
     * have got without this plugin. */
    if (aUplayId != TARGET_ID || !Enabled())
        return real;

    if (InterlockedIncrement(&g_answered) == 1)
        Log("answer id=%d -> owned  (the game's own answer was %u)",
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
    int rc;

    /* One call: the shared layer waits for the module (a quarter second for
     * the first minute, a second after that - the policy this plugin's own
     * loop used, moved rather than reinvented), hooks the export's own
     * entry point and hands back the trampoline. What used to be four
     * failure paths with an MH_Uninitialize each is now the layer's
     * problem: it owns MinHook's state for the whole DLL and never shuts it
     * down, which is the other reason a plugin must not carry the library. */
    rc = pHookAddLate(HOOK_TAG, TARGET_DLL, TARGET_FN,
                      (void *)HookIsOwned, (void **)&g_realIsOwned);
    if (rc == SH_HOOK_OK) {
        Log("install: %s!%s asked for, the framework installs it "
            "(original %p)", TARGET_DLL, TARGET_FN,
            (void *)g_realIsOwned);
        Log("install: id %d alone will be answered owned", TARGET_ID);
        return;
    }
    g_realIsOwned = NULL;
    Log("install: %s!%s -> %s - not one call is touched, the game keeps "
        "its own answers", TARGET_DLL, TARGET_FN,
        rc == SH_HOOK_DUP ? "already hooked by somebody else"
                          : "refused (the reason is in "
                            "scripthook_hooks.log)");
}

/* ---- menu -------------------------------------------------------------- */

/* The signature the framework's menu callbacks use (ShMenuFn). */
typedef void (*MenuFn)(uint32_t menu, uint32_t item, int value, void *user);

static uint32_t g_menu;

static void OnUnlock(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    InterlockedExchange(&g_enabled, value ? 1 : 0);
    /* A session that began with the switch off has no log open; turning
     * it on opens one, so the flip itself is on record. */
    if (Enabled()) OpenLog();
    Log("menu: unlock=%s (takes effect on the next launch)",
        Enabled() ? "on" : "off");
    SaveIni();
}

/* ---- text ---------------------------------------------------------
 * The plugin's own text, compiled in: lang.ini beside this source only
 * has to carry what it changes, and the menu reads with or without it.
 * Keys are stable IDs. Late-bound, like the rest of this plugin.
 */
typedef struct { const char *key; const char *text; } TextRow;
typedef int (*LangDeclare_t)(const char *owner, const char *lang,
                             const TextRow *rows, int n);
static LangDeclare_t pLangDeclare;

static const TextRow kEn[] = {
    { "@lu.page",   "Unlock all game languages" },
    { "@lu.unlock", "Unlock all languages (takes effect after a restart)" },
    { "@lu.hint",   "Removes the RU/CN build's restriction on the game's "
                    "language choice." }
};

static const TextRow kZh[] = {
    { "@lu.page",   "解锁游戏全部语言" },
    { "@lu.unlock", "解锁全部语言（重启游戏后生效）" },
    { "@lu.hint",   "解除 RU/CN 版本对游戏语言选择的限制" }
};

static void TextInit(void) {
    static int done;
    HMODULE mod;

    if (done) return;
    mod = GetModuleHandleA("dinput8.dll");
    if (!mod) return;
    if (!pLangDeclare)
        *(FARPROC *)&pLangDeclare = GetProcAddress(mod, "ShLangDeclare");
    if (!pLangDeclare) return;
    done = 1;
    pLangDeclare("AllLanguages", "en-US", kEn,
                 (int)(sizeof(kEn) / sizeof(kEn[0])));
    pLangDeclare("AllLanguages", "zh-CN", kZh,
                 (int)(sizeof(kZh) / sizeof(kZh[0])));
}

static void BuildMenu(HMODULE m) {
    uint32_t (*menuCreate)(const char *) = NULL;
    int (*menuToggle)(uint32_t, const char *, int, MenuFn, void *) = NULL;
    int (*menuHint)(uint32_t, const char *) = NULL;

    *(FARPROC *)&menuCreate = GetProcAddress(m, "ShMenuCreate");
    *(FARPROC *)&menuToggle = GetProcAddress(m, "ShMenuToggle");
    *(FARPROC *)&menuHint   = GetProcAddress(m, "ShMenuHint");
    if (!menuCreate || !menuToggle) return;

    TextInit();
    g_menu = menuCreate("@lu.page");
    menuToggle(g_menu, "@lu.unlock",
               Enabled(), OnUnlock, NULL);
    if (menuHint)
        menuHint(g_menu, "@lu.hint");
    Log("menu created");
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
    {
        HMODULE di = GetModuleHandleA("dinput8.dll");

        /* The shared hook layer's two entry points, by name. A framework
         * old enough not to have them leaves these null and the install is
         * skipped with a line saying so - the plugin stays loadable, it
         * just does nothing, which is the same contract as every other
         * late binding here. */
        if (di) {
            *(FARPROC *)&pHookAddLate =
                GetProcAddress(di, "ShHookAddLate");
            *(FARPROC *)&pHookRemove =
                GetProcAddress(di, "ShHookRemove");
        }
    }

    if (Enabled()) {
        OpenLog();
        Log("--- AllLanguages: the RU/CN language lock ---");
        Log("build " __DATE__ " " __TIME__);
        Log("config: unlock=on, id %d (built in), ini=%s",
            TARGET_ID, g_iniPath[0] ? g_iniPath : "(none)");

        if (pHookAddLate)
            InstallHook();
        else
            Log("install: the framework carries no shared hook layer - "
                "not one call is touched, the game keeps its own "
                "answers");
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
        {
            HANDLE h = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);

            if (h) CloseHandle(h);   /* never waited on */
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        /* The detour is this module's code, so an unload that left it in
         * place would be a jump into unmapped memory the next time the game
         * asked. There is no thread of ours left that could take it back,
         * so it happens here: the shared layer takes back this plugin's own
         * hook by tag, and nobody else's. */
        if (pHookRemove) {
            pHookRemove(HOOK_TAG);
            g_realIsOwned = NULL;
        }
    }
    return TRUE;
}
