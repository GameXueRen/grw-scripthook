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
        !p_PyGILState_Ensure || !p_PyGILState_Release)
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

/* The runtime: loads sh.py, imports plugins, drives tick()
 * and hot reload from python itself, one PyRun per frame.
 */
static const char *BOOT =
"import sys, traceback\n"
"import sh\n"
"sh._plugins = {}\n"
"sh._frame_fns = []\n"
"def _log(*a):\n"
"    sh.log(' '.join(str(x) for x in a))\n"
"def _load_all():\n"
"    import os, importlib\n"
"    d = sh.plugin_dir()\n"
"    for name in sorted(os.listdir(d)):\n"
"        if not name.endswith('.py') or name == 'sh.py':\n"
"            continue\n"
"        mod = name[:-3]\n"
"        try:\n"
"            m = importlib.import_module(mod)\n"
"            sh._plugins[mod] = m\n"
"            if hasattr(m, 'start'):\n"
"                m.start()\n"
"            if hasattr(m, 'on_frame'):\n"
"                sh._frame_fns.append((mod, m.on_frame))\n"
"            _log('loaded', mod)\n"
"        except Exception:\n"
"            _log('FAILED', mod)\n"
"            _log(traceback.format_exc())\n"
"try:\n"
"    _load_all()\n"
"except Exception:\n"
"    _log(traceback.format_exc())\n";

static const char *FRAME =
"import sh\n"
"for _n, _f in list(sh._frame_fns):\n"
"    try:\n"
"        _f()\n"
"    except Exception:\n"
"        import traceback\n"
"        sh.log('on_frame FAILED ' + _n)\n"
"        sh.log(traceback.format_exc())\n"
"        sh._frame_fns.remove((_n, _f))\n";

/* Exported for sh.py, so python can log into pyhost.log. */
__declspec(dllexport) void PyHostLog(const char *msg) {
    Log("%s", msg);
}
__declspec(dllexport) const char *PyHostGameDir(void) {
    return g_gameDir;
}

static void FrameCb(void *user) {
    PyGILState_STATE st;
    (void)user;
    if (!g_ready) return;
    st = p_PyGILState_Ensure();
    p_PyRun_SimpleString(FRAME);
    p_PyGILState_Release(st);
}

static DWORD WINAPI Boot(LPVOID unused) {
    HMODULE hook;
    int (*reg)(ShFrameFn_t, void *);
    PyGILState_STATE st;
    (void)unused;

    GameDir();
    while (!ShIsInGame()) Sleep(500);
    if (!LoadPython()) return 0;

    st = p_PyGILState_Ensure();
    p_PyRun_SimpleString(BOOT);
    p_PyGILState_Release(st);
    g_ready = 1;
    Log("python up, plugins loaded");

    hook = GetModuleHandleA("dinput8.dll");
    if (hook) {
        *(FARPROC *)&reg =
            GetProcAddress(hook, "ShRegisterFrameCallback");
        if (reg && reg(FrameCb, NULL)) {
            Log("frame callback registered");
            return 0;
        }
    }
    /* Old hook without the callback: 100 Hz thread drive. */
    Log("no frame callback, thread mode");
    for (;;) { FrameCb(NULL); Sleep(10); }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD why, LPVOID resv) {
    (void)inst; (void)resv;
    if (why == DLL_PROCESS_ATTACH)
        CreateThread(NULL, 0, Boot, NULL, 0, NULL);
    return TRUE;
}
