/* The mode blacklist, in a plugin small enough to read in one go.
 *
 * It declares that it must not run in Ghost Mode (幽灵/魅影模式, the
 * permadeath campaign) - single player, so the whole thing can be tried
 * without a second player and without matchmaking - and then does the
 * three things a plugin has to do about being blocked:
 *
 *   ShPluginBlacklist   says which modes it must not run in. Once, from
 *                       its own source; nothing in an ini can widen or
 *                       narrow it.
 *   ShPluginOnBlocked   is told when the answer flips. Stopping the work
 *                       is the plugin's own job: the framework hides the
 *                       menu row and says so, and that is all it can do
 *                       (see the blacklist group in scripthook.h).
 *   ShPluginAllowed     is what the tick asks on every pass, so a mode
 *                       set while the tick was asleep is noticed at once
 *                       even before the callback arrives.
 *
 * Declaring anything also takes it out of the default: a plugin that never
 * declares is blocked in Ghost War and Mercenaries. This one is in Ghost
 * Mode and nowhere else, so it keeps its row in the campaign and in the
 * front end - which is the other half of this demonstration.
 *
 * What it does while it is allowed is deliberately trivial - a HUD line
 * with a counter - so "the work stopped" is something you can see rather
 * than something you have to believe. Three things are visible together:
 *
 *   - its row is gone from the F4 root menu while the mode is on the
 *     blacklist, and back when it is not;
 *   - Mod settings -> Plugins names it as switched off by that mode;
 *   - logs\scripthook_blacklist_sample.log gets a line on every flip, and
 *     logs\scripthook_blacklist.log gets the framework's own view.
 *
 * It late binds every framework call by name, the way the probes and
 * EnemyReinforce do, so it needs no import library.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#include "log.h"
#include "scripthook.h"

/* ---- what we call in the framework ------------------------------------ */

typedef int          (*GetVersion_t)(void);
typedef int          (*Blacklist_t)(uint32_t);
typedef uint32_t     (*BlacklistModes_t)(void);
typedef int          (*OnBlocked_t)(ShPluginBlockedFn, void *);
typedef int          (*Allowed_t)(void);
typedef int          (*BlockedBy_t)(void);
typedef const char  *(*ModeName_t)(int);
typedef int          (*BlockedText_t)(uint32_t, char *, int);
typedef uint32_t     (*MenuCreate_t)(const char *);
typedef int          (*MenuToggle_t)(uint32_t, const char *, int,
                                     ShMenuFn, void *);
typedef int          (*MenuAction_t)(uint32_t, const char *, ShMenuFn,
                                     void *);
typedef int          (*MenuStatus_t)(uint32_t, const char *);
typedef int          (*MenuStatusF_t)(uint32_t, const char *, ...);
typedef int          (*MenuSetValue_t)(uint32_t, const char *, int);
typedef int          (*MenuHint_t)(uint32_t, const char *);
typedef int          (*LastError_t)(void);
typedef uint32_t     (*HudCreate_t)(const char *, int, int);
typedef int          (*HudSet_t)(uint32_t, const char *);
typedef int          (*HudShow_t)(uint32_t, int);

static GetVersion_t     pGetVersion;
static Blacklist_t      pBlacklist;
static BlacklistModes_t pBlacklistModes;
static OnBlocked_t      pOnBlocked;
static Allowed_t        pAllowed;
static BlockedBy_t      pBlockedBy;
static ModeName_t       pModeName;
static BlockedText_t    pBlockedText;
static MenuCreate_t     pMenuCreate;
static MenuToggle_t     pMenuToggle;
static MenuAction_t     pMenuAction;
static MenuStatus_t     pMenuStatus;
static MenuStatusF_t    pMenuStatusF;
static MenuSetValue_t   pMenuSetValue;
static MenuHint_t       pMenuHint;
static LastError_t      pLastError;
static HudCreate_t      pHudCreate;
static HudSet_t         pHudSet;
static HudShow_t        pHudShow;

