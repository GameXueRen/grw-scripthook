/* HUD text slots, one plate per slot, drawn by the engine
 * through the native UI. Slots pack per corner from whoever
 * registered, so an absent plugin leaves no gap. */
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "log.h"

#define HUD_SLOTS   32
#define HUD_TEXT    512
#define HUD_NAME    32
#define HUD_LINES   12
#define HUD_LINE    96
#define HUD_TICK_MS 120

/* Geometry in HUD pixels. */
#define LINE_H      26.0f
#define PAD         10.0f
#define MARGIN      24.0f
#define CHAR_W      9.5f
#define MIN_W       120.0f
#define MAX_W       720.0f
#define GAP         8.0f
/* How far below the top edge the centred column starts: clear of
 * the menu, which owns the top left corner. */
#define TOAST_OFF_Y 56.0f

/* Toasts are few. Every slot costs a panel and its labels against
 * the engine's finite supply of widgets. */
#define TOAST_SLOTS 4

typedef struct {
    int      used;
    int      visible;
    int      anchor;
    int      priority;
    uint32_t colour;
    char     name[HUD_NAME];
    char     text[HUD_TEXT];
    uint64_t until;   /* 0 stays; else the tick count it leaves at */
    uint64_t ms;      /* the time it asked for, to start it again */
} HudSlot;

/* What the engine currently shows for a slot. */
typedef struct {
    int      gen;
    uint32_t panel;
    uint32_t line[HUD_LINES];
    char     shown[HUD_LINES][HUD_LINE];
    int      lines;
    uint32_t colour;
    int      visible;
    float    x, y, w, h;
} HudView;

static HudSlot g_slots[HUD_SLOTS];
static HudView g_view[HUD_SLOTS];
static CRITICAL_SECTION g_lock;
static volatile int g_lockReady = 0;
static volatile LONG g_started = 0;
static volatile int g_rev = 0;

extern void ShSetError(int err);

static void HudLock(void) {
    if (g_lockReady) EnterCriticalSection(&g_lock);
}

static void HudUnlock(void) {
    if (g_lockReady) LeaveCriticalSection(&g_lock);
}

static void HudChanged(void) {
    g_rev++;
}

/* Slots for one corner, priority first then registration,
 * so layout holds whichever plugin loaded first.
 */
static int Gather(int anchor, int *out, int max) {
    int n = 0, pass, i;

    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < HUD_SLOTS && n < max; i++) {
            HudSlot *s = &g_slots[i];
            if (!s->used || !s->visible || !s->text[0]) continue;
            if (s->anchor != anchor) continue;
            if (pass == 0 && s->priority >= 0) continue;
            if (pass == 1 && s->priority < 0) continue;
            out[n++] = i;
        }
    }
    return n;
}

/* Split text into lines, measure the widest one. */
static int SplitLines(const char *text, char lines[][HUD_LINE],
                      int *widest) {
    int n = 0, w = 0;
    const char *p = text;

    *widest = 0;
    while (*p && n < HUD_LINES) {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        if (len > HUD_LINE - 1) len = HUD_LINE - 1;
        memcpy(lines[n], p, len);
        lines[n][len] = 0;
        if (len > w) w = len;
        n++;
        if (!e) break;
        p = e + 1;
    }
    *widest = w;
    return n;
}

static void DropView(HudView *v) {
    if (v->panel && v->gen == ShUiGen()) ShUiDestroy(v->panel);
    memset(v, 0, sizeof(*v));
}

static int EnsureView(HudView *v, float x, float y, float w, float h) {
    int i;

    if (v->panel && v->gen == ShUiGen()) return 1;
    memset(v, 0, sizeof(*v));
    v->gen = ShUiGen();
    v->panel = ShUiPanel(x, y, w, h, 0x000000, 0.7f);
    if (!v->panel) return 0;
    for (i = 0; i < HUD_LINES; i++) {
        v->line[i] = ShUiLabel(v->panel, PAD, PAD + LINE_H * (float)i,
                               w - 2 * PAD, LINE_H, " ", 0xFFFFFF);
        ShUiShow(v->line[i], 0);
    }
    v->x = x; v->y = y; v->w = w; v->h = h;
    v->visible = 1;
    v->colour = 0xFFFFFF;
    return 1;
}

