/* First person. The camera sits at the player's eye and the
 * head is hidden, so the body and weapon stay drawn.
 */
/* Binds late by choice. Plugins may import the ScriptHook
 * directly instead, since the loader loads them from a
 * thread rather than from DllMain. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "scripthook.h"

/* The mode switches on a keypress, so the walk has to keep
 * up with it. 250 left the camera held far too long. */
#define TICK_MS     60
#define MAX_PARTS   64

/* The player position is already at head height, so forward
 * is the only offset needed to clear the face.
 */
#define FWD_DEF     15.0f
#define FWD_MIN     0.0f
#define FWD_MAX     60.0f
#define FWD_STEP    5.0f
#define UP_DEF      0.0f
#define UP_MIN      -30.0f
#define UP_MAX      30.0f
#define UP_STEP     5.0f

/* How long the aim has to hold before the camera is handed
 * over, so the zoom into the body is never on screen. */
#define SETTLE_DEF  600.0f
#define SETTLE_MIN  0.0f
#define SETTLE_MAX  2000.0f
#define SETTLE_STEP 50.0f

typedef int (*IsInGame_t)(void);
typedef int (*GameState_t)(void);
typedef int (*GetPlayer_t)(ShPlayer *);
typedef int (*InputCtx_t)(void);
typedef int (*FirstPerson_t)(float, float);
typedef void (*Release_t)(uint32_t);
typedef int (*SetBlur_t)(int);
typedef int (*HeadNodes_t)(uint64_t, uint64_t *, int);
typedef void (*HeadInvalidate_t)(void);
typedef void (*HeadClearMiss_t)(void);
typedef int (*SetVisible_t)(uint64_t, uint64_t, int, int);
typedef int (*FpActive_t)(void);
typedef uint32_t (*MenuCreate_t)(const char *);
typedef uint32_t (*MenuSub_t)(uint32_t, const char *);
typedef int (*MenuToggle_t)(uint32_t, const char *, int,
                            ShMenuFn, void *);
typedef int (*MenuNumber_t)(uint32_t, const char *, float, float,
                            float, float, ShMenuFn, void *);
typedef int (*MenuStatus_t)(uint32_t, const char *);
typedef int (*MenuAction_t)(uint32_t, const char *, ShMenuFn, void *);
typedef int (*MenuHint_t)(uint32_t, const char *);
typedef int (*SceneCount_t)(void);
typedef uint64_t (*SceneAt_t)(int);
typedef uint64_t (*SceneRoot_t)(uint64_t);
typedef int (*ChildCount_t)(uint64_t);
typedef uint64_t (*ChildAt_t)(uint64_t, int);
typedef int (*WidgetClass_t)(uint64_t, char *, int);
typedef int (*WidgetGetS_t)(uint64_t, uint32_t, char *, int);
typedef int (*SceneName_t)(uint64_t, char *, int);
typedef int (*WidgetPropType_t)(uint64_t, uint32_t);
typedef int (*LogPath_t)(const char *, char *, int);

static LogPath_t    g_logPath;

/* Config convention, shared by every plugin: the .ini sits
 * beside the .asi and takes its base name, so
 * plugins\firstperson\firstperson.asi pairs with
 * plugins\firstperson\firstperson.ini. The name is read from
 * the module file rather than hardcoded, so the convention
 * holds for any plugin and survives a rename. */
static HINSTANCE g_inst = NULL;
static char      g_name[64];
static char      g_iniPath[MAX_PATH];

static IsInGame_t   g_inGame;
static GameState_t  g_state;
static GetPlayer_t  g_getPlayer;
static FirstPerson_t g_fp;
static Release_t    g_release;
static HeadNodes_t  g_headNodes;
static HeadInvalidate_t g_headInvalidate;
static HeadClearMiss_t g_headClearMiss;
static SetVisible_t g_setVisible;
static FpActive_t   g_fpActive;
static InputCtx_t   g_inputCtx;
static MenuStatus_t g_status;
static SetBlur_t    g_setBlur;
static SceneCount_t  g_sceneCount;
static SceneAt_t     g_sceneAt;
static SceneRoot_t   g_sceneRoot;
static ChildCount_t  g_childCount;
static ChildAt_t     g_childAt;
static WidgetClass_t g_widgetClass;
static WidgetGetS_t  g_widgetGetS;
static SceneName_t   g_sceneName;
static WidgetPropType_t g_widgetPropType;

static uint32_t g_menu = 0;
static volatile int   g_on = 0;
static volatile int   g_wantHide = 1;
static volatile int   g_settleMs = (int)SETTLE_DEF;

/* The eye offset can differ per stance or vehicle: a motorbike
 * leans, a helicopter seat sits higher, a passenger looks out
 * from a different place. Every category has its own forward
 * and height, chosen by the player. On foot lumps every stance
 * together; ground vehicles lump cars, bikes and boats. */
