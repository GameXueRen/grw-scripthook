/* A shared menu, one root owned by the API. Every plugin
 * registers a submenu, so sixteen addons cost sixteen rows.
 * Drawn by the engine itself through the native UI. */
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
#define TICK_MS     40
#define OPTS        12

/* Geometry in HUD pixels. */
#define MENU_X      16.0f
#define MENU_Y      16.0f
#define MENU_W      400.0f
#define PAD         16.0f
#define TITLE_H     34.0f
#define ROW_H       28.0f
#define BAR_DY      -4.0f
#define BAR_H       22.0f
#define VALUE_W     150.0f

#define C_TITLE     0xFFD25Au
#define C_ROW       0xD2D2D2u
#define C_SEL       0x8CF0FFu
#define C_FOOT      0x8C8C8Cu
#define C_STATUS    0xA0E6A0u
#define C_BAR       0x28465Au

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

/* What is on screen, so only differences are pushed. */
typedef struct {
    char name[LABEL];
    char value[32];
    int  shown;
    int  selected;
} RowView;

typedef struct {
    char    title[LABEL];
    char    status[96];
    char    footer[32];
    int     rows;
    int     sel;
    RowView row[VISIBLE];
} View;

static Menu g_menus[MENUS];
static uint32_t g_root = 0;
static volatile uint32_t g_current = 0;
static volatile int g_open = 0;
static volatile int g_key = VK_F4;
static volatile int g_greeted = 0;
static volatile int g_started = 0;
static CRITICAL_SECTION g_lock;
static volatile int g_lockReady = 0;

/* Native widget ids, valid for one UI generation. built is
 * written by the build worker and read by the menu thread, so
 * it is the one volatile field: everything else is only
 * touched while built is 1. */
static struct {
    volatile int built;
    int          gen, shown;
    uint32_t panel, title, bar, footer, status;
    uint32_t name[VISIBLE], value[VISIBLE];
    View     drawn;
} g_ui;

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

/* Free a menu and everything under it. Caller holds the
 * lock. Depth is bounded by MENUS, so recursion is safe. */
