/* Crazy Cars. Every fifteen seconds, thirty vehicles are
 * dropped on the player, one every 200ms.
 */
/* Binds late by choice. Plugins may import the ScriptHook
 * directly instead, since the loader loads them from a
 * thread rather than from DllMain. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "scripthook.h"

#define WAVE_MS      15000
#define WAVE_COUNT   30
#define DROP_MS      200
#define DROP_HEIGHT  60.0f
#define KEY_TOGGLE   VK_F6

#define RING_RADIUS  22.0f
#define SAMPLE_MS    100

/* The lead is the fall time, sqrt(2h/g), because that is
 * how long the car is airborne before it arrives.
 */
#define GRAVITY      9.81f
#define LEAD_SCALE   1.0f

enum { PAT_OVERHEAD = 0, PAT_RING, PAT_LEAD, PAT_COUNT };

static const char *const PATTERN_NAME[PAT_COUNT] = {
    "overhead", "ring", "lead"
};

typedef struct { uint32_t id; const char *name; } Vehicle;

typedef int (*IsInGame_t)(void);
typedef int (*Count_t)(void);
typedef const Vehicle *(*At_t)(int);
typedef uint64_t (*Spawn_t)(uint32_t, const ShVec3 *);
typedef int (*PlayerPos_t)(ShVec3 *);

static IsInGame_t  g_inGame;
static Count_t     g_count;
static At_t        g_at;
static Spawn_t     g_spawn;
static PlayerPos_t g_playerPos;

static volatile int g_on = 1;
static volatile int g_dropped = 0;
static volatile int g_wave = 0;
static volatile int g_left = 0;
static volatile int g_pattern = PAT_OVERHEAD;

/* Written by the sampler, read by the wave thread. */
static volatile float g_velX = 0.0f;
static volatile float g_velY = 0.0f;

static unsigned long g_seed;

static unsigned long NextRand(void) {
    g_seed = g_seed * 1103515245UL + 12345UL;
    return (g_seed >> 16) & 0x7FFF;
}

/* Velocity from successive positions, smoothed, because a
 * single frame delta jitters badly while turning.
 */
static DWORD WINAPI SampleThread(LPVOID p) {
    ShVec3 prev = { 0.0f, 0.0f, 0.0f };
    DWORD last = 0;
    int have = 0;
    (void)p;

    for (;;) {
        ShVec3 now;
        DWORD t;
        Sleep(SAMPLE_MS);
        if (!g_inGame() || !g_playerPos(&now)) { have = 0; continue; }
        t = GetTickCount();
        /* Sleep overshoots, so take dt from the clock. */
        if (have && t > last) {
            float dt = (float)(t - last) / 1000.0f;
            float vx = (now.x - prev.x) / dt;
            float vy = (now.y - prev.y) / dt;
            g_velX = g_velX * 0.55f + vx * 0.45f;
            g_velY = g_velY * 0.55f + vy * 0.45f;
        }
        prev = now;
        last = t;
        have = 1;
    }
    return 0;
}

/* Position is read per car, so the pile follows you as
 * you move rather than landing where the wave started.
 */
static void DropOne(int index) {
    const Vehicle *v;
    ShVec3 pos;
    int n = g_count();

    if (n <= 0) return;
    v = g_at((int)(NextRand() % (unsigned long)n));
    if (!v) return;
    if (!g_playerPos(&pos)) return;

    if (g_pattern == PAT_RING) {
        double step = 6.2831853 / (double)WAVE_COUNT;
        double a = step * (double)index + (double)g_wave * 0.4;
        pos.x += (float)cos(a) * RING_RADIUS;
        pos.y += (float)sin(a) * RING_RADIUS;
    } else if (g_pattern == PAT_LEAD) {
        float lead = (float)sqrt(2.0 * DROP_HEIGHT / GRAVITY)
                   * LEAD_SCALE;
        pos.x += g_velX * lead;
        pos.y += g_velY * lead;
    }

    pos.z += DROP_HEIGHT;
    if (g_spawn(v->id, &pos)) g_dropped++;
}

static DWORD WINAPI WaveThread(LPVOID p) {
    (void)p;
    for (;;) {
        int i;

        Sleep(WAVE_MS);
        if (!g_on || !g_inGame()) continue;

        g_wave++;
        g_pattern = g_wave % PAT_COUNT;
        for (i = 0; i < WAVE_COUNT; i++) {
            if (!g_on || !g_inGame()) break;
            g_left = WAVE_COUNT - i;
            DropOne(i);
            Sleep(DROP_MS);
        }
        g_left = 0;
    }
    return 0;
}

static HWND g_wnd;

static LRESULT CALLBACK OvlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        char line[192];
        float speed = (float)sqrt((double)(g_velX * g_velX +
                                           g_velY * g_velY));

        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_on ? RGB(255, 160, 60)
                              : RGB(150, 150, 150));
        snprintf(line, sizeof(line),
                 "CRAZY CARS %s   wave %d   %s   dropped %d   "
                 "incoming %d   %.0fm/s   [F6]",
                 g_on ? "ON" : "off", g_wave,
                 PATTERN_NAME[g_pattern], g_dropped, g_left, speed);
        TextOutA(dc, 8, 8, line, (int)strlen(line));
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_TIMER) {
        static int was = 0;
        int down = (GetAsyncKeyState(KEY_TOGGLE) & 0x8000) != 0;
        if (down && !was) g_on = !g_on;
        was = down;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    if (m == WM_DESTROY) { g_wnd = NULL; PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI OvlThread(LPVOID p) {
    WNDCLASSA wc;
    MSG msg;
    (void)p;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = OvlProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "GRWCrazyCars";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    g_wnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        /* Below the spawner menu, which owns y 60 to 360. */
        "GRWCrazyCars", "CrazyCars", WS_POPUP,
        12, 380, 640, 34,
        NULL, NULL, wc.hInstance, NULL);
    if (!g_wnd) return 1;

    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
    SetTimer(g_wnd, 1, 120, NULL);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

static DWORD WINAPI BindThread(LPVOID p) {
    HMODULE m = NULL;
    (void)p;

    while (!m) {
        m = GetModuleHandleA("dinput8.dll");
        if (!m) Sleep(500);
    }
    *(FARPROC *)&g_inGame = GetProcAddress(m, "ShIsInGame");
    *(FARPROC *)&g_count = GetProcAddress(m, "ShVehicleCount");
    *(FARPROC *)&g_at = GetProcAddress(m, "ShVehicleAt");
    *(FARPROC *)&g_spawn = GetProcAddress(m, "ShSpawnVehicle");
    *(FARPROC *)&g_playerPos = GetProcAddress(m,
                                              "ShGetPlayerPosition");
    if (!g_inGame || !g_count || !g_at || !g_spawn || !g_playerPos)
        return 1;

    g_seed = GetTickCount();
    CreateThread(NULL, 0, SampleThread, NULL, 0, NULL);
    CreateThread(NULL, 0, WaveThread, NULL, 0, NULL);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        CreateThread(NULL, 0, OvlThread, NULL, 0, NULL);
        CreateThread(NULL, 0, BindThread, NULL, 0, NULL);
    }
    return TRUE;
}
