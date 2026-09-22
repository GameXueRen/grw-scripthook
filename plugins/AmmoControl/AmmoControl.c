/* Ammo control: the magazine capacity multiplier, and a reload that happens by
 * itself when the magazine runs dry.
 *
 * Every change goes through the framework's own engine calls - ShSetAmmoScale
 * for the capacity, ShFakeKey for the reload - so this installs no hook of its
 * own, patches no code and writes no engine memory.
 *
 * The two halves:
 *
 *   1. CAPACITY. The old plugin's six steps, as integer pairs so the result is
 *      exactly reproducible (0.50 = 1/2 ... 2.00 = 2/1). A change takes effect
 *      at the next refill: an ammo crate is what puts the number to use. At
 *      1.00x the framework installs nothing at all - the game keeps its own
 *      numbers - which is what that row is for.
 *
 *   2. AUTO RELOAD. ShGetAmmoRounds returns the rounds left in the magazine of
 *      the player's own weapon - the object the engine hands its capacity
 *      function, 0x180 into it; see the framework header and
 *      docs/ammocapacity-reverse.md section 9 - and when that reaches the
 *      threshold this presses the reload key for the player. The key is a row
 *      rather than a constant because the game's binding is the player's own
 *      business: it has to be set to whatever the game reloads on.
 *
 *      A weapon switch clears the reload state before the count is looked at
 *      (SwitchWatch): the wait and the backoff are about the weapon that just
 *      left, and a weapon that is already at the threshold would otherwise
 *      inherit them and not reload in time.
 *
 * Reading the rounds needs the capacity hook to be installed, because that hook
 * is what sees the engine ask for a capacity, and that call is what says which
 * weapon is in hand. So switching auto reload ON asks for a scale and puts the
 * wanted one straight back, which leaves the hook in place while the game's own
 * numbers stay in force. With auto reload OFF and 1.00x selected, nothing is
 * installed and this plugin changes nothing at all.
 *
 * The default blacklist applies (Ghost War and Mercenaries): while the mode is
 * not allowed the scale is handed back to the game and no key is pressed - a
 * scaled magazine in a PvP mode is an advantage this has no business taking.
 *
 * Text: kEn / kZh here, overridable by plugins\AmmoControl\lang.ini.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scripthook.h"
#include "log.h"

/* ---- the capacity steps ---------------------------------------------- */

/* 0.20x to 2.00x in 0.20x steps, 1.00x being the game's own numbers. Integer
 * pairs, so the result is exactly reproducible (0.20 = 1/5 ... 2.00 = 2/1),
 * and 1.00x is the pair 1/1 rather than 5/5: that is what makes its row
 * install no hook at all - see ShSetAmmoScale. */
typedef struct { const char *label; int num, den; } Step;

static const Step kSteps[] = {
    { "0.20x", 1, 5 }, { "0.40x", 2, 5 }, { "0.60x", 3, 5 },
    { "0.80x", 4, 5 }, { "1.00x", 1, 1 }, { "1.20x", 6, 5 },
    { "1.40x", 7, 5 }, { "1.60x", 8, 5 }, { "1.80x", 9, 5 },
    { "2.00x", 2, 1 }
};
#define STEP_N    ((int)(sizeof(kSteps) / sizeof(kSteps[0])))
#define STEP_ONE  4                 /* the 1.00x row: installs nothing */

static const char *kStepPtr[STEP_N];

/* ---- the rows that are choices -------------------------------------- */

/* Reload thresholds: 0 to 5 rounds left. */
static char        kThreshOpts[6][2];
static const char *kThreshPtr[6];
#define THRESH_N   6
#define THRESH_ONE 1

/* The reload key: the letters, then the digits. A row rather than a constant,
 * because this has to match whatever the game is bound to reload on. */
static char        kKeyOpts[36][3];
static const char *kKeyPtr[36];
#define KEY_N       36
#define KEY_VK(i)   ((i) < 26 ? ('A' + (i)) : ('0' + (i) - 26))
#define KEY_DEFAULT 17              /* R, the game's own default */

/* ---- state ----------------------------------------------------------- */

static char     g_ini[MAX_PATH];
static char     g_name[64];
static uint32_t g_menu;

/* The menu thread writes these; the worker thread reads them. */
static volatile LONG g_step = STEP_ONE;
static volatile LONG g_auto;
static volatile LONG g_thresh = THRESH_ONE;
static volatile LONG g_key = KEY_DEFAULT;

/* The worker thread alone writes these; the status line reads them. */
static volatile LONG g_rounds = -1;     /* -1 = not readable yet */
static int           g_hooked;          /* the capacity hook is installed */
static int           g_applied = -1;    /* the step actually written */
static int           g_waiting;         /* asked for a reload, waiting for it */
static DWORD         g_pressedAt;
/* The weapon the rounds reading was about, as the framework names it
 * (ShGetAmmoObject).  A change here is the one signal that the weapon in hand
 * is not the one this plugin was following - see SwitchWatch. */
static uint64_t      g_lastWeapon;

/* ---- settings -------------------------------------------------------- */

static void SaveInt(const char *key, LONG value) {
    char text[24];

    snprintf(text, sizeof(text), "%ld", (long)value);
    if (!WritePrivateProfileStringA("Settings", key, text, g_ini))
        Log("ac: could not write %s=%s to %s", key, text, g_ini);
}

static void LoadSettings(void) {
    LONG step, auto_, thresh, key;

    step   = (LONG)GetPrivateProfileIntA("Settings", "multiplier", STEP_ONE, g_ini);
    auto_  = (LONG)GetPrivateProfileIntA("Settings", "autoreload", 0, g_ini);
    thresh = (LONG)GetPrivateProfileIntA("Settings", "threshold", THRESH_ONE, g_ini);
    key    = (LONG)GetPrivateProfileIntA("Settings", "key", KEY_DEFAULT, g_ini);

    if (step < 0 || step >= STEP_N) step = STEP_ONE;
    if (thresh < 0 || thresh >= THRESH_N) thresh = THRESH_ONE;
    if (key < 0 || key >= KEY_N) key = KEY_DEFAULT;

    InterlockedExchange(&g_step, step);
    InterlockedExchange(&g_auto, auto_ ? 1 : 0);
    InterlockedExchange(&g_thresh, thresh);
    InterlockedExchange(&g_key, key);
}

