/* A shared menu, one root owned by the API. Every plugin
 * registers a submenu, so sixteen addons cost sixteen rows.
 *
 * The menu model (menus, items, navigation, callbacks) lives
 * here. Rendering is done by the D3D11 overlay in
 * scripthook_ovl.cpp: every frame it snapshots the current menu
 * through ShMenuCaptureView and draws it with Dear ImGui inside
 * the game's Present call, using the same look and interaction
 * as the original native-UI menu. The overlay declares itself
 * ready via ShMenuSetOverlayReady; until then the menu does not
 * open at all - neither the hotkey nor ShMenuOpen - which is
 * what keeps the keyboard, and every plugin that asks
 * ShMenuIsOpen(), out of a menu nobody can see. */
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "scripthook_tick.h"
#include "log.h"

/* One slot per menu, and a plugin that offers its settings per
 * kind of moment spends several: the first person plugin alone
 * takes ten (its own page, five categories, four presets). The
 * count has to cover every plugin's submenus at once, not just
 * the rows on the root.
 */
#define MENUS       64
#define ITEMS       96
/* Room for a whole row label. The longest one the framework builds itself
 * is a plugin switch - "<folder>(<page name>)" - where a folder is up to
 * 63 bytes and the page name is a title; 160 covers that with room to
 * spare and still keeps the menu model small (64 x 96 labels).
 * ShMenuRow.name and ShMenuView.title are the same string on the way to
 * the renderer and are sized to match. */
#define LABEL       160
#define VISIBLE     12
#define TICK_MS     40
/* Options a list row can carry. A list WRAPS at the ends - one step past
 * the last option is the first - which is what makes a clock row possible:
 * hours are 24 options and minutes 60, and the player cycles them with
 * left/right. That is why this is no longer 12: a row that needs 24 would
 * have been silently cut to 12 before. */
#define OPTS        64

/* A key that is held down repeats, and gets quicker while it is held:
 * a pause first so a single press stays a single step, then a slow
 * beat, then one step per tick. TICK_MS is 40, so the fastest repeat
 * is the poll rate itself.
 */
#define REPEAT_DELAY_MS 350u
#define REPEAT_SLOW_MS   80u
#define REPEAT_FAST_MS   40u

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
    char     owner[48];   /* owning plugin folder name; "" = built-in */
    char     hint[384];   /* a page's hint can be several long lines */
    char     status[192];
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
static volatile LONG g_started = 0;
static CRITICAL_SECTION g_lock;
static volatile int g_lockReady = 0;

extern void ShSetError(int err);

static void Lock(void) { if (g_lockReady) EnterCriticalSection(&g_lock); }
static void Unlock(void) { if (g_lockReady) LeaveCriticalSection(&g_lock); }

/* Copies display text back to a character boundary, so a cut string
 * never ends in half a multi-byte character - a renderer draws that as
 * "?". Defined with the capture helpers below. */
static void SafeCopy(char *dst, size_t cap, const char *src);

static Menu *MenuOf(uint32_t h) {
    if (h == 0 || h > MENUS) return NULL;
    if (!g_menus[h - 1].used) return NULL;
    return &g_menus[h - 1];
}

static uint32_t NewMenu(const char *title, uint32_t parent,
                        const char *owner) {
    int i;

    for (i = 0; i < MENUS; i++) {
        if (g_menus[i].used) continue;
        memset(&g_menus[i], 0, sizeof(g_menus[i]));
        g_menus[i].used = 1;
        g_menus[i].parent = parent;
        if (title) SafeCopy(g_menus[i].title, sizeof(g_menus[i].title),
                            title);
        if (owner) {
            strncpy(g_menus[i].owner, owner,
                    sizeof(g_menus[i].owner) - 1);
            g_menus[i].owner[sizeof(g_menus[i].owner) - 1] = 0;
        }
        return (uint32_t)(i + 1);
    }
    return 0;
}

#ifdef _MSC_VER
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define SH_CALLER_ADDR() _ReturnAddress()
#else
#define SH_CALLER_ADDR() __builtin_return_address(0)
#endif

/* Which plugin is calling?  The return address sits in the caller's
 * code, so FROM_ADDRESS names the .asi that created the menu; our
 * own dll and the exe mean built-in (owner "", main-ini fallback).
 * Runs once per ShMenuCreate, off the hot path. */
static void OwnerFromAddress(char *out, int cap) {
    void *ra = SH_CALLER_ADDR();
    HMODULE m = NULL;
    char path[MAX_PATH];
    const char *name;
    size_t n;

    out[0] = 0;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)ra, &m) || !m)
        return;
    path[0] = 0;
    GetModuleFileNameA(m, path, sizeof(path));
    name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    if (!_stricmp(name, "dinput8.dll") || !_stricmp(name, "GRW.exe"))
        return;                       /* built-in */
    n = strlen(name);
    if (n > 4 && !_stricmp(name + n - 4, ".asi")) n -= 4;
    else if (n > 4 && !_stricmp(name + n - 4, ".dll")) n -= 4;
    if (n >= (size_t)cap) n = (size_t)cap - 1;
    memcpy(out, name, n);
    out[n] = 0;
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
    if (label) SafeCopy(it->label, sizeof(it->label), label);
    return it;
}

