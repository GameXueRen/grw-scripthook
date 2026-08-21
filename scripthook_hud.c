/* HUD text slots, one plate per slot, drawn by the engine
 * through the native UI. Slots pack per corner from whoever
 * registered, so an absent plugin leaves no gap. */
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"

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

typedef struct {
    int      used;
    int      visible;
    int      anchor;
    int      priority;
    uint32_t colour;
    char     name[HUD_NAME];
    char     text[HUD_TEXT];
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
static volatile int g_started = 0;
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
            if ((s->anchor & 3) != anchor) continue;
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

    if (!EnsureView(v, x, y, w, h)) return;
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
    for (a = 0; a < 4; a++) {
        float y;
        int top = (a == SH_HUD_TOPLEFT || a == SH_HUD_TOPRIGHT);
        int left = (a == SH_HUD_TOPLEFT || a == SH_HUD_BOTTOMLEFT);

        HudLock();
        n = Gather(a, list, HUD_SLOTS);
        HudUnlock();
        y = top ? MARGIN : sh - MARGIN;
        for (i = 0; i < n; i++) {
            float h = SlotHeight(list[i]);
            float w = SlotWidth(list[i]);
            float x = left ? MARGIN : sw - MARGIN - w;
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

static DWORD WINAPI HudThread(LPVOID p) {
    int seen = -1, gen = -1;
    (void)p;

    for (;;) {
        int rev = g_rev, g;
        Sleep(HUD_TICK_MS);
        if (!ShUiReady()) continue;
        g = ShUiGen();
        if (rev == seen && g == gen) continue;
        SyncAll();
        seen = rev;
        gen = g;
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
