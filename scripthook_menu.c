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
#include <stdarg.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"

#define MENUS       24
#define ITEMS       96
#define LABEL       48
#define VISIBLE     12
#define TICK_MS     40
#define OPTS        12

enum { IT_ACTION = 0, IT_SUB, IT_TOGGLE, IT_NUMBER, IT_LIST,
       IT_KEYBIND };

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

/* ---- key-bind capture state (used by ValueText below) ------------- */
static volatile int g_capActive = 0;   /* a capture is waiting        */
static const Item *g_capItem = NULL;   /* the row waiting for a key  */

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

/* ---- key-bind rows ------------------------------------------------
 * A row that stores a virtual key (Item.value = VK code).  Pressing
 * Enter on it arms a capture: the menu thread then ignores normal
 * navigation and waits for the player to press one key (Esc cancels).
 * The captured VK is stored back and the row's callback fires, so a
 * "change this hotkey" row is a single key press away.
 * ------------------------------------------------------------------ */
#define VK_NONE 0

/* Render the friendly name of a virtual key into out (>= n bytes). */
static void VkName(int vk, char *out, int n) {
    char *p = out;
    int left = n;
#define PUSH1(c)   do { if (left > 1) { *p++ = (c); left--; } } while (0)
#define PUSH2(a,b) do { PUSH1(a); PUSH1(b); } while (0)
    if (n <= 0) return;
    out[0] = 0;
    if (vk == VK_NONE) { PUSH2('?', '?'); }
    else if (vk >= '0' && vk <= '9') PUSH1((char)vk);
    else if (vk >= 'A' && vk <= 'Z') PUSH1((char)vk);
    else if (vk >= VK_F1 && vk <= VK_F24) {
        PUSH1('F');
        if (vk - VK_F1 + 1 >= 10) PUSH1((char)('0' + (vk - VK_F1 + 1) / 10));
        PUSH1((char)('0' + (vk - VK_F1 + 1) % 10));
    }
    else switch (vk) {
    case VK_ESCAPE:   memcpy(p, "Esc", 4); break;
    case VK_RETURN:   memcpy(p, "Enter", 6); break;
    case VK_TAB:      memcpy(p, "Tab", 4); break;
    case VK_BACK:     memcpy(p, "Bksp", 5); break;
    case VK_SPACE:    memcpy(p, "Space", 6); break;
    case VK_UP:       memcpy(p, "Up", 3); break;
    case VK_DOWN:     memcpy(p, "Down", 5); break;
    case VK_LEFT:     memcpy(p, "Left", 5); break;
    case VK_RIGHT:    memcpy(p, "Right", 6); break;
    case VK_HOME:     memcpy(p, "Home", 5); break;
    case VK_END:      memcpy(p, "End", 4); break;
    case VK_DELETE:   memcpy(p, "Del", 4); break;
    case VK_INSERT:   memcpy(p, "Ins", 4); break;
    default:          snprintf(out, n - 1, "VK%02X", vk); return;
    }
#undef PUSH1
#undef PUSH2
    out[n - 1] = 0;
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
        /* Integer step with a whole current value renders as an
         * integer (< 30 >); fractional steps keep two decimals. */
        if (it->step >= 1.0f && it->num == (float)(int)it->num)
            snprintf(out, n, "< %.0f >", it->num);
        else
            snprintf(out, n, "< %.2f >", it->num);
    else if (it->kind == IT_LIST && it->nopts)
        snprintf(out, n, "< %s >",
                 ShLangFor(scope, it->opts[it->value % it->nopts]));
    else if (it->kind == IT_KEYBIND) {
        if (g_capActive && it == g_capItem) {
            snprintf(out, n, "< ... >");   /* waiting for a key */
        } else {
            char kn[16];
            VkName(it->value, kn, sizeof(kn));
            snprintf(out, n, "< %s >", kn);
        }
    }
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

static unsigned char g_keyWas[256];

static int Pressed(int vk) {
    int d = (GetAsyncKeyState(vk) & 0x8000) != 0;
    int hit = d && !g_keyWas[vk & 0xFF];
    g_keyWas[vk & 0xFF] = (unsigned char)d;
    return hit;
}

/* The menu keys are polled, so a background game window must not
 * react to keys meant for the window in front. Forget held keys
 * while unfocused so nothing fires when focus comes back. */
static void ResetKeys(void) {
    memset(g_keyWas, 0, sizeof(g_keyWas));
}

/* ---- key-bind capture ---------------------------------------------
 * Enter on an IT_KEYBIND row arms a capture: normal navigation is
 * paused and the next key press becomes the row's value.  The old
 * value stays in place until a new one is chosen, so cancelling just
 * clears the capture flag.  Menu state is only touched from the menu
 * thread while it holds the lock, so these helpers are called there. */
static uint32_t g_capMenu = 0;
static int  g_capRow = -1;
static unsigned char g_capPrev[256];

/* Keys that may be bound: no mouse buttons, no bare modifiers, and
 * no keys the menu itself needs (Enter arms the capture, Esc cancels,
 * the menu hotkey closes the menu). */
static int CapBindable(int vk) {
    if (vk <= 0x06 || vk >= 0xFE) return 0;
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
        vk == VK_LWIN || vk == VK_RWIN) return 0;
    if (vk == VK_RETURN || vk == VK_ESCAPE || vk == g_key) return 0;
    return 1;
}

