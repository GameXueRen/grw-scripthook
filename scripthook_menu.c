/* A shared menu, one root owned by the API. Every plugin
 * registers a submenu, so sixteen addons cost sixteen rows.
 */
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"

#define MENUS       24
#define ITEMS       96
#define LABEL       48
#define VISIBLE     12
#define ROW_H       18
#define PAD         8
#define MARGIN      12
#define TICK_MS     40
#define OPTS        12

enum { IT_ACTION = 0, IT_SUB, IT_TOGGLE, IT_NUMBER, IT_LIST };

typedef struct {
    int      used;
    int      kind;
    char     label[LABEL];
    uint32_t sub;
    int      value;
    float    num, lo, hi, step;
    const char *opts[OPTS];
    int      nopts;
    ShMenuFn fn;
    void    *user;
} Item;

typedef struct {
    int      used;
    char     title[LABEL];
    char     status[96];
    uint32_t parent;
    int      sel;
    int      top;
    int      count;
    Item     items[ITEMS];
} Menu;

static Menu g_menus[MENUS];
static uint32_t g_root = 0;
static volatile uint32_t g_current = 0;
static volatile int g_open = 0;
static volatile int g_key = VK_F4;
static volatile int g_greeted = 0;
static volatile int g_started = 0;
static CRITICAL_SECTION g_lock;
static volatile int g_lockReady = 0;
static HWND g_wnd = NULL;
static HFONT g_font = NULL;

extern void ShSetError(int err);

static void Lock(void) { if (g_lockReady) EnterCriticalSection(&g_lock); }
static void Unlock(void) { if (g_lockReady) LeaveCriticalSection(&g_lock); }

static Menu *MenuOf(uint32_t h) {
    if (h == 0 || h > MENUS) return NULL;
    if (!g_menus[h - 1].used) return NULL;
    return &g_menus[h - 1];
}

static uint32_t NewMenu(const char *title, uint32_t parent) {
    int i;

    for (i = 0; i < MENUS; i++) {
        if (g_menus[i].used) continue;
        memset(&g_menus[i], 0, sizeof(g_menus[i]));
        g_menus[i].used = 1;
        g_menus[i].parent = parent;
        if (title) {
            strncpy(g_menus[i].title, title, LABEL - 1);
            g_menus[i].title[LABEL - 1] = 0;
        }
        return (uint32_t)(i + 1);
    }
    return 0;
}

static Item *NewItem(Menu *m, int kind, const char *label,
                     ShMenuFn fn, void *user) {
    Item *it;

    if (!m || m->count >= ITEMS) return NULL;
    it = &m->items[m->count++];
    memset(it, 0, sizeof(*it));
    it->used = 1;
    it->kind = kind;
    it->fn = fn;
    it->user = user;
    if (label) {
        strncpy(it->label, label, LABEL - 1);
        it->label[LABEL - 1] = 0;
    }
    return it;
}

/* Rendered text for the value side of a row. */
static void ValueText(const Item *it, char *out, int n) {
    out[0] = 0;
    if (it->kind == IT_SUB) snprintf(out, n, ">");
    else if (it->kind == IT_TOGGLE)
        snprintf(out, n, it->value ? "[on]" : "[off]");
    else if (it->kind == IT_NUMBER)
        snprintf(out, n, "< %.2f >", it->num);
    else if (it->kind == IT_LIST && it->nopts)
        snprintf(out, n, "< %s >", it->opts[it->value % it->nopts]);
}

/* Receivers run on the menu thread, so the lock is dropped
 * around the call and any API call is legal inside one.
 */
static void Fire(uint32_t menu, int idx, Item *it) {
    ShMenuFn fn = it->fn;
    void *user = it->user;
    int v = (it->kind == IT_NUMBER) ? (int)it->num : it->value;

    if (!fn) return;
    Unlock();
    fn(menu, (uint32_t)idx, v, user);
    Lock();
}

