/* First person. The camera sits at the player's eye and the
 * head is hidden, so the body and weapon stay drawn.
 */
/* The eye is not computed here. The ScriptHook asks the engine
 * for the head position - the argument its own head function
 * takes is captured once, then that same function is asked
 * again every frame - and writes the answer where the camera
 * position goes. That is what makes the view agree with the
 * engine on slopes, in vehicles and through a respawn, instead
 * of being a reading of ours pushed around the camera basis.
 *
 * What this plugin owns is therefore small: the offset, the
 * on/off state, the head, and telling the player what is
 * happening.
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

/* The eye offset, in centimetres, in world axes. The engine's
 * head position is already the eye, so the useful range is
 * small: these are the final centimetres of taste, not a
 * distance to walk forwards.
 */
#define OFF_DEF     0.0f
#define OFF_MIN     -100.0f
#define OFF_MAX     100.0f
#define OFF_STEP    1.0f

/* How long an aim keeps the eye before the engine's own aim
 * camera takes over. 0 hands it over the instant the aim
 * starts, which is what the table does. */
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
typedef int (*ViewMode_t)(void);
typedef uint32_t (*ToastEx_t)(const char *, uint32_t, uint32_t);
typedef int (*ToastSet_t)(uint32_t, const char *, uint32_t,
                          uint32_t);
typedef void (*HandoverClear_t)(void);
typedef uint32_t (*MenuCreate_t)(const char *);
typedef uint32_t (*MenuSub_t)(uint32_t, const char *);
typedef int (*MenuToggle_t)(uint32_t, const char *, int,
                            ShMenuFn, void *);
typedef int (*MenuNumber_t)(uint32_t, const char *, float, float,
                            float, float, ShMenuFn, void *);
typedef int (*MenuList_t)(uint32_t, const char *, const char **,
                          int, int, ShMenuFn, void *);
typedef int (*MenuSetValue_t)(uint32_t, const char *, int);
typedef int (*MenuStatus_t)(uint32_t, const char *);
typedef int (*MenuStatusF_t)(uint32_t, const char *, ...);
typedef const char *(*LangFor_t)(const char *, const char *);
typedef int (*MenuIsOpen_t)(void);
typedef int (*MenuHint_t)(uint32_t, const char *);
typedef int (*LogPath_t)(const char *, char *, int);

/* The engine side of the view. Optional: a ScriptHook without
 * them is one we do not know how to patch, and the camera then
 * falls back to the placement it has always used. */
typedef int      (*Fp2Install_t)(void);
typedef int      (*Fp2Extras_t)(uint32_t);
typedef int      (*Fp2Ready_t)(void);
typedef uint32_t (*Fp2Missing_t)(void);
typedef void     (*Fp2Enable_t)(int);
typedef void     (*Fp2SetOffset_t)(float, float, float);
typedef void     (*Fp2Settle_t)(uint32_t);
typedef void     (*Fp2Gate_t)(int *, int *, int *, int *);
typedef int      (*Fp2HeadOk_t)(void);
typedef void     (*Fp2HeadShow_t)(int);
typedef int      (*Fp2Bow_t)(void);
typedef uint32_t (*Fp2Age_t)(void);

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
static ViewMode_t   g_viewMode;
static ToastEx_t    g_toastEx;
static ToastSet_t   g_toastSet;
static HandoverClear_t g_handoverClear;
static InputCtx_t   g_inputCtx;
static MenuIsOpen_t g_menuIsOpen;
static MenuSetValue_t g_menuSetValue;
static MenuStatus_t g_status;
static MenuStatusF_t g_statusF;
static LangFor_t    g_langFor;
static SetBlur_t    g_setBlur;

static Fp2Install_t   g_fpxInstall;
static Fp2Extras_t    g_fpxExtras;
static Fp2Ready_t     g_fpxReady;
static Fp2Missing_t   g_fpxMissing;
static Fp2Enable_t    g_fpxEnable;
static Fp2SetOffset_t g_fpxSetOff;
static Fp2Settle_t    g_fpxSettle;
static Fp2Gate_t      g_fpxGate;
static Fp2HeadOk_t    g_fpxHeadOk;
static Fp2HeadShow_t  g_fpxHeadShow;
static Fp2Bow_t       g_fpxBow;
static Fp2Age_t       g_fpxAge;
static int            g_fpxUp = 0;   /* engine sites are patched */

static uint32_t g_menu = 0;
static volatile int   g_on = 0;
static volatile int   g_wantHide = 1;
/* [Settings] engine_camera=0 leaves the eye to the placement
 * the ScriptHook has always used, which is the way back if the
 * engine's own head function ever disagrees with a build. */
static volatile int   g_engineCam = 1;
/* [Settings] engine_extras takes the body and shoulder patches,
 * one bit each: 1 the body position hook, 2 body visibility,
 * 4 the shoulder swap, 8 the wall push. All four by default -
 * they are what the table does, and a build that disagrees with
 * one of them can leave it out bit by bit. */
static volatile int   g_extras = 15;

/* The offset is not one number, it is one per kind of moment:
 * a motorcycle leans, a helicopter seat sits high, a passenger
 * looks out from somewhere else entirely. Each kind keeps its
 * own three axes, and the engine's input context says which one
 * is in play - unless the player has picked a preset, which
 * then holds whatever the world is doing. */
enum {
    CAT_FOOT = 0,   /* on foot, any stance */
    CAT_LAND,       /* ground vehicle: car, motorbike, boat */
    CAT_PLANE,      /* airplane */
    CAT_HELI,       /* helicopter */
    CAT_RIDER,      /* riding along as a passenger */
    CAT_COUNT
};

/* Menu titles and ini suffixes, per category. The English
 * titles are the lookup keys for [zh_cn.First person]. */
static const char *g_catName[CAT_COUNT] = {
    "On foot", "Ground vehicle", "Airplane",
    "Helicopter", "Passenger"
};
static const char *g_catTag[CAT_COUNT] = {
    "foot", "land", "plane", "heli", "rider"
};

/* Custom presets: a saved set of offsets to switch to by hand,
 * for the places the automatic choice gets wrong. The list row
 * is 0 = Auto (follow the categories), 1..4 = force a preset. */