/* ---- the capacity ---------------------------------------------------- */

/* Write the wanted scale. force makes it write even when it already did - the
 * call after EnsureHook, where 1.00x has to be written back so the game's own
 * number is what the engine computes. */
static void ApplyScale(int force) {
    int step = (int)g_step;
    int n = kSteps[step].num, d = kSteps[step].den;
    int ok;

    if (!force && step == g_applied) return;
    if (!force && !g_hooked && step == STEP_ONE) return;
    ok = ShSetAmmoScale(n, d);
    if (!ok) {
        Log("ac: scale %s refused (%08x, %s)", kSteps[step].label,
            ShLastError(), ShErrorString(ShLastError()));
        return;
    }
    g_applied = step;
    if (step != STEP_ONE) g_hooked = 1;
    Log("ac: scale %s", kSteps[step].label);
}

/* The rounds are only readable once the hook is in: it is what sees the engine
 * ask for a capacity, and that call is what hands over the weapon object. Ask
 * for 2/1 to install it, then write the scale that was actually wanted. */
static void EnsureHook(void) {
    if (!g_hooked) {
        if (!ShSetAmmoScale(2, 1)) {
            Log("ac: could not install the capacity hook (%08x, %s)",
                ShLastError(), ShErrorString(ShLastError()));
            return;
        }
        g_hooked = 1;
        Log("ac: capacity hook installed (it is what makes the rounds readable)");
        ApplyScale(1);
        return;
    }
    ApplyScale(1);
}

/* The wanted scale is re-asserted every couple of seconds, and handed back the
 * moment the mode stops allowing this plugin: the framework blacklists by mode,
 * and the answer can change under a long session. */
static void ApplyWatch(void) {
    static DWORD last;

    if ((long)(GetTickCount() - last) < 2000) return;
    last = GetTickCount();

    if (ShPluginAllowed()) {
        ApplyScale(0);
    } else if (g_hooked) {
        ShSetAmmoScale(1, 1);
        g_hooked = 0;
        g_applied = STEP_ONE;
        Log("ac: handed the magazine back to the game (mode not allowed)");
    }
}

/* ---- the reload ------------------------------------------------------ */

/* Press once when the magazine is down to the threshold, then wait for the
 * magazine to come back. A weapon with no reserve left never comes back, so
 * after that wait the next attempt is held off - rather than clicking away on
 * every poll.
 *
 * 3500 ms rather than 2500: a normal reload lands about 2.8 s after the press
 * in the sessions of 2026-09-21, so the shorter wait reported every reload as
 * "no reserve left" and held the next attempt back for nothing
 * (logs\AmmoControl.log 00:58:05 pressed -> 00:58:08 back to 50). */
#define RELOAD_WAIT_MS 3500
#define RELOAD_BACKOFF 6000

/* A weapon switch carries reload state that was never about this weapon.
 *
 * The wait and the 6000 ms backoff belong to the weapon that just left, and a
 * switch to a weapon that is ALREADY at the threshold inherits them - the
 * threshold branch below cannot clear them, because the new weapon's rounds are
 * not above the threshold either - so its first reload waits for a timer that
 * was never about it.  Reported 2026-09-22: "with auto reload on, switching
 * weapons does not pick up the new magazine in time, so it does not reload";
 * the sidearm sat at 3 with the threshold at 5 and the press was held off for
 * up to 3.5 s + 6 s.
 *
 * What says the weapon changed is the OBJECT the rounds reading is about
 * (ShGetAmmoObject), not the count: two weapons read the same number all the
 * time, and the value a switch shows is the one this plugin was just told.
 *
 * The first attempt used the framework's call trace (ShGetAmmoCalls) and had to
 * be thrown out: the capacity call is the engine's SHARED magazine call, so a
 * transition is any change in which object was asked about, other entities'
 * weapons included, and clearing on each of them pressed the reload key again
 * every time one arrived (logs\AmmoControl.log 2026-09-22 13:40:25.810 and
 * 13:40:26.062: two presses 250 ms apart, one per transition).
 *
 * Called after the rounds have been read, because that read is what sets the
 * object being watched. */
static void SwitchWatch(void) {
    uint64_t obj;

    if (!ShGetAmmoObject(&obj)) return;     /* nothing has been read yet */
    if (g_lastWeapon && obj != g_lastWeapon) {
        Log("ac: another weapon is in hand (%016llX) - reload state cleared",
            (unsigned long long)obj);
        g_waiting = 0;
        g_pressedAt = 0;                /* no backoff: it belonged to the other */
    }
    g_lastWeapon = obj;
}

/* ---- the HUD's own number -------------------------------------------- */