static void CapClear(void) {
    g_capActive = 0;
    g_capItem = NULL;
    g_capRow = -1;
}

/* Poll one capture tick.  Runs under the menu lock.  Returns 1 when
 * capture ended (bound or cancelled), 0 while still waiting. */
static int CapTickLocked(void) {
    Menu *m;
    Item *it;
    int vk;

    if (!g_capActive || g_capRow < 0) { CapClear(); return 1; }
    m = MenuOf(g_capMenu);
    if (!m || g_capRow >= m->count) { CapClear(); return 1; }
    it = &m->items[g_capRow];
    if (it->kind != IT_KEYBIND) { CapClear(); return 1; }

    /* Esc cancels the capture (checked before the bindable scan). */
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) &&
        !g_capPrev[VK_ESCAPE]) {
        g_keyWas[VK_ESCAPE & 0xFF] = 1;
        CapClear();
        return 1;
    }
    for (vk = 1; vk < 256; vk++) {
        int d = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (!d || g_capPrev[vk] || !CapBindable(vk)) continue;
        /* A fresh press of a bindable key. */
        it->value = vk;                                 /* bind */
        g_keyWas[vk & 0xFF] = 1;        /* consume the press */
        CapClear();
        Fire(g_capMenu, (uint32_t)(it - m->items), it);
        return 1;
    }
    /* No new key yet: track what is held so a release+repress of a
     * key already down when capture armed is not treated as fresh. */
    for (vk = 1; vk < 256; vk++)
        g_capPrev[vk] = (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
    return 0;
}

/* True while the game window has the focus. Any window of this
 * process counts, which covers both windowed and borderless
 * fullscreen; a backgrounded game reports the window in front. */
static int WindowFocused(void) {
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
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
        } else if (it->kind == IT_KEYBIND) {
            /* Arm a capture: pause navigation and take the next key
             * press as this row's new value.  Snapshot the keys that
             * are already down (Enter armed it) so a held key is not
             * mistaken for the new one. */
            g_capMenu = g_current;
            g_capRow = m->sel;
            g_capItem = it;
            g_capActive = 1;
            for (int i = 1; i < 256; i++)
                g_capPrev[i] =
                    (GetAsyncKeyState(i) & 0x8000) ? 1 : 0;
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

/* strncpy truncates by bytes, which can split a UTF-8 sequence
 * and leave the renderer with an invalid lead byte (it draws it
 * as '?'). Copy then back up to the start of the last code point
 * if the cut landed inside a multi-byte sequence. */
static void SafeCopy(char *dst, size_t cap, const char *src) {
    size_t n;
    if (cap == 0) return;
    n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        memcpy(dst, src, n);
        /* Walk back over any UTF-8 continuation bytes (0x80..0xBF). */
        while (n > 0 && (unsigned char)dst[n-1] >= 0x80 &&
               (unsigned char)dst[n-1] <  0xC0) n--;
        /* The lead byte at n-1 (if any) starts a multi-byte sequence
         * whose full length would extend past cap-1; drop it. */
        if (n > 0) {
            unsigned char b = (unsigned char)dst[n-1];
            if (b >= 0xC2) {
                int need = (b < 0xE0) ? 2 : (b < 0xF0) ? 3 : 4;
                if ((size_t)(n - 1 + need) > cap - 1) dst[--n] = 0;
            }
        }
        dst[n] = 0;
    } else {
        memcpy(dst, src, n + 1);
    }
}