enum {
    CAT_FOOT = 0,    /* on foot, any stance */
    CAT_LAND,        /* ground vehicle (car, motorbike, boat) */
    CAT_PLANE,       /* airplane */
    CAT_HELI,        /* helicopter */
    CAT_RIDER,       /* riding along as a passenger */
    CAT_COUNT
};

/* Menu titles and ini suffixes, per category. */
static const char *g_catName[CAT_COUNT] = {
    "On foot", "Ground vehicle", "Airplane",
    "Helicopter", "Passenger"
};
static const char *g_catTag[CAT_COUNT] = {
    "foot", "land", "plane", "heli", "rider"
};

/* cm, per category. Active at any time is g_cat, driven by
 * the engine input context: OnFoot, Vehicle, Airplane,
 * Helicopter or VehiclePassenger. The array holds the menu
 * defaults for categories without an ini entry yet. */
static volatile float g_fwd[CAT_COUNT];
static volatile float g_up[CAT_COUNT];
static volatile int   g_cat = CAT_FOOT;

static void ResetCatDefaults(void) {
    int i;
    for (i = 0; i < CAT_COUNT; i++) {
        g_fwd[i] = FWD_DEF;
        g_up[i] = UP_DEF;
    }
}

/* The engine input context names what the player is doing.
 * Menu, drone and pause contexts carry no category; those
 * keep whatever was current. */
static int CatFromCtx(int ctx) {
    switch (ctx) {
    case SH_CTX_ONFOOT:            return CAT_FOOT;
    case SH_CTX_VEHICLE:           return CAT_LAND;
    case SH_CTX_AIRPLANE:          return CAT_PLANE;
    case SH_CTX_HELICOPTER:        return CAT_HELI;
    case SH_CTX_VEHICLE_PASSENGER: return CAT_RIDER;
    default:                       return -1;
    }
}

/* Counted so the status line can say where the walk got
 * to, rather than only whether it matched. */
static volatile int g_nScenes, g_nWidgets, g_nLabels, g_nText;
static volatile int g_nSights;

/* Diagnostic log: firstperson.log beside the game log folder.
 * Written from the tick thread only, on state changes, so the
 * menu return and stowed/parachute cases leave a trace of what
 * the plugin saw instead of a guess. */
static FILE *g_diag = NULL;
static char  g_diagPath[MAX_PATH];