/* A second source, and the reason it was worth adding.
 *
 * Everything above reads the rounds out of the weapon the engine hands to its
 * capacity function and then works out WHICH weapon that was - "the one that
 * moved", "the one that last moved", "the newest one the engine asked about
 * that is the player's". docs/ammocapacity-reverse.md section 9 is the record
 * of how far that got, and the answer there is what the measurement supports:
 * right while the weapon is being fired, and for a moment after a switch. It
 * cannot be better than that, because the capacity call is SHARED with every
 * other entity's magazine and the weapon objects do not say whose they are (no
 * owner on the stowed ones that resolves to the player, no pointer to the
 * player anywhere in their first 0x400 bytes, and none of the player's own 96
 * objects pointing at one).
 *
 * The game is already showing the answer on its HUD. The framework can walk the
 * engine's own widget tree and read a label's text - ShGameSceneAt, ShSceneRoot,
 * ShWidgetChildAt, ShWidgetGetS - so the number the player is looking at can
 * just be read. That is:
 *
 *   - the player's weapon by construction: the HUD shows his, not an NPC's;
 *   - live every frame, so a weapon switch, a fresh world and a refill need no
 *     discovery and no wait for the engine to ask about anything - which is
 *     exactly the three cases the capacity call cannot answer;
 *   - the number on the screen, chambered round and all: a weapon whose HUD
 *     shows 6 cannot read 5 here, because the 5 never enters.
 *
 * Finding it, in three tiers, because a widget handle does not survive a world
 * reload while its PLACE does:
 *
 *   1. once found, one property read per poll - the handle is held and nothing
 *      is walked at all;
 *   2. when that handle stops answering (world reload, HUD rebuild: docs/ui.md
 *      says every widget the engine had is destroyed), the search starts from
 *      the remembered PLACE - the scene by name and the spot in it - so it is
 *      one scene and a handful of labels rather than the whole tree;
 *   3. if that misses, the whole tree is walked, which is where the label was
 *      found in the first place (the shape: the number that stops at the
 *      separator, "5/" beside the reserve "45") and where the second way in
 *      lives (the numeric label that drops by one with a shot).
 *
 * The place is kept in the ini, so a fresh launch starts already knowing it.
 * It is only ever a HINT: the shape check has the last word, so a stale anchor
 * costs a wider search, never a wrong number. */
#define HUD_CAND_MAX   128          /* the compass alone carries 60 of these */
#define HUD_WALK_MAX   600          /* widgets one pass may look at: the label
                                     * lives in scene 2 of the drawn ones, so
                                     * this only ever matters to the full walk */
#define HUD_SEEK_MS    1000         /* how often to look while nothing is locked */
#define HUD_LINK_MS    700          /* how soon after a shot a label must move */
#define HUD_MISS_MAX   3            /* shots without a drop before re-finding */
#define HUD_SCENE_MAX  24
#define HUD_ANCHOR_PX  24.0f        /* how near the remembered spot counts as it */
#define HUD_SCENE_DEAD_MS  5000     /* no drawn scene this long: remake ours */
#define HUD_SCENE_RETRY_MS 15000    /* and not more often than this */

/* The magazine label's remembered place. Measured 2026-09-22 14:01: scene
 * HUD_WeaponItemDisplay, "5/" at (121,-201), with the reserve "45" at
 * (121,-176) next to it - which is why those are the shipped defaults, for a
 * first launch that has not found it yet. The coordinates are in the
 * framework's own 1920 by 1080 reference space (the engine scales that to the
 * screen - docs/ui.md), so they do not move with the resolution or the window. */
static char  g_hudSceneName[64] = "HUD_WeaponItemDisplay";
static int   g_hudAnchorX = 121;
static int   g_hudAnchorY = -201;

/* A count, optionally followed by the rest of the readout. `mag` says the text
 * ENDS at the separator - "5/" - which is the shape the magazine label has:
 * the HUD draws "5/45" as two labels, the number and its slash in one and the
 * reserve in the next (measured 2026-09-22 14:01:02, scene
 * HUD_WeaponItemDisplay: "5/" at (121,-201), "45" at (121,-176)). Nothing else
 * on the HUD reads that way: the compass is plain numbers, "195", "210". That
 * shape is what finds the label without waiting for a shot. */
static int HudNumber(const char *s, int *out, int *mag) {
    int v = 0, n = 0;

    while (*s == ' ' || *s == '\t') s++;
    while (*s >= '0' && *s <= '9' && n < 3) { v = v * 10 + (*s - '0'); s++; n++; }
    if (!n) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (mag) *mag = 0;
    if (*s) {
        if (*s != '/' && *s != '|' && *s != '.' && *s != 'x' && *s != ':')
            return 0;
        if (mag && !s[1]) *mag = 1;     /* ends at the separator: "5/" */
    }
    *out = v;
    return 1;
}

typedef struct {
    uint64_t widget;
    int      value;                 /* -1 = never read */
    int      mag;                   /* digits up to the separator: "5/" */
    int      vis;
    int      scene;                 /* index into g_hudNames */
    float    x, y;
} HudCand;

static HudCand  g_hudCand[HUD_CAND_MAX];
static int      g_hudN;
static int      g_hudWalked;
static DWORD    g_hudSeekAt;            /* when the last search pass ran */
static int      g_hudTries;             /* passes since the last lock: the seek
                                         * backs off, so a HUD whose label never
                                         * turns up is not walked every second */
static char     g_hudNames[HUD_SCENE_MAX][64];  /* the scenes of the last pass */
static int      g_hudNameN;
static int      g_hudFull;              /* the next pass walks every scene */
static uint64_t g_hudLabel;             /* the magazine label, 0 = not found */
static int      g_hudLastVal = -1;      /* the locked label's last reading */
static volatile LONG g_hudRounds = -1;  /* what it says */
static DWORD    g_hudShotAt;            /* when the hook last saw a shot */
static int      g_hudHookPrev = -1;
static int      g_hudMisses;            /* shots the locked label slept through */
static int      g_hudSaidSeeking;       /* the "still looking" line, once */
static int      g_hudSaidScenes;        /* the scene list: once a session */
static int      g_hudVerbose;           /* log every label: only when the label
                                         * cannot be found, which is the one
                                         * moment that list is worth having */
static int      g_hudDrawn;             /* scenes the last pass saw drawn */
static DWORD    g_hudSceneAt;           /* when our own scene was made */
static DWORD    g_hudSceneRetryAt;      /* the last remake attempt */
static DWORD    g_hudSeekLogAt;         /* the "still nothing" line, throttled */
static uint32_t g_hudScene;             /* hidden scene, see HudScene */
static int      g_hudSceneTried;

