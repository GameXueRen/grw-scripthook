/* Python plugin host. Embeds the Windows CPython from
 * <gamedir>/python/ and runs the .py plugins.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "scripthook.h"

/* The C API is loaded dynamically so this .asi still loads
 * with no python folder installed. Py_ssize_t is intptr_t.
 */
typedef intptr_t Py_ssize_t;
typedef void PyObject;
typedef int PyGILState_STATE;

static void      (*p_Py_InitializeEx)(int);
static int       (*p_Py_IsInitialized)(void);
static void      (*p_PyEval_SaveThread_v)(void);
static int       (*p_PyRun_SimpleString)(const char *);
static PyGILState_STATE (*p_PyGILState_Ensure)(void);
static void      (*p_PyGILState_Release)(PyGILState_STATE);
static void      (*p_Py_SetPythonHome)(const wchar_t *);
static void      (*p_Py_SetPath)(const wchar_t *);

static HMODULE g_py;
static char    g_gameDir[MAX_PATH];
static volatile int g_ready = 0;

static void Log(const char *fmt, ...) {
    char path[MAX_PATH + 32], line[512];
    va_list ap;
    FILE *f;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    snprintf(path, sizeof(path), "%s\\pyhost.log", g_gameDir);
    f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
}

static void GameDir(void) {
    char *s;
    GetModuleFileNameA(NULL, g_gameDir, sizeof(g_gameDir));
    s = strrchr(g_gameDir, '\\');
    if (s) *s = 0;
}

static FARPROC Need(const char *name) {
    FARPROC p = GetProcAddress(g_py, name);
    if (!p) Log("missing python symbol %s", name);
    return p;
}

static int LoadPython(void) {
    char dll[MAX_PATH + 32];
    wchar_t home[MAX_PATH], paths[MAX_PATH * 3];

    snprintf(dll, sizeof(dll), "%s\\python\\python312.dll",
             g_gameDir);
    g_py = LoadLibraryA(dll);
    if (!g_py) {
        Log("no python at %s (error %lu), pyhost idle",
            dll, GetLastError());
        return 0;
    }
    *(FARPROC *)&p_Py_SetPythonHome = Need("Py_SetPythonHome");
    *(FARPROC *)&p_Py_SetPath       = Need("Py_SetPath");
    *(FARPROC *)&p_Py_InitializeEx  = Need("Py_InitializeEx");
    *(FARPROC *)&p_Py_IsInitialized = Need("Py_IsInitialized");
    *(FARPROC *)&p_PyRun_SimpleString = Need("PyRun_SimpleString");
    *(FARPROC *)&p_PyGILState_Ensure  = Need("PyGILState_Ensure");
    *(FARPROC *)&p_PyGILState_Release = Need("PyGILState_Release");
    *(FARPROC *)&p_PyEval_SaveThread_v =
        GetProcAddress(g_py, "PyEval_SaveThread");
    if (!p_Py_InitializeEx || !p_PyRun_SimpleString ||
        !p_PyGILState_Ensure || !p_PyGILState_Release ||
        !p_PyEval_SaveThread_v)
        return 0;

    _snwprintf(home, MAX_PATH, L"%hs\\python", g_gameDir);
    /* python312.zip is the stdlib; plugins live beside the
     * game so scripts are where users expect files. */
    _snwprintf(paths, MAX_PATH * 3,
               L"%hs\\python\\python312.zip;%hs\\python;%hs\\plugins",
               g_gameDir, g_gameDir, g_gameDir);
    if (p_Py_SetPythonHome) p_Py_SetPythonHome(home);
    if (p_Py_SetPath) p_Py_SetPath(paths);
    p_Py_InitializeEx(0);
    return p_Py_IsInitialized && p_Py_IsInitialized();
}

/* The runtime lives in python/_pyhost.py: loading, hot
 * reload and dispatch. Editing it needs no rebuild. */
