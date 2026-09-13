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
 * ---- one shot, then the rules come back out ---------------------------
 *
 * The engine probes the names it wants in ONE burst during startup: over
 * fifteen sessions (logs\skipintro.log, 10116 lines) every process had
 * the same shape - four or five names asked for inside the same
 * millisecond, about 18 to 30 seconds after the hooks went in, and not
 * one further probe until that process exited. Once the burst is over,
 * intercepting buys nothing; it just sits in every file call the game
 * makes for the rest of the session.
 *
 * So the plugin gives its rules back, on a watcher thread, as soon as
 * either of these is true:
 *
 *   1. every name the ENABLED groups cover has been intercepted - 4/4
 *      with both groups on, 2/2 with one - the main way out;
 *   2. the main menu has been up once - the keeper, for a session where
 *      the count never fills because the engine never asks for a name.
 *
 * After that the plugin holds nothing at all for the rest of the
 * session, and the two switches can only take effect on the next launch -
 * the line under the menu says so. With the interception layer behind it
 * (scripthook_files.c) that is literally true: the layer uninstalls its
 * own hooks when the last rule goes, so a session that started with this
 * plugin on and released ends with not one file call intercepted, which
 * the IAT write-back this plugin used to do could never promise.
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
 * How the hiding is done: the framework's file interception layer owns
 * the file APIs for the whole process (scripthook_files.c), so this
 * plugin registers one SH_FILE_HIDE rule per name and gets the answer a
 * missing file would have produced, without touching a byte of memory
 * itself. BinkOpen is still not touched at all: hooking it cannot skip
 * clips (see above) and the existence layer alone turns the whole
 * sequence off.
 *
 * The layer is also what makes the four names a set of rules rather than
 * four hand-written detours: the two switches add and remove their two
 * rules live, and the layer installs itself with the first rule and takes
 * itself out with the last. That is why this plugin no longer parses the
 * import table: it used to write four slots in GRW.exe - which only ever
 * affected the game's own calls - while the layer answers for every
 * module in the process.
 *
 * ---- and there is nothing like it in the Forge mod loader ------------
 *
 * Checked 2026-09-13: Forge serves loose files from mods\ over entries
 * INSIDE .forge archives (it reads them through the same layer now) - it
 * neither hides nor replaces a loose file on disk, which is what these
 * clips are. It used to be the reason two modules could not both be on
 * CreateFileW (MinHook keeps one hook per target); with the layer owning
 * the target, that whole class of collision is gone. So there is no
 * duplicate implementation of "skip the intro" to factor out into a
 * shared API.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* The framework's file interception layer, whose rules this plugin uses
 * instead of patching the game's import table itself. Linked, not
 * late-bound: the types are the contract, and getting them wrong would be
 * worse than an unresolved symbol. */
#include "scripthook.h"

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

/* The names are matched by the layer's own rule table now, so no wide
 * copies are kept here: the only conversion is the one at registration. */

/* ---- the four rules ----------------------------------------------------
 * One per name, and these are what the layer runs: a HIDE rule answers the
 * call the way a missing file would, and its after callback is only there
 * to count the name as intercepted - which is what the release condition
 * waits for. A rule that has answered once is the proof.
 *
 * They are added and removed live by the two switches, and the layer
 * installs its own hooks with the first of them and takes them out again
 * with the last: this plugin never owns a hook.
 */
typedef struct {
    ShFileRule   *rule;
    int           group;    /* 0 launch, 1 legal                    */
    char          name[NAME_MAX];
    volatile LONG hit;
} SkipRule;

#define RULES_MAX (ARRAY_LEN(g_launch) + ARRAY_LEN(g_legal))

static SkipRule       g_rules[RULES_MAX];
static volatile LONG  g_nrules;     /* rules live right now            */
static volatile LONG  g_released;   /* 1 = the rules are out again     */
static DWORD          g_started;    /* when the first rule went in     */

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

/* ---- the switches, and the rules they own -----------------------------
 *
 * Both switches are live until the release; after it the layer may already
 * have taken its hooks away, which is exactly what "takes effect after a
 * restart" means on the line under the menu.
 */

static int WantLaunch(void) {
    return InterlockedCompareExchange(&g_skipLaunch, 0, 0) ? 1 : 0;
}

static int WantLegal(void) {
    return InterlockedCompareExchange(&g_skipLegal, 0, 0) ? 1 : 0;
}

static int Released(void) {
    return InterlockedCompareExchange(&g_released, 0, 0) ? 1 : 0;
}

/* The layer's after callback on a HIDE rule: the call was answered, and
 * that is all the release needs to know about it. */
static void NoteHidden(ShFileCall *call, void *user) {
    SkipRule *r = (SkipRule *)user;

    (void)call;
    if (r) InterlockedIncrement(&r->hit);
}

/* The rules of one group, on or off. Each name is its own rule, because
 * that is what the layer takes: one file, one answer. Everything the
 * layer needs is copied at registration, so the wide name below is only
 * alive for the call. */
