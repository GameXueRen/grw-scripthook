/* Where the game goes when a non-campaign mode returns to the main menu.
 *
 * The symptom, as reported: from Ghost War, Ghost Mode, a special
 * operation or any other non-campaign mode, returning to the main menu
 * makes the client vanish and come back up on its own. Every one of
 * them does it, and the community's answer is that this is how the
 * game works - the modes hold content the campaign does not, so the
 * client restarts rather than swapping it in place.
 *
 * That makes it a design, not a crash, which changes what can be done
 * about it. There is no fault to repair; to skip the restart the
 * engine would have to re-initialise its content in place, and whether
 * that is even reachable depends entirely on one thing this probe
 * exists to find out:
 *
 *   is the exit an unconditional "restart the client", or is it the
 *   far side of a condition - something that says "this mode and the
 *   one you are going back to do not share content, so restart"?
 *
 * A condition can be looked at; an unconditional exit cannot. So this
 * plugin does not try to change anything. It records, in
 * logs\ModeExitProbe.log:
 *
 *   - who asked for the exit, as a stack of module+offset frames. The
 *     immediate caller of ExitProcess is usually the C runtime, so the
 *     useful frame is further up: the game's own code is what we need
 *     the address of.
 *   - the exit code, the thread, and the state the game was in.
 *   - whether the process restarted itself, and with what command
 *     line. A fresh process that loads this plugin logs its own
 *     command line at startup, so the two can be compared even when
 *     the restart was performed by something outside this process.
 *
 * Every hook records and then calls the original. Nothing is blocked,
 * no code is answered differently, and a failure to install is logged
 * and left alone. Removing the plugin folder restores everything.
 *
 * Reading the result: find the "exit" line, take the first frame that
 * belongs to GRW.exe, and that RVA is the address to disassemble.
 *
 * ---------------------------------------------------------------------
 * What the probe found (2026-09-11), and the verdict: not fixable.
 *
 *   - The exit is asked for by the game itself: ExitProcess(0x29) with
 *     the game in MenuOrLobby, on a stack eight frames deep and every
 *     frame inside GRW.exe. Nothing in scripthook_crash.log matches it,
 *     so it is not a fault - the client leaves on purpose, carrying a
 *     non-zero code.
 *   - The relaunch is decided outside the process. The client starts
 *     Uplay with "-gamelauncher_wait_handle <n>" when it launches, so
 *     an instance of the launcher sits watching it; the exit that
 *     follows a mode switch starts no process of its own, and the game
 *     comes back as a new process started with
 *     "/launchedfromotherexec". The decision to come back is Uplay's,
 *     taken from the exit code, and it cannot be reached from here.
 *   - The code that asks for the exit is virtualised. The bytes at the
 *     calling RVAs are obfuscated control flow - constant arithmetic,
 *     register shuffling and far conditional jumps - which is a
 *     protection layer, not readable logic. There is nothing to read,
 *     so there is no condition to look for and none to flip.
 *
 * Verdict: this is the engine's design, not a defect. Blocking the
 * exit would only leave a client that has already torn its content
 * down, and the relaunch would happen anyway, being decided outside.
 * The plugin therefore stays a probe and is taken back out of the game
 * folder; the finding is recorded here and in the README so nobody
 * walks this path twice.
 * ---------------------------------------------------------------------
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "third_party/minhook/include/MinHook.h"

/* How many frames of the calling stack to record. The first two are
 * usually the runtime's own doexit/ExitProcess path. */
#define STACK_MAX 10

/* The framework's read-only state, bound by name at startup. */
typedef int (*GetState_t)(void);
typedef int (*StateName_t)(char *buf, int len);
typedef uint32_t (*GetUiState_t)(void);

static GetState_t    g_getState;
static StateName_t   g_stateName;
static GetUiState_t  g_getUiState;

/* ntdll's stack walk is exported by kernel32 under this name. */
typedef USHORT (WINAPI *CaptureStack_t)(ULONG framesToSkip,
                                        ULONG framesToCapture,
                                        PVOID *backTrace,
                                        PULONG backTraceHash);
static CaptureStack_t g_captureStack;

/* Logging is defined further down; the stack-walk binding below wants
 * to report what it found, so it needs the declaration here. */
static void ProbeLog(const char *fmt, ...);

/* The walker lives in ntdll and is forwarded by two other modules
 * depending on the Windows version. The first probe looked it up in
 * kernel32 alone, did not find it, and every stack in the log came out
 * empty - which is exactly the one thing this plugin cannot afford, so
 * all three are tried now. */
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
    ProbeLog("stack walk: not found - the caller will not be named");
}