static void HudWalk(uint64_t w, int depth, float px, float py, int scene) {
    char cls[64], text[64];
    float pos[3] = { 0.0f, 0.0f, 0.0f };
    uint32_t vis = 0;
    int i, n, v = 0, mag = 0;

    if (!w || depth > 8 || g_hudWalked > HUD_WALK_MAX) return;
    g_hudWalked++;

    cls[0] = 0;
    text[0] = 0;
    ShWidgetClass(w, cls, sizeof(cls));
    ShWidgetGetV(w, SH_P_POSITION, pos, 3);
    ShWidgetGetU(w, SH_P_VISIBLE, &vis);
    if (ShWidgetGetS(w, SH_P_TEXT, text, sizeof(text)) && text[0] &&
        HudNumber(text, &v, &mag)) {
        if (g_hudN < HUD_CAND_MAX) {
            HudCand *c = &g_hudCand[g_hudN++];

            c->widget = w;
            c->value = -1;
            c->mag = mag;
            c->vis = (int)vis;
            c->scene = scene;
            c->x = px + pos[0];
            c->y = py + pos[1];
        }
        if (g_hudVerbose)
            Log("ac: hud label cls=%s vis=%u at=(%.0f,%.0f) \"%s\" = %d%s",
                cls, vis, px + pos[0], py + pos[1], text, v,
                mag ? " (number up to the separator: this is the shape the "
                      "magazine label has)" : "");
    }

    n = ShWidgetChildCount(w);
    for (i = 0; i < n; i++)
        HudWalk(ShWidgetChildAt(w, i), depth + 1, px + pos[0], py + pos[1],
                scene);
}

/* The scene list is the set of scenes the render hook saw being drawn, and that
 * hook is only installed once one of OUR scenes exists - so with no scene of
 * ours the game's own widget tree can never be reached at all. An empty scene
 * is enough, and it draws nothing.
 *
 * `show` matters for the remake below: a scene made before the UI manager was
 * running is a scene the engine never ticks, and a scene that is never ticked
 * is a scene that never appears in the list. Hidden is the polite default (it
 * is empty anyway); shown is what the second attempt uses. */
static void HudScene(int show) {
    if (g_hudScene || !ShUiReady()) return;
    g_hudScene = ShUiSceneCreate("AmmoControl", -1);
    if (g_hudScene) {
        ShUiSceneShow(g_hudScene, show);
        g_hudSceneAt = GetTickCount();
        Log("ac: hud: empty scene up (%s) - the game's drawn scenes can be "
            "listed now", show ? "shown" : "hidden");
        return;
    }
    if (!g_hudSceneTried) {
        g_hudSceneTried = 1;
        Log("ac: hud: no scene yet (the UI manager is not up); will retry");
    }
}

/* One pass over the drawn scenes. `all` walks every one of them; without it the
 * pass is aimed at the REMEMBERED scene only, which is the point of keeping the
 * place: after a world reload the label is found again in one scene instead of
 * a thousand widgets across twenty-four. */
static void HudTree(int all) {
    HudCand old[HUD_CAND_MAX];
    int prevN = g_hudN, i, j, scenes = 0, walked = 0;

    memcpy(old, g_hudCand, sizeof(old));
    HudScene(0);
    g_hudN = 0;
    g_hudNameN = 0;
    g_hudWalked = 0;

    for (i = 0; i < ShGameSceneCount() && i < HUD_SCENE_MAX; i++) {
        uint64_t s = ShGameSceneAt(i);
        char nm[64];
        int idx;

        if (!s) continue;
        nm[0] = 0;
        ShGameSceneName(s, nm, sizeof(nm));
        scenes++;
        if (g_hudVerbose)
            Log("ac: hud scene %d = %s", i, nm[0] ? nm : "(unnamed)");
        if (!all && (!nm[0] || strcmp(nm, g_hudSceneName) != 0)) continue;
        if (g_hudNameN < HUD_SCENE_MAX) {
            strncpy(g_hudNames[g_hudNameN], nm[0] ? nm : "?", 
                    sizeof(g_hudNames[0]) - 1);
            g_hudNames[g_hudNameN][sizeof(g_hudNames[0]) - 1] = 0;
        }
        idx = g_hudNameN++;
        walked++;
        HudWalk(ShSceneRoot(s), 0, 0.0f, 0.0f, idx);
    }

    /* A label that was known keeps its last reading, so the "dropped by one"
     * comparison survives a re-walk. */
    for (i = 0; i < g_hudN; i++) {
        for (j = 0; j < prevN; j++) {
            if (old[j].widget == g_hudCand[i].widget) {
                g_hudCand[i].value = old[j].value;
                break;
            }
        }
    }

    if (!g_hudSaidScenes) {
        g_hudSaidScenes = 1;
        if (!scenes)
            Log("ac: hud: no scene is drawn yet (not in game, or the HUD is "
                "hidden)");
        else
            Log("ac: hud: %d scene(s) drawn, %d walked, %d numeric label(s)",
                scenes, walked, g_hudN);
    }
    if (g_hudVerbose) {
        /* The list above only exists when the label could not be found: it is
         * the answer to "what did it have to choose from". */
        Log("ac: hud: walked %d%s - the numeric labels above are everything it "
            "saw", g_hudN, all ? " (every scene)" : " (the remembered scene)");
        g_hudVerbose = 0;
    }
    g_hudDrawn = scenes;            /* the remake in HudTick watches this */
}

/* Write the label's place into the ini, so the next launch starts knowing it.
 * Only when it actually changed - a world reload finds the same spot again and
 * has no reason to write the file. */