static int Pressed(int vk) {
    static unsigned char was[256];
    int d = (GetAsyncKeyState(vk) & 0x8000) != 0;
    int hit = d && !was[vk & 0xFF];
    was[vk & 0xFF] = (unsigned char)d;
    return hit;
}

/* Selection scrolls with the cursor, so a long menu shows a
 * window of rows rather than running off the screen.
 */
static void Scroll(Menu *m) {
    if (m->sel < m->top) m->top = m->sel;
    if (m->sel >= m->top + VISIBLE) m->top = m->sel - VISIBLE + 1;
    if (m->top < 0) m->top = 0;
}

static void Navigate(void) {
    Menu *m = MenuOf(g_current);
    Item *it;
    int left, right;

    if (!m || m->count == 0) return;
    if (Pressed(VK_UP)) m->sel = (m->sel + m->count - 1) % m->count;
    if (Pressed(VK_DOWN)) m->sel = (m->sel + 1) % m->count;
    Scroll(m);

    it = &m->items[m->sel];
    left = Pressed(VK_LEFT);
    right = Pressed(VK_RIGHT);
    if (left || right) {
        int dir = right ? 1 : -1;
        if (it->kind == IT_NUMBER) {
            it->num += it->step * dir;
            if (it->num < it->lo) it->num = it->lo;
            if (it->num > it->hi) it->num = it->hi;
            Fire(g_current, m->sel, it);
        } else if (it->kind == IT_LIST && it->nopts) {
            it->value = (it->value + it->nopts + dir) % it->nopts;
            Fire(g_current, m->sel, it);
        }
    }
    if (Pressed(VK_RETURN)) {
        if (it->kind == IT_SUB && it->sub) {
            g_current = it->sub;
        } else if (it->kind == IT_TOGGLE) {
            it->value = !it->value;
            Fire(g_current, m->sel, it);
        } else {
            Fire(g_current, m->sel, it);
        }
    }
    if (Pressed(VK_BACK) || Pressed(VK_ESCAPE)) {
        if (m->parent) g_current = m->parent;
        else g_open = 0;
    }
}

static void PaintMenu(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc, row;
    HFONT oldFont;
    Menu *m;
    int i, y = PAD, w;
    char val[32], line[128];

    GetClientRect(h, &rc);
    w = rc.right - rc.left;
    FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    oldFont = (HFONT)SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);

    Lock();
    m = MenuOf(g_current);
    if (m) {
        SetTextColor(dc, RGB(255, 210, 90));
        TextOutA(dc, PAD, y, m->title, (int)strlen(m->title));
        y += ROW_H + 2;

        for (i = m->top; i < m->count && i < m->top + VISIBLE; i++) {
            Item *it = &m->items[i];
            int cur = (i == m->sel);

            if (cur) {
                HBRUSH b = CreateSolidBrush(RGB(40, 70, 90));
                row.left = 2; row.right = w - 2;
                row.top = y - 1; row.bottom = y + ROW_H - 2;
                FillRect(dc, &row, b);
                DeleteObject(b);
            }
            SetTextColor(dc, cur ? RGB(140, 240, 255)
                                 : RGB(210, 210, 210));
            TextOutA(dc, PAD + 4, y, it->label, (int)strlen(it->label));
            ValueText(it, val, sizeof(val));
            if (val[0]) {
                SIZE sz;
                GetTextExtentPoint32A(dc, val, (int)strlen(val), &sz);
                TextOutA(dc, w - PAD - sz.cx, y, val, (int)strlen(val));
            }
            y += ROW_H;
        }
        if (m->count > VISIBLE) {
            SetTextColor(dc, RGB(140, 140, 140));
            snprintf(line, sizeof(line), "%d of %d", m->sel + 1,
                     m->count);
            TextOutA(dc, PAD, y + 2, line, (int)strlen(line));
            y += ROW_H;
        }
        if (m->status[0]) {
            SetTextColor(dc, RGB(160, 230, 160));
            TextOutA(dc, PAD, y + 2, m->status,
                     (int)strlen(m->status));
        }
    }
    Unlock();

    SelectObject(dc, oldFont);
    EndPaint(h, &ps);
}

