/* File interception, the worked example: a watcher, and a rule that
 * actually answers.
 *
 * This plugin is what docs/file-interception.md points at, in the same way
 * blacklist_sample.c is what docs/plugin-blacklist.md points at, so it
 * ships switched on and is built with every other plugin. It does three
 * things, and they are the three things a file rule is for:
 *
 *   watch   a DECIDE rule with no before callback and an after one: it is
 *           shown every open and every read, changes nothing, and counts
 *           them. This is what a diagnostic probe is (forgeprobe and
 *           GhostWipeProbe are the real ones).
 *   hide    a HIDE rule for a name that does not exist on disk, so the
 *           demonstration cannot break anything: with it on, any code that
 *           asks whether demo_hidden.bin is there is told it is not.
 *   query   the menu's status line reads ShFileMatchCount / ShFileInstalled
 *           / ShFileCallCount, which is what those are for.
 *
 * Two switches, both live from the F4 menu ("File interception sample"),
 * both persisted to the plugin's own ini:
 *
 *   [Settings]
 *   watch=1     show and count every open and read
 *   hide=0      answer "no such file" for demo_hidden.bin
 *
 * What to look for in a session: the status line's counts move as the game
 * loads; with `watch` on, logs\file_watch_sample.log records the first few
 * calls it was shown (which module asked, which API, which file) - and the
 * same calls are in logs\scripthook_files.log, attributed to this plugin
 * by name.
 *
 * Nothing here takes a hook of its own: the layer owns them all. That is
 * the point of the layer - this plugin, GhostNoWipe, skipintro and the
 * forge loader can all want CreateFileW and none of them has to know the
 * others exist.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "scripthook.h"
#include "log.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Live switches, read by a callback that runs on arbitrary engine threads
 * and written by the menu on its own thread, so they are volatile LONG
 * touched through Interlocked*. */
static volatile LONG g_watch = 1;
static volatile LONG g_hide;
static volatile LONG g_seen;            /* calls the watcher was shown */
static volatile LONG g_hidden;          /* times the hide rule answered */

static ShFileRule *g_watchRule;
static ShFileRule *g_hideRule;

static uint32_t g_menu;
typedef int (*MenuStatusF_t)(uint32_t menu, const char *fmt, ...);
static MenuStatusF_t g_statusF;

/* ---- what the watcher does (the layer's after callback) ---------------
 *
 * It runs inside the call it is about, on the caller's thread: keep it
 * short. Logging the first few and counting the rest is the whole point -
 * a per-call log line would be a per-call file write, which is exactly
 * what the layer's own rule about logging avoids.
 */
static void WatchAfter(ShFileCall *call, void *user) {
    char path[MAX_PATH];
    LONG n;

    (void)user;

    n = InterlockedIncrement(&g_seen);
    if (n > 8) return;

    path[0] = 0;
    if (call->path)
        WideCharToMultiByte(CP_ACP, 0, call->path, -1, path, sizeof(path),
                            NULL, NULL);
    else if (call->pathA)
        snprintf(path, sizeof(path), "%s", call->pathA);

    /* `api` is the kernel32 name ("CreateFileW", "ReadFile"), `done` is
     * what the call says it transferred, `error` is GetLastError at that
     * moment. A callback may change `result`/`error` before returning. */
    Log("watch %-22s ok=%d err=%lu done=%lu  %s",
        call->api ? call->api : "?", call->result ? 1 : 0,
        (unsigned long)call->error, (unsigned long)call->done,
        path[0] ? path : (call->handle ? "(handle)" : "(none)"));
}

/* ---- the rules --------------------------------------------------------- */

static void WatchOn(void) {
    ShFileRuleDesc d;

    if (g_watchRule) return;

    memset(&d, 0, sizeof(d));
    d.group  = SH_FILE_OPEN | SH_FILE_READ;
    d.action = SH_FILE_DECIDE;      /* a watcher: no before callback */
    d.after  = WatchAfter;

    g_watchRule = ShFileRuleAdd(&d);
    if (!g_watchRule)
        Log("watch rule was refused - see logs\\scripthook_files.log");
}

static void WatchOff(void) {
    if (!g_watchRule) return;
    ShFileRuleDel(g_watchRule);
    g_watchRule = NULL;
}

static void HideOn(void) {
    static const wchar_t name[] = L"demo_hidden.bin";
    ShFileRuleDesc d;

    if (g_hideRule) return;

    memset(&d, 0, sizeof(d));
    d.name   = name;                        /* the layer copies it */
    d.group  = SH_FILE_ATTR | SH_FILE_OPEN; /* the two a probe asks */
    d.action = SH_FILE_HIDE;

    g_hideRule = ShFileRuleAdd(&d);
    if (!g_hideRule)
        Log("hide rule was refused - see logs\\scripthook_files.log");
}

static void HideOff(void) {
    if (!g_hideRule) return;
    ShFileRuleDel(g_hideRule);
    g_hideRule = NULL;
}

/* ---- the line under the menu ------------------------------------------
 *
 * The whole query surface in one sentence: how many rules this plugin has
 * in the layer, whether the layer's hooks are up at all, and how many
 * calls have reached its dispatch this session.
 */