enum {
    PRESET_AUTO = -1,  /* follow g_cat as usual */
    PRESET_1,          /* index 0 of the preset arrays */
    PRESET_2,
    PRESET_3,
    PRESET_4,
    PRESET_COUNT       /* number of custom presets */
};
static const char *g_presetName[PRESET_COUNT] = {
    "Custom 1", "Custom 2", "Custom 3", "Custom 4"
};
static const char *g_presetTag[PRESET_COUNT] = {
    "preset1", "preset2", "preset3", "preset4"
};

/* cm, per category and per preset: X, Y and Z in world axes. */
static volatile float g_catX[CAT_COUNT];
static volatile float g_catY[CAT_COUNT];
static volatile float g_catZ[CAT_COUNT];
static volatile float g_preX[PRESET_COUNT];
static volatile float g_preY[PRESET_COUNT];
static volatile float g_preZ[PRESET_COUNT];
static volatile int   g_cat = CAT_FOOT;
static volatile int   g_presetSel = PRESET_AUTO;
static volatile int   g_settleMs = (int)SETTLE_DEF;

static void ResetOffsets(void) {
    int i;
    for (i = 0; i < CAT_COUNT; i++) {
        g_catX[i] = OFF_DEF;
        g_catY[i] = OFF_DEF;
        g_catZ[i] = OFF_DEF;
    }
    for (i = 0; i < PRESET_COUNT; i++) {
        g_preX[i] = OFF_DEF;
        g_preY[i] = OFF_DEF;
        g_preZ[i] = OFF_DEF;
    }
}

/* The engine input context names what the player is doing.
 * Menu, drone and pause contexts carry no category of their
 * own - a drone is a view the engine draws itself and no
 * offset of ours reaches it - so those keep whatever was
 * current. */
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

/* A hotkey flips first/third person while playing: the same
 * toggle as the menu's Enabled row. The key edge is polled on
 * its own thread - hiding the head can sweep the whole heap
 * for tens of seconds (a group that only appears on the first
 * aim), and the flip must stay live through that. Index 0 is
 * "None", meaning the hotkey is off. */
#define HOTKEYS     4
static const int g_hotVk[HOTKEYS] = {
    0,             /* None: hotkey disabled */
    VK_OEM_PLUS,   /* = */
    VK_F2,
    VK_F3
};
static const char *g_hotName[HOTKEYS] = {
    "None", "Equal (=)", "F2", "F3"
};
static volatile int g_hotKey = 0;   /* index into g_hotVk, 0 = off */

/* Diagnostic log: firstperson.log beside the game log folder.
 * Written from the tick thread only, on state changes, so a
 * view that will not come back leaves a trace of what the
 * plugin saw instead of a guess. */
static FILE *g_diag = NULL;
static char  g_diagPath[MAX_PATH];
static int   g_diagOn = 0;      /* [Settings] diag, default off */