static volatile LONG g_enabled = 1;

static int Enabled(void) {
    return InterlockedCompareExchange(&g_enabled, 0, 0) ? 1 : 0;
}

/* ---- logging ---------------------------------------------------------- */

static FILE *g_log;
static LONG  g_logBusy;

static void ProbeLog(const char *fmt, ...) {
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
        /* The process id is on every line: the game relaunches itself
         * (or is relaunched), so one log file holds several processes
         * and the events have to be told apart. */
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

    /* The main module is GRW.exe, so its folder is the game dir. */
    if (!GetModuleFileNameA(NULL, path, MAX_PATH)) return;
    slash = strrchr(path, '\\');
    if (!slash) return;
    slash[1] = 0;

    len = (int)strlen(path);
    if (len + 5 >= (int)sizeof(path)) return;
    strcpy(path + len, "logs");
    CreateDirectoryA(path, NULL);
    if (len + 26 < (int)sizeof(path))
        strcpy(path + len, "logs\\ModeExitProbe.log");
    else
        strcpy(path + len, "ModeExitProbe.log");
    g_log = fopen(path, "a");
}

/* ---- naming an address ------------------------------------------------ */

/* "GRW.exe+0x1A2B3C" for anything inside a module, "0x..." otherwise.
 * The game's own frames are the ones that matter; the rest are there
 * to show how deep the runtime plumbing goes. */
static void DescribeAddr(void *addr, char *out, size_t n) {
    HMODULE mod = NULL;
    char    file[MAX_PATH];
    const char *base;
    uintptr_t off;

    out[0] = 0;
    if (!addr) {
        snprintf(out, n, "(null)");
        return;
    }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) && mod) {
        if (!GetModuleFileNameA(mod, file, sizeof(file)))
            snprintf(file, sizeof(file), "?");
        base = strrchr(file, '\\');
        base = base ? base + 1 : file;
        off = (uintptr_t)addr - (uintptr_t)mod;
        snprintf(out, n, "%s+0x%llX", base, (unsigned long long)off);
        return;
    }
    snprintf(out, n, "0x%p", addr);
}

static const char *StateText(void) {
    static char buf[64];
    buf[0] = 0;
    if (g_stateName && !g_stateName(buf, (int)sizeof(buf)))
        buf[0] = 0;
    if (!buf[0]) snprintf(buf, sizeof(buf), "state#%d",
                         g_getState ? g_getState() : -1);
    return buf;
}

/* The stack, one frame per line. This is the whole point of the
 * plugin: the frame that names the game's own function. */
static void LogStack(const char *what, const char *detail) {
    void *frames[STACK_MAX];
    char  name[80];
    USHORT n = 0, i;

    ProbeLog("%s %s  state=%s ui=0x%X tid=%lu",
             what, detail, StateText(),
             g_getUiState ? g_getUiState() : 0u,
             (unsigned long)GetCurrentThreadId());

    if (g_captureStack)
        n = g_captureStack(0, STACK_MAX, frames, NULL);
    for (i = 0; i < n; i++) {
        DescribeAddr(frames[i], name, sizeof(name));
        ProbeLog("   frame[%u] %s", (unsigned)i, name);
    }
    if (!n)
        ProbeLog("   (no stack: CaptureStackBackTrace unavailable)");
}

/* ---- the hooks -------------------------------------------------------- */

typedef VOID (WINAPI *ExitProcess_t)(UINT code);
typedef BOOL (WINAPI *TerminateProcess_t)(HANDLE proc, UINT code);
typedef BOOL (WINAPI *CreateProcessW_t)(LPCWSTR app, LPWSTR cmd,
                                        LPSECURITY_ATTRIBUTES pa,
                                        LPSECURITY_ATTRIBUTES ta,
                                        BOOL inherit, DWORD flags,
                                        LPVOID env, LPCWSTR dir,
                                        LPSTARTUPINFOW si,
                                        LPPROCESS_INFORMATION pi);

static ExitProcess_t      g_realExitProcess;
static TerminateProcess_t g_realTerminateProcess;
static CreateProcessW_t   g_realCreateProcessW;

/* ExitProcess ends up in RtlExitUserProcess, and both may be reached
 * from the runtime's own exit path; only the first sighting is worth a
 * stack, the rest are noise. */
static volatile LONG g_exitSeen;

static VOID WINAPI HookExitProcess(UINT code) {
    if (Enabled() && !InterlockedExchange(&g_exitSeen, 1)) {
        char det[64];
        snprintf(det, sizeof(det), "ExitProcess(0x%X)", (unsigned)code);
        LogStack("exit ", det);
    }
    g_realExitProcess(code);
}