static const char *BOOT =
"import traceback\n"
"try:\n"
"    import _pyhost\n"
"except Exception:\n"
"    import sh\n"
"    sh.log('runtime FAILED\\n' + traceback.format_exc())\n";

static const char *FRAME = "_pyhost.tick()\n";

/* One line of python, queued for the driver thread: the
 * menu callback runs on the game thread and must not touch
 * the interpreter itself. */
#define PEND_MAX 8
static char g_pend[PEND_MAX][128];
static volatile int g_pendN = 0;

static void Queue(const char *code) {
    int n = g_pendN;
    if (n >= PEND_MAX) return;
    strncpy(g_pend[n], code, sizeof(g_pend[0]) - 1);
    g_pend[n][sizeof(g_pend[0]) - 1] = 0;
    g_pendN = n + 1;
}

/* Menu rows. The list of plugins is python's business, so
 * the rows are fixed and the actions call into it. */
static uint32_t g_menu;
static int g_autoReload = 1;

static void OnReload(uint32_t menu, uint32_t item, int value,
                     void *user) {
    (void)menu; (void)item; (void)value; (void)user;
    Queue("_pyhost.reload_all()");
}

static void OnList(uint32_t menu, uint32_t item, int value,
                   void *user) {
    (void)menu; (void)item; (void)value; (void)user;
    Queue("_pyhost.report()");
}

static void OnAuto(uint32_t menu, uint32_t item, int value,
                   void *user) {
    (void)menu; (void)item; (void)user;
    g_autoReload = value;
    Queue(value ? "_pyhost.set_auto(True)"
                : "_pyhost.set_auto(False)");
}

static void BuildMenu(void) {
    g_menu = ShMenuCreate("Python mods");
    if (!g_menu) return;
    ShMenuAction(g_menu, "Reload all", OnReload, NULL);
    ShMenuAction(g_menu, "Log loaded mods", OnList, NULL);
    ShMenuToggle(g_menu, "Watch files", g_autoReload, OnAuto, NULL);
}

/* Exported for sh.py, so python can log into pyhost.log. */
__declspec(dllexport) void PyHostLog(const char *msg) {
    Log("%s", msg);
}

/* Python asks for the menu status line and pending code. */
__declspec(dllexport) void PyHostStatus(const char *text) {
    if (g_menu) ShMenuStatus(g_menu, text);
}

/* A plugin's own menu. The item callback lands on the game
 * thread, so it only queues a call for the driver. */
static void OnPlugin(uint32_t menu, uint32_t item, int value,
                     void *user) {
    char code[128];
    (void)menu; (void)item;
    snprintf(code, sizeof(code), "_pyhost.fire(%d, %d)",
             (int)(intptr_t)user, value);
    Queue(code);
}

__declspec(dllexport) uint32_t PyHostMenu(const char *title) {
    return ShMenuCreate(title);
}

__declspec(dllexport) uint32_t PyHostMenuSub(uint32_t parent,
                                             const char *label) {
    return ShMenuSub(parent, label);
}

__declspec(dllexport) int PyHostMenuAction(uint32_t menu,
                                           const char *label,
                                           int token) {
    return ShMenuAction(menu, label, OnPlugin,
                        (void *)(intptr_t)token);
}

__declspec(dllexport) int PyHostMenuToggle(uint32_t menu,
                                           const char *label,
                                           int initial, int token) {
    return ShMenuToggle(menu, label, initial, OnPlugin,
                        (void *)(intptr_t)token);
}

__declspec(dllexport) int PyHostMenuNumber(uint32_t menu,
                                           const char *label,
                                           float initial, float lo,
                                           float hi, float step,
                                           int token) {
    return ShMenuNumber(menu, label, initial, lo, hi, step,
                        OnPlugin, (void *)(intptr_t)token);
}

/* The engine keeps the option strings, so they are copied
 * into a table that lives as long as the process. */
#define OPT_MAX 256
static char *g_opts[OPT_MAX];
static int g_optN;