/* ---- the rows the mode blacklist takes away ---------------------------
 * A plugin the current play mode has switched off is not drawn and cannot
 * be reached: not its row on the root, not the page behind it. One
 * predicate answers that for every place which walks rows, so the
 * capture, the navigation, the footer count and the Enter key cannot
 * disagree - a row that is invisible but still selectable is exactly the
 * bug this is asking for.
 *
 * Hidden means: an IT_SUB row whose child page belongs to a plugin
 * (owner[0]) that ShPluginHidden says is off. Built-in pages have an
 * empty owner and are never hidden.
 */
static int RowVisible(const Menu *m, int idx) {
    const Item *it;
    const Menu *cm;

    if (!m || idx < 0 || idx >= m->count) return 0;
    it = &m->items[idx];
    if (it->kind != IT_SUB || !it->sub) return 1;
    cm = MenuOf(it->sub);
    if (!cm || !cm->owner[0]) return 1;         /* a built-in page */
    return !ShPluginHidden(cm->owner);
}

static int VisibleCount(const Menu *m) {
    int i, n = 0;

    for (i = 0; i < m->count; i++)
        if (RowVisible(m, i)) n++;
    return n;
}

/* How many visible rows sit at or before idx: one based, for the footer. */
static int VisibleOrdinal(const Menu *m, int idx) {
    int i, n = 0;

    for (i = 0; i <= idx && i < m->count; i++)
        if (RowVisible(m, i)) n++;
    return n;
}

/* The row a step of dir lands on, skipping the hidden ones and wrapping
 * around; idx itself when it is the only visible row there is. */
static int NextVisible(const Menu *m, int idx, int dir) {
    int step, i;

    if (m->count < 1) return 0;
    for (step = 1; step <= m->count; step++) {
        i = ((idx + dir * step) % m->count + m->count) % m->count;
        if (RowVisible(m, i)) return i;
    }
    return idx;
}

/* Park the selection on a visible row: back up if it sits on a hidden one
 * (a menu that lost a row keeps its place), forward if there was nothing
 * behind it. */
static void FixSel(Menu *m) {
    int i;

    if (m->count < 1) { m->sel = 0; return; }
    if (RowVisible(m, m->sel)) return;
    for (i = m->sel - 1; i >= 0; i--)
        if (RowVisible(m, i)) { m->sel = i; return; }
    for (i = m->sel + 1; i < m->count; i++)
        if (RowVisible(m, i)) { m->sel = i; return; }
    m->sel = 0;
}

/* A page whose own plugin is switched off cannot be shown, so the menu
 * steps back out of it - the same place ESC would go. */
static void BackOutOfHiddenPage(const Menu *m) {
    if (!m || m->parent == 0) return;
    if (!m->owner[0] || !ShPluginHidden(m->owner)) return;
    g_current = m->parent;
}

/* ---- key-bind rows ------------------------------------------------
 * A row that stores a virtual key (Item.value = VK code).  Pressing
 * Enter on it arms a capture: the menu thread then ignores normal
 * navigation and waits for the player to press one key (Esc cancels).
 * The captured VK is stored back and the row's callback fires, so a
 * "change this hotkey" row is a single key press away.
 * ------------------------------------------------------------------ */
/* Rendered text for the value side of a row. Fixed words ("on"/"off",
 * a list option) go through the text lookup with the row's owner;
 * number and arrow formats are language-neutral. */
static void ValueText(const char *owner, const Item *it, char *out, int n) {
    out[0] = 0;
    if (it->kind == IT_SUB) snprintf(out, n, ">");
    else if (it->kind == IT_TOGGLE)
        snprintf(out, n, "[%s]", it->value
                                      ? ShLangText(owner, "@menu.on")
                                      : ShLangText(owner, "@menu.off"));
    else if (it->kind == IT_NUMBER)
        /* Integer step with a whole current value renders as an
         * integer (< 30 >); fractional steps keep two decimals. */
        if (it->step >= 1.0f && it->num == (float)(int)it->num)
            snprintf(out, n, "< %.0f >", it->num);
        else
            snprintf(out, n, "< %.2f >", it->num);
    else if (it->kind == IT_LIST && it->nopts)
        snprintf(out, n, "< %s >",
                 ShLangText(owner,
                            it->opts[((it->value % it->nopts) +
                                      it->nopts) % it->nopts]));
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
/* Signalled by every push; see CallPush and CallThread. Auto-reset, made
 * once, under the lock that makes the thread. Volatile because the consumer
 * reads it from its own loop: the thread is created and the event is made in
 * the same critical section, so the consumer can get there first - and it
 * must see the handle the moment it appears rather than keep a cached NULL
 * and sleep out the rest of the session. */
static volatile HANDLE g_callEvent = NULL;

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
    } else {
        /* The row's value already flipped in the model, so a drop
         * desyncs plugin state from the UI - leave a trace. */
        Log("menu: callback queue full, dropped menu=%u item=%u",
            (unsigned)menu, (unsigned)item);
    }
    /* Created under the lock: two threads pushing at the same instant both
     * saw g_callThread==NULL and each started a permanent consumer. */
    if (!g_callThread) {
        g_callThread = CreateThread(NULL, 0, CallThread, NULL, 0, NULL);
        /* Only ever read as "the consumer exists"; nothing waits on it, so
         * the thread object is released at once. */
        if (g_callThread) CloseHandle(g_callThread);
    }
    /* Made under the same lock as the thread, for the same reason: two
     * pushers at the same instant must not each make one. A signalled
     * auto-reset event stays signalled until somebody waits on it, so a push
     * that lands between the consumer's look at the queue and its wait still
     * wakes it - nothing is lost by waiting instead of polling. */
    if (!g_callEvent) g_callEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (g_callEvent) SetEvent(g_callEvent);
    Unlock();
}

