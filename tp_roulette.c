/* API exerciser: every 30s teleport to random ground,
 * randomise health, and toggle godmode. Overlay reports.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "scripthook.h"

#define TP_INTERVAL_MS   30000
#define CLEARANCE        1.0f

/* Keep inside the ABI's streamed collision radius, with
 * a margin so a teleport does not land on the edge.
 */
#define TP_RADIUS        (SH_STREAM_RADIUS * 0.8f)

static HWND   g_wnd;
static DWORD  g_nextTick;
static int    g_cycles;
static char   g_state[96] = "?";
static char   g_last[192] = "starting";
static char   g_why[128] = "";
static unsigned long g_seed;

static float RandUnit(void) {
    g_seed = g_seed * 1103515245u + 12345u;
    return (float)((g_seed >> 16) & 0x7FFF) / 32767.0f;
}

static void Report(const char *what, int ok) {
    snprintf(g_why, sizeof(g_why), "%s %s %s", what,
             ok ? "ok" : "FAIL",
             ok ? "" : ShErrorString(ShLastError()));
}

static void RunCycle(void) {
    ShVec3 pos;
    float x, y, gz = 0.0f;
    uint32_t cur = 0, max = 0, want;
    int ok;

    if (!ShIsInGame()) {
        snprintf(g_last, sizeof(g_last), "waiting, state %s", g_state);
        return;
    }

    /* Pick inside a disc, since a square would exceed the
     * radius diagonally and always miss there.
     */
    if (!ShGetPlayerPosition(&pos)) {
        Report("position", 0);
        snprintf(g_last, sizeof(g_last), "%s", g_why);
        return;
    }
    {
        float ang = RandUnit() * 6.2831853f;
        float rad = TP_RADIUS * sqrtf(RandUnit());
        x = pos.x + cosf(ang) * rad;
        y = pos.y + sinf(ang) * rad;
    }

    ok = ShGroundHeight(x, y, &gz);
    Report("ground", ok);
    if (!ok) {
        snprintf(g_last, sizeof(g_last), "%s", g_why);
        return;
    }

    ok = ShTeleportPlayerToGround(x, y, CLEARANCE);
    Report("teleport", ok);

    want = 2 + (uint32_t)(RandUnit() * 90.0f);
    if (!ShSetHealthPlayer(want)) Report("health", 0);
    ShGetHealthPlayer(&cur, &max);
    ShSetGodModePlayer((g_cycles & 1) ? 1 : 0);
    ShGetPlayerPosition(&pos);

    g_cycles++;
    snprintf(g_last, sizeof(g_last),
             "#%d at %.0f %.0f %.0f  ground %.1f  hp %u/%u  god %d",
             g_cycles, pos.x, pos.y, pos.z, gz, cur, max,
             (g_cycles & 1) ? 0 : 1);
}

static LRESULT CALLBACK OvlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        char buf[256];
        long remain;
        HBRUSH bg;

        GetClientRect(h, &rc);
        bg = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);

        remain = (long)(g_nextTick - GetTickCount());
        if (remain < 0) remain = 0;

        /* Draw only. The worker owns all API calls. */
        SetTextColor(dc, RGB(0, 255, 0));
        snprintf(buf, sizeof(buf),
                 "API TEST  next %ld.%01lds  state %s",
                 remain / 1000, (remain % 1000) / 100, g_state);
        TextOutA(dc, 8, 4, buf, (int)strlen(buf));

        SetTextColor(dc, RGB(0, 190, 0));
        TextOutA(dc, 8, 22, g_last, (int)strlen(g_last));

        SetTextColor(dc, RGB(255, 140, 0));
        TextOutA(dc, 8, 40, g_why, (int)strlen(g_why));

        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_TIMER) { InvalidateRect(h, NULL, FALSE); return 0; }
    if (m == WM_DESTROY) { g_wnd = NULL; PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI WorkerThread(LPVOID p) {
    static char prev[96] = "";
    (void)p;
    g_nextTick = GetTickCount() + TP_INTERVAL_MS;
    for (;;) {
        Sleep(100);

        /* Display only. dinput8 tracks state on its own. */
        ShGetGameStateName(g_state, sizeof(g_state));
        if (strcmp(prev, g_state) != 0) {
            snprintf(g_last, sizeof(g_last), "state -> %s (%d)",
                     g_state, ShGetGameState());
            strncpy(prev, g_state, sizeof(prev) - 1);
            prev[sizeof(prev) - 1] = 0;
        }

        if ((long)(GetTickCount() - g_nextTick) < 0) continue;
        RunCycle();
        g_nextTick = GetTickCount() + TP_INTERVAL_MS;
    }
    return 0;
}

static DWORD WINAPI OvlThread(LPVOID p) {
    WNDCLASSA wc;
    MSG msg;
    (void)p;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = OvlProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "GRWApiTest";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    g_wnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        "GRWApiTest", "API", WS_POPUP,
        12, 80, 620, 64,
        NULL, NULL, wc.hInstance, NULL);
    if (!g_wnd) return 1;

    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
    SetTimer(g_wnd, 1, 100, NULL);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

static void Startup(void) {
    LARGE_INTEGER c;

    QueryPerformanceCounter(&c);
    g_seed = (unsigned long)(c.QuadPart ^ GetTickCount());

    CreateThread(NULL, 0, OvlThread, NULL, 0, NULL);
    CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        Startup();
    }
    return TRUE;
}