static void Diag(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    SYSTEMTIME st;

    /* Diagnostics are opt-in ([Settings] diag=1): the beat line
     * alone used to write+flush once a second for the whole
     * process lifetime, which is where the megabyte logs came
     * from. */
    if (!g_diagOn) return;
    if (!g_diagPath[0]) {
        if (g_logPath &&
            g_logPath("firstperson.log", g_diagPath,
                      sizeof(g_diagPath))) {
        } else {
            GetModuleFileNameA(NULL, g_diagPath,
                               sizeof(g_diagPath));
            { char *s = strrchr(g_diagPath, '\\');
              if (s) s[1] = 0; }
            /* Bound the append: an exe path near MAX_PATH
             * used to strcat past the buffer. */
            if (strlen(g_diagPath) + 16 < sizeof(g_diagPath))
                strcat(g_diagPath, "firstperson.log");
            else
                return;
        }
        g_diag = fopen(g_diagPath, "w");
    }
    if (!g_diag) return;
    GetLocalTime(&st);
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    fprintf(g_diag, "[%02u:%02u:%02u.%03u] %s\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    fflush(g_diag);
}

/* ---- the head, when the engine cannot name it -------------
 * The engine's own visibility call needs a pointer it builds
 * for itself; where that chain does not resolve - a ScriptHook
 * without the sites, or a moment when the rig is not the one
 * it expects - the head is hidden the old way, by scanning the
 * entity for its head nodes. The two never run at once: a node
 * hold and the engine's own flag would fight, and the head
 * would flicker.
 */
static uint64_t g_root = 0;
static uint64_t g_hideRoot = 0;
static uint64_t g_parts[MAX_PARTS];
static int      g_nparts = 0;

/* Set while the view is not ours, so the hold is lifted and
 * the head is shown for as long as that is true. */
static volatile int g_headAway = 0;

/* The engine rebuilds the head group on an outfit change or
 * a respawn, which drops a node hold and shows the head again.
 * A periodic reapply catches that, so the hide self heals. */
#define REHIDE_MS       300u
/* Finding the head group sweeps the whole address space, tens of
 * seconds of work that the kernel now hands back in slices of a
 * few hundred milliseconds. So what a try cost is what tells the
 * two failures apart: a slow one was sweeping and picks the
 * sweep up on the very next tick, a fast one was a remembered
 * miss and only has to be asked again once something may have
 * changed the answer.
 */
#define SWEEP_STEP_MS     60u
#define RESWEEP_RETRY_MS  60000u
#define SWEEP_COST_MS     150u
static uint64_t g_hideAt = 0;   /* last successful hide tick */
static uint64_t g_hideTry = 0;  /* last failed scan tick */

/* The head render nodes live on the soldier entity. The
 * root re-parents to a vehicle on every mount, so resolving
 * the head from the root would lose it each time. The root is
 * still read for body swap detection below.
 */
static uint64_t PlayerHeadEnt(void) {
    ShPlayer p;

    memset(&p, 0, sizeof(p));
    if (!g_getPlayer || !g_getPlayer(&p)) return 0;
    return p.entity ? p.entity : p.root;
}

/* Entity wide show: releases every hold on the root and
 * unhides all nodes in one deferred call, applied on the
 * game thread against the live node list. */
static void ShowHead(void) {
    if (g_fpxUp && g_fpxHeadShow) g_fpxHeadShow(1);
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

/* HideHead and how long it took. The cost is the only way to
 * tell a call that was still sweeping the heap from one that
 * simply found nothing, and the two have to be retried
 * completely differently.
 */
static int HideHeadTimed(uint64_t root, uint64_t *cost) {
    uint64_t t0 = GetTickCount64();
    int n = HideHead(root);
    *cost = GetTickCount64() - t0;
    return n;
}

/* 1 while the engine's own call is the one hiding the head, so
 * the node scan has to stay out of it. */
static int EngineHidesHead(void) {
    return g_engineCam && g_fpxUp && g_fpxHeadOk && g_fpxHeadOk();
}

/* ---- the camera ------------------------------------------- */

/* The offset is all this plugin contributes to the eye: the
 * ScriptHook does the rest inside the engine's frame. Taking
 * the camera is still a camera call, because that is what
 * marks the position as ours. */
/* The three numbers in force right now: a preset if one has
 * been picked, otherwise whatever the current category says. */
static void ActiveOffset(float *x, float *y, float *z) {
    if (g_presetSel >= 0 && g_presetSel < PRESET_COUNT) {
        *x = g_preX[g_presetSel];
        *y = g_preY[g_presetSel];
        *z = g_preZ[g_presetSel];
        return;
    }
    if (g_cat < 0 || g_cat >= CAT_COUNT) {
        *x = OFF_DEF; *y = OFF_DEF; *z = OFF_DEF;
        return;
    }
    *x = g_catX[g_cat];
    *y = g_catY[g_cat];
    *z = g_catZ[g_cat];
}

static void PushCamera(void) {
    float x, y, z;

    ActiveOffset(&x, &y, &z);
    if (g_fpxSetOff) g_fpxSetOff(x / 100.0f, y / 100.0f, z / 100.0f);
    if (g_fp) g_fp(0.0f, 0.0f);
}

/* The hook reapplies the eye every frame until it is given
 * back, so a screen the player opens has to release it. */
static volatile int g_held = 0;

static void Hold(int want) {
    if (want == g_held) return;
    g_held = want;
    if (want) {
        if (g_engineCam && g_fpxUp && g_fpxEnable) g_fpxEnable(1);
        PushCamera();
    } else {
        if (g_fpxUp && g_fpxEnable) g_fpxEnable(0);
        if (g_release) g_release(SH_CAM_POS);
    }
}

/* ---- what the bar says -----------------------------------
 * One line across the top of the screen, so nothing here has
 * to be guessed at from the view alone. Only a state that
 * changed is said, so a wait that runs for seconds does not
 * announce itself on every one of those ticks.
 */
enum {
    SAY_NONE = 0,
    SAY_FP_SCANNING,   /* looking for the head group            wait */
    SAY_FP_SWAP,       /* a new body: hiding it again           wait */
    SAY_FP_REHIDE,     /* the head came back: hiding it again   wait */
    SAY_FP_NOSITE,     /* no engine sites: the old placement    brief */
    SAY_FP_HIDDEN,     /* head hidden                          brief */
    SAY_FP_NOHIDE,     /* first person on, hide head is off    brief */
    SAY_TP             /* third person                         brief */
};
static volatile int      g_said = SAY_NONE;
static uint32_t          g_toastId = 0;

/* When the wait now running started, for the step that says
 * what would end it. 0 = no wait is being timed. */
static volatile uint64_t g_waitFrom = 0;

/* A wait is said for at most this long. Waiting is worth
 * saying, but a line that nothing ever moves on from and that
 * nobody can dismiss is worse than no line, so eventually it
 * goes on its own. */
#define SAY_WAIT_MS 60000u

/* Amber while something is being waited for, green once it is
 * done, plain white for a plain change of view. */
#define SAY_RGB_BUSY  0xFFD24Au
#define SAY_RGB_DONE  0x7CFF8Au
#define SAY_RGB_PLAIN 0xFFFFFFu

/* ms 0 keeps the line up until the state moves on. */
static void Say(int state, const char *text, uint32_t rgb,
                uint32_t ms) {
    if (g_said == state) return;
    g_said = state;
    if (!g_toastEx) return;
    /* Same line again rather than a second one: a state that
     * moves on - scanning to hidden - must not stack up. A
     * line whose time is up is gone and cannot be set, so it
     * is simply made again - not noticing that lost every
     * message after the first one. */
    if (g_toastId && g_toastSet &&
        g_toastSet(g_toastId, text, rgb, ms))
        return;
    g_toastId = g_toastEx(text, rgb, ms);
}

/* Start timing a wait, unless one is already being timed: a
 * wait that is already running keeps its own clock. */
static void WaitFrom(uint64_t now) {
    if (!g_waitFrom) g_waitFrom = now;
}

static void WaitEnd(void) {
    g_waitFrom = 0;
}

static void Report(void);
static void SaveIni(void);

/* Turn first person on or off. Runs on both the menu worker
 * (Enabled toggle) and the tick thread (hotkey), so it must
 * not touch the player: resolving it can fall back to a heap
 * scan. Everything here is camera state and deferred calls.
 */
static void SetFp(int on) {
    if (on) {
        g_on = 1;
        g_nparts = 0;
        g_headAway = 0;
        /* Fresh start: drop any "the group was not there" note
         * and any backoff left over from before, so the first
         * try happens on the next tick instead of waiting it
         * out. */
        if (g_headClearMiss) g_headClearMiss();
        g_hideTry = 0;
        if (g_fpxSettle) g_fpxSettle((uint32_t)g_settleMs);
        Hold(1);
        if (g_setBlur) g_setBlur(0);
        WaitEnd();
        if (g_wantHide && !EngineHidesHead()) {
            /* The engine cannot name the head yet, so the old
             * scan is what will hide it - and that can run for
             * tens of seconds. Say so rather than leave a head
             * on screen with no word for it. */
            WaitFrom(GetTickCount64());
            Say(SAY_FP_SCANNING, "第一人称已开启，正在扫描头部并隐藏…",
                SAY_RGB_BUSY, SAY_WAIT_MS);
        } else if (g_wantHide) {
            Say(SAY_FP_HIDDEN, "第一人称已开启，头部已隐藏",
                SAY_RGB_DONE, SH_TOAST_MS_DEFAULT);
        } else {
            Say(SAY_FP_NOHIDE, "第一人称已开启", SAY_RGB_PLAIN,
                SH_TOAST_MS_DEFAULT);
        }
    } else {
        g_on = 0;
        g_headAway = 0;
        /* Turning first person off is a change of view, not a
         * camera handed to the engine for an aim, so the grace
         * that keeps the head hidden across an aim has to go
         * with it. */
        if (g_handoverClear) g_handoverClear();
        ShowHead();
        if (g_setBlur) g_setBlur(1);
        Hold(0);
        WaitEnd();
        Say(SAY_TP, "第三人称已开启", SAY_RGB_PLAIN,
            SH_TOAST_MS_DEFAULT);
    }
    /* The Enabled row shows the state a hotkey may have just
     * changed behind the menu's back; sync it so the next
     * capture renders the truth. */
    if (g_menuSetValue && g_menu)
        g_menuSetValue(g_menu, "Enabled", g_on);
    Report();
}

/* The callbacks must not touch the player: resolving it and
 * hiding the head can each fall back to a full address space
 * scan, which would stall the menu callback thread for
 * seconds. The tick thread does that work on its own clock.
 */
static void OnToggle(uint32_t menu, uint32_t item, int value,
                     void *user) {
    (void)menu; (void)item; (void)user;
    SetFp(value);
}

/* Hotkey row: pick which key flips the view. Index 0 is
 * "None", which disables the hotkey entirely. */
static void OnHotKey(uint32_t menu, uint32_t item, int value,
                     void *user) {
    (void)menu; (void)item; (void)user;
    if (value >= 0 && value < HOTKEYS) g_hotKey = value;
    SaveIni();
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
        WaitEnd();
        if (g_on)
            Say(SAY_FP_NOHIDE, "第一人称已开启", SAY_RGB_PLAIN,
                SH_TOAST_MS_DEFAULT);
    } else if (g_on) {
        if (EngineHidesHead()) {
            Say(SAY_FP_HIDDEN, "第一人称已开启，头部已隐藏",
                SAY_RGB_DONE, SH_TOAST_MS_DEFAULT);
        } else {
            WaitFrom(GetTickCount64());
            Say(SAY_FP_SCANNING, "第一人称已开启，正在扫描头部并隐藏…",
                SAY_RGB_BUSY, SAY_WAIT_MS);
        }
    }
    Report();
}

/* Each category owns its own three sliders; user carries the
 * category and the axis (cat*3 + 0 X, +1 Y, +2 Z). */
static void OnCatSlide(uint32_t menu, uint32_t item, int value,
                          void *user) {
    int code = (int)(intptr_t)user;
    int set = code / 3, axis = code % 3;
    (void)menu; (void)item;

    if (set < 0 || set >= CAT_COUNT) return;
    if (axis == 0)      g_catX[set] = (float)value;
    else if (axis == 1) g_catY[set] = (float)value;
    else if (axis == 2) g_catZ[set] = (float)value;
    else return;
    SaveIni();
    /* Only while we already own it, or dragging a slider would
     * take the camera back during a screen - and only for the
     * set actually in force, so tuning a helicopter seat does
     * not move the eye while walking. */
    if (g_on && g_held && g_presetSel == PRESET_AUTO && set == g_cat)
        PushCamera();
    Report();
}

/* Each preset owns its own sliders; user carries the preset and
 * the axis (preset*3 + 0 X, +1 Y, +2 Z). */
static void OnPresetSlide(uint32_t menu, uint32_t item, int value,
                          void *user) {
    int code = (int)(intptr_t)user;
    int set = code / 3, axis = code % 3;
    (void)menu; (void)item;

    if (set < 0 || set >= PRESET_COUNT) return;
    if (axis == 0)      g_preX[set] = (float)value;
    else if (axis == 1) g_preY[set] = (float)value;
    else if (axis == 2) g_preZ[set] = (float)value;
    else return;
    SaveIni();
    if (g_on && g_held && g_presetSel == set) PushCamera();
    Report();
}

/* Preset list row: 0 = Auto (follow the categories), 1..4 =
 * force that preset whatever the world is doing. */
static void OnPresetList(uint32_t menu, uint32_t item, int value,
                         void *user) {
    (void)menu; (void)item; (void)user;
    g_presetSel = value - 1;
    if (g_presetSel < PRESET_AUTO) g_presetSel = PRESET_AUTO;
    if (g_presetSel >= PRESET_COUNT) g_presetSel = PRESET_COUNT - 1;
    SaveIni();
    if (g_on && g_held) PushCamera();
    Report();
}

/* How long an aim keeps the eye. The window lives in the
 * ScriptHook, which is the only place that sees the aim begin. */
static void OnSettle(uint32_t menu, uint32_t item, int value,
                     void *user) {
    (void)menu; (void)item; (void)user;
    g_settleMs = value;
    if (g_fpxSettle) g_fpxSettle((uint32_t)g_settleMs);
    SaveIni();
}

/* ---- where the view stands ------------------------------- */

/* Paused counts as in game, but the player lookup falls
 * back to a heap scan while a menu is up. Nothing here is
 * urgent enough to pay for that, so the tick waits. */
static int Playing(void) {
    if (g_state) return g_state() == SH_STATE_INGAME;
    return g_inGame && g_inGame();
}

/* Whether the camera should stay ours. The pause menu, the map
 * and the loadout only cover the world: it is still there and
 * still ours to look through, and handing the camera back only
 * to take it again is what costs the frames where the view is
 * third person again. */
static int HoldThroughScreens(void) {
    int s;
    if (!g_state) return g_inGame ? g_inGame() : 0;
    s = g_state();
    return s == SH_STATE_INGAME || s == SH_STATE_PAUSED;
}

/* Views the engine drives itself - a drone, the binoculars, a
 * cutscene. The camera is genuinely not ours there and the
 * head is meant to show. */
static int EngineView(void) {
    int s;
    if (!g_state) return 0;
    s = g_state();
    return s == SH_STATE_DRONE || s == SH_STATE_BINOCULAR ||
           s == SH_STATE_CINEMATIC;
}

/* The name of the set in force, put through the menu's own
 * translation table so the row does not read half in one
 * language and half in another. */
static const char *ActiveSetName(void) {
    const char *name, *trans;

    if (g_presetSel >= 0 && g_presetSel < PRESET_COUNT)
        name = g_presetName[g_presetSel];
    else if (g_cat >= 0 && g_cat < CAT_COUNT)
        name = g_catName[g_cat];
    else
        return "";
    if (!g_langFor) return name;
    trans = g_langFor("First person", name);
    return trans ? trans : name;
}

/* The row is printed from a template, and the template is
 * translated before it is filled in - so the English below is a
 * key in [zh_cn.First person] rather than something the player
 * ever reads. The table supplies the whole line, placeholders
 * and all.
 */
static void SayOffset(const char *tmpl, const char *set, float x,
                      float y, float z) {
    char line[128];

    if (g_statusF) { g_statusF(g_menu, tmpl, set, x, y, z); return; }
    snprintf(line, sizeof(line), tmpl, set, x, y, z);
    g_status(g_menu, line);
}

static void SayParts(const char *tmpl, int parts, const char *set,
                     float x, float y, float z) {
    char line[128];

    if (g_statusF) {
        g_statusF(g_menu, tmpl, parts, set, x, y, z);
        return;
    }
    snprintf(line, sizeof(line), tmpl, parts, set, x, y, z);
    g_status(g_menu, line);
}

/* Which set is in force and what it says, so a change of stance
 * or a preset pick is visible on the row rather than only in the
 * view. */
#define STATUS_OFF     "off"
#define STATUS_BARE    "on, head left visible [%s %+.0f %+.0f %+.0f]"
#define STATUS_AWAY    "on, head shown (view taken) [%s %+.0f %+.0f %+.0f]"
#define STATUS_NOSITE  "on, no engine sites [%s %+.0f %+.0f %+.0f]"
#define STATUS_HIDDEN  "on, head hidden [%s %+.0f %+.0f %+.0f]"
#define STATUS_PARTS   "on, head hidden (%d parts) [%s %+.0f %+.0f %+.0f]"
#define STATUS_WAIT    "on, aim once to hide the head [%s %+.0f %+.0f %+.0f]"

static void Report(void) {
    float x, y, z;
    const char *set;

    if (!g_status) return;
    if (!g_on) {
        if (g_statusF) g_statusF(g_menu, STATUS_OFF);
        else           g_status(g_menu, STATUS_OFF);
        return;
    }

    ActiveOffset(&x, &y, &z);
    set = ActiveSetName();

    if (!g_wantHide)
        SayOffset(STATUS_BARE, set, x, y, z);
    else if (g_headAway)
        SayOffset(STATUS_AWAY, set, x, y, z);
    else if (!g_fpxUp)
        SayOffset(STATUS_NOSITE, set, x, y, z);
    else if (EngineHidesHead())
        SayOffset(STATUS_HIDDEN, set, x, y, z);
    else if (g_nparts > 0)
        SayParts(STATUS_PARTS, g_nparts, set, x, y, z);
    else
        SayOffset(STATUS_WAIT, set, x, y, z);
}

/* ---- the tick -------------------------------------------- */

static DWORD WINAPI TickThread(LPVOID p) {
    int said = 0, dPlay = -1;
    uint64_t lastBeat = 0;
    (void)p;

    for (;;) {
        uint64_t root;
        int playing, menu = 0, drone = 0, ads = 0, fresh = 0;
        uint64_t nowMs;

        Sleep(TICK_MS);
        nowMs = GetTickCount64();
        playing = Playing();
        if (playing != dPlay) {
            Diag("playing=%d", playing);
            dPlay = playing;
        }

        /* The gates are the engine's own bytes, patched in the
         * ScriptHook where the engine writes them. Reading them
         * here is only so the bar can say why the view went. */
        if (g_fpxUp && g_fpxGate)
            g_fpxGate(&menu, &drone, &ads, &fresh);

        if (nowMs - lastBeat >= 1000) {
            lastBeat = nowMs;
            float ax, ay, az;
            ActiveOffset(&ax, &ay, &az);
            Diag("beat: play=%d on=%d held=%d want=%d away=%d n=%d "
                 "menu=%d drone=%d ads=%d fresh=%d bow=%d age=%u "
                 "headok=%d cat=%d preset=%d off=%.0f,%.0f,%.0f",
                 playing, g_on, g_held, g_wantHide, g_headAway,
                 g_nparts, menu, drone, ads, fresh,
                 g_fpxBow ? g_fpxBow() : -1,
                 g_fpxAge ? g_fpxAge() : 0u,
                 g_fpxHeadOk ? g_fpxHeadOk() : -1,
                 g_cat, g_presetSel, ax, ay, az);
        }

        if (!g_on || !HoldThroughScreens()) {
            /* Not playing, or a screen is up: the camera goes
             * back and the head comes with it. */
            if (g_held || g_nparts) {
                ShowHead();
                Hold(0);
                g_headAway = 0;
                WaitEnd();
                Report();
            }
            continue;
        }

        Hold(1);

        /* Which set of offsets is in force. Only while the
         * choice is automatic, and only when the kind of moment
         * actually changes - the context read is cheap, but
         * pushing the camera is not something to do for nothing.
         */
        if (g_presetSel == PRESET_AUTO && g_inputCtx) {
            int c = CatFromCtx(g_inputCtx());
            if (c >= 0 && c != g_cat) {
                Diag("cat %s -> %s", g_catName[g_cat], g_catName[c]);
                g_cat = c;
                PushCamera();
                Report();
            }
        }

        if (!playing) continue;

        /* A menu, the drone or a cutscene is a view the engine
         * drives, and the head is meant to show in it. The
         * engine side already lets go of the camera there, so
         * all that is left is the head. */
        if (menu || drone || EngineView()) {
            if (!g_headAway) {
                g_headAway = 1;
                ShowHead();
                WaitEnd();
                Report();
                Diag("away: menu=%d drone=%d engine=%d",
                     menu, drone, EngineView());
            }
            continue;
        }
        if (g_headAway) {
            g_headAway = 0;
            Report();
        }

        if (!g_wantHide) continue;

        /* Where the engine can name the head it hides it
         * itself, every frame, from inside its own frame. Only
         * when it cannot does the scan run. */
        if (EngineHidesHead()) {
            if (g_nparts) {
                /* The two must never hold at once. */
                if (g_setVisible && g_hideRoot)
                    g_setVisible(g_hideRoot, 0, 1, 0);
                g_nparts = 0;
                g_hideRoot = 0;
            }
            if (g_said != SAY_FP_HIDDEN) {
                WaitEnd();
                Say(SAY_FP_HIDDEN, "第一人称已开启，头部已隐藏",
                    SAY_RGB_DONE, SH_TOAST_MS_DEFAULT);
                Report();
            }
            continue;
        }

        root = PlayerHeadEnt();
        if (!root) continue;
        /* A new body means the old parts are gone, so the hold
         * is dropped and armed again on the new one. */
        if (root != g_root) {
            ShowHead();
            if (g_headInvalidate) g_headInvalidate();
            g_root = root;
            g_nparts = 0;
            PushCamera();
            WaitFrom(nowMs);
            Say(SAY_FP_SWAP, "已切换角色，正在重新隐藏头部…",
                SAY_RGB_BUSY, SAY_WAIT_MS);
            Diag("body swap root=%p", (void *)(uintptr_t)root);
        }

        if (g_nparts == 0) {
            WaitFrom(nowMs);
            if (nowMs >= g_hideTry) {
                uint64_t cost = 0;
                int ok = HideHeadTimed(root, &cost);
                g_hideTry = nowMs +
                    (cost >= SWEEP_COST_MS ? SWEEP_STEP_MS
                                           : RESWEEP_RETRY_MS);
                if (ok) {
                    g_hideAt = nowMs;
                    Report();
                    said = 0;
                    WaitEnd();
                    Say(SAY_FP_HIDDEN, "第一人称已开启，头部已隐藏",
                        SAY_RGB_DONE, SH_TOAST_MS_DEFAULT);
                } else if (!said) {
                    Report();
                    said = 1;
                }
            }
        } else if (nowMs - g_hideAt >= REHIDE_MS) {
            uint64_t cost = 0;
            int ok = HideHeadTimed(root, &cost);
            g_hideAt = nowMs;
            if (!ok) {
                g_nparts = 0;
                WaitFrom(nowMs);
                Say(SAY_FP_REHIDE, "头部重新出现，正在再次隐藏…",
                    SAY_RGB_BUSY, SAY_WAIT_MS);
            }
        }
    }
    return 0;
}

/* The flip hotkey lives on its own thread. Hiding the head
 * can sweep the whole heap while the group the game creates
 * on the first aim has not appeared yet - tens of seconds in
 * which the tick thread is stuck - and the flip must answer
 * regardless. SetFp is safe from here: it only flips g_on
 * and queues camera/visibility state for the game thread.
 */
static DWORD WINAPI HotkeyThread(LPVOID p) {
    int down, prev = 0;
    uint64_t at = 0;
    (void)p;

    for (;;) {
        Sleep(30);
        /* Playing state, not in the ScriptHook menu. The game's
         * own pause screens keep playing true. */
        if (g_hotKey > 0 && Playing() &&
            (!g_menuIsOpen || !g_menuIsOpen())) {
            down = (GetAsyncKeyState(g_hotVk[g_hotKey]) &
                    0x8000) != 0;
            if (down && !prev && GetTickCount64() - at >= 300u) {
                at = GetTickCount64();
                Diag("hotkey flip -> fp=%d", !g_on);
                SetFp(!g_on);
            }
            prev = down;
        } else {
            prev = 0;
        }
    }
    return 0;
}

/* ---- settings -------------------------------------------- */

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
    char *end;
    double v;
    if (!GetPrivateProfileStringA("Settings", key, "", buf,
                                  sizeof(buf), path))
        return def;
    if (!buf[0]) return def;
    /* atof("abc") is 0, which silently zeroed an offset;
     * require the value to parse. */
    v = strtod(buf, &end);
    if (end == buf) return def;
    return (float)v;
}