/* ---- state ------------------------------------------------------------ */

static HINSTANCE     g_inst;
static uint32_t      g_menu;
static uint32_t      g_hud;
static volatile LONG g_on = 1;        /* the plugin's own switch */
static volatile LONG g_ticks;         /* work done while allowed     */
static volatile LONG g_blocked;       /* what the framework last said */
static volatile LONG g_started;

/* The bits that block, as text: "Ghost War+MERCENARIES". */
static void BitsText(uint32_t bits, char *out, int cap) {
    out[0] = 0;
    if (pBlockedText) pBlockedText(bits, out, cap);
    if (!out[0]) snprintf(out, (size_t)cap, "0x%X", (unsigned)bits);
}

static void SetStatus(void) {
    char text[64];

    if (!g_menu || !pMenuStatusF) return;
    if (InterlockedCompareExchange(&g_blocked, 0, 0)) {
        BitsText((uint32_t)(pBlockedBy ? pBlockedBy() : 0), text,
                 (int)sizeof(text));
        pMenuStatusF(g_menu, "off: blocked by %s", text);
    } else if (!InterlockedCompareExchange(&g_on, 0, 0)) {
        pMenuStatus(g_menu, "off: switched off in the menu");
    } else {
        pMenuStatus(g_menu, "on");
    }
}

/* The framework's own view, for the log and the menu. */
static void LogState(const char *what) {
    uint32_t modes = pBlacklistModes ? pBlacklistModes() : 0;
    uint32_t blocked = (uint32_t)(pBlockedBy ? pBlockedBy() : 0);
    int allowed = pAllowed ? pAllowed() : 1;
    char text[64];

    BitsText(blocked, text, (int)sizeof(text));
    Log("sample: %s - allowed=%d, blockedBy=%s, in force=0x%02X",
        what, allowed, blocked ? text : "nothing", (unsigned)modes);
}

/* ---- the callback: this is where the work stops ------------------------
 * It arrives on a framework thread, at most once per flip, so it is a good
 * place for exactly this much: park the work and remember the answer. The
 * tick below reaches the same conclusion on its own, which is what a
 * plugin that wants to be safe without the callback does. */
static void OnBlocked(int allowed, int blocked, void *user) {
    char text[64];

    (void)user;
    InterlockedExchange(&g_blocked, allowed ? 0 : 1);
    BitsText((uint32_t)blocked, text, (int)sizeof(text));
    Log("sample: %s (blocked by %s)", allowed ? "allowed again" : "blocked",
        allowed ? "nothing" : text);
    if (g_hud && pHudShow) pHudShow(g_hud, allowed);
    SetStatus();
}

/* ---- the work --------------------------------------------------------- */
static DWORD WINAPI TickThread(LPVOID p) {
    (void)p;
    for (;;) {
        Sleep(500);
        /* Asked every pass and not remembered: the mode can be set while
         * this thread is asleep, and the callback is the polite path, not
         * the only one. */
        if (!InterlockedCompareExchange(&g_on, 0, 0)) continue;
        if (pAllowed && !pAllowed()) continue;
        InterlockedIncrement(&g_ticks);
        if (g_hud && pHudSet) {
            char line[64];
            snprintf(line, sizeof(line), "blacklist sample: %ld",
                     (long)InterlockedCompareExchange(&g_ticks, 0, 0));
            pHudSet(g_hud, line);
        }
        SetStatus();
    }
    return 0;
}

/* ---- the menu --------------------------------------------------------- */
static void OnActive(uint32_t menu, uint32_t item, int value, void *user) {
    (void)item; (void)user;
    InterlockedExchange(&g_on, value ? 1 : 0);
    if (g_hud && pHudShow) pHudShow(g_hud, value ? 1 : 0);
    SetStatus();
    if (pMenuSetValue) pMenuSetValue(menu, "Active", value);
}

