/* A shared menu, one root owned by the API. Every plugin
 * registers a submenu, so sixteen addons cost sixteen rows.
 *
 * The menu model (menus, items, navigation, callbacks) lives
 * here. Rendering is done by the D3D11 overlay in
 * scripthook_ovl.cpp: every frame it snapshots the current menu
 * through ShMenuCaptureView and draws it with Dear ImGui inside
 * the game's Present call, using the same look and interaction
 * as the original native-UI menu. The overlay declares itself
 * ready via ShMenuSetOverlayReady; until then the menu never
 * swallows the keyboard. */
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
    char     hint[128];
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
static volatile int g_started = 0;
static CRITICAL_SECTION g_lock;
static volatile int g_lockReady = 0;

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

/* Rendered text for the value side of a row. Fixed words and list
 * options are translated in the menu's scope; number and arrow
 * formats are language-neutral. */
static void ValueText(const char *scope, const Item *it,
                      char *out, int n) {
    out[0] = 0;
    if (it->kind == IT_SUB) snprintf(out, n, ">");
    else if (it->kind == IT_TOGGLE)
        snprintf(out, n, "[%s]", it->value ? ShLangFor(scope, "on")
                                           : ShLangFor(scope, "off"));
    else if (it->kind == IT_NUMBER)
        snprintf(out, n, "< %.2f >", it->num);
    else if (it->kind == IT_LIST && it->nopts)
        snprintf(out, n, "< %s >",
                 ShLangFor(scope, it->opts[it->value % it->nopts]));
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
    /* Arrows and WASD both navigate; each is polled independently,
     * so the two coexist without stealing presses from each other. */
    if (Pressed(VK_UP) || Pressed('W'))
        m->sel = (m->sel + m->count - 1) % m->count;
    if (Pressed(VK_DOWN) || Pressed('S'))
        m->sel = (m->sel + 1) % m->count;
    Scroll(m);

    it = &m->items[m->sel];
    left = Pressed(VK_LEFT) || Pressed('A');
    right = Pressed(VK_RIGHT) || Pressed('D');
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

/* Keys are taken only while the menu is actually drawn. A
 * menu that is wanted but cannot render yet (overlay not up,
 * game state not PLAYING, a world reload) must never swallow
 * the keyboard: that is exactly the bug where the character
 * freezes with no menu on screen.
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

/* The overlay (scripthook_ovl.cpp) renders on the game render
 * thread. Until it is up, the menu is wanted-but-not-drawable
 * and must neither capture the keyboard nor claim visibility. */
static volatile int g_ovlReady = 0;

void ShMenuSetOverlayReady(int ready) {
    g_ovlReady = ready ? 1 : 0;
}

/* Dotted title path from the first submenu under the root down to
 * m, e.g. "First person.Custom". Empty for the root itself. Used as
 * the translation scope, so a deeper menu first matches its own
 * section and falls back up its ancestors: [zh_cn.A.B.C] ->
 * [zh_cn.A.B] -> [zh_cn.A] -> [zh_cn]. */
static void MenuPath(const Menu *m, char *out, int n) {
    const char *titles[8];
    int k = 0;
    size_t used = 0;
    const Menu *cur = m;

    out[0] = 0;
    if (n <= 0) return;
    while (cur && cur->parent && k < 8) {
        titles[k++] = cur->title;
        cur = MenuOf(cur->parent);
    }
    while (k > 0) {
        const char *t = titles[--k];
        int w = snprintf(out + used, n - used, "%s%s",
                         used ? "." : "", t);
        if (w < 0) break;
        used += (size_t)w;
        if (used >= (size_t)n) break;
    }
    out[n - 1] = 0;
}

/* Snapshot the current menu for the overlay renderer. Labels,
 * values and the title are translated here (the model keeps the
 * English originals), then copied so the renderer can draw them
 * without holding the lock. Items are scoped to this menu's title
 * path; the title itself is scoped to the parent's path so it
 * matches the row that led here. */
void ShMenuCaptureView(ShMenuView *v) {
    Menu *m;
    int i;

    memset(v, 0, sizeof(*v));
    Lock();
    m = MenuOf(g_current);
    if (m) {
        char path[64], parentPath[64];
        Menu *pm = MenuOf(m->parent);

        /* The root's rows and the submenus' titles read from the
         * global table ([lang]), because MenuPath is empty for the
         * root and for any menu whose parent is the root. Deeper
         * menus use the dotted title path from the root's child
         * down, falling back to [lang] level by level. */
        MenuPath(m, path, sizeof(path));
        MenuPath(pm, parentPath, sizeof(parentPath));

        /* The root shows the control hints; every submenu shows the
         * plugin's own hint (ShMenuHint), translated in its scope.
         * An unset hint stays empty and takes no room. */
        if (m->parent == 0)
            snprintf(v->hint, sizeof(v->hint), "%s\n%s",
                     ShLang("F4 toggle menu, Enter select, ESC back"),
                     ShLang("\xE2\x86\x91 \xE2\x86\x93 or W/S select, "
                            "\xE2\x86\x90 \xE2\x86\x92 or A/D adjust"));
        else if (m->hint[0])
            strncpy(v->hint, ShLangFor(path, m->hint),
                    sizeof(v->hint) - 1);

        strncpy(v->title, ShLangFor(parentPath, m->title),
                sizeof(v->title) - 1);
        strncpy(v->status, ShLangFor(path, m->status),
                sizeof(v->status) - 1);
        for (i = m->top; i < m->count && i < m->top + VISIBLE; i++) {
            ShMenuRow *r = &v->row[v->rows];
            strncpy(r->name, ShLangFor(path, m->items[i].label),
                    sizeof(r->name) - 1);
            ValueText(path, &m->items[i], r->value, sizeof(r->value));
            r->selected = (i == m->sel);
            if (r->selected) v->sel = v->rows;
            v->rows++;
        }
        if (m->count > VISIBLE)
            snprintf(v->footer, sizeof(v->footer), "%d / %d",
                     m->sel + 1, m->count);
    }
    Unlock();
}

/* Keys are polled here; the overlay draws the result. */
static DWORD WINAPI MenuThread(LPVOID p) {
    (void)p;

    for (;;) {
        Sleep(TICK_MS);

        EscDeferTick();

        /* F4 first, on every tick, before any menu work that
         * could block: the toggle must never depend on the
         * menu having rendered. */
        if (Pressed(g_key)) {
            g_open = !g_open;
            if (g_open) g_current = g_root;
        }

        if (!g_open) {
            MenuCapture(0);
            continue;
        }

        /* Open but not yet renderable: the overlay initialises
         * on the first Present. Hand the keys back and wait;
         * F4 keeps polling meanwhile. */
        if (!g_ovlReady) {
            MenuCapture(0);
            continue;
        }

        Lock();
        Navigate();
        Unlock();

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

/* The hint shown under the title of a submenu. The root menu always
 * shows the control hints instead. Empty text clears it. */
SH_API int ShMenuHint(uint32_t menu, const char *text) {
    Menu *m;

    Lock();
    m = MenuOf(menu);
    if (!m) { Unlock(); ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (text) {
        strncpy(m->hint, text, sizeof(m->hint) - 1);
        m->hint[sizeof(m->hint) - 1] = 0;
    } else {
        m->hint[0] = 0;
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

/* Entering Playing used to auto-open the menu once so the F4 key
 * was discoverable. That surprised players, so the menu only opens
 * on F4 now; the entry point stays for the state machine. */
void ShMenuOnEnterPlaying(void) {
}
