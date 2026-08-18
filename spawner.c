/* Vehicle spawner. An in game menu over the catalogue,
 * driven entirely by the ScriptHook ABI.
 */
/* Plugins load from inside dinput8's DllMain, so a static
 * import on it deadlocks the loader. Bind late.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "scripthook.h"

#define KEY_TOGGLE  VK_F5
#define KEY_UP      VK_UP
#define KEY_DOWN    VK_DOWN
#define KEY_PGUP    VK_PRIOR
#define KEY_PGDN    VK_NEXT
#define KEY_SPAWN   VK_RETURN

#define ROWS        13
#define AHEAD       6.0f
#define LIFT        1.0f
#define POLL_MS     40

typedef struct { uint32_t id; const char *name; } Vehicle;

typedef int (*IsInGame_t)(void);
typedef int (*Count_t)(void);
typedef const Vehicle *(*At_t)(int);
typedef uint64_t (*Spawn_t)(uint32_t, const ShVec3 *);
typedef int (*PlayerPos_t)(ShVec3 *);

static Count_t      g_count;
static At_t         g_at;
static Spawn_t      g_spawn;
static PlayerPos_t  g_playerPos;
static IsInGame_t   g_inGame;

static volatile int g_open = 0;
static volatile int g_sel = 0;
static volatile int g_busy = 0;
static volatile int g_spawned = 0;
static char g_status[96] = "";

static int Total(void) { return g_count ? g_count() : 0; }

static const char *NameAt(int i) {
    const Vehicle *v = g_at ? g_at(i) : NULL;
    return v ? v->name : "";
}

/* The catalogue flags the two that freeze on entry. */
static int IsHazard(int i) {
    return strstr(NameAt(i), "FREEZES") != NULL;
}

static void SpawnSelected(void) {
    const Vehicle *v;
    ShVec3 pos;
    uint64_t ent;

    if (!g_spawn || !g_at || !g_playerPos) return;
    v = g_at(g_sel);
    if (!v) return;
    if (!g_playerPos(&pos)) {
        snprintf(g_status, sizeof(g_status), "no player position");
        return;
    }
    pos.x += AHEAD;
    pos.z += LIFT;

    g_busy = 1;
    ent = g_spawn(v->id, &pos);
    g_busy = 0;
    if (ent) {
        g_spawned++;
        snprintf(g_status, sizeof(g_status), "spawned %s", v->name);
    } else {
        snprintf(g_status, sizeof(g_status), "spawn failed");
    }
}

static void Move(int delta) {
    int n = Total();
    if (n <= 0) return;
    g_sel += delta;
    while (g_sel < 0) g_sel += n;
    g_sel %= n;
}

/* Async key state works while the game has focus, which a
 * window message loop would not.
 */
static int Pressed(int vk) {
    static unsigned char was[256];
    int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    int hit = down && !was[vk & 0xFF];
    was[vk & 0xFF] = (unsigned char)down;
    return hit;
}

static DWORD WINAPI InputThread(LPVOID p) {
    (void)p;
    for (;;) {
        Sleep(POLL_MS);
        if (Pressed(KEY_TOGGLE)) g_open = !g_open;
        if (!g_open || g_busy) continue;
        if (Pressed(KEY_UP)) Move(-1);
        if (Pressed(KEY_DOWN)) Move(1);
        if (Pressed(KEY_PGUP)) Move(-10);
        if (Pressed(KEY_PGDN)) Move(10);
        if (Pressed(KEY_SPAWN)) SpawnSelected();
    }
    return 0;
}

static HWND g_wnd;

static void PaintList(HDC dc) {
    int n = Total();
    int first = g_sel - ROWS / 2;
    int i, y = 30;
    char line[160];

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 210, 90));
    snprintf(line, sizeof(line),
             "VEHICLE SPAWNER   %d of %d   spawned %d",
             n ? g_sel + 1 : 0, n, g_spawned);
    TextOutA(dc, 10, 8, line, (int)strlen(line));

    if (first < 0) first = 0;
    if (first > n - ROWS) first = n - ROWS;
    if (first < 0) first = 0;

    for (i = first; i < n && i < first + ROWS; i++) {
        int cur = (i == g_sel);
        COLORREF col = RGB(190, 190, 190);
        if (IsHazard(i)) col = RGB(230, 110, 110);
        if (cur) col = RGB(120, 235, 255);
        SetTextColor(dc, col);
        snprintf(line, sizeof(line), "%s %2d  %s",
                 cur ? ">" : " ", i, NameAt(i));
        TextOutA(dc, 10, y, line, (int)strlen(line));
        y += 17;
    }

    SetTextColor(dc, RGB(150, 150, 150));
    TextOutA(dc, 10, y + 6,
             "F5 close   arrows move   PgUp PgDn jump   Enter spawn",
             53);
    if (g_status[0]) {
        SetTextColor(dc, RGB(160, 230, 160));
        TextOutA(dc, 10, y + 24, g_status, (int)strlen(g_status));
    }
}

static LRESULT CALLBACK OvlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;

        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (g_open) PaintList(dc);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_TIMER) {
        ShowWindow(h, g_open ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (g_open) InvalidateRect(h, NULL, FALSE);
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
    wc.lpszClassName = "GRWSpawner";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    g_wnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        "GRWSpawner", "Spawner", WS_POPUP,
        12, 60, 470, 300,
        NULL, NULL, wc.hInstance, NULL);
    if (!g_wnd) return 1;

    SetTimer(g_wnd, 1, 100, NULL);
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
    if (!g_count || !g_at || !g_spawn || !g_playerPos) return 1;

    CreateThread(NULL, 0, InputThread, NULL, 0, NULL);
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
