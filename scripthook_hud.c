/* One overlay per corner, owned by the API. Slots pack from
 * whoever registered, so an absent plugin leaves no gap.
 */
/* Windows are sized to their content and opaque, which is
 * the only shape proven to draw over the game.
 */
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"

#define HUD_SLOTS   32
#define HUD_TEXT    512
#define HUD_NAME    32
#define HUD_LINE_H  17
#define HUD_PAD     6
#define HUD_MARGIN  12
#define HUD_TICK_MS 120
#define HUD_ANCHORS 4

typedef struct {
    int      used;
    int      visible;
    int      anchor;
    int      priority;
    uint32_t colour;
    char     name[HUD_NAME];
    char     text[HUD_TEXT];
} HudSlot;

static HudSlot g_slots[HUD_SLOTS];
static CRITICAL_SECTION g_lock;
static volatile int g_lockReady = 0;
static volatile int g_started = 0;
static HWND g_win[HUD_ANCHORS];
static HFONT g_font = NULL;

/* Re-asserting topmost every tick makes the game fight us
 * for z-order and hangs it, so each window is touched only
 * when its content or geometry actually changes. */
static volatile int g_rev = 0;
static int g_seen[HUD_ANCHORS];
static int g_shown[HUD_ANCHORS];
static RECT g_last[HUD_ANCHORS];
static int g_placed[HUD_ANCHORS];

extern void ShSetError(int err);

static void HudLock(void) {
    if (g_lockReady) EnterCriticalSection(&g_lock);
}

static void HudUnlock(void) {
    if (g_lockReady) LeaveCriticalSection(&g_lock);
}

/* Marks the overlay dirty, so the timer knows there is
 * something new to draw. */
static void HudChanged(void) {
    g_rev++;
}

static int AnchorOf(HWND h) {
    int i;
    for (i = 0; i < HUD_ANCHORS; i++)
        if (g_win[i] == h) return i;
    return 0;
}

/* Slots for one corner, priority first then registration,
 * so layout holds whichever plugin loaded first.
 */
static int Gather(int anchor, HudSlot **out, int max) {
    int n = 0, pass, i;

    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < HUD_SLOTS && n < max; i++) {
            HudSlot *s = &g_slots[i];
            if (!s->used || !s->visible || !s->text[0]) continue;
            if ((s->anchor & 3) != anchor) continue;
            if (pass == 0 && s->priority >= 0) continue;
            if (pass == 1 && s->priority < 0) continue;
            out[n++] = s;
        }
    }
    return n;
}

static int LineCount(const char *s) {
    int n = 1;
    while (*s) { if (*s == '\n') n++; s++; }
    return n;
}

static void PaintHud(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    HFONT oldFont;
    HudSlot *list[HUD_SLOTS];
    int n, i, y = HUD_PAD;

    GetClientRect(h, &rc);
    FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    oldFont = (HFONT)SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);

    HudLock();
    n = Gather(AnchorOf(h), list, HUD_SLOTS);
    for (i = 0; i < n; i++) {
        const char *p = list[i]->text;
        uint32_t c = list[i]->colour;

        SetTextColor(dc, RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF,
                             c & 0xFF));
        while (*p) {
            const char *e = strchr(p, '\n');
            int len = e ? (int)(e - p) : (int)strlen(p);
            TextOutA(dc, HUD_PAD, y, p, len);
            y += HUD_LINE_H;
            if (!e) break;
            p = e + 1;
        }
        y += HUD_PAD;
    }
    HudUnlock();

    SelectObject(dc, oldFont);
    EndPaint(h, &ps);
}

/* Sized to fit its text. Returns 1 when the window needs a
 * repaint, so an idle overlay costs nothing at all.
 */
static int FitWindow(HWND h, int anchor) {
    HDC dc = GetDC(h);
    HFONT oldFont = (HFONT)SelectObject(dc, g_font);
    HudSlot *list[HUD_SLOTS];
    int n, i, lines = 0, w = 0;
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int ht, x, y;

    HudLock();
    n = Gather(anchor, list, HUD_SLOTS);
    for (i = 0; i < n; i++) {
        const char *p = list[i]->text;
        lines += LineCount(p);
        while (*p) {
            const char *e = strchr(p, '\n');
            int len = e ? (int)(e - p) : (int)strlen(p);
            SIZE sz;
            if (GetTextExtentPoint32A(dc, p, len, &sz) && sz.cx > w)
                w = sz.cx;
            if (!e) break;
            p = e + 1;
        }
    }
    HudUnlock();

    SelectObject(dc, oldFont);
    ReleaseDC(h, dc);

    if (n == 0 || lines == 0) {
        if (g_shown[anchor]) {
            ShowWindow(h, SW_HIDE);
            g_shown[anchor] = 0;
        }
        return 0;
    }
    w += HUD_PAD * 2;
    ht = lines * HUD_LINE_H + n * HUD_PAD + HUD_PAD;

    x = (anchor == SH_HUD_TOPLEFT || anchor == SH_HUD_BOTTOMLEFT)
        ? HUD_MARGIN : sw - HUD_MARGIN - w;
    y = (anchor == SH_HUD_TOPLEFT || anchor == SH_HUD_TOPRIGHT)
        ? HUD_MARGIN : sh - HUD_MARGIN - ht;

    if (g_shown[anchor] && g_last[anchor].left == x &&
        g_last[anchor].top == y && g_last[anchor].right == w &&
        g_last[anchor].bottom == ht)
        return 0;

    g_last[anchor].left = x;
    g_last[anchor].top = y;
    g_last[anchor].right = w;
    g_last[anchor].bottom = ht;

    /* Topmost is claimed once. Later moves keep the z-order
     * we already have, which is what the game can live with.
     */
    SetWindowPos(h, g_placed[anchor] ? NULL : HWND_TOPMOST,
                 x, y, w, ht,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW |
                 (g_placed[anchor] ? SWP_NOZORDER : 0));
    g_placed[anchor] = 1;
    g_shown[anchor] = 1;
    return 1;
}