/* The [MenuOrder] weights reorder the ROOT menu's rows in the
 * model itself, so navigation (which walks m->items) and the
 * visible order always agree. The cursor follows its row across
 * the sort. Unlisted rows use the default weight and keep their
 * relative order (stable sort). A quick ordered check skips the
 * sort once the rows are already in weight order. */
static void ReorderRoot(Menu *m) {
    int i, j, selPos = -1;
    char selLabel[LABEL];

    if (m->parent != 0 || m->count < 2) return;

    for (i = 1; i < m->count; i++) {
        int w0 = ShConfigGetInt("MenuOrder",
                                m->items[i - 1].label, 1000);
        int w1 = ShConfigGetInt("MenuOrder",
                                m->items[i].label, 1000);
        if (w0 > w1) break;
    }
    if (i >= m->count) return; /* already ordered */

    strncpy(selLabel, m->items[m->sel].label, sizeof(selLabel) - 1);
    selLabel[sizeof(selLabel) - 1] = 0;

    for (i = 1; i < m->count; i++) {
        Item it = m->items[i];
        int wi = ShConfigGetInt("MenuOrder", it.label, 1000);
        j = i;
        while (j > 0) {
            int wj = ShConfigGetInt("MenuOrder",
                                    m->items[j - 1].label, 1000);
            if (wj <= wi) break;
            m->items[j] = m->items[j - 1];
            j--;
        }
        m->items[j] = it;
    }

    for (i = 0; i < m->count; i++)
        if (!strcmp(m->items[i].label, selLabel)) { selPos = i; break; }
    if (selPos >= 0) m->sel = selPos;
    if (m->top > m->sel) m->top = m->sel;
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

        v->isRoot = (m->parent == 0);

        /* Keep the root's plugin rows ordered by [MenuOrder] in
         * the model, so navigation matches what is on screen. */
        if (m->parent == 0) ReorderRoot(m);

        /* The root's rows and the submenus' titles read from the
         * global table ([lang]), because MenuPath is empty for the
         * root and for any menu whose parent is the root. Deeper
         * menus use the dotted title path from the root's child
         * down, falling back to [lang] level by level. */
        MenuPath(m, path, sizeof(path));
        MenuPath(pm, parentPath, sizeof(parentPath));

        /* The root shows the control hints; every submenu shows the
         * plugin's own hint (ShMenuHint), translated in its scope.
         * Third-party plugins that never call ShMenuHint get a hint
         * from the [MenuHints] config section, keyed by menu title,
         * translated the same way. An unset hint stays empty and
         * takes no room. */
        if (m->parent == 0)
            snprintf(v->hint, sizeof(v->hint), "%s\n%s",
                     ShLang("F4 toggle menu, Enter select, ESC back"),
                     ShLang("\xE2\x86\x91 \xE2\x86\x93 or W/S select, "
                            "\xE2\x86\x90 \xE2\x86\x92 or A/D adjust"));
        else if (m->hint[0])
            SafeCopy(v->hint, sizeof(v->hint),
                     ShLangFor(path, m->hint));
        else {
            char confHint[128];
            if (ShConfigGetStr("MenuHints", m->title, NULL,
                               confHint, sizeof(confHint)) &&
                confHint[0])
                SafeCopy(v->hint, sizeof(v->hint),
                         ShLangFor(path, confHint));
        }

        SafeCopy(v->title, sizeof(v->title),
                 ShLangFor(parentPath, m->title));
        SafeCopy(v->status, sizeof(v->status),
                 ShLangFor(path, m->status));
        for (i = m->top; i < m->count && i < m->top + VISIBLE; i++) {
            ShMenuRow *r = &v->row[v->rows];
            SafeCopy(r->name, sizeof(r->name),
                     ShLangFor(path, m->items[i].label));
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

/* Enter the root menu. The highlighted row is remembered across
 * open/close: the selection already lives on the Menu between F4
 * toggles, so reopening simply leaves it where it was. Two guards
 * keep the state sane if rows changed while the menu was closed:
 *   - the selection is clamped to a live row (a row can only have
 *     disappeared while the menu was hidden);
 *   - ReorderRoot keeps the highlight pinned to its own row through
 *     the [MenuOrder] sort, and Scroll brings it back into view. */
static void OpenRoot(void) {
    g_current = g_root;
    Lock();
    {
        Menu *r = MenuOf(g_root);
        if (r) {
            if (r->count <= 0) {
                r->sel = 0;
                r->top = 0;
            } else {
                if (r->sel < 0 || r->sel >= r->count)
                    r->sel = r->count - 1;
            }
            ReorderRoot(r);
            Scroll(r);
        }
    }
    Unlock();
}

/* Keys are polled here; the overlay draws the result. */
static DWORD WINAPI MenuThread(LPVOID p) {
    (void)p;

    for (;;) {
        Sleep(TICK_MS);

        EscDeferTick();

        /* Background window: the menu must not react to keys.
         * Forget held keys too, so nothing fires on refocus. */
        if (!WindowFocused()) {
            if (g_capActive) CapClear();
            ResetKeys();
            continue;
        }

        /* F4 first, on every tick, before any menu work that
         * could block: the toggle must never depend on the
         * menu having rendered. */
        if (Pressed(g_key)) {
            g_open = !g_open;
            if (g_open) {
                /* The Chinese chat box must yield the keyboard
                 * (it captured it to type); the menu owns the
                 * capture while it is up. */
                ShChatClose();
                OpenRoot();
            }
        }

        if (!g_open) {
            if (g_capActive) CapClear();
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
        if (g_capActive) CapTickLocked();
        else             Navigate();
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

/* A key-bind row: shows the current key and, when the player presses
 * Enter on it, captures the next key press as the new value.  initial
 * is a VK code (VK_NONE = unset).  The callback fires with the new
 * VK.  The displayed key name is language-neutral.
 */
SH_API int ShMenuKeyBind(uint32_t menu, const char *label,
                         int initial, ShMenuFn fn, void *user) {
    Item *it;

    Lock();
    it = NewItem(MenuOf(menu), IT_KEYBIND, label, fn, user);
    if (it) it->value = initial;
    Unlock();
    return it != NULL;
}

/* Sync an existing row's displayed value without firing its
 * callback: a plugin changed the state behind the menu's back
 * (a hotkey flip), and the next capture must show it. Matches
 * on the row label inside that one menu, so identical labels
 * in different submenus do not collide. No-op on IT_NUMBER
 * rows, whose state only the slider owns. */
SH_API int ShMenuSetValue(uint32_t menu, const char *label,
                          int value) {
    Menu *m;
    int i;

    Lock();
    m = MenuOf(menu);
    if (!m || !label) { Unlock(); ShSetError(SH_ERR_BAD_ARG); return 0; }
    for (i = 0; i < m->count; i++) {
        Item *it = &m->items[i];
        if (strcmp(it->label, label) != 0) continue;
        if (it->kind == IT_TOGGLE) {
            it->value = value ? 1 : 0;
            Unlock();
            ShSetError(SH_OK);
            return 1;
        }
        if (it->kind == IT_LIST && it->nopts > 0) {
            it->value = value % it->nopts;
            Unlock();
            ShSetError(SH_OK);
            return 1;
        }
    }
    Unlock();
    ShSetError(SH_ERR_NO_CANDIDATE);
    return 0;
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

/* The status line from a printf template: translate the English
 * template in the menu's own scope, then format once and store the
 * final text. The capture path translates m->status again, which
 * is a no-op for the stored result, so this is safe.
 */
SH_API int ShMenuStatusF(uint32_t menu, const char *fmt, ...) {
    char path[64];
    char tmpl[96];
    char text[96];
    va_list ap;
    Menu *m;

    if (!fmt) return 0;

    Lock();
    m = MenuOf(menu);
    if (!m) { Unlock(); ShSetError(SH_ERR_BAD_ARG); return 0; }
    MenuPath(m, path, sizeof(path));
    SafeCopy(tmpl, sizeof(tmpl), ShLangFor(path, fmt));
    Unlock();

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), tmpl, ap);
    va_end(ap);
    return ShMenuStatus(menu, text);
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
    if (g_open) OpenRoot();
}

/* Entering Playing used to auto-open the menu once so the F4 key
 * was discoverable. That surprised players, so the menu only opens
 * on F4 now; the entry point stays for the state machine. */
void ShMenuOnEnterPlaying(void) {
}
