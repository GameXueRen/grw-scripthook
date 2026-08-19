/* Shoot a car, the car goes up. A test of the OnHit
 * callback, and the reason the event exists.
 */
/* Binds late by choice. Plugins may import the ScriptHook
 * directly instead, since the loader loads them from a
 * thread rather than from DllMain. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "scripthook.h"

#define FLING_HEIGHT  90.0f

typedef int (*IsInGame_t)(void);
typedef int (*HookInstall_t)(void);
typedef int (*OnHit_t)(ShHitFn, void *, int);
typedef int (*Place_t)(uint64_t, const ShVec3 *, const ShVec3 *);

static Place_t g_place;
static volatile int g_hits = 0;
static volatile int g_flung = 0;

/* Called by the API on its own thread, so every call in
 * here is a plain API call and nothing else.
 */
static void OnHit(const ShHit *hit, void *user) {
    ShVec3 up = hit->pos;
    (void)user;

    g_hits++;
    if (hit->kind != SH_KIND_VEHICLE) return;

    up.z += FLING_HEIGHT;
    if (g_place && g_place(hit->root, &up, NULL)) g_flung++;
}

static HWND g_wnd;

static LRESULT CALLBACK OvlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        char line[128];

        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 200, 60));
        snprintf(line, sizeof(line),
                 "FALLING CARS   hits %d   flung %d",
                 g_hits, g_flung);
        TextOutA(dc, 8, 8, line, (int)strlen(line));
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_TIMER) { InvalidateRect(h, NULL, FALSE); return 0; }
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
    wc.lpszClassName = "GRWHitFling";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    g_wnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        /* Status bars stack below the spawner menu. */
        "GRWHitFling", "Fling", WS_POPUP,
        12, 420, 380, 34,
        NULL, NULL, wc.hInstance, NULL);
    if (!g_wnd) return 1;

    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
    SetTimer(g_wnd, 1, 150, NULL);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

/* The hook needs the game running, so subscribe once it
 * is. This is the only waiting the plugin does.
 */
static DWORD WINAPI SubscribeThread(LPVOID p) {
    HMODULE m = NULL;
    IsInGame_t inGame = NULL;
    HookInstall_t install = NULL;
    OnHit_t onHit = NULL;
    (void)p;

    while (!m) {
        m = GetModuleHandleA("dinput8.dll");
        if (!m) Sleep(500);
    }
    *(FARPROC *)&inGame = GetProcAddress(m, "ShIsInGame");
    *(FARPROC *)&install = GetProcAddress(m, "ShHitHookInstall");
    *(FARPROC *)&onHit = GetProcAddress(m, "ShOnHit");
    *(FARPROC *)&g_place = GetProcAddress(m, "ShPlaceEntity");
    if (!inGame || !install || !onHit || !g_place) return 1;

    while (!inGame() || !install()) Sleep(500);
    onHit(OnHit, NULL, 0);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        CreateThread(NULL, 0, OvlThread, NULL, 0, NULL);
        CreateThread(NULL, 0, SubscribeThread, NULL, 0, NULL);
    }
    return TRUE;
}