static void DropMenu(uint32_t h) {
    Menu *m = MenuOf(h);
    int i;

    if (!m) return;
    for (i = 0; i < m->count; i++)
        if (m->items[i].kind == IT_SUB && m->items[i].sub)
            DropMenu(m->items[i].sub);
    memset(m, 0, sizeof(*m));
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

/* A plugin callback can be heavy (a heap scan, a node walk,
 * visibility jobs on the game thread), and running it on the
 * menu thread would freeze navigation for the whole duration.
 * Dispatch callbacks to a single worker instead: the item's
 * state is already applied by the caller, so the callback only
 * notifies the plugin. FIFO order keeps rapid toggles sane.
 */
#define CALL_MAX 32
typedef struct {
    ShMenuFn fn;
    void *user;
    uint32_t menu, item;
    int value;
} Call;

static Call g_call[CALL_MAX];
static volatile int g_callHead = 0;
static volatile int g_callTail = 0;
static HANDLE g_callThread = NULL;

static DWORD WINAPI CallThread(LPVOID p);

static void CallPush(ShMenuFn fn, void *user, uint32_t menu,
                     uint32_t item, int value) {
    int next;

    if (!fn) return;
    Lock();
    next = (g_callHead + 1) % CALL_MAX;
    if (next != g_callTail) {
        g_call[g_callHead].fn = fn;
        g_call[g_callHead].user = user;
        g_call[g_callHead].menu = menu;
        g_call[g_callHead].item = item;
        g_call[g_callHead].value = value;
        g_callHead = next;
    }
    Unlock();
    if (!g_callThread)
        g_callThread = CreateThread(NULL, 0, CallThread, NULL, 0, NULL);
}

static DWORD WINAPI CallThread(LPVOID p) {
    (void)p;
    for (;;) {
        Call c;
        int has = 0;

        Lock();
        if (g_callHead != g_callTail) {
            c = g_call[g_callTail];
            g_callTail = (g_callTail + 1) % CALL_MAX;
            has = 1;
        }
        Unlock();
        if (!has) { Sleep(5); continue; }
        if (c.fn) c.fn(c.menu, c.item, c.value, c.user);
    }
    return 0;
}

static void Fire(uint32_t menu, int idx, Item *it) {
    int v = (it->kind == IT_NUMBER) ? (int)it->num : it->value;

    CallPush(it->fn, it->user, menu, (uint32_t)idx, v);
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

/* Snapshot of the current menu. Caller holds the lock. */
static void Capture(View *v) {
    Menu *m = MenuOf(g_current);
    int i;

    memset(v, 0, sizeof(*v));
    if (!m) return;
    strncpy(v->title, m->title, LABEL - 1);
    strncpy(v->status, m->status, sizeof(v->status) - 1);
    for (i = m->top; i < m->count && i < m->top + VISIBLE; i++) {
        RowView *r = &v->row[v->rows];
        strncpy(r->name, m->items[i].label, LABEL - 1);
        ValueText(&m->items[i], r->value, sizeof(r->value));
        r->shown = 1;
        r->selected = (i == m->sel);
        if (r->selected) v->sel = v->rows;
        v->rows++;
    }
    if (m->count > VISIBLE)
        snprintf(v->footer, sizeof(v->footer), "%d of %d",
                 m->sel + 1, m->count);
}

static float RowY(int i) {
    return PAD + TITLE_H + ROW_H * (float)i;
}

static float PanelHeight(const View *v) {
    float h = PAD + TITLE_H + ROW_H * (float)v->rows + PAD;
    if (v->footer[0]) h += ROW_H;
    if (v->status[0]) h += ROW_H;
    return h;
}

static void DropWidgets(void) {
    if (g_ui.built && g_ui.gen == ShUiGen() && g_ui.panel)
        ShUiDestroy(g_ui.panel);
    memset(&g_ui, 0, sizeof(g_ui));
}

/* Every widget of the menu, or none of it. A single create
 * can fail when the game state flickers, and a menu missing
 * half its rows never repaired itself. */
static int Complete(void) {
    int i;

    if (!g_ui.panel || !g_ui.bar || !g_ui.title) return 0;
    if (!g_ui.footer || !g_ui.status) return 0;
    for (i = 0; i < VISIBLE; i++)
        if (!g_ui.name[i] || !g_ui.value[i]) return 0;
    return 1;
}

/* One creation per widget, hidden rows included, so later
 * updates are text and position only. */
static int BuildWidgets(void) {
    int i;

    memset(&g_ui, 0, sizeof(g_ui));
    if (!ShUiReady()) return 0;
    g_ui.gen = ShUiGen();
    g_ui.panel = ShUiPanel(MENU_X, MENU_Y, MENU_W, 200.0f, 0x000000, 0.8f);
    if (!g_ui.panel) return 0;
    g_ui.bar = ShUiImage(g_ui.panel, PAD / 2, RowY(0) + BAR_DY,
                         MENU_W - PAD, BAR_H, C_BAR, 0.9f);
    g_ui.title = ShUiLabel(g_ui.panel, PAD, PAD, MENU_W - 2 * PAD,
                           TITLE_H, " ", C_TITLE);
    for (i = 0; i < VISIBLE; i++) {
        g_ui.name[i] = ShUiLabel(g_ui.panel, PAD + 8.0f, RowY(i),
                                 MENU_W - VALUE_W - PAD, ROW_H, " ",
                                 C_ROW);
        g_ui.value[i] = ShUiLabel(g_ui.panel, MENU_W - PAD - VALUE_W,
                                  RowY(i), VALUE_W, ROW_H, " ", C_ROW);
    }
    g_ui.footer = ShUiLabel(g_ui.panel, PAD, RowY(0), MENU_W - 2 * PAD,
                            ROW_H, " ", C_FOOT);
    g_ui.status = ShUiLabel(g_ui.panel, PAD, RowY(0), MENU_W - 2 * PAD,
                            ROW_H, " ", C_STATUS);

    /* Destroying the panel takes the subtree with it, so the
     * next tick starts clean instead of leaking slots. */
    if (!Complete()) {
        ShUiDestroy(g_ui.panel);
        memset(&g_ui, 0, sizeof(g_ui));
        return 0;
    }

    /* Hide the whole menu in one batched job: the creates
     * above were one job each, but ~30 more for the hides
     * would double a cold first build. */
    if (ShUiBegin()) {
        for (i = 0; i < VISIBLE; i++) {
            ShUiShow(g_ui.name[i], 0);
            ShUiShow(g_ui.value[i], 0);
        }
        ShUiShow(g_ui.footer, 0);
        ShUiShow(g_ui.status, 0);
        ShUiShow(g_ui.panel, 0);
        ShUiCommit();
    } else {
        for (i = 0; i < VISIBLE; i++) {
            ShUiShow(g_ui.name[i], 0);
            ShUiShow(g_ui.value[i], 0);
        }
        ShUiShow(g_ui.footer, 0);
        ShUiShow(g_ui.status, 0);
        ShUiShow(g_ui.panel, 0);
    }
    /* shown first: the menu thread reads both, and built is
     * the volatile handoff that says the rest is ready. */
    g_ui.shown = 0;
    g_ui.built = 1;
    return 1;
}

static void SetTextIf(uint32_t id, char *have, int cap,
                      const char *want) {
    if (strcmp(have, want) == 0) return;
    strncpy(have, want, cap - 1);
    have[cap - 1] = 0;
    ShUiSetText(id, want[0] ? want : " ");
}

/* Push the differences between the drawn view and v. */
static void Sync(const View *v) {
    View *d = &g_ui.drawn;
    float y;
    int i;

    SetTextIf(g_ui.title, d->title, LABEL, v->title);
    for (i = 0; i < VISIBLE; i++) {
        const RowView *r = &v->row[i];
        RowView *dr = &d->row[i];

        if (r->shown != dr->shown) {
            ShUiShow(g_ui.name[i], r->shown);
            ShUiShow(g_ui.value[i], r->shown);
            dr->shown = r->shown;
        }
        if (!r->shown) continue;
        SetTextIf(g_ui.name[i], dr->name, LABEL, r->name);
        SetTextIf(g_ui.value[i], dr->value, sizeof(dr->value), r->value);
        if (r->selected != dr->selected) {
            ShUiSetColour(g_ui.name[i], r->selected ? C_SEL : C_ROW);
            ShUiSetColour(g_ui.value[i], r->selected ? C_SEL : C_ROW);
            dr->selected = r->selected;
        }
    }
    if (v->sel != d->sel || v->rows != d->rows) {
        ShUiSetPos(g_ui.bar, PAD / 2, RowY(v->sel) + BAR_DY);
        d->sel = v->sel;
    }
    y = RowY(v->rows);
    if (v->rows != d->rows || strcmp(v->footer, d->footer) != 0 ||
        strcmp(v->status, d->status) != 0) {
        if (v->footer[0]) {
            ShUiSetPos(g_ui.footer, PAD, y + 2.0f);
            y += ROW_H;
        }
        if (v->status[0]) ShUiSetPos(g_ui.status, PAD, y + 2.0f);
        if ((v->footer[0] != 0) != (d->footer[0] != 0))
            ShUiShow(g_ui.footer, v->footer[0] != 0);
        if ((v->status[0] != 0) != (d->status[0] != 0))
            ShUiShow(g_ui.status, v->status[0] != 0);
        SetTextIf(g_ui.footer, d->footer, sizeof(d->footer), v->footer);
        SetTextIf(g_ui.status, d->status, sizeof(d->status), v->status);
        ShUiSetSize(g_ui.panel, MENU_W, PanelHeight(v));
        d->rows = v->rows;
    }
}

/* Keys are taken only while the menu is actually drawn. A
 * menu that is wanted but cannot render yet (UI scene not up,
 * game state not PLAYING, a world reload) must never swallow
 * the keyboard: that is exactly the bug where the character
 * freezes with no menu on screen, and where F4 looks dead
 * while the menu thread sits inside a widget job.
 */
static volatile int g_captureNow = 0;
static volatile int g_escDefer = 0;
static DWORD g_escDeferAt = 0;

/* Not SetCapture: that is a Win32 API and the name collides. */
static void MenuCapture(int on) {
    if (on == g_captureNow) return;
    g_captureNow = on;
    ShCaptureKeys(on);
    if (on) {
        /* While the menu is up, ESC belongs to it: the menu
         * navigates with it (back a level, or exit). Without
         * the block the game would open its own pause menu on
         * the same press and the two fight. */
        ShBlockKey(VK_ESCAPE, 1);
        g_escDefer = 0;
        return;
    }
    /* Releasing capture: if ESC is still held, the very press
     * that closed the menu is in flight. Keep it blocked until
     * the key goes up (or a short timeout) so it never reaches
     * the game and pops its pause menu on the way out. */
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
        g_escDefer = 1;
        g_escDeferAt = GetTickCount();
        return;
    }
    ShBlockKey(VK_ESCAPE, 0);
}

/* A deferred ESC block ends once the key is up, or after a
 * second so a stuck key cannot swallow ESC forever. */
static void EscDeferTick(void) {
    DWORD now;
    if (!g_escDefer) return;
    now = GetTickCount();
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) == 0 ||
        (int)(now - g_escDeferAt) > 1000) {
        g_escDefer = 0;
        ShBlockKey(VK_ESCAPE, 0);
    }
}