static void HudSaveAnchor(const HudCand *c) {
    static char  savedScene[64];
    static int   savedX = 0x7FFFFFFF, savedY = 0x7FFFFFFF;
    const char  *nm = (c->scene >= 0 && c->scene < g_hudNameN)
                          ? g_hudNames[c->scene] : "";
    char         num[16];
    int          x = (int)(c->x + (c->x < 0 ? -0.5f : 0.5f));
    int          y = (int)(c->y + (c->y < 0 ? -0.5f : 0.5f));

    if (nm[0]) {
        strncpy(g_hudSceneName, nm, sizeof(g_hudSceneName) - 1);
        g_hudSceneName[sizeof(g_hudSceneName) - 1] = 0;
    }
    g_hudAnchorX = x;
    g_hudAnchorY = y;
    if (savedX == x && savedY == y && strcmp(savedScene, g_hudSceneName) == 0)
        return;
    savedX = x;
    savedY = y;
    strncpy(savedScene, g_hudSceneName, sizeof(savedScene) - 1);
    savedScene[sizeof(savedScene) - 1] = 0;
    WritePrivateProfileStringA("Settings", "hud_scene", g_hudSceneName, g_ini);
    snprintf(num, sizeof(num), "%d", x);
    WritePrivateProfileStringA("Settings", "hud_x", num, g_ini);
    snprintf(num, sizeof(num), "%d", y);
    WritePrivateProfileStringA("Settings", "hud_y", num, g_ini);
    Log("ac: hud: remembered %s (%d,%d) - the next launch aims straight at it",
        g_hudSceneName, x, y);
}

/* The place from the ini. The shipped defaults are the measured ones, so a
 * first launch has something to aim at before it has ever found the label; from
 * then on the file carries whatever the last session found. */
static void HudLoadAnchor(void) {
    if (!g_ini[0]) return;
    GetPrivateProfileStringA("Settings", "hud_scene", g_hudSceneName,
                             g_hudSceneName, sizeof(g_hudSceneName), g_ini);
    g_hudAnchorX = (int)GetPrivateProfileIntA("Settings", "hud_x", g_hudAnchorX,
                                              g_ini);
    g_hudAnchorY = (int)GetPrivateProfileIntA("Settings", "hud_y", g_hudAnchorY,
                                              g_ini);
    Log("ac: hud: aiming at %s (%d,%d), from the ini", g_hudSceneName,
        g_hudAnchorX, g_hudAnchorY);
}

static void HudLock(int i, const char *why) {
    const HudCand *c = &g_hudCand[i];

    g_hudLabel = c->widget;
    g_hudLastVal = c->value;
    g_hudMisses = 0;
    g_hudSaidSeeking = 0;
    g_hudFull = 0;                      /* the anchor is what the next aim uses */
    g_hudTries = 0;                     /* a fresh label searches at full speed */
    InterlockedExchange(&g_hudRounds, c->value >= 0 ? c->value : -1);
    Log("ac: hud: the magazine label is at (%.0f,%.0f) in %s, \"%d\" (shown %d) "
        "- %s; reading it from here on", c->x, c->y,
        (c->scene >= 0 && c->scene < g_hudNameN) ? g_hudNames[c->scene] : "?",
        c->value, c->vis, why);
    HudSaveAnchor(c);
}

/* How often to search while nothing is locked: a second to begin with, because
 * the first second of a session is the case that matters, and longer once it is
 * clear this HUD is not going to answer - so a loadout whose label never turns
 * up costs a walk every few seconds instead of one every second. */
static DWORD HudSeekMs(void) {
    if (g_hudTries < 5) return HUD_SEEK_MS;
    if (g_hudTries < 10) return 3000;
    return 5000;
}

/* One poll. Once the label is found this is a single property read and nothing
 * else - no walking at all; while it is not, a search pass, aimed at the
 * remembered place first and the whole tree only if that misses.
 *
 * `playing` is what the whole thing hangs on: a pause, a map, a loadout, a
 * loading screen and the game over screen do not draw the HUD, so there is
 * nothing to read and nothing to find in them - and searching them was the only
 * place this path could cost anything real (a whole-tree walk a second, for an
 * answer that cannot exist). The locked handle is kept across them, so coming
 * back to play needs no search at all. Called from the worker. */