/* Touched only on a real change. Re-asserting topmost every
 * tick makes the game fight us for z-order and hang.
 */
static int FitMenu(void) {
    static int shown = 0, placed = 0, lastHt = 0;
    Menu *m;
    int rows, ht;

    if (!g_wnd) return 0;
    if (!g_open) {
        if (shown) { ShowWindow(g_wnd, SW_HIDE); shown = 0; }
        return 0;
    }

    Lock();
    m = MenuOf(g_current);
    rows = m ? m->count : 0;
    if (rows > VISIBLE) rows = VISIBLE + 1;
    if (m && m->status[0]) rows++;
    Unlock();

    ht = PAD * 2 + ROW_H + 2 + rows * ROW_H;
    if (shown && ht == lastHt) return 0;
    lastHt = ht;

    SetWindowPos(g_wnd, placed ? NULL : HWND_TOPMOST,
                 MARGIN, MARGIN, 380, ht,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW |
                 (placed ? SWP_NOZORDER : 0));
    placed = 1;
    shown = 1;
    return 1;
}

/* What the menu currently looks like. Repainting only when
 * this changes keeps an open menu idle. Caller holds the
 * lock. */
static unsigned MenuSig(void) {
    Menu *m = MenuOf(g_current);
    const char *p;
    unsigned s = (unsigned)g_current * 2654435761u;
    int i;

    if (!m) return s;
    s = s * 31u + (unsigned)m->sel;
    s = s * 31u + (unsigned)m->top;
    s = s * 31u + (unsigned)m->count;
    for (p = m->status; *p; p++) s = s * 31u + (unsigned char)*p;

    for (i = m->top; i < m->count && i < m->top + VISIBLE; i++) {
        char v[32];
        ValueText(&m->items[i], v, sizeof(v));
        for (p = v; *p; p++) s = s * 31u + (unsigned char)*p;
    }
    return s;
}

static LRESULT CALLBACK MenuProc(HWND h, UINT msg, WPARAM w,
                                 LPARAM l) {
    if (msg == WM_PAINT) { PaintMenu(h); return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, msg, w, l);
}

static DWORD WINAPI MenuThread(LPVOID p) {
    WNDCLASSA wc;
    MSG msg;
    unsigned sig = 0, lastSig = ~0u;
    int moved;
    (void)p;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = MenuProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "GRWScriptHookMenu";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    g_font = CreateFontA(15, 0, 0, 0, FW_BOLD, 0, 0, 0,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                         FF_DONTCARE, "Consolas");

    g_wnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        "GRWScriptHookMenu", "ScriptHookMenu", WS_POPUP,
        MARGIN, MARGIN, 380, 200, NULL, NULL, wc.hInstance, NULL);
    if (!g_wnd) return 1;

    for (;;) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(TICK_MS);

        if (Pressed(g_key)) {
            g_open = !g_open;
            if (g_open) g_current = g_root;
        }
        if (g_open) {
            Lock();
            Navigate();
            sig = MenuSig();
            Unlock();
        }
        moved = FitMenu();
        if (g_open && (moved || sig != lastSig)) {
            lastSig = sig;
            InvalidateRect(g_wnd, NULL, FALSE);
        }
    }
    return 0;
}

static void EnsureMenu(void) {
    if (g_started) return;
    g_started = 1;
    InitializeCriticalSection(&g_lock);
    g_lockReady = 1;
    g_root = NewMenu("SCRIPTHOOK", 0);
    CreateThread(NULL, 0, MenuThread, NULL, 0, NULL);
}