/* One slot: create or update its plate and lines. */
static void SyncSlot(int idx, float x, float y) {
    HudSlot s;
    HudView *v = &g_view[idx];
    char lines[HUD_LINES][HUD_LINE];
    int n, widest, i;
    float w, h;

    HudLock();
    s = g_slots[idx];
    HudUnlock();

    n = SplitLines(s.text, lines, &widest);
    w = (float)widest * CHAR_W + 2 * PAD;
    if (w < MIN_W) w = MIN_W;
    if (w > MAX_W) w = MAX_W;
    h = (float)n * LINE_H + 2 * PAD;

    if (!EnsureView(v, x, y, w, h)) {
        Log("slot %d: no plate at %.0f,%.0f", idx, x, y);
        return;
    }
    if (v->x != x || v->y != y) {
        ShUiSetPos(v->panel, x, y);
        v->x = x; v->y = y;
    }
    if (v->w != w || v->h != h) {
        ShUiSetSize(v->panel, w, h);
        for (i = 0; i < HUD_LINES; i++)
            ShUiSetSize(v->line[i], w - 2 * PAD, LINE_H);
        v->w = w; v->h = h;
    }
    for (i = 0; i < HUD_LINES; i++) {
        int want = i < n;
        if (want != (i < v->lines)) ShUiShow(v->line[i], want);
        if (!want) continue;
        if (strcmp(v->shown[i], lines[i]) != 0) {
            strncpy(v->shown[i], lines[i], HUD_LINE - 1);
            ShUiSetText(v->line[i], lines[i][0] ? lines[i] : " ");
        }
        if (v->colour != s.colour) ShUiSetColour(v->line[i], s.colour);
    }
    v->lines = n;
    v->colour = s.colour;
    if (!v->visible) { ShUiShow(v->panel, 1); v->visible = 1; }
}

/* Slot height, for stacking before the slot is built. */
static float SlotHeight(int idx) {
    char lines[HUD_LINES][HUD_LINE];
    int widest, n;
    HudLock();
    n = SplitLines(g_slots[idx].text, lines, &widest);
    HudUnlock();
    return (float)n * LINE_H + 2 * PAD;
}

static float SlotWidth(int idx) {
    char lines[HUD_LINES][HUD_LINE];
    int widest;
    float w;
    HudLock();
    SplitLines(g_slots[idx].text, lines, &widest);
    HudUnlock();
    w = (float)widest * CHAR_W + 2 * PAD;
    if (w < MIN_W) w = MIN_W;
    if (w > MAX_W) w = MAX_W;
    return w;
}

/* Lay every corner out and push whatever changed. */
static void SyncAll(void) {
    int list[HUD_SLOTS], n, a, i;
    int active[HUD_SLOTS];
    /* The HUD lays out in a 1920 x 1080 reference space and
     * the engine scales it to the screen. */
    float sw = 1920.0f;
    float sh = 1080.0f;

    memset(active, 0, sizeof(active));
    for (a = 0; a <= SH_HUD_TOPCENTER; a++) {
        float y;
        int centred = (a == SH_HUD_TOPCENTER);
        int top = (a == SH_HUD_TOPLEFT || a == SH_HUD_TOPRIGHT ||
                   centred);
        int left = (a == SH_HUD_TOPLEFT || a == SH_HUD_BOTTOMLEFT);

        HudLock();
        n = Gather(a, list, HUD_SLOTS);
        HudUnlock();
        /* The centred column hangs below the top edge rather than
         * touching it, so it reads as a banner and not as another
         * corner, and so it never sits where the menu draws. */
        y = centred ? MARGIN + TOAST_OFF_Y
                    : (top ? MARGIN : sh - MARGIN);
        for (i = 0; i < n; i++) {
            float h = SlotHeight(list[i]);
            float w = SlotWidth(list[i]);
            float x = centred ? (sw - w) * 0.5f
                              : (left ? MARGIN : sw - MARGIN - w);
            if (!top) y -= h;
            SyncSlot(list[i], x, y);
            active[list[i]] = 1;
            y += top ? h + GAP : -GAP;
        }
    }
    for (i = 0; i < HUD_SLOTS; i++) {
        HudView *v = &g_view[i];
        if (active[i] || !v->panel) continue;
        if (v->gen != ShUiGen()) { memset(v, 0, sizeof(*v)); continue; }
        if (!g_slots[i].used) { DropView(v); continue; }
        if (v->visible) { ShUiShow(v->panel, 0); v->visible = 0; }
    }
}