static void HudTick(int hookRounds, int playing, int sameWeapon) {
    DWORD now = GetTickCount();
    char text[64];
    int v = 0, mag = 0, i, nMag = 0;

    if (!playing) {
        g_hudHookPrev = -1;         /* nothing across a screen is a shot */
        return;
    }

    if (hookRounds >= 0) {
        if (g_hudHookPrev >= 0 && hookRounds == g_hudHookPrev - 1) {
            /* A drop counts as a shot only on the same weapon: the capacity
             * call flips between objects on a switch and between the engine's
             * own bookkeeping calls, and counting those as shots is what let
             * the self check throw a good label away. */
            if (sameWeapon) {
                g_hudShotAt = now;
                g_hudMisses++;
            }
        }
        g_hudHookPrev = hookRounds;
    }

    /* ---- 1. the label is known: one read, no walking ---- */
    if (g_hudLabel) {
        if (ShWidgetGetS(g_hudLabel, SH_P_TEXT, text, sizeof(text)) &&
            HudNumber(text, &v, &mag)) {
            if (g_hudLastVal >= 0 && v != g_hudLastVal)
                g_hudMisses = 0;        /* it moved: it is the live one */
            g_hudLastVal = v;
            InterlockedExchange(&g_hudRounds, v);
            if (g_hudMisses < HUD_MISS_MAX) return;
            Log("ac: hud: the label slept through %d shot(s), so it is not the "
                "magazine after all - looking again", g_hudMisses);
        } else {
            /* A dead handle: a world reload destroys every widget the engine
             * had (docs/ui.md). Look again, starting from the remembered place. */
            Log("ac: hud: the label stopped answering (world reload or HUD "
                "rebuild) - looking for it again in %s near (%d,%d)",
                g_hudSceneName, g_hudAnchorX, g_hudAnchorY);
        }
        g_hudLabel = 0;
        g_hudLastVal = -1;
        g_hudMisses = 0;
        g_hudSaidSeeking = 0;
        g_hudTries = 0;             /* a lost label is looked for at full speed */
        InterlockedExchange(&g_hudRounds, -1);
    }

    /* ---- 2. looking for it ---- */
    if (g_hudSeekAt && (DWORD)(now - g_hudSeekAt) < HudSeekMs()) return;
    g_hudSeekAt = now;
    g_hudTries++;

    /* Our empty scene is what makes the framework's drawn scene list exist at
     * all, and a scene the engine does not tick never appears in it. Made
     * before the UI manager was running, it can stay unticked for the whole
     * session - which is the shape of the one failure seen in the field: the
     * first entry after a launch read nothing however long it was left, while
     * every later entry worked. So when nothing at all has been drawn, make the
     * scene again - this time SHOWN. It is empty, so it draws nothing, but a
     * scene being drawn is exactly what that list is built from. */
    if (!g_hudDrawn && g_hudScene &&
        (DWORD)(now - g_hudSceneAt) > HUD_SCENE_DEAD_MS &&
        (DWORD)(now - g_hudSceneRetryAt) > HUD_SCENE_RETRY_MS) {
        g_hudSceneRetryAt = now;
        ShUiSceneDestroy(g_hudScene);
        g_hudScene = 0;
        g_hudSaidScenes = 0;
        Log("ac: hud: nothing has been drawn for %ums, so the empty scene did "
            "not take - making it again, this time shown",
            (unsigned)HUD_SCENE_DEAD_MS);
        HudScene(1);
        if (g_hudScene) return;     /* this pass went on the remake */
    }

    HudTree(g_hudFull);
    g_hudFull = 1;                  /* if this pass misses, widen the next one */

    {
        int anchor = -1, shape = -1, shot = -1;
        int anchorVis = 1, shapeVis = 1;
        float best = HUD_ANCHOR_PX;

        for (i = 0; i < g_hudN; i++) {
            HudCand *c = &g_hudCand[i];
            float dx, dy, d;
            int vis;

            if (!ShWidgetGetS(c->widget, SH_P_TEXT, text, sizeof(text)) ||
                !HudNumber(text, &v, &mag)) {
                c->value = -1;
                continue;               /* not drawn now, or not a number */
            }
            if (shot < 0 && c->value >= 0 && v == c->value - 1 &&
                g_hudShotAt && (DWORD)(now - g_hudShotAt) <= HUD_LINK_MS)
                shot = i;
            if (c->mag) {
                /* A label the HUD has not marked as shown yet still counts, and
                 * that is not a detail: at the first entry of a session the
                 * counter carries its text before it is being shown, and
                 * insisting on "shown" is what left it unread until a shot or a
                 * step made the HUD refresh itself. Being shown decides first,
                 * being nearest the remembered spot decides between equals. */
                vis = c->vis ? 0 : 1;
                nMag++;
                if (shape < 0 || vis < shapeVis) { shape = i; shapeVis = vis; }
                dx = c->x - (float)g_hudAnchorX;
                dy = c->y - (float)g_hudAnchorY;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                d = dx + dy;
                if (d <= HUD_ANCHOR_PX &&
                    (anchor < 0 || vis < anchorVis ||
                     (vis == anchorVis && d <= best))) {
                    anchor = i;
                    anchorVis = vis;
                    best = d;
                }
            }
            c->value = v;
        }

        /* The shot first - it is what the hook can vouch for - then the
         * remembered place, then the shape anywhere in whatever was walked. */
        if (shot >= 0)
            HudLock(shot, "it dropped by one with the shot the hook saw");
        else if (anchor >= 0)
            HudLock(anchor, anchorVis
                ? "it is where the label was last found, the number up to the "
                  "separator"
                : "it is where the label was last found, and its text is there "
                  "even though the HUD has not shown it yet");
        else if (shape >= 0)
            HudLock(shape, shapeVis
                ? "it is the number up to the separator, the shape the HUD "
                  "draws beside the reserve"
                : "it is the only number up to a separator on the HUD");
    }

    if (!g_hudLabel) {
        if (!g_hudSaidSeeking) {
            g_hudSaidSeeking = 1;
            g_hudVerbose = 1;           /* next pass writes every label it sees */
            Log("ac: hud: no magazine label yet (%d numeric label(s) in %d "
                "scene(s) walked) - aiming at %s (%d,%d); a shot finds it too",
                g_hudN, g_hudNameN, g_hudSceneName, g_hudAnchorX, g_hudAnchorY);
        } else if ((DWORD)(now - g_hudSeekLogAt) >= 5000) {
            /* Which piece is missing, once the first line has been written: a
             * list that never fills in, a scene with no numbers in it, or
             * numbers that do not match - three different fixes. */
            g_hudSeekLogAt = now;
            Log("ac: hud: still nothing - ui ready %d, our scene %u, drawn %d, "
                "walked %d scene(s), labels %d of them %d up to a separator",
                ShUiReady(), g_hudScene, g_hudDrawn, g_hudNameN, g_hudN, nMag);
        }
    }
}

static void AutoReload(void) {
    DWORD now = GetTickCount();
    int rounds = (int)g_rounds;

    if (!g_auto || rounds < 0) return;
    if (!ShPluginAllowed()) return;
    if (ShGetGameState() != SH_STATE_INGAME) return;
    if (rounds > (int)g_thresh) { g_waiting = 0; return; }

    if (g_waiting) {
        if ((long)(now - g_pressedAt) < RELOAD_WAIT_MS) return;
        g_waiting = 0;
        g_pressedAt = now;
        Log("ac: the reload at %d did not come back - no reserve left?", rounds);
        return;
    }
    if ((long)(now - g_pressedAt) < RELOAD_BACKOFF) return;

    ShFakeKey(KEY_VK((int)g_key), 1);
    g_pressedAt = now;
    g_waiting = 1;
    Log("ac: magazine at %d (threshold %d) - pressed '%c'", rounds,
        (int)g_thresh, (char)KEY_VK((int)g_key));
}