/* Push the menu's view to the engine. A menu update is ~30
 * property ops, and doing them one at a time costs a game
 * thread queue round trip each; batching them into one job
 * turns the whole update into a single round trip. Falls back
 * to individual ops if no batch slot is free. show only makes
 * the panel visible (the open path); the warmup keeps it
 * hidden. */
static void PushView(const View *v, int show) {
    int batched = ShUiBegin();

    Sync(v);
    if (show && !g_ui.shown) {
        ShUiShow(g_ui.panel, 1);
        g_ui.shown = 1;
    }
    if (batched) ShUiCommit();
}

/* The first build resolves the engine scene, scans the asset
 * tables (a whole address space walk on a cold session) and
 * creates ~29 widgets through the game thread: seconds on the
 * first open. Run it on a worker so the menu thread keeps
 * polling F4; opening then shows prebuilt widgets. A failed
 * build backs off so a missing asset cannot make us rescan
 * the whole address space every tick. */
static volatile int g_buildPending = 0;
static HANDLE g_buildThread = NULL;

static DWORD WINAPI BuildWorker(LPVOID p) {
    (void)p;
    for (;;) {
        if (!g_buildPending) { Sleep(40); continue; }
        g_buildPending = 0;
        if (g_ui.built) continue;
        if (BuildWidgets()) continue;
        Sleep(500);
    }
    return 0;
}