static void GroupRules(int group, int on) {
    const char *const *list = group ? g_legal : g_launch;
    int  count = group ? (int)ARRAY_LEN(g_legal) : (int)ARRAY_LEN(g_launch);
    int  base  = group ? (int)ARRAY_LEN(g_launch) : 0;
    int  i;

    for (i = 0; i < count; i++) {
        SkipRule *r = &g_rules[base + i];

        if (on) {
            ShFileRuleDesc d;
            wchar_t        w[NAME_MAX];

            if (r->rule) continue;
            if (MultiByteToWideChar(CP_ACP, 0, list[i], -1, w,
                                    NAME_MAX) <= 0)
                continue;

            snprintf(r->name, sizeof(r->name), "%s", list[i]);
            r->group = group;

            memset(&d, 0, sizeof(d));
            d.name   = w;                            /* copied by the layer */
            d.group  = SH_FILE_ATTR | SH_FILE_OPEN;  /* what it used to hook */
            d.action = SH_FILE_HIDE;
            d.after  = NoteHidden;
            d.user   = r;
            r->rule  = ShFileRuleAdd(&d);
            if (r->rule) {
                InterlockedIncrement(&g_nrules);
                SkipLog("rule  hide %s (group %s)", r->name,
                        group ? "legal" : "launch");
            } else {
                SkipLog("rule for %s was refused by the layer", r->name);
            }
        } else {
            if (!r->rule) continue;
            ShFileRuleDel(r->rule);
            r->rule = NULL;
            InterlockedDecrement(&g_nrules);
        }
    }
}

/* How many names the enabled groups cover right now, and how many of them
 * have actually been asked about: 4/4 with both on, 2/2 with one, 0 with
 * neither. */
static int NeedCount(void) {
    int n = 0;

    if (WantLaunch()) n += (int)ARRAY_LEN(g_launch);
    if (WantLegal())  n += (int)ARRAY_LEN(g_legal);
    return n;
}

static int HaveCount(void) {
    int i, c = 0;

    for (i = 0; i < (int)ARRAY_LEN(g_rules); i++)
        if (InterlockedCompareExchange(&g_rules[i].hit, 0, 0)) c++;
    return c;
}

/* The four detours that used to live here - GetFileAttributesA/W and
 * CreateFileA/W, each answering not-found for the names above - are the
 * layer's now, and this plugin has no hook, no function pointer and no
 * opinion about which module in the process is asking. */

/* ---- the rules this plugin registered ----------------------------------- */

/* Everything past this point is bookkeeping: the switches, the release and
 * the menu. The interception itself is the layer's. */
/* ---- the release -------------------------------------------------------
 * What has been intercepted, and giving the rules back.
 */

/* The line under the menu. One sentence, and it is true in every state the
 * plugin can be in: the switches are honoured while the hooks are in, but
 * for the rest of the session - which is most of it - changing one can only
 * shape the next launch. English template; ShMenuStatusF translates it in
 * this menu's own scope. */
static void UpdateStatus(void) {
    if (!g_menu || !g_statusF) return;
    g_statusF(g_menu, "Changing a switch takes effect after a restart");
}

/* Give the rules back. This runs on the watcher thread, and the flag goes
 * up first: a call already inside the layer's dispatch stops hiding
 * anything from that moment on. The layer takes its hooks out when the
 * last rule is gone, so this is also what puts the process back the way it
 * was found - there is no slot of ours to check any more. */
static void ReleaseAll(const char *why) {
    if (InterlockedExchange(&g_released, 1)) return;   /* once, ever */
    GroupRules(0, 0);
    GroupRules(1, 0);
    SkipLog("released (%s) after %lu ms; %lu file call(s) had been through "
            "the layer by then",
            why, (unsigned long)(GetTickCount() - g_started),
            (unsigned long)ShFileCallCount());
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
            ReleaseAll(why);
            return 0;
        }
        if (need == 0) {
            ReleaseAll("both groups were switched off");
            return 0;
        }
        if (g_getGameState && g_getGameState() == SH_STATE_MENU_LOCAL) {
            ReleaseAll("the main menu was up");
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
    if (!Released()) GroupRules(0, WantLaunch());
    SkipLog("menu: skip_launch_videos=%d%s", WantLaunch(),
            Released() ? " (the rules are already out - this applies to the "
                         "next launch)" : "");
    SaveIni();
}

static void OnLegal(int v) {
    InterlockedExchange(&g_skipLegal, v ? 1 : 0);
    if (!Released()) GroupRules(1, WantLegal());
    SkipLog("menu: skip_legal_videos=%d%s", WantLegal(),
            Released() ? " (the rules are already out - this applies to the "
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
    SkipLog("--- skipintro plugin, hiding through the framework's layer ---");
    ResolveIniPath();
    LoadConfig();
    LogList("launch targets", g_launch, (int)ARRAY_LEN(g_launch));
    LogList("legal targets", g_legal, (int)ARRAY_LEN(g_legal));

    di = GetModuleHandleA("dinput8.dll");
    if (di) {
        *(FARPROC *)&g_getGameState = GetProcAddress(di, "ShGetGameState");
        BuildMenu(di);
    }

    /* Nothing enabled means nothing to give back: an install with both
     * groups off costs the game not one intercepted call, all session
     * long - the layer is not even installed. */
    if (!WantLaunch() && !WantLegal()) {
        SkipLog("both groups are off - not one file call is intercepted");
        return 0;
    }

    g_started = GetTickCount();
    GroupRules(0, WantLaunch());
    GroupRules(1, WantLegal());
    SkipLog("ready - %ld rule(s) in the layer, and they come back out as "
            "soon as the enabled names have been intercepted",
            (long)InterlockedCompareExchange(&g_nrules, 0, 0));
    UpdateStatus();
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