static LRESULT CALLBACK HudProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) { PaintHud(h); return 0; }
    if (m == WM_TIMER) {
        int a = AnchorOf(h);
        int rev = g_rev;
        int moved = FitWindow(h, a);

        if (moved || rev != g_seen[a]) {
            g_seen[a] = rev;
            if (g_shown[a]) InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI HudThread(LPVOID p) {
    WNDCLASSA wc;
    MSG msg;
    int i;
    (void)p;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = HudProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "GRWScriptHookHud";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    g_font = CreateFontA(14, 0, 0, 0, FW_BOLD, 0, 0, 0,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                         FF_DONTCARE, "Consolas");

    for (i = 0; i < HUD_ANCHORS; i++) {
        g_win[i] = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            "GRWScriptHookHud", "ScriptHook", WS_POPUP,
            HUD_MARGIN, HUD_MARGIN, 320, 34,
            NULL, NULL, wc.hInstance, NULL);
        if (g_win[i]) SetTimer(g_win[i], 1, HUD_TICK_MS, NULL);
    }

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

static void EnsureHud(void) {
    if (g_started) return;
    g_started = 1;
    InitializeCriticalSection(&g_lock);
    g_lockReady = 1;
    CreateThread(NULL, 0, HudThread, NULL, 0, NULL);
}

SH_API uint32_t ShHudCreate(const char *name, int anchor,
                            int priority) {
    int i;

    EnsureHud();
    HudLock();
    for (i = 0; i < HUD_SLOTS; i++) {
        if (g_slots[i].used) continue;
        memset(&g_slots[i], 0, sizeof(g_slots[i]));
        g_slots[i].used = 1;
        g_slots[i].visible = 1;
        g_slots[i].anchor = anchor & 3;
        g_slots[i].priority = priority;
        g_slots[i].colour = 0xFFFFFF;
        if (name) {
            strncpy(g_slots[i].name, name, HUD_NAME - 1);
            g_slots[i].name[HUD_NAME - 1] = 0;
        }
        HudUnlock();
        ShSetError(SH_OK);
        return (uint32_t)(i + 1);
    }
    HudUnlock();
    ShSetError(SH_ERR_NO_CANDIDATE);
    return 0;
}

static HudSlot *SlotOf(uint32_t h) {
    if (h == 0 || h > HUD_SLOTS) return NULL;
    if (!g_slots[h - 1].used) return NULL;
    return &g_slots[h - 1];
}

SH_API int ShHudSet(uint32_t hud, const char *text) {
    HudSlot *s;

    HudLock();
    s = SlotOf(hud);
    if (!s) { HudUnlock(); ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (text) {
        strncpy(s->text, text, HUD_TEXT - 1);
        s->text[HUD_TEXT - 1] = 0;
    } else {
        s->text[0] = 0;
    }
    HudUnlock();
    HudChanged();
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShHudColour(uint32_t hud, uint32_t rgb) {
    HudSlot *s;

    HudLock();
    s = SlotOf(hud);
    if (!s) { HudUnlock(); ShSetError(SH_ERR_BAD_ARG); return 0; }
    s->colour = rgb;
    HudUnlock();
    HudChanged();
    return 1;
}

SH_API int ShHudShow(uint32_t hud, int visible) {
    HudSlot *s;

    HudLock();
    s = SlotOf(hud);
    if (!s) { HudUnlock(); ShSetError(SH_ERR_BAD_ARG); return 0; }
    s->visible = visible ? 1 : 0;
    HudUnlock();
    HudChanged();
    return 1;
}

SH_API void ShHudDestroy(uint32_t hud) {
    HudSlot *s;

    HudLock();
    s = SlotOf(hud);
    if (s) memset(s, 0, sizeof(*s));
    HudUnlock();
    HudChanged();
}
