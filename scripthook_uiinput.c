/* Keyboard and pointer events for plugin scenes. */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"

#define MAX_SCENES 16
#define POLL_MS    8

extern void ShSetError(int err);
extern int  ShBlockKey(int vk, int on);
extern int  ShCaptureKeys(int on);

static ShUiInputFn g_fn[MAX_SCENES + 1];
static void       *g_user[MAX_SCENES + 1];
static volatile uint32_t g_focus;
static HANDLE g_thread;
static uint8_t g_down[256];
static uint8_t g_held[256];      /* consumed on down, blocked */
static POINT g_lastPos;

/* pointer in the 1920 x 1080 reference space */
static void Pointer(int *x, int *y) {
    HWND w = GetForegroundWindow();
    POINT p;
    RECT rc;
    GetCursorPos(&p);
    *x = p.x; *y = p.y;
    if (!w || !GetClientRect(w, &rc)) return;
    ScreenToClient(w, &p);
    if (rc.right > 0 && rc.bottom > 0) {
        *x = (int)((float)p.x * 1920.0f / (float)rc.right);
        *y = (int)((float)p.y * 1080.0f / (float)rc.bottom);
    }
}

static int Deliver(int type, int key, int x, int y) {
    uint32_t s = g_focus;
    ShUiEvent e;
    ShUiInputFn fn;
    if (!s || s > MAX_SCENES) return 0;
    fn = g_fn[s];
    if (!fn) return 0;
    e.type = type; e.key = key; e.x = x; e.y = y;
    return fn(s, &e, g_user[s]);
}

static DWORD WINAPI InputThread(LPVOID arg) {
    (void)arg;
    for (;;) {
        int vk, x, y;
        POINT p;
        Sleep(POLL_MS);
        if (!g_focus) {
            for (vk = 1; vk < 256; vk++)
                if (g_held[vk]) { ShBlockKey(vk, 0); g_held[vk] = 0; }
            continue;
        }
        Pointer(&x, &y);
        GetCursorPos(&p);
        if (p.x != g_lastPos.x || p.y != g_lastPos.y) {
            g_lastPos = p;
            Deliver(SH_UI_EV_MOVE, 0, x, y);
        }
        for (vk = 1; vk < 256; vk++) {
            int now = (GetAsyncKeyState(vk) & 0x8000) != 0;
            if (now == g_down[vk]) continue;
            g_down[vk] = (uint8_t)now;
            if (now) {
                if (Deliver(SH_UI_EV_DOWN, vk, x, y)) {
                    ShBlockKey(vk, 1);
                    g_held[vk] = 1;
                }
            } else {
                Deliver(SH_UI_EV_UP, vk, x, y);
                if (g_held[vk]) { ShBlockKey(vk, 0); g_held[vk] = 0; }
            }
        }
    }
    return 0;
}

SH_API int ShUiSetInput(uint32_t scene, ShUiInputFn fn, void *user) {
    if (scene == 0 || scene > MAX_SCENES) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    g_fn[scene] = fn;
    g_user[scene] = user;
    if (fn && !g_thread)
        g_thread = CreateThread(NULL, 0, InputThread, NULL, 0, NULL);
    return 1;
}

/* one scene holds focus and captures the keyboard */
SH_API int ShUiFocus(uint32_t scene, int take) {
    if (scene == 0 || scene > MAX_SCENES) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (take) g_focus = scene;
    else if (g_focus == scene) g_focus = 0;
    ShCaptureKeys(g_focus != 0);
    return 1;
}

SH_API uint32_t ShUiFocused(void) {
    return g_focus;
}