__declspec(dllexport) int PyHostMenuList(uint32_t menu,
                                         const char *label,
                                         const char *csv,
                                         int initial, int token) {
    const char **opts;
    char *copy, *p;
    int n = 0, base = g_optN;

    if (!csv) return 0;
    copy = _strdup(csv);
    if (!copy) return 0;
    for (p = strtok(copy, "|"); p && g_optN < OPT_MAX;
         p = strtok(NULL, "|")) {
        g_opts[g_optN++] = _strdup(p);
        n++;
    }
    free(copy);
    if (!n) return 0;
    opts = (const char **)&g_opts[base];
    return ShMenuList(menu, label, opts, n, initial, OnPlugin,
                      (void *)(intptr_t)token);
}

__declspec(dllexport) int PyHostMenuStatus(uint32_t menu,
                                           const char *text) {
    return ShMenuStatus(menu, text);
}

__declspec(dllexport) int PyHostMenuClear(uint32_t menu) {
    return ShMenuClear(menu);
}

__declspec(dllexport) int PyHostMenuDestroy(uint32_t menu) {
    return ShMenuDestroy(menu);
}

/* Hit and shot events. The engine reports them on its own
 * thread, so each one is copied into a ring and python
 * drains it from the driver. */
#define EVT_MAX 64
static ShHit  g_hits[EVT_MAX];
static ShShot g_shots[EVT_MAX];
static volatile int g_hitW, g_hitR, g_shotW, g_shotR;

static void OnHitEvent(const ShHit *hit, void *user) {
    int w = g_hitW;
    (void)user;
    if (!hit) return;
    g_hits[w % EVT_MAX] = *hit;
    g_hitW = w + 1;
}

static void OnFireEvent(const ShShot *shot, void *user) {
    int w = g_shotW;
    (void)user;
    if (!shot) return;
    g_shots[w % EVT_MAX] = *shot;
    g_shotW = w + 1;
}

__declspec(dllexport) int PyHostWatchHits(int on) {
    if (on) {
        ShHitHookInstall();
        return ShOnHit(OnHitEvent, NULL, 0);
    }
    return ShOffHit(OnHitEvent);
}

__declspec(dllexport) int PyHostWatchFire(int on) {
    if (on) {
        ShHitHookInstall();
        return ShOnFire(OnFireEvent, NULL, 0);
    }
    return ShOffFire(OnFireEvent);
}

/* UI input. The engine wants a verdict now, so the handler
 * declares up front whether the scene eats its keys, and
 * the event itself is queued for python. */
typedef struct { uint32_t scene; ShUiEvent ev; } UiRec;
static UiRec g_uiEvts[EVT_MAX];
static volatile int g_uiW, g_uiR;
static int g_uiEat;

static int OnUiInput(uint32_t scene, const ShUiEvent *e,
                     void *user) {
    int w = g_uiW;
    (void)user;
    if (!e) return 0;
    g_uiEvts[w % EVT_MAX].scene = scene;
    g_uiEvts[w % EVT_MAX].ev = *e;
    g_uiW = w + 1;
    return g_uiEat;
}

__declspec(dllexport) int PyHostUiInput(uint32_t scene, int eat) {
    g_uiEat = eat ? 1 : 0;
    return ShUiSetInput(scene, OnUiInput, NULL);
}

__declspec(dllexport) int PyHostNextUiEvent(uint32_t *scene,
                                            ShUiEvent *out) {
    if (!scene || !out || g_uiR == g_uiW) return 0;
    if (g_uiW - g_uiR > EVT_MAX) g_uiR = g_uiW - EVT_MAX;
    *scene = g_uiEvts[g_uiR % EVT_MAX].scene;
    *out = g_uiEvts[g_uiR % EVT_MAX].ev;
    g_uiR++;
    return 1;
}

/* Async commit: the completion is queued the same way. */
static volatile int g_commitW, g_commitR;
static int g_commitOk[EVT_MAX];