/* ---- the page -------------------------------------------------------- */

enum { ROW_MULT = 1, ROW_AUTO, ROW_THRESH, ROW_KEY };

static void OnRow(uint32_t menu, uint32_t item, int value, void *user) {
    int which = (int)(intptr_t)user;

    (void)menu; (void)item;

    switch (which) {
    case ROW_MULT:
        InterlockedExchange(&g_step, value);
        SaveInt("multiplier", value);
        Log("ac: multiplier %s", kSteps[value].label);
        ApplyScale(1);
        break;
    case ROW_AUTO:
        InterlockedExchange(&g_auto, value ? 1 : 0);
        SaveInt("autoreload", value ? 1 : 0);
        Log("ac: auto reload %s", value ? "on" : "off");
        if (value) EnsureHook();
        break;
    case ROW_THRESH:
        InterlockedExchange(&g_thresh, value);
        SaveInt("threshold", value);
        Log("ac: reload when down to %d", value);
        break;
    case ROW_KEY:
        InterlockedExchange(&g_key, value);
        SaveInt("key", value);
        Log("ac: reload key '%c'", (char)KEY_VK(value));
        break;
    default:
        break;
    }
}

static void BuildMenu(void) {
    ShMenuHint(g_menu, "@ac.hint");
    ShMenuList(g_menu, "@ac.mult", kStepPtr, STEP_N, (int)g_step, OnRow,
               (void *)(intptr_t)ROW_MULT);
    ShMenuToggle(g_menu, "@ac.auto", (int)g_auto, OnRow,
                 (void *)(intptr_t)ROW_AUTO);
    ShMenuList(g_menu, "@ac.thresh", kThreshPtr, THRESH_N, (int)g_thresh, OnRow,
               (void *)(intptr_t)ROW_THRESH);
    ShMenuList(g_menu, "@ac.key", kKeyPtr, KEY_N, (int)g_key, OnRow,
               (void *)(intptr_t)ROW_KEY);
    ShMenuStatus(g_menu, "@ac.st.norounds");
}

/* One number, from whichever source could answer it: the HUD's own reading when
 * there is one, and the capacity call when the HUD's cannot be read. The line
 * says which - so a wrong number reads as a wrong SOURCE instead of a mystery. */
static void RefreshStatus(void) {
    int rounds = (int)g_rounds, hud = (int)g_hudRounds;
    const char *onoff = ShLangText(g_name, g_auto ? "@ac.on" : "@ac.off");
    const char *cap = kSteps[(int)g_step].label;

    if (!ShMenuIsShowing(g_menu)) return;
    if (rounds < 0 && hud < 0) {
        ShMenuStatus(g_menu, "@ac.st.norounds");
        return;
    }
    if (hud >= 0)
        ShMenuStatusF(g_menu, "@ac.status.hud", hud, (int)g_thresh, onoff, cap);
    else
        ShMenuStatusF(g_menu, "@ac.status.plain", rounds, (int)g_thresh, onoff,
                      cap);
}

/* ---- the plugin's own name ------------------------------------------ */

/* From the module path, like every other plugin here: the ini, the log and the
 * text owner all follow the file name. */
static void NameFromModule(HINSTANCE inst) {
    char mod[MAX_PATH];
    char *base, *dot;
    size_t n;

    g_name[0] = 0;
    if (!inst || !GetModuleFileNameA(inst, mod, sizeof(mod))) return;
    base = strrchr(mod, '\\');
    base = base ? base + 1 : mod;
    dot = strrchr(base, '.');
    n = dot ? (size_t)(dot - base) : strlen(base);
    if (n == 0 || n >= sizeof(g_name)) { g_name[0] = 0; return; }
    memcpy(g_name, base, n);
    g_name[n] = 0;
}

/* ---- text ------------------------------------------------------------ */

static const ShText kEn[] = {
    { "@ac.page",   "Ammo control" },
    { "@ac.hint",   "Raises or lowers all weapons' ammo cap" },
    { "@ac.mult",   "Capacity multiplier (applies on refill)" },
    { "@ac.auto",   "Auto reload" },
    { "@ac.thresh", "Reload when down to" },
    { "@ac.key",    "Reload key (same as in-game)" },
    { "@ac.on",     "on" },
    { "@ac.off",    "off" },
    { "@ac.status.hud", "magazine %d (HUD)  reload at %d  auto %s  cap %s" },
    { "@ac.status.plain", "magazine %d  reload at %d  auto %s  cap %s" },
    { "@ac.st.norounds", "no rounds yet - switch a weapon or take an ammo crate" }
};

static const ShText kZh[] = {
    { "@ac.page",   "弹药控制" },
    { "@ac.hint",   "增加或减少所有武器可携带的弹药数量上限" },
    { "@ac.mult",   "弹药上限倍率（修改后需到弹药箱补给生效）" },
    { "@ac.auto",   "自动换弹" },
    { "@ac.thresh", "剩余多少时换弹" },
    { "@ac.key",    "换弹键（需与游戏内设置的按键一致）" },
    { "@ac.on",     "开" },
    { "@ac.off",    "关" },
    { "@ac.status.hud", "弹匣 %d（HUD）  剩 %d 换弹  自动 %s  上限 %s" },
    { "@ac.status.plain", "弹匣 %d  剩 %d 换弹  自动 %s  上限 %s" },
    { "@ac.st.norounds", "还没有弹匣数 —— 换一次枪或进一次弹药箱" }
};

/* ---- start up -------------------------------------------------------- */