SH_API uint32_t ShMenuCreate(const char *title) {
    Menu *root;
    uint32_t h;
    Item *it;

    EnsureMenu();
    Lock();
    h = NewMenu(title, g_root);
    root = MenuOf(g_root);
    if (h && root) {
        it = NewItem(root, IT_SUB, title, NULL, NULL);
        if (it) it->sub = h;
    }
    Unlock();
    ShSetError(h ? SH_OK : SH_ERR_NO_CANDIDATE);
    return h;
}

SH_API uint32_t ShMenuSub(uint32_t parent, const char *label) {
    uint32_t h;
    Item *it;
    Menu *m;

    EnsureMenu();
    Lock();
    h = NewMenu(label, parent);
    m = MenuOf(parent);
    if (h && m) {
        it = NewItem(m, IT_SUB, label, NULL, NULL);
        if (it) it->sub = h;
    }
    Unlock();
    return h;
}

SH_API int ShMenuAction(uint32_t menu, const char *label,
                        ShMenuFn fn, void *user) {
    Item *it;

    Lock();
    it = NewItem(MenuOf(menu), IT_ACTION, label, fn, user);
    Unlock();
    return it != NULL;
}

SH_API int ShMenuToggle(uint32_t menu, const char *label,
                        int initial, ShMenuFn fn, void *user) {
    Item *it;

    Lock();
    it = NewItem(MenuOf(menu), IT_TOGGLE, label, fn, user);
    if (it) it->value = initial ? 1 : 0;
    Unlock();
    return it != NULL;
}

SH_API int ShMenuNumber(uint32_t menu, const char *label,
                        float initial, float lo, float hi,
                        float step, ShMenuFn fn, void *user) {
    Item *it;

    Lock();
    it = NewItem(MenuOf(menu), IT_NUMBER, label, fn, user);
    if (it) {
        it->num = initial;
        it->lo = lo;
        it->hi = hi;
        it->step = step;
    }
    Unlock();
    return it != NULL;
}

/* The option strings are borrowed, so they must outlive the
 * menu. String literals are the intended case.
 */
SH_API int ShMenuList(uint32_t menu, const char *label,
                      const char **opts, int n, int initial,
                      ShMenuFn fn, void *user) {
    Item *it;
    int i;

    if (n > OPTS) n = OPTS;
    Lock();
    it = NewItem(MenuOf(menu), IT_LIST, label, fn, user);
    if (it) {
        for (i = 0; i < n; i++) it->opts[i] = opts[i];
        it->nopts = n;
        it->value = (n > 0) ? (initial % n) : 0;
    }
    Unlock();
    return it != NULL;
}

/* The line under the items, for whatever the last action
 * has to report. Empty text removes it.
 */
SH_API int ShMenuStatus(uint32_t menu, const char *text) {
    Menu *m;

    Lock();
    m = MenuOf(menu);
    if (!m) { Unlock(); ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (text) {
        strncpy(m->status, text, sizeof(m->status) - 1);
        m->status[sizeof(m->status) - 1] = 0;
    } else {
        m->status[0] = 0;
    }
    Unlock();
    return 1;
}

SH_API void ShMenuSetKey(int vk) { g_key = vk; }
SH_API int  ShMenuIsOpen(void) { return g_open; }

SH_API void ShMenuOpen(int open) {
    EnsureMenu();
    g_open = open ? 1 : 0;
    if (g_open) g_current = g_root;
}

/* Opened once per session on entering Playing, so the key
 * is discoverable without anyone being told it.
 */
void ShMenuOnEnterPlaying(void) {
    Menu *root;
    int have = 0;

    if (!g_started || g_greeted) return;
    Lock();
    root = MenuOf(g_root);
    if (root) {
        have = root->count > 0;
        if (have)
            strncpy(root->status, "F4 opens this menu",
                    sizeof(root->status) - 1);
    }
    Unlock();
    if (!have) return;
    g_greeted = 1;
    g_current = g_root;
    g_open = 1;
}