static void Diag(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    SYSTEMTIME st;

    if (!g_diagPath[0]) {
        if (g_logPath &&
            g_logPath("firstperson.log", g_diagPath,
                      sizeof(g_diagPath))) {
        } else {
            GetModuleFileNameA(NULL, g_diagPath,
                               sizeof(g_diagPath));
            { char *s = strrchr(g_diagPath, '\\');
              if (s) s[1] = 0; }
            strcat(g_diagPath, "firstperson.log");
        }
        g_diag = fopen(g_diagPath, "w");
    }
    if (!g_diag) return;
    GetLocalTime(&st);
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    fprintf(g_diag, "[%02u:%02u:%02u.%03u] %s\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    fflush(g_diag);
}

static int Aiming(void);
static void SaveIni(void);

static uint64_t g_root = 0;
static uint64_t g_hideRoot = 0;
static uint64_t g_parts[MAX_PARTS];
static int      g_nparts = 0;
/* The engine can take the camera away - a stowed weapon
 * widens the view, a parachute pulls back, a drone flies.
 * While that view is up, hiding the head shows a headless
 * body in it. This is set while the camera is not ours so
 * the hide is lifted until first person comes back. */
static volatile int g_headAway = 0;
/* The engine rebuilds the head group on an outfit change or
 * a respawn, which drops the persistent hold and shows the
 * head again. A periodic reapply catches that, so the hide
 * self heals instead of relying on the one shot attempt.
 * Finding a not yet existing group sweeps the heap, so the
 * failure path is rate limited to its own beat. */
#define REHIDE_MS       300u
#define HIDE_RETRY_MS   500u
static uint64_t g_hideAt = 0;   /* last successful hide tick */
static uint64_t g_hideTry = 0;  /* last failed scan tick */

/* The head render nodes live on the soldier entity. The
 * root re-parents to a vehicle on every mount, so resolving
 * the head from the root would lose it each time (the head
 * pump picks the entity for the same reason). The root is
 * still read for body swap detection below.
 */
static uint64_t PlayerHeadEnt(void) {
    ShPlayer p;

    memset(&p, 0, sizeof(p));
    if (!g_getPlayer || !g_getPlayer(&p)) return 0;
    return p.entity ? p.entity : p.root;
}

/* The eye follows the head bone inside the engine's own
 * frame, so nothing here runs per frame. The active category
 * owns the offsets; every camera push reads it so a category
 * switch that lands mid aim applies the right seat.
 */
static void PushCamera(void) {
    if (g_fp) g_fp(g_fwd[g_cat] / 100.0f, g_up[g_cat] / 100.0f);
}

/* The hook reapplies the eye every frame until it is given
 * back, so a screen the player opens has to release it. The
 * drone owns its own camera and fights us for it. */
static volatile int g_held = 0;

static void Hold(int want) {
    if (want == g_held) return;
    g_held = want;
    if (want) PushCamera();
    else if (g_release) g_release(SH_CAM_POS);
}

/* Entity wide show: releases every hold on the root and
 * unhides all nodes in one deferred call, applied on the
 * game thread against the live node list. */
static void ShowHead(void) {
    if (g_setVisible && g_hideRoot)
        g_setVisible(g_hideRoot, 0, 1, 0);
    g_nparts = 0;
    g_hideRoot = 0;
}

/* The aim group that names the head parts appears the first
 * time the player aims, so this keeps trying until it does.
 */
static int HideHead(uint64_t root) {
    int n, i;

    if (!g_headNodes || !g_setVisible) return 0;
    n = g_headNodes(root, g_parts, MAX_PARTS);
    if (n <= 0) return 0;

    for (i = 0; i < n; i++)
        g_setVisible(root, g_parts[i], 0, 1);
    g_nparts = n;
    g_hideRoot = root;
    return n;
}

static void Report(void) {
    char line[96];
    size_t used;

    if (!g_status) return;
    if (!g_on)
        snprintf(line, sizeof(line), "off");
    else if (!g_widgetGetS)
        snprintf(line, sizeof(line),
                 "on, no widget tree: update the ScriptHook");
    else if (!g_wantHide)
        snprintf(line, sizeof(line), "on, head left visible");
    else if (g_headAway)
        snprintf(line, sizeof(line),
                 "on, head shown (view taken)");
    else if (g_nparts > 0)
        snprintf(line, sizeof(line), "on, head hidden (%d parts)",
                 g_nparts);
    else
        snprintf(line, sizeof(line), "on, aim once to hide the head");
    used = strlen(line);
    if (g_on && g_cat >= 0 && g_cat < CAT_COUNT)
        snprintf(line + used, sizeof(line) - used, " [%s]",
                 g_catName[g_cat]);
    g_status(g_menu, line);
}

/* The callbacks must not touch the player: resolving it and
 * hiding the head can each fall back to a full address space
 * scan, which would stall the menu callback thread for
 * seconds. The tick thread does that work on its own clock.
 */
static void OnToggle(uint32_t menu, uint32_t item, int value,
                     void *user) {
    (void)menu; (void)item; (void)user;

    if (value) {
        g_on = 1;
        g_nparts = 0;
        g_headAway = 0;
        /* Start on the category the engine input context says
         * we are in, not on whatever the last session left. */
        if (g_inputCtx) {
            int c = CatFromCtx(g_inputCtx());
            if (c >= 0) g_cat = c;
        }
        Hold(1);
        if (g_setBlur) g_setBlur(0);
    } else {
        g_on = 0;
        g_headAway = 0;
        ShowHead();
        if (g_setBlur) g_setBlur(1);
        Hold(0);
    }
    Report();
}

static void OnHide(uint32_t menu, uint32_t item, int value,
                   void *user) {
    (void)menu; (void)item; (void)user;
    g_wantHide = value;
    SaveIni();
    if (!value) {
        g_headAway = 0;
        ShowHead();
    }
    Report();
}

/* Each category owns its own sliders; user carries the
 * category and the axis (cat*2 + 0 forward, +1 height). */
static void OnCatSlide(uint32_t menu, uint32_t item, int value,
                       void *user) {
    int code = (int)(intptr_t)user;
    int cat = code >> 1, up = code & 1;
    (void)menu; (void)item;
    if (cat < 0 || cat >= CAT_COUNT) return;
    if (up) g_up[cat] = (float)value;
    else    g_fwd[cat] = (float)value;
    SaveIni();
    /* Only while we already own it, or adjusting a slider
     * would take the camera back during a screen. */
    if (g_on && g_held && cat == g_cat) PushCamera();
}

/* 0 hands the camera over the instant iron sights come up,
 * which shows the eye flying into the body. */
static void OnSettle(uint32_t menu, uint32_t item, int value,
                     void *user) {
    (void)menu; (void)item; (void)user;
    g_settleMs = value;
    SaveIni();
}

/* The weapon prompt names the mode Alt switches TO, not
 * the one you are in. So a label reading OVER THE SHOULDER
 * means iron sights are up right now. */
/* Iron sights are the engine's own aim camera. Holding the
 * eye there rips the scope glass off the weapon and smears
 * the scenery through the temporal upscaler. */
/* The label holds a localisation key, not the drawn text:
 * [AIMMODE_PC_OTS] is what renders as OVER THE SHOULDER.
 * The literal is kept for a build that resolves inline. */
#define WANT_KEY     "AIMMODE_PC_OTS"
#define WANT_LABEL   "OVER THE SHOULDER"
#define WANT_SCENE   "HUD_WeaponItemDisplay"
#define WALK_DEPTH   10
#define WALK_BUDGET  600

static int Contains(const char *hay, const char *needle) {
    int i, j;

    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j]; j++) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (b >= 'a' && b <= 'z') b = (char)(b - 32);
            if (a != b) break;
        }
        if (!needle[j]) return 1;
    }
    return 0;
}