static DWORD WINAPI PluginThread(LPVOID param) {
    char logFile[80];
    int i;

    NameFromModule((HINSTANCE)param);
    if (!g_name[0]) strcpy(g_name, "AmmoControl");

    snprintf(logFile, sizeof(logFile), "%s.log", g_name);
    LogInitAlways(logFile);

    while (!GetModuleHandleA("dinput8.dll")) Sleep(500);

    if (!ShPluginIniPath(g_name, g_ini, sizeof(g_ini)))
        g_ini[0] = 0;
    LoadSettings();
    HudLoadAnchor();

    for (i = 0; i < STEP_N; i++) kStepPtr[i] = kSteps[i].label;
    for (i = 0; i < THRESH_N; i++) {
        snprintf(kThreshOpts[i], sizeof(kThreshOpts[i]), "%d", i);
        kThreshPtr[i] = kThreshOpts[i];
    }
    for (i = 0; i < KEY_N; i++) {
        kKeyOpts[i][0] = (char)KEY_VK(i);
        kKeyOpts[i][1] = 0;
        kKeyPtr[i] = kKeyOpts[i];
    }

    ShLangDeclare(g_name, "en-US", kEn, (int)(sizeof(kEn) / sizeof(kEn[0])));
    ShLangDeclare(g_name, "zh-CN", kZh, (int)(sizeof(kZh) / sizeof(kZh[0])));

    g_menu = ShMenuCreate("@ac.page");
    if (!g_menu) {
        Log("ac: no menu page (%08x) - nothing to drive", ShLastError());
        return 0;
    }
    BuildMenu();

    Log("ac: page up, multiplier %s, auto reload %s (at %d, key '%c'), rounds "
        "from the HUD, the capacity call as the fallback",
        kSteps[(int)g_step].label, g_auto ? "on" : "off", (int)g_thresh,
        (char)KEY_VK((int)g_key));

    ApplyScale(0);                      /* the ini's own value, if not 1.00x */
    /* The capacity call is what answers when the HUD's number cannot be read,
     * and it is also what sees a shot - the second way to identify the label
     * (its shape is the first, and that one needs no hook). */
    if (g_auto) EnsureHook();

    for (;;) {
        int rounds, st, playing, sameWeapon = 0;
        uint64_t wo;

        Sleep(250);
        RefreshStatus();
        ApplyWatch();

        st = ShGetGameState();
        playing = (st == SH_STATE_INGAME);
        if (st != SH_STATE_INGAME && st != SH_STATE_PAUSED)
            continue;

        /* Reading the rounds walks every weapon the engine has been asking
         * about (it is what ShGetAmmoRounds weighs up), and its only two
         * consumers are auto reload and the status line. With auto reload off
         * there is nothing to do while this page is not the page on screen:
         * ShMenuIsShowing asks about EXACTLY this page, so the scan is skipped
         * with the menu closed, on any other plugin's page, and inside any
         * submenu of someone else's. The loop reads again the moment the page
         * comes up, so the line follows within one tick - the framework shows
         * what was written last, which for that first tick is the old value. */
        if (g_auto || ShMenuIsShowing(g_menu)) {
            if (!ShGetAmmoRounds(&rounds)) {
                rounds = -1;
                if ((int)g_rounds >= 0 &&
                    ShLastError() == SH_ERR_NO_CANDIDATE &&
                    (int)g_hudRounds < 0) {
                    /* A load or a respawn puts it out of reach until the engine
                     * asks for a capacity again; say so once, not every poll -
                     * and not at all while the HUD is carrying the number. */
                    InterlockedExchange(&g_rounds, -1);
                    Log("ac: rounds not readable any more (no capacity asked "
                        "yet)");
                }
            }
            /* Whether this reading is about the weapon the last one was about.
             * Taken before SwitchWatch, which is what updates that memory: a
             * drop on the SAME weapon is a shot, and a drop across a switch is
             * not - see the shot check in HudTick. */
            sameWeapon = (ShGetAmmoObject(&wo) && g_lastWeapon &&
                          wo == g_lastWeapon);
            /* After the read, because the read is what names the object: a
             * switch drops the reload state that belonged to the other weapon. */
            SwitchWatch();
        } else {
            rounds = -1;
        }

        /* The HUD's own number, read whenever anything could want it: auto
         * reload uses it, and this page shows it. With neither true nothing is
         * walked and the old path costs exactly what it did - and nothing at
         * all is read or searched outside play, which is where the HUD is not
         * drawn anyway. */
        if (g_auto || ShMenuIsShowing(g_menu))
            HudTick(rounds, playing, sameWeapon);

        /* Which number is acted on: the HUD's when it could be read - it is the
         * one the player is looking at - and the capacity call otherwise. The
         * tag says which answered, because that is what a wrong number is. */
        {
            int fromHud = ((int)g_hudRounds >= 0);

            if (fromHud) rounds = (int)g_hudRounds;
            if (rounds < 0) {
                /* Neither source has a number. Forget the old one rather than
                 * let auto reload press a key against a magazine that is no
                 * longer being read. */
                if ((int)g_rounds >= 0) {
                    InterlockedExchange(&g_rounds, -1);
                    Log("ac: no rounds from either source - the old value is "
                        "forgotten");
                }
            } else if (rounds != (int)g_rounds) {
                int was = (int)g_rounds;

                if (was < 0)
                    Log("ac: rounds readable now (%d)%s", rounds,
                        fromHud ? " [hud]" : "");
                /* A shot's step is one; anything bigger is a reload or another
                 * weapon, and that is what a switch has to show. */
                else if (rounds > was || was - rounds > 5)
                    Log("ac: magazine %d -> %d%s%s", was, rounds,
                        rounds > was ? " (reload, or another weapon)" : "",
                        fromHud ? " [hud]" : "");
                InterlockedExchange(&g_rounds, rounds);
            }
        }

        AutoReload();
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    HANDLE h;

    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    DisableThreadLibraryCalls(inst);
    h = CreateThread(NULL, 0, PluginThread, (LPVOID)inst, 0, NULL);
    if (h) CloseHandle(h);
    return TRUE;
}
