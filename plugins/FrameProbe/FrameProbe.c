/* FrameProbe: what the framework's frame hook actually does.
 *
 * scripthook.h has declared ShRegisterFrameCallback for a long time, and
 * this build is the first with an implementation behind it
 * (scripthook_frame.c; the site and the bytes come from GhostHook). This
 * plugin is how that gets checked from inside a running game: it registers
 * one callback, counts the calls, and writes a line a second.
 *
 * Read the log:
 *   a count that tracks the frame rate   the hook is in, the callback runs
 *   a count stuck at 0                   the site went stale - the address
 *                                        is named in scripthook_frame.log
 *   "no ShRegisterFrameCallback"         the framework predates the hook
 *
 * Late binding, like the other probes here: a framework built before the
 * implementation exports nothing by that name, and this plugin says so
 * instead of failing to load at all. It installs no hook of its own and
 * needs no import library.
 *
 * Logs to <gamedir>\logs\FrameProbe.log
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#include "scripthook.h"
#include "log.h"

/* Nothing newer than the first version of the API: the registration has
 * been declared since then, and this plugin binds it by name either way. */
SH_REQUIRES_API(1);

typedef int      (*RegisterFrame_t)(ShFrameFn_t fn, void *user);
typedef void     (*UnregisterFrame_t)(ShFrameFn_t fn);
typedef uint32_t (*MenuCreate_t)(const char *);
typedef int      (*MenuStatusF_t)(uint32_t menu, const char *fmt, ...);
typedef int      (*LangDeclare_t)(const char *owner, const char *lang,
                                  const ShText *rows, int n);

static RegisterFrame_t   pRegister;
static UnregisterFrame_t pUnregister;
static MenuCreate_t      pMenuCreate;
static MenuStatusF_t     pMenuStatusF;
static LangDeclare_t     pLangDeclare;
static uint32_t          g_menu;

static volatile LONG g_frames;   /* calls since the last report */
static volatile LONG g_total;    /* calls since registration    */
static volatile LONG g_peak;     /* busiest second so far       */

static const ShText kEn[] = {
    { "@fprobe.page",   "Frame probe" },
    { "@fprobe.status", "frame callback: %d call(s)/s, peak %d, total %d" }
};
static const ShText kZh[] = {
    { "@fprobe.page",   "帧回调探针" },
    { "@fprobe.status", "帧回调：%d 次/秒，峰值 %d，累计 %d" }
};

/* Runs on the game thread, inside the engine's own spawn-director update.
 * Keep it this short: the frame waits for it. */
static void OnFrame(void *user) {
    (void)user;
    InterlockedIncrement(&g_frames);
    InterlockedIncrement(&g_total);
}

/* One line a second, so a count of zero is as visible as a count that
 * tracks the frame rate. */
static DWORD WINAPI TickThread(LPVOID p) {
    (void)p;

    for (;;) {
        LONG n, peak, total;

        Sleep(1000);
        n = InterlockedExchange(&g_frames, 0);
        total = InterlockedCompareExchange(&g_total, 0, 0);
        if (n > g_peak) g_peak = n;
        peak = g_peak;

        Log("frame: %ld call(s) in the last second (peak %ld, total %ld)",
            n, peak, total);
        if (pMenuStatusF && g_menu)
            pMenuStatusF(g_menu, "@fprobe.status", (int)n, (int)peak,
                         (int)total);
    }
    return 0;
}

static DWORD WINAPI BindThread(LPVOID p) {
    HMODULE mod = NULL;
    HANDLE h;
    (void)p;

    LogInit("FrameProbe.log");
    while (!mod) {
        mod = GetModuleHandleA("dinput8.dll");
        if (!mod) Sleep(500);
    }

    *(FARPROC *)&pRegister    = GetProcAddress(mod, "ShRegisterFrameCallback");
    *(FARPROC *)&pUnregister  = GetProcAddress(mod, "ShUnregisterFrameCallback");
    *(FARPROC *)&pMenuCreate  = GetProcAddress(mod, "ShMenuCreate");
    *(FARPROC *)&pMenuStatusF = GetProcAddress(mod, "ShMenuStatusF");
    *(FARPROC *)&pLangDeclare = GetProcAddress(mod, "ShLangDeclare");

    if (!pRegister) {
        Log("frame: this framework has no ShRegisterFrameCallback - the frame "
            "hook is not in it (it needs the build that carries "
            "scripthook_frame.c)");
        return 1;
    }
    Log("frame: exports register=%d unregister=%d", pRegister != NULL,
        pUnregister != NULL);

    if (pLangDeclare) {
        pLangDeclare("FrameProbe", "en-US", kEn, 2);
        pLangDeclare("FrameProbe", "zh-CN", kZh, 2);
    }
    if (pMenuCreate) g_menu = pMenuCreate("@fprobe.page");

    /* Unregister first. The loader runs its plugin set more than once per
     * process (two "plugin scan done" lines, seconds apart, in
     * scripthook.log), so this module can be bound twice; without this the
     * callback would run twice a frame and the count below would be double
     * the frame rate. Unregistering something that is not registered does
     * nothing. */
    if (pUnregister) pUnregister(OnFrame);

    if (!pRegister(OnFrame, NULL)) {
        Log("frame: registration refused - the site did not hold the pinned "
            "prologue; scripthook_frame.log names the address");
        return 1;
    }
    Log("frame: registered; the first count follows in a second");

    h = CreateThread(NULL, 0, TickThread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        CreateThread(NULL, 0, BindThread, NULL, 0, NULL);
    return TRUE;
}