static void OnShowState(uint32_t menu, uint32_t item, int value,
                        void *user) {
    (void)item; (void)value; (void)user;
    LogState("asked from the menu");
    pMenuStatus(menu, "state written to the log");
}

static DWORD WINAPI InitThread(LPVOID p) {
    HMODULE mod = NULL;

    (void)p;
    while (!mod) {
        mod = GetModuleHandleA("dinput8.dll");
        if (!mod) Sleep(500);
    }

    *(FARPROC *)&pGetVersion = GetProcAddress(mod, "ShGetVersion");
    *(FARPROC *)&pBlacklist = GetProcAddress(mod, "ShPluginBlacklist");
    *(FARPROC *)&pBlacklistModes =
        GetProcAddress(mod, "ShPluginBlacklistModes");
    *(FARPROC *)&pOnBlocked = GetProcAddress(mod, "ShPluginOnBlocked");
    *(FARPROC *)&pAllowed = GetProcAddress(mod, "ShPluginAllowed");
    *(FARPROC *)&pBlockedBy = GetProcAddress(mod, "ShPluginBlockedBy");
    *(FARPROC *)&pModeName = GetProcAddress(mod, "ShPlayModeName");
    *(FARPROC *)&pBlockedText = GetProcAddress(mod, "ShBlockedText");
    *(FARPROC *)&pMenuCreate = GetProcAddress(mod, "ShMenuCreate");
    *(FARPROC *)&pMenuToggle = GetProcAddress(mod, "ShMenuToggle");
    *(FARPROC *)&pMenuAction = GetProcAddress(mod, "ShMenuAction");
    *(FARPROC *)&pMenuStatus = GetProcAddress(mod, "ShMenuStatus");
    *(FARPROC *)&pMenuStatusF = GetProcAddress(mod, "ShMenuStatusF");
    *(FARPROC *)&pMenuSetValue = GetProcAddress(mod, "ShMenuSetValue");
    *(FARPROC *)&pMenuHint = GetProcAddress(mod, "ShMenuHint");
    *(FARPROC *)&pLastError = GetProcAddress(mod, "ShLastError");
    *(FARPROC *)&pHudCreate = GetProcAddress(mod, "ShHudCreate");
    *(FARPROC *)&pHudSet = GetProcAddress(mod, "ShHudSet");
    *(FARPROC *)&pHudShow = GetProcAddress(mod, "ShHudShow");

    if (!pGetVersion || !pBlacklist || !pOnBlocked || !pAllowed ||
        !pBlockedBy || !pMenuCreate || !pMenuToggle || !pMenuAction ||
        !pMenuStatus || !pHudCreate || !pHudSet) {
        Log("sample: a required export is missing, giving up");
        return 1;
    }
    while (!pGetVersion()) Sleep(500);

    /* Declare the blacklist before anything else runs, so the framework
     * knows where this plugin may be from the first moment. Ghost Mode
     * only: every other mode is fine. */
    if (!pBlacklist(SH_MODE_BLACKLIST_GHOST_MODE))
        Log("sample: the declaration was refused (last error %d)",
            pLastError ? pLastError() : -1);
    pOnBlocked(OnBlocked, NULL);

    g_hud = pHudCreate("blacklist_sample", SH_HUD_TOPLEFT, 0);
    g_menu = pMenuCreate("Blacklist sample");
    if (g_menu) {
        pMenuToggle(g_menu, "Active", 1, OnActive, NULL);
        pMenuAction(g_menu, "Write the state to the log", OnShowState,
                    NULL);
        pMenuHint(g_menu,
                  "Declared: blocked in Ghost Mode. The row and this page "
                  "go away in that mode, and the HUD line stops.");
    }
    LogState("started");
    SetStatus();
    CreateThread(NULL, 0, TickThread, NULL, 0, NULL);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        if (InterlockedExchange(&g_started, 1)) return TRUE;
        DisableThreadLibraryCalls(g_inst = inst);
        LogInit("scripthook_blacklist_sample.log");
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
    }
    return TRUE;
}