/* ---- toasts -------------------------------------------------------
 * A toast is only data here: a text, a colour and a time. None of it
 * goes near the engine's own UI, which takes tens of seconds to come
 * up after a load and costs a widget per line; whoever draws reads
 * this and puts it on screen itself.
 */

typedef struct {
    int      busy;
    uint64_t born;    /* when it went up, for the fade in */
    uint64_t until;   /* 0 stays; else the tick count it leaves at */
    uint64_t ms;
    uint32_t rgb;
    char     text[SH_TOAST_TEXT];
} Toast;

static Toast g_toast[TOAST_SLOTS];

static void EnsureHud(void);   /* starts the lock these share */

/* Fade in as it arrives and out as it goes, so a line does not
 * simply flash on and off the screen. */
#define TOAST_FADE_IN_MS  150u
#define TOAST_FADE_OUT_MS 400u

static int ToastAlpha(const Toast *t, uint64_t now) {
    uint64_t age = now - t->born;
    int a = 255;

    if (age < TOAST_FADE_IN_MS)
        a = (int)(age * 255u / TOAST_FADE_IN_MS);
    if (t->until) {
        /* Only ever asked while the line still has time left. */
        uint64_t left = t->until - now;
        int out = (int)(left * 255u / TOAST_FADE_OUT_MS);
        if (left < TOAST_FADE_OUT_MS && out < a) a = out;
    }
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return a;
}

SH_API int ShHudToastSnapshot(ShToastView *out, int max) {
    uint64_t now = GetTickCount64();
    int i, n = 0;

    EnsureHud();
    HudLock();
    for (i = 0; i < TOAST_SLOTS; i++) {
        Toast *t = &g_toast[i];
        int alpha;

        if (!t->busy || !t->text[0]) continue;
        if (t->until && (int64_t)(now - t->until) >= 0) {
            t->busy = 0;
            Log("toast %d left on its own", i + 1);
            continue;
        }
        alpha = ToastAlpha(t, now);
        if (alpha <= 0) continue;
        if (out && n < max) {
            strncpy(out[n].text, t->text, SH_TOAST_TEXT - 1);
            out[n].text[SH_TOAST_TEXT - 1] = 0;
            out[n].rgb = t->rgb;
            out[n].alpha = alpha;
        }
        n++;
    }
    HudUnlock();
    return n;
}

/* Take down whatever has been up long enough. A few comparisons
 * per slot on the HUD's own beat. */
static void ExpireSlots(void) {
    uint64_t now = GetTickCount64();
    int i, changed = 0;

    HudLock();
    for (i = 0; i < HUD_SLOTS; i++) {
        HudSlot *s = &g_slots[i];
        if (!s->used || !s->until) continue;
        if ((int64_t)(now - s->until) < 0) continue;
        s->text[0] = 0;
        s->until = 0;
        changed = 1;
        Log("line %u left on its own", (unsigned)(i + 1));
    }
    HudUnlock();
    if (changed) HudChanged();
}

/* The engine UI takes tens of seconds to come up after a load - it
 * scans its assets first - and nothing can be drawn before that. A
 * line said into that gap used to time out before it was ever on
 * screen, so the clock starts when there is somewhere to draw. */
static void RestartTimers(void) {
    uint64_t now = GetTickCount64();
    int i, n = 0;

    HudLock();
    for (i = 0; i < HUD_SLOTS; i++) {
        HudSlot *s = &g_slots[i];
        if (!s->used || !s->ms) continue;
        s->until = now + s->ms;
        n++;
    }
    HudUnlock();
    if (n) Log("ui up: %d line(s) given their time back", n);
}

static DWORD WINAPI HudThread(LPVOID p) {
    int seen = -1, gen = -1, uiUp = 0;
    (void)p;

    for (;;) {
        int rev = g_rev, g;
        Sleep(HUD_TICK_MS);
        if (!ShUiReady()) { uiUp = 0; continue; }
        if (!uiUp) { uiUp = 1; RestartTimers(); }
        ExpireSlots();
        g = ShUiGen();
        if (rev == seen && g == gen) continue;
        SyncAll();
        seen = rev;
        gen = g;
    }
    return 0;
}