static DWORD WINAPI CallThread(LPVOID p) {
    (void)p;
    for (;;) {
        Call c;
        int has = 0;

        ShTickPing(SH_TICK_MENUCALL);
        Lock();
        if (g_callHead != g_callTail) {
            c = g_call[g_callTail];
            g_callTail = (g_callTail + 1) % CALL_MAX;
            has = 1;
        }
        Unlock();
        if (!has) {
            /* Nothing to run, and this is where the idle cost went: it used
             * to be Sleep(5), which is the lock taken 200 times a second to
             * find the queue empty. The timeout is not a poll - it is what
             * keeps the ping above at its own cadence (10ms, the period the
             * tick table expects of this thread), and a wait with a timeout
             * costs no lock and no work. */
            if (g_callEvent) WaitForSingleObject(g_callEvent, 10);
            else Sleep(5);                  /* the event would not be made */
            continue;
        }
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

/* Read only. Pressed() above rewrites g_keyWas, so asking it twice in
 * one tick loses the answer, and a key being held has to be asked
 * about on every tick. This one only reports whether it is down.
 */
static int KeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

/* A direction that is being held. Navigation and the value edit are
 * tracked apart: one can still be held while the other has just been
 * let go, and they must not reset each other.
 */
typedef struct {
    int   active;   /* the key is down                        */
    int   dir;      /* -1 or +1                               */
    DWORD next;     /* GetTickCount() it may act again after   */
    int   n;        /* repeats so far, drives the speed up     */
} Hold;

static Hold g_holdNav;
static Hold g_holdVal;

static void HoldReset(void) {
    g_holdNav.active = 0;
    g_holdNav.n = 0;
    g_holdVal.active = 0;
    g_holdVal.n = 0;
}

/* Whether a held direction may act this tick: the press itself, then
 * a pause, then a slow beat, then one step per tick. Returns 1 to act,
 * 2 on the tick the key was let go, 0 otherwise.
 */
static int HoldTick(Hold *h, int down, int dir) {
    DWORD now = GetTickCount();

    if (!down) {
        int was = h->active;
        h->active = 0;
        h->n = 0;
        return was ? 2 : 0;
    }
    if (!h->active || h->dir != dir) {
        h->active = 1;
        h->dir = dir;
        h->n = 0;
        h->next = now + REPEAT_DELAY_MS;
        return 1;
    }
    if ((int)(now - h->next) < 0) return 0;
    h->n++;
    h->next = now + (h->n < 4 ? REPEAT_SLOW_MS : REPEAT_FAST_MS);
    return 1;
}

/* The menu keys are polled, so a background game window must not
 * react to keys meant for the window in front. Forget held keys
 * while unfocused so nothing fires when focus comes back. */
static void ResetKeys(void) {
    memset(g_keyWas, 0, sizeof(g_keyWas));
    HoldReset();
}

/* Selection scrolls with the cursor, so a long menu shows a
 * window of rows rather than running off the screen. The window is
 * counted in VISIBLE rows and not in raw ones, so hidden rows take no
 * room on screen and its first row is always one the player can see.
 */
static void Scroll(Menu *m) {
    int i, back = 0;

    FixSel(m);
    m->top = m->sel;
    for (i = m->sel - 1; i >= 0 && back < VISIBLE - 1; i--) {
        if (!RowVisible(m, i)) continue;
        m->top = i;
        back++;
    }
    if (m->top < 0) m->top = 0;
}

/* Defined below with the rest of the back-key mode, and named here because
 * leaving a page comes first in Navigate: which key does it is the mode's
 * answer, not a fixed pair. */
static int LeaveKey(int vk);

static void Navigate(void) {
    Menu *m = MenuOf(g_current);
    Item *it;
    int upE, dnE, lfE, rtE, backE, delE;
    int upHeld, dnHeld, lfHeld, rtHeld;
    int navDown, navDir, valDown, valDir, r;

    if (!m) return;
    /* The page itself can be one the mode has taken away: step out of it
     * before a key lands anywhere. */
    BackOutOfHiddenPage(m);
    m = MenuOf(g_current);
    if (!m) return;

    /* Leaving comes first, and it works on a page with nothing on it: the
     * plugin list with plugins\ empty, or a page whose every row the mode has
     * taken away. This used to sit below a "nothing to navigate" return, so on
     * such a page Back and ESC did nothing at all and only F4 - which is not
     * part of navigation - closed the menu. Reported 2026-09-23 against the
     * Plugin switches page with no plugins installed.
     *
     * Which of the two keys it is is the [Settings] backkey mode, and only the
     * key that mode names counts: the other is inert in the menu while it is
     * up (it stays hidden from the game, it just does nothing here). Both are
     * polled on every capture whatever the mode says they are for - Pressed()
     * carries the edge state, and a key only asked about in one mode would
     * bring a stale "was down" into the other, the same reason the arrows below
     * are polled one by one. Reported 2026-09-26: this test named both keys, so
     * the mode changed what the game could not see and nothing else. */
    backE = Pressed(VK_ESCAPE);
    delE  = Pressed(VK_BACK);
    if ((backE && LeaveKey(VK_ESCAPE)) || (delE && LeaveKey(VK_BACK))) {
        HoldReset();
        if (m->parent) g_current = m->parent;
        else g_open = 0;
        return;
    }

    if (m->count == 0) return;
    /* A page whose rows are all hidden has nothing to move to either. */
    if (VisibleCount(m) == 0) return;
    /* The arrows navigate and nothing else does. WASD used to navigate as
     * well, which is why the menu had to hide the whole keyboard while it
     * was open; giving it back is what lets the player keep playing. Each
     * arrow is polled on its own so none of them is left with a stale idea
     * of whether it is down.
     */
    upE = Pressed(VK_UP);
    dnE = Pressed(VK_DOWN);
    lfE = Pressed(VK_LEFT);
    rtE = Pressed(VK_RIGHT);

    upHeld = upE || KeyDown(VK_UP);
    dnHeld = dnE || KeyDown(VK_DOWN);
    lfHeld = lfE || KeyDown(VK_LEFT);
    rtHeld = rtE || KeyDown(VK_RIGHT);

    navDown = upHeld || dnHeld;
    navDir  = dnHeld ? 1 : -1;
    valDown = lfHeld || rtHeld;
    valDir  = rtHeld ? 1 : -1;

    if (HoldTick(&g_holdNav, navDown, navDir) == 1)
        m->sel = NextVisible(m, m->sel, navDir);
    Scroll(m);

    it = &m->items[m->sel];
    r = HoldTick(&g_holdVal, valDown, valDir);
    if (r == 1) {
        if (it->kind == IT_NUMBER) {
            float was = it->num;
            it->num += it->step * valDir;
            if (it->num < it->lo) it->num = it->lo;
            if (it->num > it->hi) it->num = it->hi;
            /* Nothing moved: sitting against a limit is no reason to
             * keep pushing callbacks into a queue of thirty two. */
            if (it->num != was) Fire(g_current, m->sel, it);
        } else if (it->kind == IT_LIST && it->nopts) {
            int was = it->value;
            it->value = (it->value + it->nopts + valDir) % it->nopts;
            if (it->value != was) Fire(g_current, m->sel, it);
        }
    } else if (r == 2) {
        /* Let go: what is on screen is what the plugin has to hold. A
         * repeat that was dropped because the callback queue was full
         * would otherwise leave the two disagreeing. */
        if (it->kind == IT_NUMBER ||
            (it->kind == IT_LIST && it->nopts))
            Fire(g_current, m->sel, it);
    }
    if (Pressed(VK_RETURN)) {
        if (it->kind == IT_SUB && it->sub && RowVisible(m, m->sel)) {
            g_current = it->sub;
            HoldReset();
        } else if (it->kind == IT_TOGGLE) {
            it->value = !it->value;
            Fire(g_current, m->sel, it);
        } else {
            Fire(g_current, m->sel, it);
        }
    }
}

/* The menu's own keys: the only ones it acts on, and the only ones it hides
 * from the game while it is up. Everything else stays the player's - WASD,
 * the mouse and the rest keep driving the character, which is what makes it
 * possible to tune a row and watch what it does.
 *
 * ESC and Backspace are the two that mean "leave" (back a level, or out), and
 * they are in the list for the same reason the rest are: a game that also saw
 * them would act on the same press.
 *
 * One key at a time, never the whole keyboard. The capture that hid every
 * key (ShCaptureKeys) froze the character for as long as the menu was open,
 * and it was one flag shared with whichever plugin last took the keyboard -
 * a chat box closing could take the menu's keys back with it. These are this
 * module's own flags, so nothing outside can clear them.
 */
static const int g_menuKeys[] = {
    VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_ESCAPE, VK_BACK
};
#define MENU_KEYS (int)(sizeof(g_menuKeys) / sizeof(g_menuKeys[0]))

/* Which keys leave a menu: 0 both (what the menu has always done), 1 Esc,
 * 2 Backspace. It is one value because three decisions rest on it and they have
 * to agree: the key a press leaves a page on (Navigate), the sentence the root's
 * hint shows, and which of the two the menu takes from the game while it is up.
 * The key the mode leaves out is the player's and is left alone - Esc under
 * "Backspace" is the game's own pause menu, and a menu that swallowed it anyway
 * would defeat the setting. Set from the settings page (ShMenuSetBackKeys) and
 * read from scripthook.ini when the menu first comes up, so a restart keeps it. */
static volatile LONG g_backKeys = 0;

/* Internal, not SH_API: the only caller is the framework's own settings page,
 * and a plugin has nothing to say about which keys the menu leaves on. Keeping
 * it out of the export table is what keeps the plugin API at version 2. */
void ShMenuSetBackKeys(int mode) {
    if (mode < 0 || mode > 2) mode = 0;
    InterlockedExchange(&g_backKeys, mode);
}

static int BackKeys(void) {
    return (int)InterlockedCompareExchange(&g_backKeys, 0, 0);
}

/* How the root's hint names them. Key names are not translated - they are what
 * is printed on the keyboard. */
static const char *BackKeysText(void) {
    int m = BackKeys();

    if (m == 1) return "Esc";
    if (m == 2) return "Backspace";
    return "Esc / Backspace";
}

/* Which of the two keys leaves a page under mode m. */
static int LeavesUnder(int m, int vk) {
    if (m == 1) return vk == VK_ESCAPE;
    if (m == 2) return vk == VK_BACK;
    return vk == VK_ESCAPE || vk == VK_BACK;
}

/* The same under the mode in force now: this is what Navigate asks before it
 * lets a press leave a page. */
static int LeaveKey(int vk) {
    return LeavesUnder(BackKeys(), vk);
}

/* The two keys that can leave a menu, whatever the mode says they are for. */
static int IsBackKey(int vk) {
    return vk == VK_ESCAPE || vk == VK_BACK;
}

/* Whether the menu takes this key from the game under mode m: every navigation
 * key, and of the two back keys only the one that leaves - the other stays with
 * the game. m is -1 when no back key is being held. */
static int HiddenUnder(int m, int vk) {
    if (m < 0) return 0;
    if (!IsBackKey(vk)) return 1;
    return LeavesUnder(m, vk);
}

/* Whether a key the menu took is still down: the press that closed the menu is
 * in flight, and handing it over now would make the game act on the way out.
 * Only the key that was taken is asked about - the other one was with the game
 * all along, so there is nothing to hand back. */
static int BackHeldUnder(int m) {
    if (m < 0) return 0;
    if (LeavesUnder(m, VK_ESCAPE) &&
        (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) return 1;
    if (LeavesUnder(m, VK_BACK) &&
        (GetAsyncKeyState(VK_BACK) & 0x8000) != 0) return 1;
    return 0;
}

/* Keys are hidden only while the menu is actually drawn. A menu that is
 * wanted but cannot render yet (overlay not up, game state not PLAYING, a
 * world reload) must hide nothing at all: that is exactly the bug where the
 * character freezes with no menu on screen.
 */
static volatile int g_captureNow = 0;
static volatile int g_leaveDefer = 0;
static DWORD g_leaveDeferAt = 0;
static int g_hotkeyHidden = 0;      /* the menu hotkey currently hidden, or 0 */
/* The mode the back key was hidden under, or -1 when none is held. What the
 * menu hands back is what it took, under the rules it took it by: the settings
 * page changes the mode while the menu is up, and ShBlockKey is one shared flag
 * per key with no owner - an unblock aimed at the wrong key would either clear
 * a plugin's own block or leave this module's behind. */
static int g_hiddenBackMode = -1;

/* Hand back the back key the menu took, and only that one. */
static void BackHandBack(void) {
    if (HiddenUnder(g_hiddenBackMode, VK_ESCAPE)) ShBlockKey(VK_ESCAPE, 0);
    if (HiddenUnder(g_hiddenBackMode, VK_BACK))   ShBlockKey(VK_BACK, 0);
    g_hiddenBackMode = -1;
}

/* Not SetCapture: that is a Win32 API and the name collides. */
static void MenuCapture(int on) {
    int i;

    if (on == g_captureNow) return;
    g_captureNow = on;

    if (on) {
        g_hiddenBackMode = BackKeys();
        for (i = 0; i < MENU_KEYS; i++)
            if (HiddenUnder(g_hiddenBackMode, g_menuKeys[i]))
                ShBlockKey(g_menuKeys[i], 1);
        g_hotkeyHidden = (int)g_key;
        if (g_hotkeyHidden) ShBlockKey(g_hotkeyHidden, 1);
        g_leaveDefer = 0;
        return;
    }

    /* Closing: the navigation keys go back at once. The back key the menu took
     * waits if it is still down - the press that closed the menu is in flight,
     * and handing it over would make the game act on the way out (its own pause
     * menu, on the way to something else). */
    for (i = 0; i < MENU_KEYS; i++)
        if (!IsBackKey(g_menuKeys[i])) ShBlockKey(g_menuKeys[i], 0);
    if (g_hotkeyHidden) {
        ShBlockKey(g_hotkeyHidden, 0);
        g_hotkeyHidden = 0;
    }
    if (BackHeldUnder(g_hiddenBackMode)) {
        g_leaveDefer = 1;
        g_leaveDeferAt = GetTickCount();
        return;
    }
    BackHandBack();
}

/* The mode can change while the menu is up - the settings page's row is inside
 * the menu, so that is the only way it is ever changed. The change has to reach
 * the keys at once: the key the new mode leaves out goes back to the game now,
 * and the one it takes over is claimed now. The alternative - doing it in the
 * setter - would have the settings page reach into capture state from another
 * module; here it is two integer comparisons per tick and no config read, since
 * g_backKeys already holds what the page wrote. */
static void SyncBackKeys(void) {
    int m = BackKeys();
    int i;

    if (!g_captureNow || g_hiddenBackMode < 0 || m == g_hiddenBackMode) return;
    for (i = 0; i < MENU_KEYS; i++) {
        int vk = g_menuKeys[i];
        int was, now;

        if (!IsBackKey(vk)) continue;
        was = HiddenUnder(g_hiddenBackMode, vk);
        now = HiddenUnder(m, vk);
        if (was && !now) ShBlockKey(vk, 0);
        if (!was && now) ShBlockKey(vk, 1);
    }
    g_hiddenBackMode = m;
}

/* A deferred leave ends once the key is up, or after a second so a stuck key
 * cannot swallow it forever. */
static void LeaveDeferTick(void) {
    DWORD now;

    if (!g_leaveDefer) return;
    now = GetTickCount();
    if (!BackHeldUnder(g_hiddenBackMode) ||
        (int)(now - g_leaveDeferAt) > 1000) {
        g_leaveDefer = 0;
        BackHandBack();
    }
}

/* The overlay (scripthook_ovl.cpp) renders on the game render
 * thread. Until it is up, the menu is wanted-but-not-drawable
 * and must neither capture the keyboard nor claim visibility. */
static volatile int g_ovlReady = 0;

void ShMenuSetOverlayReady(int ready) {
    g_ovlReady = ready ? 1 : 0;
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
        /* The source may already have been cut somewhere else: the room
         * was there, the character was not. */
        ShUtf8Trim(dst);
    }
}

/* The settings page's order rows write [MenuOrder] while the menu is
 * up, so the shortcut in ReorderRoot cannot trust an unchanged row
 * count. Bumped by ShMenuOrderDirty, read by ReorderRoot. */
static volatile LONG g_orderGen = 0;

void ShMenuOrderDirty(void) {
    InterlockedIncrement(&g_orderGen);
}

/* The [MenuOrder] weights reorder the ROOT menu's rows in the
 * model itself, so navigation (which walks m->items) and the
 * visible order always agree. The cursor follows its row across
 * the sort. Unlisted rows use the default weight and keep their
 * relative order (stable sort). A quick ordered check skips the
 * sort once the rows are already in weight order - or once nothing
 * has changed, which now includes the weights themselves. */
static void ReorderRoot(Menu *m) {
    int i, j, selPos = -1;
    char selLabel[LABEL];
    LONG gen = g_orderGen;
    /* ShConfigGetInt is a table scan; without this guard it ran on
     * the menu thread's every capture (~25/s) for an order that only
     * changes when the root's row set changes. */
    static uint32_t lastMenu = 0;
    static int lastCount = -1;
    static LONG lastGen = 0;

    if (m->parent != 0 || m->count < 2) return;
    if (lastMenu == (uint32_t)(m - g_menus) + 1 &&
        lastCount == m->count && lastGen == gen)
        return;
    lastMenu = (uint32_t)(m - g_menus) + 1;
    lastCount = m->count;
    lastGen = gen;

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

/* The root's reorderable rows, as the settings page needs them: every
 * page the root holds, in the order it draws them. A built-in page is
 * ordered like any other - the settings page included, which is only
 * first because its weight defaults to 0 - and a page the mode has taken
 * away is not in the root as the player sees it, so it is not listed and
 * its weight is left alone for the day it comes back. */
int ShMenuRootOrderRows(ShMenuOrderRow *out, int cap) {
    Menu *r;
    int i, n = 0;

    Lock();
    r = MenuOf(g_root);
    if (!r) { Unlock(); return 0; }
    for (i = 0; i < r->count; i++) {
        const Item *it = &r->items[i];
        Menu *cm;

        if (it->kind != IT_SUB || !it->sub) continue;
        cm = MenuOf(it->sub);
        if (!cm) continue;
        if (!RowVisible(r, i)) continue;            /* not in the root now */
        if (out && n < cap) {
            SafeCopy(out[n].key, sizeof(out[n].key), it->label);
            SafeCopy(out[n].owner, sizeof(out[n].owner), cm->owner);
        }
        n++;
    }
    Unlock();
    return n;
}

/* Put the cursor on a named row of one menu and scroll it into view.
 * A page that rebuilds its own rows - the ordering page does, on every
 * move - needs to put the highlight back where the player left it. */
int ShMenuSelectRow(uint32_t menu, const char *key) {
    Menu *m;
    int i;

    if (!key || !key[0]) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    Lock();
    m = MenuOf(menu);
    if (m) {
        for (i = 0; i < m->count; i++) {
            if (strcmp(m->items[i].label, key)) continue;
            m->sel = i;
            Scroll(m);
            Unlock();
            ShSetError(SH_OK);
            return 1;
        }
    }
    Unlock();
    ShSetError(SH_ERR_BAD_ARG);
    return 0;
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
    /* A page the mode has taken away is not drawn: step back out first,
     * so the overlay never shows a plugin that is supposed to be gone. */
    if (m) {
        BackOutOfHiddenPage(m);
        m = MenuOf(g_current);
    }
    if (m) {
        const char *owner = m->owner;

        v->isRoot = (m->parent == 0);

        /* Keep the root's plugin rows ordered by [MenuOrder] in
         * the model, so navigation matches what is on screen. */
        if (m->parent == 0) ReorderRoot(m);
        /* The sort, or a row going away, can leave the selection parked on
         * a row that is no longer drawn. */
        Scroll(m);

        /* The root shows the control hints; every submenu shows the
         * plugin's own hint, or the one written for its page key
         * ("<key>.hint") - which is how a plugin with no source gets a
         * hint at all. A hint nobody wrote stays empty and takes no
         * room, so this asks whether there is text before showing it. */
        if (m->parent == 0) {
            /* The root's hint names the keys that leave a menu, and which those
             * are is a setting: it is built from the same value the menu itself
             * presses on, so the line can never promise a key that does
             * nothing. ShTextFormat checks a translation's own conversions
             * against the en-US template, the way the footer below is built. */
            const char *en = ShTextEnUS(NULL, "@menu.root.hint");
            const char *tr = ShLangText(NULL, "@menu.root.hint");

            if (!en) en = "%s back";
            ShTextFormat(v->hint, sizeof(v->hint), en, tr ? tr : en,
                         BackKeysText());
        }
        else if (m->hint[0])
            SafeCopy(v->hint, sizeof(v->hint),
                     ShLangText(owner, m->hint));
        else {
            char hintKey[LABEL + 16];   /* "<页面键>.hint" */

            snprintf(hintKey, sizeof(hintKey), "%s.hint", m->title);
            if (ShLangHas(owner, hintKey))
                SafeCopy(v->hint, sizeof(v->hint),
                         ShLangText(owner, hintKey));
        }

        SafeCopy(v->title, sizeof(v->title),
                 ShLangText(owner, m->title));
        SafeCopy(v->status, sizeof(v->status),
                 ShLangText(owner, m->status));
        for (i = m->top; i < m->count && v->rows < VISIBLE; i++) {
            ShMenuRow *r = &v->row[v->rows];
            const Item *it = &m->items[i];
            /* A submenu row shows the child menu's title, so it is
             * translated with the CHILD's owner: the root is built
             * in, but its plugin rows must read that plugin's own
             * lang.ini first. */
            const char *rowOwner = owner;
            /* A row the mode has switched off takes no room: the window
             * is filled with rows the player can actually see, and it is
             * asked here so nothing below it has to know. */
            if (!RowVisible(m, i)) continue;
            if (it->kind == IT_SUB) {
                Menu *cm = MenuOf(it->sub);
                if (cm && cm->owner[0]) rowOwner = cm->owner;
            }
            SafeCopy(r->name, sizeof(r->name),
                     ShLangText(rowOwner, it->label));
            ValueText(owner, it, r->value, sizeof(r->value));
            r->selected = (i == m->sel);
            if (r->selected) v->sel = v->rows;
            v->rows++;
        }
        {
            int vis = VisibleCount(m);
            if (vis > VISIBLE) {
                const char *fen = ShTextEnUS(NULL, "@menu.footer.pos");

                ShTextFormat(v->footer, sizeof(v->footer),
                             fen ? fen : "%d / %d",
                             ShLangText(NULL, "@menu.footer.pos"),
                             VisibleOrdinal(m, m->sel), vis);
            }
        }
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
    char ev[192];

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

    /* One line per open: this is the moment a player is looking at the
     * framework's answer, and the mode it has is what every mode-dependent
     * decision - the plugin blacklist above all - rests on. A report of
     * "the plugins were not blocked in PvP" is only actionable with both
     * facts, because they separate "no mode was ever read" from "a mode
     * was read and nothing followed it". */
    ShPlayModeEvidence(ev, sizeof(ev));
    Log("menu open: mode=%d mask=%02X - %s", ShSelectedPlayMode(),
        (unsigned)ShPlayModeBit(ShSelectedPlayMode()), ev);
}

/* Keys are polled here; the overlay draws the result. */
static DWORD WINAPI MenuThread(LPVOID p) {
    (void)p;

    for (;;) {
        Sleep(TICK_MS);
        ShTickPing(SH_TICK_MENU);

        LeaveDeferTick();
        SyncBackKeys();

        /* Background window: the menu must not react to keys.
         * Forget held keys too, so nothing fires on refocus. */
        if (!ShGameFocused()) {
            ResetKeys();
            continue;
        }

        /* F4 first, on every tick, before any menu work that
         * could block: the toggle must never depend on the
         * menu having rendered. */
        if (Pressed(g_key)) {
            /* The overlay is what draws the menu, and it only comes up on the
             * game's first Present. Before that there is nothing to draw, and
             * opening the model anyway would tell every plugin that asks
             * ShMenuIsOpen() that a menu is on screen while the screen shows
             * none of it - their own hotkeys go dead and the player has no way
             * to see why. So the press is dropped. Once the overlay is up,
             * which is a second or two into the loading screen, the next press
             * works; one line records the dropped one. */
            if (!g_ovlReady) {
                static volatile LONG said;

                if (InterlockedExchange(&said, 1) == 0)
                    Log("menu: hotkey pressed before the overlay is up - "
                        "ignored, there is nothing to draw with yet");
            } else {
                g_open = !g_open;
                HoldReset();   /* opened or closed: nothing is held now */
                if (g_open) {
                    /* A plugin's text box (the Chinese chat box is one)
                     * yields the keyboard by itself: it polls
                     * ShMenuIsOpen() and closes, which releases the
                     * capture - the menu only has to take the keys from
                     * here, not hand them over. */
                    OpenRoot();
                }
            }
        }

        if (!g_open) {
            MenuCapture(0);
            continue;
        }

        /* The net under the guard above: nothing opens the menu before the
         * overlay is up, so this is here for an overlay that goes away again
         * (a device loss would call ShMenuSetOverlayReady(0)). A menu that is
         * wanted but has nothing to render it must hide nothing at all. */
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
    for (;;) {
        LONG s = InterlockedCompareExchange(&g_started, 0, 0);
        if (s == 1) return;
        if (s == 2) { Sleep(0); continue; }
        if (InterlockedCompareExchange(&g_started, 2, 0)) continue;
        LogInit("scripthook_menu.log");
        InitializeCriticalSection(&g_lock);
        g_lockReady = 1;              /* lock live before the thread */
        /* The back keys, before anything can draw a hint or press one: the page
         * that changes them needs a value to start from, and the hint is built
         * the first time the menu is drawn. */
        ShMenuSetBackKeys((int)ShConfigGetInt("Settings", "backkey", 0));
        g_root = NewMenu("SCRIPTHOOK", 0, NULL);
        {
            HANDLE h = CreateThread(NULL, 0, MenuThread, NULL, 0, NULL);

            if (h) CloseHandle(h);   /* never waited on */
        }
        InterlockedExchange(&g_started, 1);
        return;
    }
}

SH_API uint32_t ShMenuCreate(const char *title) {
    Menu *root;
    uint32_t h;
    Item *it;
    char owner[48];

    OwnerFromAddress(owner, sizeof(owner));
    EnsureMenu();
    Lock();
    h = NewMenu(title, g_root, owner);
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
    m = MenuOf(parent);
    /* The submenu belongs to whoever owns the parent, so a
     * built-in page added under a plugin menu still translates
     * from that plugin's ini. */
    h = NewMenu(label, parent, m ? m->owner : NULL);
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
        /* C % keeps the sign of the dividend: a negative initial from
         * an ini read would index opts[-1] at the next capture. */
        it->value = (n > 0) ? ((initial % n) + n) % n : 0;
    }
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
            /* Same sign rule as ShMenuList: normalize negatives. */
            it->value = ((value % it->nopts) + it->nopts) % it->nopts;
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
        SafeCopy(m->status, sizeof(m->status), text);
    } else {
        m->status[0] = 0;
    }
    Unlock();
    return 1;
}

/* Drop every stored line: they are text in the language that was
 * active when they were written. Called by the text layer on a
 * language switch; a page that keeps its line current writes it again
 * on its next tick. */
void ShMenuStatusResetAll(void) {
    int i;

    EnsureMenu();
    Lock();
    for (i = 0; i < MENUS; i++)
        if (g_menus[i].used) g_menus[i].status[0] = 0;
    Unlock();
}

/* The status line from a printf template: take the template's text for
 * this menu's owner, format it once, store the result. The capture
 * path translates m->status again, which is a no-op for the stored
 * result, so this is safe.
 *
 * ShTextFormatV does the formatting, which is what lets a translation
 * reorder the values ("%2$s" first) and what keeps a mistyped
 * conversion from reaching vsnprintf at all - the formatter in
 * scripthook_config.c checks it against `en` and falls back to it.
 */
SH_API int ShMenuStatusF(uint32_t menu, const char *fmt, ...) {
    char tmpl[512];              /* the template, in this menu's text */
    char text[384];              /* the line that template formats into */
    const char *en;              /* the same template in en-US */
    va_list ap;
    Menu *m;

    if (!fmt) return 0;

    Lock();
    m = MenuOf(menu);
    if (!m) { Unlock(); ShSetError(SH_ERR_BAD_ARG); return 0; }
    /* A caller that passes an ID has its en-US row; one that passes the
     * English literal has no row, and the literal is the answer. */
    en = ShTextEnUS(m->owner, fmt);
    if (!en) en = fmt;
    SafeCopy(tmpl, sizeof(tmpl), ShLangText(m->owner, fmt));
    Unlock();

    va_start(ap, fmt);
    ShTextFormatV(text, sizeof(text), en, tmpl, ap);
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
        SafeCopy(m->hint, sizeof(m->hint), text);
    } else {
        m->hint[0] = 0;
    }
    Unlock();
    return 1;
}

SH_API void ShMenuSetKey(int vk) { g_key = vk; }
SH_API int  ShMenuIsOpen(void) { return g_open; }

/* Is that menu the page the player is looking at?  Exactly that page:
 * the capture path draws the current page's status line and nobody
 * else's (ShMenuCaptureView), so while the player is inside a submenu
 * the parent page is NOT showing even though the player came through
 * it.  Asking about the parent there is 0; asking about the submenu is
 * what a line inside that submenu passes.
 *
 * The menu also has to be up: with it closed nothing is on screen,
 * whatever page g_current still holds from last time.  A 0 for a menu
 * that exists and is simply not showing is an answer rather than a
 * failure; the error is set only when the id names no menu. */
SH_API int ShMenuIsShowing(uint32_t menu) {
    int hit;

    if (!menu) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    Lock();
    if (!MenuOf(menu)) {
        Unlock();
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    hit = (g_open && g_current == menu) ? 1 : 0;
    Unlock();
    ShSetError(SH_OK);   /* "not showing" is an answer, not an error */
    return hit;
}

SH_API void ShMenuOpen(int open) {
    EnsureMenu();
    /* Same rule as the hotkey: a menu that nothing can draw yet is worse than
     * no menu, because the plugins that ask ShMenuIsOpen() act on it. Closing
     * is always allowed - only opening has to wait for the overlay. */
    if (open && !g_ovlReady) return;
    g_open = open ? 1 : 0;
    if (g_open) OpenRoot();
}

/* Entering Playing used to auto-open the menu once so the F4 key
 * was discoverable. That surprised players, so the menu only opens
 * on F4 now; the entry point stays for the state machine. */
void ShMenuOnEnterPlaying(void) {
}
