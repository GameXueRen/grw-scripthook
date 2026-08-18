/* Teleport gun. Shoot anywhere, arrive there. The hit
 * carries a normal, so it lands you on the surface.
 */
/* Plugins load from inside dinput8's DllMain, so a static
 * import on it deadlocks the loader. Bind late.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "scripthook.h"

/* Along the surface normal, so a wall shot puts you in
 * front of the wall and a floor shot above the floor.
 */
#define STANDOFF   1.2f
#define MIN_RANGE  3.0f

typedef int (*IsInGame_t)(void);
typedef int (*HookInstall_t)(void);
typedef int (*OnHit_t)(ShHitFn, void *, int);
typedef int (*Teleport_t)(const ShVec3 *, const ShVec3 *);

static Teleport_t g_tp;
static volatile int g_shots = 0;
static volatile int g_jumps = 0;
static volatile int g_armed = 1;

static void OnHit(const ShHit *hit, void *user) {
    ShVec3 dest = hit->pos;
    float n = hit->normal.x * hit->normal.x
            + hit->normal.y * hit->normal.y
            + hit->normal.z * hit->normal.z;
    (void)user;

    g_shots++;
    if (!g_armed || !g_tp) return;
    /* Point blank shots would strand you inside cover. */
    if (hit->distance < MIN_RANGE) return;

    if (n > 0.25f) {
        dest.x += hit->normal.x * STANDOFF;
        dest.y += hit->normal.y * STANDOFF;
        dest.z += hit->normal.z * STANDOFF;
    } else {
        dest.z += STANDOFF;
    }
    if (g_tp(&dest, NULL)) g_jumps++;
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
        SetTextColor(dc, g_armed ? RGB(120, 230, 255)
                                 : RGB(150, 150, 150));
        snprintf(line, sizeof(line),
                 "TELEPORT GUN %s   hits %d   jumps %d   [F7]",
                 g_armed ? "ARMED" : "off", g_shots, g_jumps);
        TextOutA(dc, 8, 8, line, (int)strlen(line));
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_TIMER) {
        static int was = 0;
        int down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (down && !was) g_armed = !g_armed;
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
    wc.lpszClassName = "GRWTpGun";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    g_wnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        /* Status bars stack below the spawner menu. */
        "GRWTpGun", "TpGun", WS_POPUP,
        12, 460, 440, 34,
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
    *(FARPROC *)&g_tp = GetProcAddress(m, "ShTeleportPlayer");
    if (!inGame || !install || !onHit || !g_tp) return 1;

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