static float ClampF(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* One key per axis per set: <tag>_x_cm, <tag>_y_cm, <tag>_z_cm.
 * The tag is the category (foot, land, plane, heli, rider) or
 * the preset (preset1 .. preset4). */
static void OffsetKey(char *key, size_t n, const char *tag, int axis) {
    static const char ax[3] = { 'x', 'y', 'z' };
    snprintf(key, n, "%s_%c_cm", tag, ax[axis]);
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

static void LoadIni(void) {
    int i;
    char key[40];

    if (!g_iniPath[0]) return;
    g_wantHide = IniBool(g_iniPath, "hide_head", g_wantHide);
    g_diagOn = IniBool(g_iniPath, "diag", 0);
    g_engineCam = IniBool(g_iniPath, "engine_camera", 1);
    g_extras = IniInt(g_iniPath, "engine_extras", 15);
    if (g_extras < 0) g_extras = 0;
    if (g_extras > 15) g_extras = 15;
    ResetOffsets();
    for (i = 0; i < CAT_COUNT; i++) {
        OffsetKey(key, sizeof(key), g_catTag[i], 0);
        g_catX[i] = ClampF(IniFloat(g_iniPath, key, OFF_DEF),
                           OFF_MIN, OFF_MAX);
        OffsetKey(key, sizeof(key), g_catTag[i], 1);
        g_catY[i] = ClampF(IniFloat(g_iniPath, key, OFF_DEF),
                           OFF_MIN, OFF_MAX);
        OffsetKey(key, sizeof(key), g_catTag[i], 2);
        g_catZ[i] = ClampF(IniFloat(g_iniPath, key, OFF_DEF),
                           OFF_MIN, OFF_MAX);
    }
    for (i = 0; i < PRESET_COUNT; i++) {
        OffsetKey(key, sizeof(key), g_presetTag[i], 0);
        g_preX[i] = ClampF(IniFloat(g_iniPath, key, OFF_DEF),
                           OFF_MIN, OFF_MAX);
        OffsetKey(key, sizeof(key), g_presetTag[i], 1);
        g_preY[i] = ClampF(IniFloat(g_iniPath, key, OFF_DEF),
                           OFF_MIN, OFF_MAX);
        OffsetKey(key, sizeof(key), g_presetTag[i], 2);
        g_preZ[i] = ClampF(IniFloat(g_iniPath, key, OFF_DEF),
                           OFF_MIN, OFF_MAX);
    }
    /* preset_active: 0 = Auto (default), 1..4 = force preset N. */
    {
        int p = IniInt(g_iniPath, "preset_active", 0);
        g_presetSel = p <= 0 ? PRESET_AUTO : p - 1;
        if (g_presetSel >= PRESET_COUNT) g_presetSel = PRESET_AUTO;
    }
    g_settleMs = IniInt(g_iniPath, "settle_ms", g_settleMs);
    if (g_settleMs < (int)SETTLE_MIN) g_settleMs = (int)SETTLE_MIN;
    if (g_settleMs > (int)SETTLE_MAX) g_settleMs = (int)SETTLE_MAX;
    g_hotKey = IniInt(g_iniPath, "hotkey_key", g_hotKey);
    if (g_hotKey < 0 || g_hotKey >= HOTKEYS) g_hotKey = 0;
}

/* Write the current settings back to <name>.ini. "Enabled" is
 * a live state, not a setting, so it is deliberately not saved
 * and always starts off. */
static void SaveIni(void) {
    char buf[64], key[40];
    int i;

    if (!g_iniPath[0]) return;
    snprintf(buf, sizeof(buf), "%d", g_wantHide);
    WritePrivateProfileStringA("Settings", "hide_head", buf,
                               g_iniPath);
    for (i = 0; i < CAT_COUNT; i++) {
        OffsetKey(key, sizeof(key), g_catTag[i], 0);
        snprintf(buf, sizeof(buf), "%.1f", g_catX[i]);
        WritePrivateProfileStringA("Settings", key, buf, g_iniPath);
        OffsetKey(key, sizeof(key), g_catTag[i], 1);
        snprintf(buf, sizeof(buf), "%.1f", g_catY[i]);
        WritePrivateProfileStringA("Settings", key, buf, g_iniPath);
        OffsetKey(key, sizeof(key), g_catTag[i], 2);
        snprintf(buf, sizeof(buf), "%.1f", g_catZ[i]);
        WritePrivateProfileStringA("Settings", key, buf, g_iniPath);
    }
    for (i = 0; i < PRESET_COUNT; i++) {
        OffsetKey(key, sizeof(key), g_presetTag[i], 0);
        snprintf(buf, sizeof(buf), "%.1f", g_preX[i]);
        WritePrivateProfileStringA("Settings", key, buf, g_iniPath);
        OffsetKey(key, sizeof(key), g_presetTag[i], 1);
        snprintf(buf, sizeof(buf), "%.1f", g_preY[i]);
        WritePrivateProfileStringA("Settings", key, buf, g_iniPath);
        OffsetKey(key, sizeof(key), g_presetTag[i], 2);
        snprintf(buf, sizeof(buf), "%.1f", g_preZ[i]);
        WritePrivateProfileStringA("Settings", key, buf, g_iniPath);
    }
    snprintf(buf, sizeof(buf), "%d",
             g_presetSel >= 0 ? g_presetSel + 1 : 0);
    WritePrivateProfileStringA("Settings", "preset_active", buf,
                               g_iniPath);
    snprintf(buf, sizeof(buf), "%d", g_settleMs);
    WritePrivateProfileStringA("Settings", "settle_ms", buf,
                               g_iniPath);
    snprintf(buf, sizeof(buf), "%d", g_hotKey);
    WritePrivateProfileStringA("Settings", "hotkey_key", buf,
                               g_iniPath);
}

/* ---- bind ------------------------------------------------ */

static DWORD WINAPI BindThread(LPVOID p) {
    HMODULE m = NULL;
    MenuCreate_t menuCreate;
    MenuSub_t   menuSub;
    MenuToggle_t menuToggle;
    MenuNumber_t menuNumber;
    MenuList_t  menuList;
    MenuHint_t  menuHint;
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
    *(FARPROC *)&g_viewMode = GetProcAddress(m, "ShCameraViewMode");
    /* Optional: the status line. An older dinput8 without it
     * simply says nothing. */
    *(FARPROC *)&g_toastEx = GetProcAddress(m, "ShToastEx");
    *(FARPROC *)&g_toastSet = GetProcAddress(m, "ShToastSet");
    *(FARPROC *)&g_handoverClear =
        GetProcAddress(m, "ShCameraHandoverClear");
    /* Optional: an older dinput8 just keeps the blur. */
    *(FARPROC *)&g_setBlur = GetProcAddress(m, "ShSetCameraBlur");
    *(FARPROC *)&g_inputCtx = GetProcAddress(m, "ShInputContext");
    *(FARPROC *)&g_menuIsOpen = GetProcAddress(m, "ShMenuIsOpen");
    *(FARPROC *)&menuCreate = GetProcAddress(m, "ShMenuCreate");
    *(FARPROC *)&menuSub = GetProcAddress(m, "ShMenuSub");
    *(FARPROC *)&menuToggle = GetProcAddress(m, "ShMenuToggle");
    *(FARPROC *)&menuNumber = GetProcAddress(m, "ShMenuNumber");
    *(FARPROC *)&menuList = GetProcAddress(m, "ShMenuList");
    *(FARPROC *)&menuHint = GetProcAddress(m, "ShMenuHint");
    *(FARPROC *)&g_menuSetValue =
        GetProcAddress(m, "ShMenuSetValue");
    *(FARPROC *)&g_status = GetProcAddress(m, "ShMenuStatus");
    *(FARPROC *)&g_statusF = GetProcAddress(m, "ShMenuStatusF");
    *(FARPROC *)&g_langFor = GetProcAddress(m, "ShLangFor");

    /* The engine side of the view. Missing from an older
     * dinput8, and that is fine: the camera then falls back to
     * the placement it has always had. */
    *(FARPROC *)&g_fpxInstall = GetProcAddress(m, "ShFp2Install");
    *(FARPROC *)&g_fpxExtras =
        GetProcAddress(m, "ShFp2InstallExtras");
    *(FARPROC *)&g_fpxReady = GetProcAddress(m, "ShFp2Ready");
    *(FARPROC *)&g_fpxMissing = GetProcAddress(m, "ShFp2Missing");
    *(FARPROC *)&g_fpxEnable = GetProcAddress(m, "ShFp2Enable");
    *(FARPROC *)&g_fpxSetOff = GetProcAddress(m, "ShFp2SetOffset");
    *(FARPROC *)&g_fpxSettle = GetProcAddress(m, "ShFp2Settle");
    *(FARPROC *)&g_fpxGate = GetProcAddress(m, "ShFp2Gate");
    *(FARPROC *)&g_fpxHeadOk = GetProcAddress(m, "ShFp2HeadOk");
    *(FARPROC *)&g_fpxHeadShow = GetProcAddress(m, "ShFp2HeadShow");
    *(FARPROC *)&g_fpxBow = GetProcAddress(m, "ShFp2Bow");
    *(FARPROC *)&g_fpxAge = GetProcAddress(m, "ShFp2Age");

    if (!g_inGame || !g_getPlayer || !g_fp || !g_release) return 1;
    if (!g_headNodes || !g_setVisible) return 1;
    if (!menuCreate || !menuToggle || !menuNumber || !g_status)
        return 1;

    if (g_fpxInstall) g_fpxInstall();
    g_fpxUp = g_fpxReady && g_fpxReady();
    ResolveIniPath(m);
    LoadIni();
    /* The body and shoulder patches are the ones that change the
     * engine whether first person is on or not, so they are off
     * until asked for: [Settings] engine_extras=1. */
    if (g_extras && g_fpxExtras) g_fpxExtras((uint32_t)g_extras);
    Diag("bind: fpx=%d miss=%03x extras=%d", g_fpxUp,
         g_fpxMissing ? (unsigned)g_fpxMissing() : 0u, g_extras);
    g_menu = menuCreate("First person");
    menuToggle(g_menu, "Enabled", 0, OnToggle, NULL);
    /* One row picks the flip key: None (=off), =, F2 or F3.
     * The row's label is the English lookup key, translated by
     * the [zh_cn.First person] table. */
    if (menuList)
        menuList(g_menu, "View toggle hotkey", g_hotName,
                 HOTKEYS, g_hotKey, OnHotKey, NULL);
    menuToggle(g_menu, "Hide head", g_wantHide, OnHide, NULL);
    /* How long an aim keeps the eye before the engine's own aim
     * camera takes over. */
    if (menuNumber)
        menuNumber(g_menu, "ADS settle ms", (float)g_settleMs,
                   SETTLE_MIN, SETTLE_MAX, SETTLE_STEP, OnSettle,
                   NULL);
    /* Which set of offsets is in force: Auto follows what the
     * player is doing, a preset holds one set regardless. */
    if (menuList) {
        static const char *opts[PRESET_COUNT + 1];
        int i;
        opts[0] = "Auto";
        for (i = 0; i < PRESET_COUNT; i++)
            opts[i + 1] = g_presetName[i];
        menuList(g_menu, "Preset", opts, PRESET_COUNT + 1,
                 g_presetSel + 1, OnPresetList, NULL);
    }
    if (menuNumber) {
        int i;

        /* One submenu per kind of moment, then one per preset:
         * each holds its own X, Y and Z. */
        if (!menuSub) {
            /* No submenus in an older ScriptHook: the on foot
             * set is the one that matters most, so it lives on
             * the root rather than not at all. */
            menuNumber(g_menu, "X cm", g_catX[CAT_FOOT],
                       OFF_MIN, OFF_MAX, OFF_STEP, OnCatSlide,
                       (void *)(intptr_t)(CAT_FOOT * 3 + 0));
            menuNumber(g_menu, "Y cm", g_catY[CAT_FOOT],
                       OFF_MIN, OFF_MAX, OFF_STEP, OnCatSlide,
                       (void *)(intptr_t)(CAT_FOOT * 3 + 1));
            menuNumber(g_menu, "Z cm", g_catZ[CAT_FOOT],
                       OFF_MIN, OFF_MAX, OFF_STEP, OnCatSlide,
                       (void *)(intptr_t)(CAT_FOOT * 3 + 2));
        }
        for (i = 0; menuSub && i < CAT_COUNT; i++) {
            uint32_t sub = menuSub(g_menu, g_catName[i]);
            /* A slot ran out rather than anything being wrong
             * with the row, so say which one went missing. */
            if (!sub) { Diag("no submenu: %s", g_catName[i]); continue; }
            menuNumber(sub, "X cm", g_catX[i],
                       OFF_MIN, OFF_MAX, OFF_STEP, OnCatSlide,
                       (void *)(intptr_t)(i * 3 + 0));
            menuNumber(sub, "Y cm", g_catY[i],
                       OFF_MIN, OFF_MAX, OFF_STEP, OnCatSlide,
                       (void *)(intptr_t)(i * 3 + 1));
            menuNumber(sub, "Z cm", g_catZ[i],
                       OFF_MIN, OFF_MAX, OFF_STEP, OnCatSlide,
                       (void *)(intptr_t)(i * 3 + 2));
        }
        for (i = 0; menuSub && i < PRESET_COUNT; i++) {
            uint32_t sub = menuSub(g_menu, g_presetName[i]);
            if (!sub) { Diag("no submenu: %s", g_presetName[i]); continue; }
            menuNumber(sub, "X cm", g_preX[i],
                       OFF_MIN, OFF_MAX, OFF_STEP, OnPresetSlide,
                       (void *)(intptr_t)(i * 3 + 0));
            menuNumber(sub, "Y cm", g_preY[i],
                       OFF_MIN, OFF_MAX, OFF_STEP, OnPresetSlide,
                       (void *)(intptr_t)(i * 3 + 1));
            menuNumber(sub, "Z cm", g_preZ[i],
                       OFF_MIN, OFF_MAX, OFF_STEP, OnPresetSlide,
                       (void *)(intptr_t)(i * 3 + 2));
        }
    }
    if (menuHint)
        menuHint(g_menu,
                 "First-person view: hide the head, and tune the "
                 "eye offset per stance or vehicle.");
    Report();

    {
        HANDLE h;
        h = CreateThread(NULL, 0, TickThread, NULL, 0, NULL);
        if (h) CloseHandle(h);
        h = CreateThread(NULL, 0, HotkeyThread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE h;
        g_inst = inst;
        DisableThreadLibraryCalls(inst);
        h = CreateThread(NULL, 0, BindThread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