static int LabelSays(uint64_t w, int depth, int *budget) {
    char cls[32], txt[160];
    int n, i;

    if (!w || depth > WALK_DEPTH) return 0;
    if (--*budget < 0) return 0;
    g_nWidgets++;

    if (g_widgetClass(w, cls, sizeof(cls)) && Contains(cls, "Label")) {
        g_nLabels++;
        if (g_widgetGetS(w, SH_P_TEXT, txt, sizeof(txt))) {
            g_nText++;
            if (Contains(txt, WANT_KEY) || Contains(txt, WANT_LABEL))
                return 1;
        }
    }

    n = g_childCount(w);
    for (i = 0; i < n; i++)
        if (LabelSays(g_childAt(w, i), depth + 1, budget)) return 1;
    return 0;
}

/* The weapon prompt stays up after ADS ends, so the aim
 * mode alone would leave the camera released for good.
 * Our own poll: the hook only blocks the GAME's reads. */
static int Aiming(void) {
    return (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
}

/* Walks the game's own UI, so it needs a ScriptHook that
 * exposes the widget tree. Older ones keep the camera. */
static int IronSights(void) {
    int i, n, budget = WALK_BUDGET;

    if (!g_sceneCount || !g_sceneAt || !g_sceneRoot ||
        !g_childCount || !g_childAt || !g_widgetClass ||
        !g_widgetGetS)
        return 0;

    g_nWidgets = 0; g_nLabels = 0; g_nText = 0;
    n = g_sceneCount();
    g_nScenes = n;
    for (i = 0; i < n; i++) {
        uint64_t s = g_sceneAt(i), root;
        char name[64];

        /* Only the weapon display carries the prompt, and
         * walking all thirteen scenes every tick is work
         * for nothing. */
        if (g_sceneName && g_sceneName(s, name, sizeof(name)) &&
            !Contains(name, WANT_SCENE))
            continue;
        root = g_sceneRoot(s);
        if (!root) continue;
        if (LabelSays(root, 0, &budget)) { g_nSights = 1; return 1; }
    }
    g_nSights = 0;
    return 0;
}

/* Every widget of every game scene, written out, because
 * the label we want is on screen and the walk does not
 * see its text. Ground truth beats another guess. */
static FILE *g_dump;

static void DumpWidget(uint64_t w, int depth, int *budget) {
    char cls[48], txt[192], pad[24];
    int n, i, got, type;

    if (!w || depth > 16 || --*budget < 0) return;

    for (i = 0; i < depth && i < 20; i++) pad[i] = ' ';
    pad[i] = 0;

    cls[0] = 0;
    g_widgetClass(w, cls, sizeof(cls));
    type = g_widgetPropType ? g_widgetPropType(w, SH_P_TEXT) : -1;
    got = g_widgetGetS(w, SH_P_TEXT, txt, sizeof(txt));

    fprintf(g_dump, "%s%016llx %-28s texttype %d %s\n", pad,
            (unsigned long long)w, cls[0] ? cls : "?", type,
            got ? txt : "<no text>");

    n = g_childCount(w);
    for (i = 0; i < n; i++)
        DumpWidget(g_childAt(w, i), depth + 1, budget);
}

static void OnDump(uint32_t menu, uint32_t item, int value,
                   void *user) {
    char path[MAX_PATH], name[64];
    int i, n, budget = 20000;
    (void)menu; (void)item; (void)value; (void)user;

    if (!g_sceneCount) return;
    /* All logs live in <gamedir>\logs; fall back to the exe
     * folder on hooks old enough to lack ShLogPath. */
    if (!g_logPath ||
        !g_logPath("firstperson_ui.log", path, sizeof(path))) {
        GetModuleFileNameA(NULL, path, sizeof(path));
        { char *s = strrchr(path, '\\'); if (s) s[1] = 0; }
        strcat(path, "firstperson_ui.log");
    }

    g_dump = fopen(path, "w");
    if (!g_dump) return;
    n = g_sceneCount();
    fprintf(g_dump, "%d scenes drawn last frame\n\n", n);
    for (i = 0; i < n; i++) {
        uint64_t s = g_sceneAt(i), root = g_sceneRoot(s);
        name[0] = 0;
        if (g_sceneName) g_sceneName(s, name, sizeof(name));
        fprintf(g_dump, "scene %d %016llx root %016llx name %s\n",
                i, (unsigned long long)s, (unsigned long long)root,
                name[0] ? name : "?");
        DumpWidget(root, 1, &budget);
        fprintf(g_dump, "\n");
    }
    fclose(g_dump);
    g_dump = NULL;
    if (g_status) g_status(g_menu, "dumped firstperson_ui.log");
}

/* Paused counts as in game, but the player lookup falls
 * back to a heap scan while a menu is up. Nothing here is
 * urgent enough to pay for that, so the tick waits. */
static int Playing(void) {
    if (g_state) return g_state() == SH_STATE_INGAME;
    return g_inGame && g_inGame();
}

/* A new body means the old parts are gone, so the hold is
 * dropped and armed again on the new one.
 */
static DWORD WINAPI TickThread(LPVOID p) {
    int said = 0, settle = 0;
    int dPlay = -1, dFp = -1, prevAim = 0;
    uint64_t lastBeat = 0;
    (void)p;

    for (;;) {
        uint64_t root;
        int playing, aimNow;
        uint64_t nowMs;

        Sleep(TICK_MS);
        nowMs = GetTickCount64();
        playing = Playing();
        if (playing != dPlay) {
            Diag("playing=%d", playing);
            dPlay = playing;
        }
        /* Periodic summary so a menu round trip leaves a trace
         * even when nothing changes: playing, held, wantHide,
         * the head hide state, and where first person stands.
         * The engine input context (ctx) shows what the player
         * is doing, which picks the eye offset category. */
        if (nowMs - lastBeat >= 1000) {
            int ctx = g_inputCtx ? g_inputCtx() : -1;
            lastBeat = nowMs;
            Diag("beat: play=%d held=%d want=%d away=%d n=%d "
                 "fp=%s cat=%d ctx=%d root=%p",
                 playing, g_held, g_wantHide, g_headAway, g_nparts,
                 g_fpActive ? (g_fpActive() ? "yes" : "no") : "?",
                 g_cat, ctx, (void *)(uintptr_t)g_root);
        }
        /* Give the camera back on every screen, not just on
         * the toggle, or the drone never gets it. */
        if (!g_on || !playing) {
            Hold(0);
            /* A pause or equipment menu blurs the world behind
             * it, so a head hidden for first person would show
             * as a headless body in that backdrop - and on the
             * frames right after the menu closes. Restore it
             * while the game camera is away, the away check on
             * the world frames hides it again when the eye is
             * really back. */
            if (g_on && g_wantHide && !g_headAway && g_nparts > 0) {
                g_headAway = 1;
                ShowHead();
                Diag("headAway=1 (screen up)");
            }
            continue;
        }
        /* The engine's aim camera owns iron sights, but
         * only while the player is actually aiming. */
        /* The wait is on the AIM, not on the mode: raising
         * the weapon zooms the eye into the body, while
         * Alt switches mode with no transition at all. */
        /* So an already settled aim hands over the moment
         * the mode changes. Taking it back is immediate. */
        aimNow = Aiming();
        if (aimNow) {
            if (settle < g_settleMs) settle += TICK_MS;
        } else {
            settle = 0;
        }
        Hold(!(settle >= g_settleMs && IronSights()));
        /* Follow the engine input context: on foot, a ground
         * vehicle, a plane, a helicopter or riding along each
         * get their own eye offset. Menus and drones carry no
         * category and leave the current one in place. */
        if (g_inputCtx && g_on) {
            int cat = CatFromCtx(g_inputCtx());
            if (cat >= 0 && cat != g_cat) {
                Diag("cat %d -> %d (%s)", g_cat, cat,
                     g_catName[cat]);
                g_cat = cat;
                if (g_held) PushCamera();
                Report();
            }
        }
        if (!g_held) continue;
        /* The head group is created the first time the game
         * camera aims, so an aim that happens while the head
         * is still visible (n==0) is the moment a missing
         * group can finally appear. Drop only the remembered
         * miss so the retry rescans for it - the found group
         * for the player body stays cached, or a vehicle ride
         * that passes through an aim wipes it and the body
         * has to sweep the whole heap again when we get out. */
        if (aimNow != prevAim) {
            prevAim = aimNow;
            if (g_wantHide && g_nparts == 0) {
                if (g_headClearMiss) g_headClearMiss();
                else if (g_headInvalidate) g_headInvalidate();
                g_hideTry = 0;
                Diag("aim edge %d -> retry hide (visible)", aimNow);
            }
        } else {
            prevAim = aimNow;
        }

        /* Whether the first person eye is really on camera.
         * This is answered by the camera hook measuring where
         * the rendered camera sits, so it stays valid even
         * while the player lookup below cannot resolve - a
         * stowed weapon, a parachute, a drone, and the frames
         * right after a menu closes all take the camera away,
         * and on those the head must show again, not stay
         * hidden behind a headless body.
         *
         * The check deliberately runs before the player lookup:
         * menus leave the lookup on a heap scan backoff, and
         * waiting for it delays showing the head for seconds.
         */
        if (g_wantHide) {
            int fpOn = g_fpActive ? g_fpActive() : 1;
            if (fpOn != dFp) {
                Diag("fpOn=%d", fpOn);
                dFp = fpOn;
            }
            if (!fpOn) {
                if (!g_headAway) {
                    g_headAway = 1;
                    Diag("headAway=1 (fp lost)");
                    ShowHead();
                    Report();
                    said = 0;
                }
                continue;
            }
            if (g_headAway) {
                /* Back in first person: drop the away state and
                 * hide the head again right away. */
                g_headAway = 0;
                g_nparts = 0;
                g_hideTry = 0;
                said = 0;
                Diag("headAway=0 (fp back)");
            }
        } else if (!said) {
            Report();
            said = 1;
        }

        /* The head nodes belong to the soldier entity; the
         * root re-parents to a vehicle on mount, so chasing
         * the root hides nothing in a car and, worse, ShowHead
         * on the flip leaves the head visible until we step
         * out. Track the soldier, whose entity only changes on
         * a true body swap (respawn, new session). */
        root = PlayerHeadEnt();
        if (!root) continue;
        if (root != g_root) {
            /* A body swap - a respawn, a new session - leaves
             * the old entity's persistent hide hold running:
             * the pump keeps hiding its head nodes even though
             * the view has moved on, and on the new entity the
             * head group may not be found yet (it appears on
             * the first aim). Drop the old hold first so the
             * head is never stuck invisible behind a hold we no
             * longer track. */
            if (g_hideRoot && g_hideRoot != root)
                ShowHead();
            g_root = root;
            g_nparts = 0;
            g_hideAt = 0;
            g_hideTry = 0;
            said = 0;
            PushCamera();
            Diag("head ent changed to %p", (void *)(uintptr_t)root);
        }
        if (g_wantHide) {
            uint64_t now = GetTickCount64();
            if (g_nparts == 0) {
                /* Not hidden yet (the head group appears the
                 * first time the player aims). A miss sweeps
                 * the heap, so only try on the slow beat. */
                if (now >= g_hideTry) {
                    g_hideTry = now + HIDE_RETRY_MS;
                    if (HideHead(root)) {
                        g_hideAt = now;
                        Report();
                        said = 0;
                        Diag("hide ok n=%d", g_nparts);
                    } else if (!said) {
                        Report();
                        said = 1;
                        Diag("hide miss (head group absent)");
                    }
                }
            } else if (now - g_hideAt >= REHIDE_MS) {
                /* The engine can rebuild the head nodes under
                 * us - an outfit swap, a new ADS state, a
                 * respawn - which drops the persistent hold
                 * and shows the head again. Re-apply on a slow
                 * beat so the hide self heals. */
                if (!HideHead(root)) {
                    g_nparts = 0;
                    g_hideTry = now + HIDE_RETRY_MS;
                    said = 0;
                    Report();
                    Diag("rehide miss");
                }
            }
        }
    }
    return 0;
}

/* The plugin's own settings live in
 * plugins/firstperson/firstperson.ini, beside the .asi.
 * They are optional: the built-in defaults stand in. */
static int  IniInt(const char *path, const char *key, int def) {
    return GetPrivateProfileIntA("Settings", key, def, path);
}

static int  IniBool(const char *path, const char *key, int def) {
    char buf[16];
    if (!GetPrivateProfileStringA("Settings", key, "", buf,
                                  sizeof(buf), path))
        return def;
    if (!buf[0]) return def;
    if (!_stricmp(buf, "1") || !_stricmp(buf, "true") ||
        !_stricmp(buf, "yes") || !_stricmp(buf, "on")) return 1;
    if (!_stricmp(buf, "0") || !_stricmp(buf, "false") ||
        !_stricmp(buf, "no") || !_stricmp(buf, "off")) return 0;
    return def;
}

static float IniFloat(const char *path, const char *key, float def) {
    char buf[64];
    if (!GetPrivateProfileStringA("Settings", key, "", buf,
                                  sizeof(buf), path))
        return def;
    if (!buf[0]) return def;
    return (float)atof(buf);
}

/* Resolve <gamedir>\plugins\<name>\<name>.ini from the plugin's
 * own file name and the loader's ShPluginIniPath, once. */
static void ResolveIniPath(HMODULE m) {
    typedef int (*PluginIni_t)(const char *, char *, int);
    PluginIni_t pluginIni = NULL;
    char mod[MAX_PATH];
    const char *base, *dot;
    size_t len;

    g_name[0] = 0;
    g_iniPath[0] = 0;
    if (!g_inst || !GetModuleFileNameA(g_inst, mod, sizeof(mod)))
        return;
    base = strrchr(mod, '\\');
    base = base ? base + 1 : mod;
    dot = strrchr(base, '.');
    len = dot ? (size_t)(dot - base) : strlen(base);
    if (len >= sizeof(g_name)) len = sizeof(g_name) - 1;
    memcpy(g_name, base, len);
    g_name[len] = 0;

    *(FARPROC *)&pluginIni = GetProcAddress(m, "ShPluginIniPath");
    if (!pluginIni || !pluginIni(g_name, g_iniPath, sizeof(g_iniPath)))
        g_iniPath[0] = 0;
}

/* The FOOT category keeps the historic keys so an old config
 * file still reads; every category also gets its own key so
 * each stance or vehicle can be tuned separately. */
static void CatKey(char *key, size_t n, int cat, int isUp) {
    if (cat == CAT_FOOT)
        snprintf(key, n, isUp ? "height_cm" : "forward_cm");
    else
        snprintf(key, n, isUp ? "up_%s_cm" : "fwd_%s_cm",
                 g_catTag[cat]);
}

static void LoadIni(void) {
    int i;
    char key[40];

    ResetCatDefaults();
    if (!g_iniPath[0]) return;
    g_wantHide = IniBool(g_iniPath, "hide_head", g_wantHide);
    for (i = 0; i < CAT_COUNT; i++) {
        CatKey(key, sizeof(key), i, 0);
        g_fwd[i] = IniFloat(g_iniPath, key, FWD_DEF);
        CatKey(key, sizeof(key), i, 1);
        g_up[i] = IniFloat(g_iniPath, key, UP_DEF);
    }
    g_settleMs = IniInt(g_iniPath, "settle_ms", g_settleMs);
}

/* Write the current settings back to <name>.ini. "Enabled" is
 * a live state, not a setting, so it is deliberately not saved
 * and always starts off. */
static void SaveIni(void) {
    char buf[64], key[40];
    int i;

    if (!g_iniPath[0]) return;
    snprintf(buf, sizeof(buf), "%d", g_wantHide);
    WritePrivateProfileStringA("Settings", "hide_head", buf, g_iniPath);
    for (i = 0; i < CAT_COUNT; i++) {
        CatKey(key, sizeof(key), i, 0);
        snprintf(buf, sizeof(buf), "%.1f", g_fwd[i]);
        WritePrivateProfileStringA("Settings", key, buf, g_iniPath);
        CatKey(key, sizeof(key), i, 1);
        snprintf(buf, sizeof(buf), "%.1f", g_up[i]);
        WritePrivateProfileStringA("Settings", key, buf, g_iniPath);
    }
    snprintf(buf, sizeof(buf), "%d", g_settleMs);
    WritePrivateProfileStringA("Settings", "settle_ms", buf, g_iniPath);
}

static DWORD WINAPI BindThread(LPVOID p) {
    HMODULE m = NULL;
    MenuCreate_t menuCreate;
    MenuSub_t   menuSub;
    MenuToggle_t menuToggle;
    MenuNumber_t menuNumber;
    (void)p;

    while (!m) {
        m = GetModuleHandleA("dinput8.dll");
        if (!m) Sleep(500);
    }
    *(FARPROC *)&g_logPath = GetProcAddress(m, "ShLogPath");
    *(FARPROC *)&g_inGame = GetProcAddress(m, "ShIsInGame");
    *(FARPROC *)&g_state = GetProcAddress(m, "ShGetGameState");
    *(FARPROC *)&g_getPlayer = GetProcAddress(m, "ShGetPlayer");
    *(FARPROC *)&g_fp = GetProcAddress(m, "ShCameraFirstPerson");
    *(FARPROC *)&g_release =
        GetProcAddress(m, "ShCameraReleaseFields");
    *(FARPROC *)&g_headNodes = GetProcAddress(m, "ShGetHeadNodes");
    *(FARPROC *)&g_headInvalidate =
        GetProcAddress(m, "ShHeadInvalidate");
    *(FARPROC *)&g_headClearMiss =
        GetProcAddress(m, "ShHeadClearMiss");
    *(FARPROC *)&g_setVisible = GetProcAddress(m, "ShSetVisible");
    /* Optional: with an older dinput8 the camera can never be
     * taken away from us, so the head stays hidden as asked. */
    *(FARPROC *)&g_fpActive =
        GetProcAddress(m, "ShCameraFirstPersonActive");
    /* Optional: an older dinput8 just keeps the blur. */
    *(FARPROC *)&g_setBlur = GetProcAddress(m, "ShSetCameraBlur");
    /* Optional: without the widget tree the camera is held
     * through iron sights, which is the old behaviour. */
    *(FARPROC *)&g_sceneCount = GetProcAddress(m, "ShGameSceneCount");
    *(FARPROC *)&g_sceneAt = GetProcAddress(m, "ShGameSceneAt");
    *(FARPROC *)&g_sceneRoot = GetProcAddress(m, "ShSceneRoot");
    *(FARPROC *)&g_childCount = GetProcAddress(m, "ShWidgetChildCount");
    *(FARPROC *)&g_childAt = GetProcAddress(m, "ShWidgetChildAt");
    *(FARPROC *)&g_widgetClass = GetProcAddress(m, "ShWidgetClass");
    *(FARPROC *)&g_widgetGetS = GetProcAddress(m, "ShWidgetGetS");
    *(FARPROC *)&g_sceneName = GetProcAddress(m, "ShGameSceneName");
    *(FARPROC *)&g_widgetPropType =
        GetProcAddress(m, "ShWidgetPropType");
    *(FARPROC *)&g_inputCtx = GetProcAddress(m, "ShInputContext");
    *(FARPROC *)&menuCreate = GetProcAddress(m, "ShMenuCreate");
    *(FARPROC *)&menuSub = GetProcAddress(m, "ShMenuSub");
    *(FARPROC *)&menuToggle = GetProcAddress(m, "ShMenuToggle");
    *(FARPROC *)&menuNumber = GetProcAddress(m, "ShMenuNumber");
    *(FARPROC *)&g_status = GetProcAddress(m, "ShMenuStatus");
    if (!g_inGame || !g_getPlayer || !g_fp || !g_release) return 1;
    if (!g_headNodes || !g_setVisible) return 1;
    if (!menuCreate || !menuToggle || !menuNumber || !g_status)
        return 1;

    ResolveIniPath(m);
    LoadIni();
    g_menu = menuCreate("First person");
    menuToggle(g_menu, "Enabled", 0, OnToggle, NULL);
    menuToggle(g_menu, "Hide head", g_wantHide, OnHide, NULL);
    /* One submenu per category: its own Forward/Height pair.
     * Each row carries the category and axis so a slider knows
     * where it writes, and entering a category's submenu shows
     * that category's current values. */
    if (menuSub) {
        int i;
        for (i = 0; i < CAT_COUNT; i++) {
            uint32_t sub = menuSub(g_menu, g_catName[i]);
            if (!sub) continue;
            menuNumber(sub, "Forward cm", g_fwd[i],
                       FWD_MIN, FWD_MAX, FWD_STEP, OnCatSlide,
                       (void *)(intptr_t)(i * 2 + 0));
            menuNumber(sub, "Height cm", g_up[i],
                       UP_MIN, UP_MAX, UP_STEP, OnCatSlide,
                       (void *)(intptr_t)(i * 2 + 1));
        }
    } else {
        /* No submenus in an older ScriptHook: keep the single
         * on foot pair on the root. */
        menuNumber(g_menu, "Forward cm", g_fwd[CAT_FOOT],
                   FWD_MIN, FWD_MAX, FWD_STEP, OnCatSlide,
                   (void *)(intptr_t)(CAT_FOOT * 2 + 0));
        menuNumber(g_menu, "Height cm", g_up[CAT_FOOT],
                   UP_MIN, UP_MAX, UP_STEP, OnCatSlide,
                   (void *)(intptr_t)(CAT_FOOT * 2 + 1));
    }
    menuNumber(g_menu, "ADS settle ms", (float)g_settleMs,
               SETTLE_MIN, SETTLE_MAX, SETTLE_STEP, OnSettle, NULL);
    {
        MenuAction_t menuAction;
        *(FARPROC *)&menuAction = GetProcAddress(m, "ShMenuAction");
        if (menuAction)
            menuAction(g_menu, "Dump UI to log", OnDump, NULL);
    }
    {
        MenuHint_t menuHint;
        *(FARPROC *)&menuHint = GetProcAddress(m, "ShMenuHint");
        if (menuHint)
            menuHint(g_menu,
                     "First-person view: hide head, adjust eye height "
                     "and distance per stance or vehicle.");
    }
    Report();

    CreateThread(NULL, 0, TickThread, NULL, 0, NULL);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = inst;
        DisableThreadLibraryCalls(inst);
        CreateThread(NULL, 0, BindThread, NULL, 0, NULL);
    }
    return TRUE;
}