/* Ask the worker to build the widgets while the menu is
 * closed, so the first F4 is a show and not a build. */
static void TryBuild(void) {
    if (g_ui.built) return;
    g_buildPending = 1;
}

/* Keys are polled here, the engine draws the result. */
static DWORD WINAPI MenuThread(LPVOID p) {
    View v;
    (void)p;

    for (;;) {
        Sleep(TICK_MS);

        EscDeferTick();

        /* F4 first, on every tick, before any UI work that
         * could block: the toggle must never depend on the
         * menu having rendered. */
        if (Pressed(g_key)) {
            g_open = !g_open;
            if (g_open) g_current = g_root;
        }

        if (g_ui.built && g_ui.gen != ShUiGen()) DropWidgets();

        if (!g_open) {
            if (g_ui.built && g_ui.shown) {
                ShUiShow(g_ui.panel, 0);
                g_ui.shown = 0;
            }
            MenuCapture(0);
            /* first F4 must be instant: build the widgets now,
             * hidden, so opening is a show and not a build */
            TryBuild();
            continue;
        }

        /* Wanted but not drawable yet: the widget build can
         * take seconds on a cold session, so it runs on the
         * build worker. Hand the keys back and wait for it;
         * F4 keeps polling meanwhile. */
        if (!g_ui.built) {
            MenuCapture(0);
            TryBuild();
            continue;
        }

        Lock();
        Navigate();
        Capture(&v);
        Unlock();

        PushView(&v, 1);
        /* Only a menu that is actually on screen takes the
         * keyboard. */
        MenuCapture(1);
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
    CreateThread(NULL, 0, BuildWorker, NULL, 0, NULL);
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

/* Drop the items but keep the row, so a plugin can rebuild
 * its own menu (a reload) without stacking duplicates. */
SH_API int ShMenuClear(uint32_t menu) {
    Menu *m;

    Lock();
    m = MenuOf(menu);
    if (m) {
        int i;
        for (i = 0; i < m->count; i++)
            if (m->items[i].kind == IT_SUB && m->items[i].sub)
                DropMenu(m->items[i].sub);
        m->count = 0;
        m->sel = 0;
    }
    Unlock();
    return m != NULL;
}

/* Remove the row itself, and its subtree with it. */
SH_API int ShMenuDestroy(uint32_t menu) {
    Menu *parent;
    int found = 0;

    if (menu == g_root) return 0;
    Lock();
    parent = MenuOf(MenuOf(menu) ? MenuOf(menu)->parent : 0);
    if (parent) {
        int i, w = 0;
        for (i = 0; i < parent->count; i++) {
            if (parent->items[i].kind == IT_SUB &&
                parent->items[i].sub == menu) {
                found = 1;
                continue;
            }
            if (w != i) parent->items[w] = parent->items[i];
            w++;
        }
        parent->count = w;
        if (parent->sel >= w) parent->sel = w ? w - 1 : 0;
    }
    DropMenu(menu);
    Unlock();
    return found;
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