static void OnCommitDone(int ok, void *user) {
    int w = g_commitW;
    (void)user;
    g_commitOk[w % EVT_MAX] = ok;
    g_commitW = w + 1;
}

__declspec(dllexport) int PyHostCommitAsync(void) {
    return ShUiCommitAsync(OnCommitDone, NULL);
}

__declspec(dllexport) int PyHostNextCommit(int *ok) {
    if (!ok || g_commitR == g_commitW) return 0;
    if (g_commitW - g_commitR > EVT_MAX)
        g_commitR = g_commitW - EVT_MAX;
    *ok = g_commitOk[g_commitR % EVT_MAX];
    g_commitR++;
    return 1;
}

/* Oldest first; skips ahead when the ring overflows. */
__declspec(dllexport) int PyHostNextHit(ShHit *out) {
    if (!out || g_hitR == g_hitW) return 0;
    if (g_hitW - g_hitR > EVT_MAX) g_hitR = g_hitW - EVT_MAX;
    *out = g_hits[g_hitR % EVT_MAX];
    g_hitR++;
    return 1;
}

__declspec(dllexport) int PyHostNextShot(ShShot *out) {
    if (!out || g_shotR == g_shotW) return 0;
    if (g_shotW - g_shotR > EVT_MAX) g_shotR = g_shotW - EVT_MAX;
    *out = g_shots[g_shotR % EVT_MAX];
    g_shotR++;
    return 1;
}
__declspec(dllexport) const char *PyHostGameDir(void) {
    return g_gameDir;
}

/* The game thread only signals. CPython inside the physics
 * hook cost stack and frame time and broke the UI. */
static HANDLE g_frameEvent;

static void FrameCb(void *user) {
    (void)user;
    if (g_frameEvent) SetEvent(g_frameEvent);
}

/* Python runs here, at frame cadence, off the game thread.
 * Calls needing the game thread queue themselves, which is
 * what the compiled plugins already do. */
static DWORD WINAPI Driver(LPVOID unused) {
    PyGILState_STATE st;
    (void)unused;

    for (;;) {
        if (g_frameEvent)
            WaitForSingleObject(g_frameEvent, 100);
        else
            Sleep(10);
        if (!g_ready) continue;
        st = p_PyGILState_Ensure();
        while (g_pendN > 0) {
            int n = --g_pendN;
            p_PyRun_SimpleString(g_pend[n]);
        }
        p_PyRun_SimpleString(FRAME);
        p_PyGILState_Release(st);
    }
}

static DWORD WINAPI Boot(LPVOID unused) {
    HMODULE hook;
    int (*reg)(ShFrameFn_t, void *);
    (void)unused;

    GameDir();
    while (!ShIsInGame()) Sleep(500);
    if (!LoadPython()) return 0;

    /* Py_InitializeEx leaves this thread holding the GIL.
     * Boot with it, then RELEASE it, or the frame hook's
     * Ensure blocks the game thread forever (it did). */
    p_PyRun_SimpleString(BOOT);
    p_PyEval_SaveThread_v();
    g_ready = 1;
    Log("python up, plugins loaded");

    g_frameEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    CreateThread(NULL, 0, Driver, NULL, 0, NULL);
    BuildMenu();

    hook = GetModuleHandleA("dinput8.dll");
    if (hook) {
        *(FARPROC *)&reg =
            GetProcAddress(hook, "ShRegisterFrameCallback");
        if (reg && reg(FrameCb, NULL)) {
            Log("frame signal registered");
            return 0;
        }
    }
    /* Old hook with no callback: the 100 ms wait timeout in
     * the driver keeps plugins ticking anyway. */
    Log("no frame signal, driver runs on its timeout");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD why, LPVOID resv) {
    (void)inst; (void)resv;
    if (why == DLL_PROCESS_ATTACH)
        CreateThread(NULL, 0, Boot, NULL, 0, NULL);
    return TRUE;
}