static void EnsureHud(void) {
    for (;;) {
        LONG s = InterlockedCompareExchange(&g_started, 0, 0);
        if (s == 1) return;
        if (s == 2) { Sleep(0); continue; }
        if (InterlockedCompareExchange(&g_started, 2, 0)) continue;
        InitializeCriticalSection(&g_lock);
        g_lockReady = 1;
        LogInit("scripthook_hud.log");
        CreateThread(NULL, 0, HudThread, NULL, 0, NULL);
        InterlockedExchange(&g_started, 1);
        return;
    }
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
        g_slots[i].anchor = anchor;
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
    s->ms = 0;
    s->until = 0;   /* ShHudSet stays until it is changed or hidden */
    HudUnlock();
    HudChanged();
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShHudFlash(uint32_t hud, const char *text, uint32_t ms) {
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
    s->ms = ms;
    s->until = ms ? GetTickCount64() + (uint64_t)ms : 0;
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

/* ---- status toast -------------------------------------------------
 * What a plugin puts up when it wants to say one thing for a
 * moment: a view changed, a scan is running. These only describe
 * the text, its colour and how long it stays, so nothing here has
 * to change if the way it is drawn does.
 */

/* The log is opened the first time a toast needs it: unlike the
 * engine HUD above, nothing here waits for a thread of its own. */
static void ToastLog(void) {
    static int done = 0;
    if (!done) { LogInit("scripthook_hud.log"); done = 1; }
}

static Toast *ToastOf(uint32_t id) {
    if (id == 0 || id > TOAST_SLOTS) return NULL;
    if (!g_toast[id - 1].busy) return NULL;
    return &g_toast[id - 1];
}

static uint32_t ToastPut(int slot, const char *text, uint32_t rgb,
                         uint32_t ms) {
    uint64_t now = GetTickCount64();
    Toast *t = &g_toast[slot];

    t->busy = 1;
    t->born = now;
    t->ms = ms;
    t->until = ms ? now + (uint64_t)ms : 0;
    t->rgb = rgb;
    if (text) {
        strncpy(t->text, text, SH_TOAST_TEXT - 1);
        t->text[SH_TOAST_TEXT - 1] = 0;
    } else {
        t->text[0] = 0;
    }
    return (uint32_t)(slot + 1);
}

SH_API uint32_t ShToast(const char *text) {
    return ShToastEx(text, 0xFFFFFF, SH_TOAST_MS_DEFAULT);
}

SH_API uint32_t ShToastEx(const char *text, uint32_t rgb,
                          uint32_t ms) {
    uint32_t id;
    int i, slot = -1;

    EnsureHud();
    HudLock();
    for (i = 0; i < TOAST_SLOTS; i++) {
        if (!g_toast[i].busy) { slot = i; break; }
    }
    /* Every line is up: the oldest gives way, because the state
     * being said now is the one worth reading. */
    if (slot < 0) {
        slot = 0;
        for (i = 1; i < TOAST_SLOTS; i++)
            if (g_toast[i].born < g_toast[slot].born) slot = i;
        ToastLog();
        Log("toasts full, the oldest gives way");
    }
    id = ToastPut(slot, text, rgb, ms);
    HudUnlock();
    ToastLog();
    Log("toast %u \"%s\" %ums", (unsigned)id,
        text ? text : "", (unsigned)ms);
    ShSetError(SH_OK);
    return id;
}

SH_API int ShToastSet(uint32_t id, const char *text, uint32_t rgb,
                      uint32_t ms) {
    Toast *t;

    EnsureHud();
    HudLock();
    t = ToastOf(id);
    /* Same line again, time restarted: a state that moves on -
     * scanning to hidden - must not stack up a second line. */
    if (t) ToastPut((int)(t - g_toast), text, rgb, ms);
    HudUnlock();
    if (!t) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShToastHide(uint32_t id) {
    Toast *t;

    EnsureHud();
    HudLock();
    t = ToastOf(id);
    if (t) t->busy = 0;
    HudUnlock();
    if (!t) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    ShSetError(SH_OK);
    return 1;
}

SH_API void ShToastClear(void) {
    int i, n = 0;

    EnsureHud();
    HudLock();
    for (i = 0; i < TOAST_SLOTS; i++) {
        if (g_toast[i].busy) n++;
        g_toast[i].busy = 0;
    }
    HudUnlock();
    if (n) { ToastLog(); Log("cleared %d toasts", n); }
}