static BOOL WINAPI HookTerminateProcess(HANDLE proc, UINT code) {
    /* Only self-termination matters here; killing something else is
     * the game minding its own business. */
    if (Enabled() && proc && proc == GetCurrentProcess() &&
        !InterlockedExchange(&g_exitSeen, 1)) {
        char det[64];
        snprintf(det, sizeof(det), "TerminateProcess(self, 0x%X)",
                 (unsigned)code);
        LogStack("term ", det);
    }
    return g_realTerminateProcess(proc, code);
}

static BOOL WINAPI HookCreateProcessW(LPCWSTR app, LPWSTR cmd,
                                      LPSECURITY_ATTRIBUTES pa,
                                      LPSECURITY_ATTRIBUTES ta,
                                      BOOL inherit, DWORD flags,
                                      LPVOID env, LPCWSTR dir,
                                      LPSTARTUPINFOW si,
                                      LPPROCESS_INFORMATION pi) {
    BOOL ok = g_realCreateProcessW(app, cmd, pa, ta, inherit, flags,
                                   env, dir, si, pi);

    if (Enabled()) {
        /* A restart of the client shows up here as a second process
         * whose command line says what it was asked to come back as. */
        ProbeLog("spawn CreateProcessW ok=%d app=%ls cmd=%ls",
                 (int)ok,
                 app ? app : L"(null)",
                 cmd ? cmd : L"(null)");
        LogStack("spawn", "(caller of CreateProcessW)");
    }
    return ok;
}

/* ---- plugin ini -------------------------------------------------------- */

static HINSTANCE g_inst = NULL;
static char      g_iniPath[MAX_PATH];

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

static void LoadConfig(void) {
    int on = 1;                     /* probe: on unless told otherwise */

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
    ProbeLog("menu: probe=%s", Enabled() ? "on" : "off");
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
        uint32_t menu = menuCreate("Mode exit probe");
        menuToggle(menu, "Log exit and relaunch", Enabled(),
                   OnEnable, NULL);
        if (menuHint)
            menuHint(menu,
                     "Records who asks the game to exit when a non-campaign "
                     "mode returns to the main menu, and whether the process "
                     "relaunched itself. Nothing is changed.");
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

    /* By name, because kernel32 is loaded before any of this runs. */
    MH_CreateHookApi(L"kernel32.dll", "ExitProcess",
                     (LPVOID)HookExitProcess,
                     (LPVOID *)&g_realExitProcess);
    MH_CreateHookApi(L"kernel32.dll", "TerminateProcess",
                     (LPVOID)HookTerminateProcess,
                     (LPVOID *)&g_realTerminateProcess);
    MH_CreateHookApi(L"kernel32.dll", "CreateProcessW",
                     (LPVOID)HookCreateProcessW,
                     (LPVOID *)&g_realCreateProcessW);
    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK) {
        ProbeLog("install: MH_EnableHook failed (%d)", (int)st);
        return;
    }
    ProbeLog("install: ExitProcess=%p TerminateProcess=%p CreateProcessW=%p",
             (void *)g_realExitProcess, (void *)g_realTerminateProcess,
             (void *)g_realCreateProcessW);
    ProbeLog("install: waiting for a mode switch to leave the client");
}

static DWORD WINAPI InitThread(LPVOID p) {
    (void)p;
    OpenLog();
    ResolveIniPath();
    LoadConfig();

    ProbeLog("--- ModeExitProbe: where a mode switch leaves the client ---");
    ProbeLog("build " __DATE__ " " __TIME__);
    ProbeLog("start: cmdline=%s", GetCommandLineA());
    ProbeLog("config: probe=%s, ini=%s", Enabled() ? "on" : "off",
             g_iniPath[0] ? g_iniPath : "(none)");

    {
        HMODULE di = GetModuleHandleA("dinput8.dll");
        if (di) {
            *(FARPROC *)&g_getState =
                GetProcAddress(di, "ShGetGameState");
            *(FARPROC *)&g_stateName =
                GetProcAddress(di, "ShGetGameStateName");
            *(FARPROC *)&g_getUiState =
                GetProcAddress(di, "ShGetUiState");
            BuildMenu(di);
        }
    }
    BindStackWalk();
    ProbeLog("bound: state=%p name=%p ui=%p stack=%p",
             (void *)g_getState, (void *)g_stateName,
             (void *)g_getUiState, (void *)g_captureStack);

    if (Enabled())
        InstallHooks();
    else
        ProbeLog("probe is off: nothing is hooked");

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