static void UpdateStatus(void) {
    if (!g_menu || !g_statusF) return;
    g_statusF(g_menu, "watching %ld, hidden %ld, rules in the layer %d, "
                      "hooks %s, calls seen %u",
              (long)InterlockedCompareExchange(&g_seen, 0, 0),
              (long)InterlockedCompareExchange(&g_hidden, 0, 0),
              ShFileMatchCount(),
              ShFileInstalled() ? "in place" : "out",
              (unsigned)ShFileCallCount());
}

/* ---- plugin ini -------------------------------------------------------- */

static HINSTANCE g_inst;
static char      g_iniPath[MAX_PATH];

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
    InterlockedExchange(&g_watch,
                        g_iniPath[0] ? GetPrivateProfileIntA("Settings", "watch",
                                                             1, g_iniPath)
                                     : 1);
    InterlockedExchange(&g_hide,
                        g_iniPath[0] ? GetPrivateProfileIntA("Settings", "hide",
                                                             0, g_iniPath)
                                     : 0);
}

static void SaveIni(void) {
    char buf[8];

    if (!g_iniPath[0]) return;
    snprintf(buf, sizeof(buf), "%d",
             (int)InterlockedCompareExchange(&g_watch, 0, 0));
    WritePrivateProfileStringA("Settings", "watch", buf, g_iniPath);
    snprintf(buf, sizeof(buf), "%d",
             (int)InterlockedCompareExchange(&g_hide, 0, 0));
    WritePrivateProfileStringA("Settings", "hide", buf, g_iniPath);
}

/* ---- menu -------------------------------------------------------------- */

typedef void (*MenuFn_t)(uint32_t menu, uint32_t item, int value, void *user);

static void OnWatch(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    InterlockedExchange(&g_watch, v ? 1 : 0);
    if (v) WatchOn(); else WatchOff();
    Log("menu: watch=%d", v);
    SaveIni();
    UpdateStatus();
}

static void OnHide(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    InterlockedExchange(&g_hide, v ? 1 : 0);
    if (v) HideOn(); else HideOff();
    Log("menu: hide=%d", v);
    SaveIni();
    UpdateStatus();
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
    { "@fw.page",  "File interception sample" },
    { "@fw.watch", "Watch file calls" },
    { "@fw.hide",  "Hide demo_hidden.bin" },
    { "@fw.hint",  "A worked example of the framework's file layer: one "
                   "watcher, one hide rule, and the query calls." }
};

static const TextRow kZh[] = {
    { "@fw.page",  "文件拦截示例" },
    { "@fw.watch", "监视文件调用" },
    { "@fw.hide",  "隐藏 demo_hidden.bin" },
    { "@fw.hint",  "框架文件层的完整示例：一个监视器、一条隐藏规则，以及查询"
                   "接口。" }
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
    pLangDeclare("file_watch_sample", "en-US", kEn,
                 (int)(sizeof(kEn) / sizeof(kEn[0])));
    pLangDeclare("file_watch_sample", "zh-CN", kZh,
                 (int)(sizeof(kZh) / sizeof(kZh[0])));
}

static void BuildMenu(HMODULE m) {
    uint32_t (*menuCreate)(const char *) = NULL;
    int (*menuToggle)(uint32_t, const char *, int, MenuFn_t, void *) = NULL;
    int (*menuHint)(uint32_t, const char *) = NULL;

    *(FARPROC *)&menuCreate = GetProcAddress(m, "ShMenuCreate");
    *(FARPROC *)&menuToggle = GetProcAddress(m, "ShMenuToggle");
    *(FARPROC *)&menuHint   = GetProcAddress(m, "ShMenuHint");
    *(FARPROC *)&g_statusF  = GetProcAddress(m, "ShMenuStatusF");
    if (!menuCreate || !menuToggle) return;

    TextInit();
    g_menu = menuCreate("@fw.page");
    menuToggle(g_menu, "@fw.watch", (int)InterlockedCompareExchange(
                   &g_watch, 0, 0), OnWatch, NULL);
    menuToggle(g_menu, "@fw.hide", (int)InterlockedCompareExchange(
                   &g_hide, 0, 0), OnHide, NULL);
    if (menuHint)
        menuHint(g_menu, "@fw.hint");
    UpdateStatus();
    Log("menu created");
}

/* ---- startup ----------------------------------------------------------- */

static DWORD WINAPI InitThread(LPVOID p) {
    HMODULE di;

    (void)p;

    /* log.h: this translation unit gets its own file and its own Log. */
    LogInit("file_watch_sample.log");
    Log("--- file interception sample ---");

    ResolveIniPath();
    LoadConfig();
    Log("watch=%ld hide=%ld",
        (long)InterlockedCompareExchange(&g_watch, 0, 0),
        (long)InterlockedCompareExchange(&g_hide, 0, 0));

    if (InterlockedCompareExchange(&g_watch, 0, 0)) WatchOn();
    if (InterlockedCompareExchange(&g_hide, 0, 0))  HideOn();

    di = GetModuleHandleA("dinput8.dll");
    if (di)
        BuildMenu(di);

    Log("ready");
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
