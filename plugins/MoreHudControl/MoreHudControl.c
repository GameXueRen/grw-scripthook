/* More HUD Control - presets, marker factions, the minimap and the map.
 *
 * The old shape was one switch per game HUD layer, ninety of them, flat. This
 * shape is six presets over the things a player actually changes, with the
 * long tail of layers kept on an advanced page.  (renamed from HudControl)
 *
 * The framework's ShGameHudShow is all or nothing: every HUD_* scene goes at
 * once. ShGameHudSceneShow (added beside it) takes a scene name, so this
 * plugin can give the player a switch for the minimap alone, the crosshair
 * alone, and so on - each one takes effect on the next frame, because the
 * framework decides per scene inside its render hook.
 *
 * No file in the game lists the scene names: they are phoenix scenes created by
 * the game's own UI code. Three sources were used to get as close to complete
 * as the data allows, and all three are in the roster below:
 *
 *   - the 53 creators registered in GR_BasicHUD / GR_CoopHUD /
 *     GR_BenchmarkViewportHUD, read out of the bootstrap container - a full
 *     census of the elements the campaign actually has;
 *   - the top level names in the running process's string pool;
 *   - the HUD widget families in the game's UI archives (DataPC_extra and the
 *     DLC forges). A layer's scene and its root widget carry the same name -
 *     HUD_Minimap is both - so a family that exists in the data is evidence of
 *     a layer even when no creator claims it. That is where the Ghost War set
 *     (HUD_Adversarial_*, HUD_ADV_*) and the debuff / jammer / connection
 *     layers come from: none of them is in a view, because none of them is a
 *     campaign element.
 *
 * A row therefore exists before its layer has ever drawn, which is what lets a
 * turret, a drone or a Ghost War layer be switched before the player has been
 * in one. ShGameHudScenes then reports what really renders, and a preset row is
 * marked the first time its layer draws; F9 dumps the ones that never did, and
 * skip= takes those out of the menu.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
/* id -> icon kind, generated from the game's archives by
 * tools/gen_hud_icon_ids.py. Regenerate after a game update. */
#include "hud_icon_ids.h"
/* faction -> RGB of the 3D marker, read out of the game's own marker style
 * resources (HUD_Marker_Red and friends) by
 * tools/gen_hud_marker_colours.py. */
#include "hud_marker_colours.h"

#define HC_MAX      192          /* switches; the roster is 72, the rest is room
                                  * for layers the game turns out to have that
                                  * this build did not know about */
#define HC_NAME     56           /* "HUD_Adversarial_WeaponItemDisplayObserver"
                                  * is 41; the framework's own name limit is 47 */
#define HC_POLL_MS  400
#define HC_LIST     8192         /* the framework's scene list, comma separated:
                                  * 128 names of up to 40 bytes do not fit in 2K */

typedef int (*FnScenes)(char *, int);
typedef int (*FnAllScenes)(char *, int);
typedef int (*FnShow)(const char *, int);
typedef int (*FnVisible)(const char *);
typedef uint32_t (*FnMenuCreate)(const char *);
typedef int (*FnToggle)(uint32_t, const char *, int, ShMenuFn, void *);
typedef int (*FnAction)(uint32_t, const char *, ShMenuFn, void *);
typedef int (*FnHint)(uint32_t, const char *);
typedef int (*FnNumber)(uint32_t, const char *, float, float, float, float,
                        ShMenuFn, void *);
typedef int (*FnList)(uint32_t, const char *, const char *const *, int, int,
                      ShMenuFn, void *);
typedef int (*FnLogPath)(const char *, char *, int);
typedef uint32_t (*FnMenuSub)(uint32_t, const char *);
typedef int (*FnSceneCount)(void);
typedef uint64_t (*FnSceneAt)(int);
typedef int (*FnSceneName)(uint64_t, char *, int);
typedef uint64_t (*FnSceneRoot)(uint64_t);
/* the widget tree, for the icon filter below */
typedef int (*FnChildCount)(uint64_t);
typedef uint64_t (*FnChildAt)(uint64_t, int);
typedef int (*FnClass)(uint64_t, char *, int);
typedef int (*FnGetS)(uint64_t, uint32_t, char *, int);
typedef int (*FnGetF)(uint64_t, uint32_t, float *);
typedef int (*FnGetV)(uint64_t, uint32_t, float *, int);
typedef int (*FnSetF)(uint64_t, uint32_t, float);
typedef int (*FnSetU)(uint64_t, uint32_t, uint32_t);
typedef int (*FnGetU)(uint64_t, uint32_t, uint32_t *);
typedef int (*FnSetV)(uint64_t, uint32_t, const float *, int);
typedef int (*FnFrameCb)(void (*)(void *), void *);
/* Framework config for a plugin's own ini (SH_API, 2026-10-23). Keys are
 * written one at a time into the section that holds them; the framework keeps
 * the rest of the file - other keys, other sections, comments - byte for byte,
 * so this plugin never parses or rewrites the file itself. */
typedef int (*FnPcfgGetStr)(const char *, const char *, const char *,
                            const char *, char *, int);
typedef int (*FnPcfgGetInt)(const char *, const char *, const char *, int);
typedef int (*FnPcfgSetStr)(const char *, const char *, const char *,
                            const char *);
typedef int (*FnPcfgSetInt)(const char *, const char *, const char *, int);
typedef int (*FnMenuSetValue)(uint32_t, const char *, int);
typedef int (*FnMenuDisable)(uint32_t, const char *, int);
/* The engine's own property table, walkable: one record per (class, id) with
 * its type and accessor addresses. Used by the property dump to answer "what
 * does this class actually expose" from data. */
typedef int (*FnPropCount)(void);
typedef int (*FnPropAt)(int, char *, int, uint32_t *, int *, uint64_t *,
                        uint64_t *);

static FnScenes     pScenes;
static FnAllScenes  pAllScenes;
static FnShow       pShow;
static FnVisible    pVisible;
static FnMenuCreate pMenuCreate;
static FnToggle     pMenuToggle;
static FnAction     pMenuAction;
static FnHint       pMenuHint;
static FnNumber     pMenuNumber;
static FnList       pMenuList;
static FnLogPath    pLogPath;
static FnMenuSub    pMenuSub;
static FnSceneCount pSceneCount;
static FnSceneAt    pSceneAt;
static FnSceneName  pSceneName;
static FnSceneRoot  pSceneRoot;
static FnChildCount pChildCount;
static FnChildAt    pChildAt;
static FnClass      pWidgetClass;
static FnGetS       pWidgetGetS;
static FnGetF       pWidgetGetF;
static FnGetV       pWidgetGetV;
static FnSetF       pWidgetSetF;
static FnSetU       pWidgetSetU;
static FnGetU       pWidgetGetU;
static FnSetV       pWidgetSetV;
static FnFrameCb    pFrameRegister;
static FnPcfgGetStr pCfgGetStr;
static FnPcfgGetInt pCfgGetInt;
static FnPcfgSetStr pCfgSetStr;
static FnPcfgSetInt pCfgSetInt;
static FnMenuSetValue pMenuSetValue;
static FnMenuDisable  pMenuDisable;
static FnPropCount pPropCount;
static FnPropAt    pPropAt;

static HINSTANCE g_inst;
static char      g_owner[64];   /* module base name: the text owner */
static char      g_dir[MAX_PATH];
static FILE     *g_log;
static volatile LONG g_logBusy;
static int       g_logOn = 1;

/* names the ini asked to hide before the scene ever drew */

/* every HUD scene this session has drawn, in first-seen order */
static char g_name[HC_MAX][HC_NAME];
/* Layers a switch asked to hide before the layer had ever drawn. LayerWant
 * parks them here so a row works on the frame its layer first appears;
 * AddScene consults it when that happens. */
static char g_hidePending[HC_MAX][HC_NAME];
static int  g_nHidePending;
static volatile LONG g_nSeen;
static int  g_want[HC_MAX];     /* 1 = shown, 0 = hidden */
static int  g_drawn[HC_MAX];    /* has this layer ever actually rendered? */

/* names the ini adds on top of the compiled roster (known= / extra=) */

/* names the ini wants out of the menu entirely: preset rows whose layer never
 * drew, once F9 has said so. Skipped means "not a HUD layer as far as this
 * plugin is concerned" - no row, and no override either. */

/* preset=0: build the menu from what the session draws instead of the
 * compiled roster. For anyone who prefers a short list over a complete one. */
static int   g_preset = 1;

/* The 3D marker switches: one bit per colour style, plus the "any of them"
 * flag the frame callback watches. Declared up here because LoadConfig reads
 * markers_off= before the marker code below is even defined. */
static volatile LONG g_marksOn;
static volatile LONG g_markOff;
/* minimap opacity / scale, in percent; 100 = untouched */
static int g_mmAlpha = 100;
static int g_mmScale = 100;
static int g_mmTold;
/* Set when the alpha write has been refused: says so once, in words, because
 * "(set=0)" on a line that is itself suppressed by g_mmTold was a diagnostic
 * that could not report the one failure it existed for. */
static int g_mmFailTold;
/* Passes to wait before reading the layout back. Writing SCALE is picked up by
 * the engine's layout pass, so a read on the same pass would show the value
 * from before; four passes later it shows what the engine actually did. */
static int g_mmVerify;

/* Discrete steps, not a free number: a percentage typed blindly is harder to
 * reason about than labelled steps, and the framework's numeric row turned out
 * not to be created at all (ShMenuNumber returned 0 every time), while list
 * rows are the shape its own menus already use.
 *
 * Opacity 20..100 and scale 50..200, both in fives. The scale band used to stop
 * at 70..130 because 50 and 200 lost the map - but that was measured while the
 * scale was still applied to the 1920x1080 canvas instead of to the map's own
 * container. Now that the anchor corner stays put, the band is opened up; if an
 * extreme still pushes the map out of sight, that is a bug to chase, not a
 * reason to hide the setting. */
#define MM_ALPHA_LO   20
#define MM_ALPHA_STEP 5
#define MM_ALPHA_N    17            /* (100 - 20) / 5 + 1 */
#define MM_SCALE_LO   50
#define MM_SCALE_STEP 5
#define MM_SCALE_N    51            /* (300 - 50) / 5 + 1 */

/* The labels are built once instead of being written out as forty-eight
 * literals; ShMenuList keeps the pointers, so the storage has to be static. */
static char        g_mmAlphaTxt[MM_ALPHA_N][8];
static char        g_mmScaleTxt[MM_SCALE_N][8];
static const char *g_mmAlphaOpts[MM_ALPHA_N];
static const char *g_mmScaleOpts[MM_SCALE_N];

static void BuildMmLabels(void) {
    int i;

    for (i = 0; i < MM_ALPHA_N; i++) {
        _snprintf(g_mmAlphaTxt[i], sizeof(g_mmAlphaTxt[i]), "%d%%",
                  MM_ALPHA_LO + i * MM_ALPHA_STEP);
        g_mmAlphaOpts[i] = g_mmAlphaTxt[i];
    }
    for (i = 0; i < MM_SCALE_N; i++) {
        _snprintf(g_mmScaleTxt[i], sizeof(g_mmScaleTxt[i]), "%d%%",
                  MM_SCALE_LO + i * MM_SCALE_STEP);
        g_mmScaleOpts[i] = g_mmScaleTxt[i];
    }
}

/* debug_dump=1 brings back the per-widget dumps (F9 child/ProbeTree/ProbeObjects
 * and the per-marker lines). They were how the marker tree was mapped out; for
 * normal play they are noise, so they are off unless asked for. */
static int g_dbg;

static uint32_t g_menu;

/* Which page owns a scene. Declared up here because the icon filter - and the
 * per-context rules inside it - are compiled long before the scene helpers. */
enum { HC_CTX_NONE = 0, HC_CTX_WORLD, HC_CTX_MINIMAP, HC_CTX_FX };

static int  ContainsCI(const char *hay, const char *needle);
static void IconHitsLog(void);
static void SafeMarkerCollapseAll(void);
static void SafeApplyMinimapFx(void);
static int  MarkerMaskFromIni(char *v);
static void LoadIconMode(const char *v);
static int  SafeRead(void *dst, uint64_t src, int n);

/* ---- log ---------------------------------------------------------------- */

static void Log(const char *fmt, ...) {
    va_list ap;
    char line[1024];
    SYSTEMTIME st;

    if (!g_logOn || !g_log) return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    while (InterlockedExchange(&g_logBusy, 1)) Sleep(1);
    if (g_log) {
        GetLocalTime(&st);
        fprintf(g_log, "%02u:%02u:%02u.%03u %s\n", st.wHour, st.wMinute,
                st.wSecond, st.wMilliseconds, line);
        fflush(g_log);
    }
    InterlockedExchange(&g_logBusy, 0);
}

static void OpenLog(void) {
    char path[MAX_PATH];

    if (pLogPath && pLogPath("MoreHudControl.log", path, sizeof(path)) && path[0]) {
        g_log = fopen(path, "a");
        if (g_log) return;
    }
    if (!g_dir[0]) return;
    _snprintf(path, sizeof(path), "%s\\logs\\MoreHudControl.log", g_dir);
    g_log = fopen(path, "a");
}

/* ---- ini ---------------------------------------------------------------- */

static char *Trim(char *s) {
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                     e[-1] == '\n')) *--e = 0;
    return s;
}


/* One comma list into one table. Kept out of LoadConfig because four keys now
 * carry lists (hidden, extra/known, skip) and the same three lines four times
 * is how one of them ends up reading into the wrong table. */

/* Icon kinds that are hidden only because a 3D marker faction was hidden. Kept
 * apart from g_iconOff so the player's own icon choices are never rewritten:
 * this mask is recomputed from the marker mask and OR-ed in at test time. */
/* marker_hide=: which properties the marker switch writes. Measured 2026-10-23:
 * VISIBLE alone (0x4) hides both the 3D markers and the minimap blips, so that
 * is the default; scale is kept as an option for the day a game build stops
 * honouring the flag. The bits are the MODE_* ones defined further down. */
/* marker_hide= is gone: a marker is hidden by clearing VISIBLE, and that is the
 * only property the plugin writes. The key is still read (see below) so an old
 * ini does not look broken, and a line says it is ignored. */
/* minimap_dots=: hide the enemy markers on the minimap by faction colour.
 *
 * The first attempt at this (2026-10-22) hid the DETECTION CLOUDS instead: those
 * are two-child containers whose first child is an 8x halo. The markers are a
 * different shape - nine-child containers with a two-child colour owner on top -
 * and live in the second HUD_MinimapIcons scene. On by default since the shape
 * was measured; set 0 to leave the minimap completely alone. */
static int g_mapDots = 1;
/* marker_deep=1 hides a marker by walking its whole subtree (the old way, ~30
 * writes). The default hides just the marker's root container: the renderer
 * inherits visibility, so one write does the job - and if a future build stops
 * inheriting it, this switch is the way back without a rebuild. */


/* ---- what the menu edits: one state, six slots --------------------------
 *
 * The menu used to be the state: every row owned its own variable and every
 * one of them was written back to the ini by hand. That is why "six preset
 * slots" was impossible - there was nothing to snapshot. Here the state is
 * one struct, the menu is a view of it, and a slot is one copy of it.
 *
 * Storage is the framework's plugin config (ShPluginConfig*): this file
 * contains no ini parser and no ini writer. One key per setting, in the
 * section that owns it - [main] for what is not per preset (which slot is in
 * use, the advanced page's hidden layers, the log switches), [presetN] for
 * everything the menu edits.
 */
#define HC_PLUGIN "MoreHudControl"
#define HC_SLOTS  6

typedef struct {
    unsigned char enemy, enemy_blind;   /* 1 = 盲区敌人照常显示（旧 enemy_view 的反） */
    unsigned char mm_bg, mm_icons;                /* scene-level, 1 = hidden */
    unsigned char mm_enemy;                       /* blips, by colour */
    unsigned char mm_vehicle, mm_collect;
    unsigned char hit_indicator;
    unsigned char wanted, tutorial;      /* 通缉等级 / 其他教程 */
    unsigned char oth_drone, oth_bino;
    unsigned short mm_scale, mm_alpha;
} HcState;

static HcState g_state;      /* in force right now */
static HcState g_saved;      /* what this slot's section already says */
static int     g_slotCur;    /* 0 .. HC_SLOTS-1 */

static void HcApplyState(void);             /* defined with the engine glue */

/* The new pages, made once by EnsureMenu() and used by the rows that need to
 * name their own menu (refresh, disabled, icons). */
static uint32_t g_pgRoot, g_pgMm, g_pgOth;

static int CfgGetInt(const char *sec, const char *key, int def) {
    if (!pCfgGetInt) return def;
    return pCfgGetInt(HC_PLUGIN, sec, key, def);
}

static void CfgPutInt(const char *sec, const char *key, int v) {
    if (pCfgSetInt) pCfgSetInt(HC_PLUGIN, sec, key, v);
}

static void CfgGetStr(const char *sec, const char *key, const char *def,
                      char *out, int n) {
    out[0] = 0;
    if (pCfgGetStr) pCfgGetStr(HC_PLUGIN, sec, key, def, out, n);
    else if (def) { strncpy(out, def, (size_t)n - 1); out[n - 1] = 0; }
}

static void CfgPutStr(const char *sec, const char *key, const char *v) {
    if (pCfgSetStr) pCfgSetStr(HC_PLUGIN, sec, key, v);
}

static const char *SlotSec(int n, char *buf, int size) {
    _snprintf(buf, (size_t)size, "preset%d", n + 1);
    buf[size - 1] = 0;
    return buf;
}

static int ClampI(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* A plain switch: 1 = shown, 0 = hidden, shown by default. The menu checkbox
 * reads the same way, so a fresh install has every box ticked and every layer
 * drawing - which is what a player expects a switch to mean. */
static int CfgOn(const char *sec, const char *key) {
    return ClampI(CfgGetInt(sec, key, 1), 0, 1);
}

static void HcReadSlot(HcState *s, int n) {
    char sec[16];

    SlotSec(n, sec, sizeof(sec));
    memset(s, 0, sizeof(*s));
    /* 1 = shown, 0 = hidden. The old three-state values still land on the
     * right side: 0 (on) and 1 (visible-only) mean shown, 2 means off. */
    s->enemy        = (unsigned char)(CfgGetInt(sec, "enemy", 0) == 2 ? 0 : 1);
    {
        /* 1 = 视野盲区的敌人照常显示（默认）；0 = 只显示视野内的。
         * 旧键 enemy_view 是反的（1 = 只显示视野内），这里换算一次。 */
        int blind = CfgGetInt(sec, "enemy_blind", -1);

        if (blind < 0) blind = CfgGetInt(sec, "enemy_view", 0) ? 0 : 1;
        s->enemy_blind = (unsigned char)ClampI(blind, 0, 1);
    }
    s->mm_bg        = (unsigned char)CfgOn(sec, "mm_bg");
    s->mm_icons     = (unsigned char)CfgOn(sec, "mm_icons");
    s->mm_enemy     = (unsigned char)(CfgGetInt(sec, "mm_enemy", 0) == 2 ? 0 : 1);
    s->mm_vehicle   = (unsigned char)CfgOn(sec, "mm_vehicle");
    s->mm_collect   = (unsigned char)CfgOn(sec, "mm_collect");
    s->hit_indicator = (unsigned char)CfgOn(sec, "hit_indicator");
    s->wanted       = (unsigned char)CfgOn(sec, "wanted");
    s->tutorial     = (unsigned char)CfgOn(sec, "tutorial");
    s->oth_drone  = (unsigned char)CfgOn(sec, "oth_drone");
    s->oth_bino   = (unsigned char)CfgOn(sec, "oth_bino");
    s->mm_scale = (unsigned short)ClampI(
        CfgGetInt(sec, "mm_scale", 100), MM_SCALE_LO,
        MM_SCALE_LO + (MM_SCALE_N - 1) * MM_SCALE_STEP);
    s->mm_alpha = (unsigned short)ClampI(
        CfgGetInt(sec, "mm_alpha", 100), MM_ALPHA_LO,
        MM_ALPHA_LO + (MM_ALPHA_N - 1) * MM_ALPHA_STEP);
}

/* Only the keys that moved are written: one switch used to mean thirty file
 * rewrites, and the framework does a temp-file-and-rename per key. One
 * changed switch is one rewrite. */
static void HcWriteSlot(const HcState *s, int n) {
    char sec[16];

    SlotSec(n, sec, sizeof(sec));
#define PUTB(f, k) do { if (s->f != g_saved.f) CfgPutInt(sec, k, s->f); } while (0)
    PUTB(enemy, "enemy");
    PUTB(enemy_blind, "enemy_blind");
    PUTB(mm_bg, "mm_bg");
    PUTB(mm_icons, "mm_icons");
    PUTB(mm_enemy, "mm_enemy");
    PUTB(mm_vehicle, "mm_vehicle");
    PUTB(mm_collect, "mm_collect");
    PUTB(hit_indicator, "hit_indicator");
    PUTB(wanted, "wanted");
    PUTB(tutorial, "tutorial");
    PUTB(oth_drone, "oth_drone");
    PUTB(oth_bino, "oth_bino");
#undef PUTB
    if (s->mm_scale != g_saved.mm_scale) CfgPutInt(sec, "mm_scale", s->mm_scale);
    if (s->mm_alpha != g_saved.mm_alpha) CfgPutInt(sec, "mm_alpha", s->mm_alpha);
    g_saved = *s;
}

/* One write and one read at startup, on a key whose value is already what we
 * write: if the framework cannot find this plugin's ini, the log says so here
 * instead of the settings silently never being saved. */
static void CfgSelfCheck(void) {
    char buf[32];

    CfgPutInt("main", "slot", g_slotCur + 1);
    CfgGetStr("main", "slot", "", buf, sizeof(buf));
    Log("config api: get=%d set=%d, write+read probe -> \"%s\" (expected \"%d\")",
        pCfgGetInt != 0, pCfgSetInt != 0, buf, g_slotCur + 1);
}

static void LoadConfig(void) {
    /* [main] - what is not per preset. */
    g_preset     = ClampI(CfgGetInt("main", "preset", 1), 0, 1);
    g_logOn      = CfgGetInt("main", "log", 0);
    g_dbg        = CfgGetInt("main", "debug_dump", 0) ? 1 : 0;
    /* marker_deep is retired: collapsing a marker is one scale write on its own
     * root now, so there is no deeper walk left to switch to. The key is still
     * read by nothing - an ini that has it simply carries a dead line. */
    g_slotCur    = ClampI(CfgGetInt("main", "slot", 1) - 1, 0, HC_SLOTS - 1);

    HcReadSlot(&g_state, g_slotCur);
    g_saved = g_state;
    HcApplyState();

    Log("config: slot=%d enemy=%d minimap=%d,%d mm_icons=%d mm_bg=%d log=%d",
        g_slotCur + 1, g_state.enemy,
        g_state.mm_alpha, g_state.mm_scale, g_state.mm_icons, g_state.mm_bg,
        g_logOn);
}

/* ---- state -------------------------------------------------------------- */


/* skip= : a preset name whose layer never drew, taken out of the menu. It is
 * also taken out of the control - a name the plugin was told not to believe in
 * does not get an override either - which is why AddScene returns before
 * anything is pushed. */

/* The roster: every HUD element this build knows the game has, with the label
 * the Chinese menu shows and the page it belongs on. Four sources, in this
 * order of authority:
 *
 *   1. the 53 creators registered in GR_BasicHUD / GR_CoopHUD /
 *      GR_BenchmarkViewportHUD (documents/…/hud-native-control.md - a full
 *      census, three views of 48 + 4 + 1 that all check out byte for byte).
 *      Those are the elements a campaign session actually has;
 *   2. the layer names actually seen rendering (logs\MoreHudControl.log);
 *   3. the top level layer names in the game's own string pool (out\
 *      hud_strings.txt, 456 strings dumped from the running process);
 *   4. the HUD widget families in the UI archives (2,208 families, 2,336
 *      resources, from the forge listings in third_party\WildlandsToolkit\
 *      _analysis). A layer's scene and its root widget share a name, so a
 *      family is evidence of a layer. This is the source that found the Ghost
 *      War set and the debuff / jammer / connection layers, none of which any
 *      creator claims.
 *
 * Where none of (2)-(4) names a creator's layer - the turret, the swimming
 * layer, the game over screen - the name is the obvious spelling, and it is a
 * guess. That is not a defect to hide: a guessed row costs one unchecked line
 * in a submenu, and F9 says which ones never drew so skip= can drop them.
 *
 * Names of widget children (HUD_Minimap_Bg, *_Container, *_DataContext), of
 * the icon textures (UI_HUD_*), and of the fonts in the same pool
 * (HUD_Regular_20_DS, HUD_Strong_30) are deliberately left out: they are not
 * scenes, so no switch can exist for them.
 *
 * Scene names are the game's own - CrossHair has no HUD_ prefix - so the list
 * is only readable if each row gets a label. Chinese labels are used when the
 * menu language is Chinese; anything else falls back to the engine name, which
 * is also the spelling the ini uses. */
/* Pages, in the order a player meets them. Two pages from the first build are
 * gone on purpose:
 *
 *   "3D 标记细分" listed HUD_Marker_* names that have never drawn as a scene in
 *   any session - the actual marker switches live on the marker page, which
 *   works on the widgets rather than on scene names;
 *   "数据补充" was a dumping ground of names found while probing, and three of
 *   them were the same layer spelled differently (HUD_Subtitle vs
 *   HUD_Subtitles, HUD_ResourcesLoot vs HUD_Ressources_Loot, ...). Keeping
 *   both spellings means one of the two rows can never do anything, so the
 *   guesswork names are dropped and the real ones stay. */
enum { GRP_CORE = 0, GRP_TOOLS, GRP_NOTIFY, GRP_INFO,
       GRP_ICON, GRP_MARK, GRP_FX, GRP_COUNT };

static const char *const g_grpZh[GRP_COUNT] = {
    NULL, "载具与工具", "提示与通知", "界面与其他",
    "图标细分", "3D 标记", "小地图显示"
};
static const char *const g_grpEn[GRP_COUNT] = {
    NULL, "Vehicles and tools", "Notices", "Interface and misc",
    "Icon kinds", "3D markers", "Minimap display"
};

typedef struct { const char *id; const char *zh; unsigned char group; } HudLabel;

static const HudLabel g_labels[] = {
    /* ---- the layers that matter most, on the page the menu opens on ---- */
    { "HUD_Minimap",                   "小地图",             GRP_CORE },
    { "HUD_MinimapIcons",              "小地图图标",         GRP_CORE },
    { "CrossHair",                     "准星",               GRP_CORE },
    { "HUD_WeaponItemDisplay",         "武器与弹药",         GRP_CORE },
    { "HUD_ContextualInteraction",     "互动提示",           GRP_CORE },
    { "HUD_3D_Marker",                 "3D 标记",            GRP_CORE },
    { "HUD_HordeCompass",              "指南针",             GRP_CORE },
    { "HUD_DamageDetection",           "受击指示",           GRP_CORE },
    { "HUD_Tools_Status",              "道具状态",           GRP_CORE },
    { "HUD_LocationInfo",              "位置信息",           GRP_CORE },
    { "HUD_ComWheel",                  "命令轮盘",           GRP_CORE },
    { "HUD_Tacmap",                    "战术地图",           GRP_CORE },

    /* ---- vehicles and the tools that take the screen over ---- */
    { "HUD_Vehicle",                   "载具状态",           GRP_TOOLS },
    { "HUD_Air_Vehicle",               "空中载具",           GRP_TOOLS },
    { "HUD_Air_Vhc_Info",              "空中载具信息",       GRP_TOOLS },
    { "HUD_Drone",                     "无人机",             GRP_TOOLS },
    { "HUD_Binocular",                 "望远镜",             GRP_TOOLS },
    { "HUD_Turret",                    "炮塔",               GRP_TOOLS },
    { "HUD_Swimming",                  "游泳",               GRP_TOOLS },
    { "HUD_ScanningCursor",            "扫描光标",           GRP_TOOLS },
    { "HUD_Detection",                 "侦测指示",           GRP_TOOLS },
    { "HUD_FactionState_Notification", "通缉等级",           GRP_TOOLS },
    { "HUD_SocialWheel",               "社交轮盘",           GRP_TOOLS },

    /* ---- the layers that appear for a moment and go again ---- */
    { "HUD_AlertMessage",              "警报消息",           GRP_NOTIFY },
    { "HUD_KillLog",                   "击杀日志",           GRP_NOTIFY },
    { "HUD_MicroNotification",         "微通知",             GRP_NOTIFY },
    { "HUD_MicroNotification_XP",      "微通知（经验）",     GRP_NOTIFY },
    { "HUD_XPNotification",            "经验通知",           GRP_NOTIFY },
    { "HUD_IntelNotification",         "情报通知",           GRP_NOTIFY },
    { "HUD_LootNotification",          "战利品提示",         GRP_NOTIFY },
    { "HUD_Ressources_Loot",           "物资拾取",           GRP_NOTIFY },
    { "HUD_WeaponUnlock",              "武器解锁",           GRP_NOTIFY },
    { "HUD_ProvinceComplete",          "省份完成",           GRP_NOTIFY },
    { "HUD_PrestigeNotification",      "威望通知",           GRP_NOTIFY },
    { "HUD_PlayerInfo",                "玩家信息",           GRP_NOTIFY },
    { "HUD_ProgressBar",               "进度条",             GRP_NOTIFY },
    { "HUD_KickVoteNotification",      "踢人投票",           GRP_NOTIFY },
    { "HUD_ControllerConnectionNotification", "手柄连接",    GRP_NOTIFY },
    { "HUD_TeamCommunication",         "队伍通讯",           GRP_NOTIFY },
    { "HUD_CampaignMissionReminder",   "任务提醒",           GRP_NOTIFY },
    { "HUD_Campaign_CommunicationNPC", "通讯提示",           GRP_NOTIFY },
    { "HUD_NPCNotification",           "NPC 通知",           GRP_NOTIFY },
    { "HUD_CloseMission",              "任务结算",           GRP_NOTIFY },
    { "HUD_Redeploy",                  "重新部署",           GRP_NOTIFY },
    { "HUD_Activity_PopUp",            "活动弹窗",           GRP_NOTIFY },

    /* ---- the frame around the game: text, menus, the shell ---- */
    { "HUD_Subtitles",                 "字幕",               GRP_INFO },
    { "HUD_SaveIcon",                  "存档图标",           GRP_INFO },
    { "HUD_GameOver",                  "阵亡界面",           GRP_INFO },
    { "HUD_Tutorial",                  "其他教程",           GRP_INFO },
    { "HUD_Ingame_Tutorial",           "游戏内教程",         GRP_INFO },
    { "HUD_ControlHelper",             "操作提示",           GRP_INFO },
    { "HUD_Cinematic",                 "过场",               GRP_INFO },
    { "HUD_Cinematic_Skip",            "过场跳过",           GRP_INFO },
    { "HUD_ChatBox",                   "聊天框",             GRP_INFO },
    { "HUD_Uplay_Popup",               "Uplay 弹窗",         GRP_INFO },
    { "HUD_AspectRatioBlackMask",      "宽屏黑边",           GRP_INFO },
    { "HUD_HitFeedback",               "命中反馈",           GRP_INFO },
    { "HUD_IntelWheel",                "情报轮盘",           GRP_INFO },
    { "HUD_IntelGathering",            "情报收集",           GRP_INFO },
    { "HUD_IntelLoot",                 "情报拾取",           GRP_INFO },
    { "HUD_Intel_Choice",              "情报选择",           GRP_INFO },
    { "HUD_PlayerProgression",         "成长提示",           GRP_INFO },

    /* marker kinds: these are names in the string pool, and whether they are
     * scenes of their own is exactly what the marker probe is still asking -
     * if they are, the 3D marker layer splits by kind, and if they are not,
     * these eighteen rows are the ones F9 reports as never drawn */

    /* ---- found in the game's own UI data, not in the three views ----
     *
     * These are widget families in DataPC_extra / the DLC archives that no
     * HUDCreator of GR_BasicHUD claims. Each exists for a reason - the debuff
     * icons show in Ghost Mode, the jammer prompt only near a jammer - so the
     * layer may well be a scene of its own even though nothing in the bootstrap
     * lists it. Same rule as above: keep the row, let F9 say whether it drew. */
    { "HUD_ActiveDebuffFeedback",      "减益反馈",           GRP_INFO },
    { "HUD_DebuffReminder",            "减益提醒",           GRP_INFO },
    { "HUD_TeamOrderReminder",         "小队指令提醒",       GRP_INFO },
    { "HUD_RadioSignal",               "无线电信号",         GRP_INFO },
    { "HUD_RemoteJammer",              "干扰器提示",         GRP_INFO },
    { "HUD_ConnectionNotification",    "连接提示",           GRP_INFO },
    { "HUD_PlayerConnection",          "玩家连接状态",       GRP_INFO },
    { "HUD_Cinematic_Intro",           "开场过场",           GRP_INFO },
    { "HUD_Damage_BloodScreen",        "受伤血屏",           GRP_INFO },
    { "HUD_Tiler_Black",               "黑边遮罩",           GRP_INFO },

    };

#define NLABELS ((int)(sizeof(g_labels) / sizeof(g_labels[0])))

static int IsMarkerKind(const char *nm) {
    return strncmp(nm, "HUD_Marker_", 11) == 0;
}

/* A layer the roster does not name still gets a row - the game may have one
 * this build never heard of - and it lands on the page its prefix suggests. */
static int GroupFor(const char *name) {
    int i;
    for (i = 0; i < NLABELS; i++)
        if (strcmp(g_labels[i].id, name) == 0) return g_labels[i].group;
    /* Marker templates are not switchable scenes; they get no row at all
     * rather than a row that cannot do anything. */
    return IsMarkerKind(name) ? GRP_COUNT : GRP_CORE;
}


/* Write the current switch state back to the ini's hidden= line. Only that
 * line is replaced - everything else is copied through, so the file's comments
 * survive. Called after every toggle, which is cheap enough (a few hundred
 * bytes, and only when the player actually flips something). */
/* Replace one key's line in the ini, copying everything else through so the
 * file's comments survive. A few hundred bytes, and only when the player
 * actually flips something. */
/* One key into [main]. The framework owns the file: it replaces the line in
 * place and leaves everything else alone, which is what this function used to
 * do by hand with a temp file. */
static void WriteIniKey(const char *key, const char *value) {
    CfgPutStr("main", key, value);
}


/* The switch state of one row, pushed to the framework.
 *
 * A hidden row is an override of 0 - that is the whole mechanism, and it is
 * pushed even for a layer that has never drawn, so a turret or a drone set up
 * in the ini is already suppressed the first frame it appears.
 *
 * A shown row is the framework's own default, so it is expressed by *removing*
 * the override (SH_HUD_INHERIT) rather than by writing a 1. That is what keeps
 * the table small: it ends up holding the hidden names and nothing else. The
 * old code pushed a 1 for every scene it saw, and the table - 64 entries - was
 * full after a dozen minutes of play, after which a genuine hide was silently
 * refused. The call returns 0 for "no entry to remove", which is exactly the
 * state we wanted, so only a refused hide is worth a line in the log. */
static void Push(const char *nm, int want) {
    if (!pShow) return;
    if (want) { pShow(nm, SH_HUD_INHERIT); return; }
    if (!pShow(nm, 0))
        Log("override refused for %s (framework table full?)", nm);
}

/* Is this scene drawing right now? A row whose scene is not on screen cannot
 * show any effect, and without this the same silence looks like a broken row.
 * -1 means the scene list is unavailable. */
static int SceneDrawing(const char *nm) {
    int i, n;

    if (!pSceneCount || !pSceneAt || !pSceneName) return -1;
    n = pSceneCount();
    if (n > 24) n = 24;
    for (i = 0; i < n; i++) {
        char sc[64];
        if (pSceneName(pSceneAt(i), sc, sizeof(sc)) && strcmp(sc, nm) == 0)
            return 1;
    }
    return 0;
}


/* A row's page. The roster says which one; a layer the roster never heard of
 * lands on the main page, or on the marker page if its name says so. */

/* One row per layer: a roster name, a name the ini added, or a scene the game
 * just drew. `drawn` is 1 only for the third kind - the roster's rows are made
 * before their layer has been seen, which is the point of them.
 *
 * The dedupe is also where "this preset name is real" is learned: the first
 * time a roster row's layer renders, its override goes in. Without that the
 * row could be unchecked in the menu and the layer would still draw - the row
 * existed, so AddScene returned here and nobody ever pushed the 0. */
static int PendingHide(const char *nm) {
    int i;

    for (i = 0; i < g_nHidePending; i++)
        if (strcmp(g_hidePending[i], nm) == 0) return 1;
    return 0;
}

static void AddScene(const char *nm, int drawn) {
    int i, n = g_nSeen;
    if (!nm[0] || strlen(nm) >= HC_NAME) return;
    /* Poll() asks the framework for the list every 400 ms; without this the
     * same handful of layers were appended again and again until the table
     * was full and everything else (HUD_Minimap) fell off the end. */
    for (i = 0; i < n; i++) {
        if (strcmp(g_name[i], nm)) continue;
        if (drawn && !g_drawn[i]) {
            g_drawn[i] = 1;
            Push(g_name[i], g_want[i]);
            Log("scene %-28s %s (first draw)", g_name[i],
                g_want[i] ? "shown" : "hidden");
        }
        return;
    }
    if (n >= HC_MAX) {
        Log("table full: %s dropped (%d rows)", nm, HC_MAX);
        return;
    }
    strncpy(g_name[n], nm, HC_NAME - 1);
    g_name[n][HC_NAME - 1] = 0;
    g_want[n] = PendingHide(nm) ? 0 : 1;
    g_drawn[n] = drawn ? 1 : 0;
    InterlockedExchange(&g_nSeen, n + 1);   /* publish after the row is filled */
    /* a shown row costs nothing (Push removes an entry), a hidden one is the
     * override the layer needs at its first frame */
    Push(g_name[n], g_want[n]);
    Log("scene %-28s %s%s", g_name[n], g_want[n] ? "shown" : "hidden",
        drawn ? "" : " (preset)");
}


/* Comma-append into a line that is about to be written to the log as an ini
 * line, so a name that does not fit is simply left out. */

/* ---- icon filter: the icons inside a scene ------------------------------
 *
 * The layer switch above stops at the scene, and that is as far as a scene name
 * goes: every hostile marker, every map facility, every collectible on the
 * minimap is a widget *inside* HUD_MinimapIcons / HUD_Minimap / the map screen,
 * so there is no HUD_Marker_FriendlyNPC scene to switch.
 *
 * What an icon widget carries is its texture in SH_P_IMAGE - and measured
 * 2026-10-20, on the game's own widgets, that value is not a name but the
 * resource id as a hex string:
 *
 *     0x00106DEDDE40   UI_HUD_Vehicle_Icon_MapDesc
 *     0x0043B8404948   UI_Map_Icon_Vehicle_Bike_MapDesc
 *     0x001099198655   GRN_HUD_MiniMapDot_Collectible_AmmoBox_MapDesc
 *
 * So the filter matches on the id. hud_icon_ids.h is the id -> kind table,
 * generated from the archives by tools/gen_hud_icon_ids.py: the names are what
 * classify an icon, but the id is what identifies it at run time.
 *
 * Hiding is SH_P_SCALE (0,0,0): measured, an alpha written between frames is
 * overwritten by the marker system before the draw, while scale is layout and
 * survives. That is also why every collapsed widget is remembered and put back
 * when its switch goes on again.
 *
 * Runs on the game thread from a frame callback, five times a second, over the
 * scenes that draw icons only - and not at all while every kind is on and
 * nothing is collapsed. */
typedef struct { const char *match; const char *zh; } IconKind;

/* Same order as KINDS in tools/gen_hud_icon_ids.py - the compile time check
 * below is what keeps the two from drifting. `match` is only a fallback for a
 * name that does not parse as a resource id. */
static const IconKind g_icons[] = {
    { "GRN_HUD_MiniMapDot_Collectible", "小地图点：收集品" },
    { "GRN_HUD_MiniMapDot_Enemy",       "小地图点：敌人" },
    { "GRN_HUD_MiniMapDot",             "小地图点：其他" },
    { "UI_HUD_MiniMap_",                "小地图箭头与目标" },
    { "UI_HUD_Minimap_",                "小地图图标" },
    { "UI_HUD_Map_Icon_",               "大地图设施" },
    /* Split 2026-10-22: the map census showed every icon on screen landing in
     * this one bucket, so the whole map could only be switched off at once.
     * The second name segment is a real grouping in the archives, and the
     * catch-all stays last so nothing slips through. */
    { "UI_Map_Icon_Resource",           "大地图：资源" },
    { "UI_Map_Icon_Vehicle",            "大地图：载具" },
    { "UI_Map_Icon_Mission",            "大地图：任务" },
    { "UI_Map_Icon_Unidad",             "大地图：联合军" },
    { "UI_Map_Icon_Teammate",           "大地图：队友" },
    { "UI_Map_Icon_Legend",             "大地图：传奇" },
    { "UI_Map_Icon_Jammer",             "大地图：干扰器" },
    { "UI_Map_Icon_Collectible",        "大地图：收集品" },
    { "UI_Map_Icon_",                   "大地图：据点与地标" },
    { "UI_HUD_WorldIcon_Activity",      "大地图活动" },
    { "UI_HUD_WorldIcon_",              "大地图地点（其他）" },
    { "UI_HUD_WorldIntel_",             "大地图情报" },
    { "UI_HUD_MarkIcon",                "世界标记（敌/友/中立）" },
    { "UI_HUD_Mark_",                   "兵种与威胁标记" },
    { "UI_HUD_Marker_",                 "物件标记" },
    { "GRN_HUD_MiniMapCorner",          "小地图折角" },
    { "UI_HUD_Beacon",                  "信标" },
    { "UI_HUD_Vehicle_Icon|UI_HUD_Icon_", "载具与物品图标" },
    /* Second pass over the archives, 2026-10-22: these families were never
     * classified, so their icons could not be hidden at all. The order here
     * must match KINDS in tools/gen_hud_icon_ids.py, and HC_ICON_KIND_COUNT
     * catches it if it does not. */
    { "UI_Map_Activity|UI_Map_Icons|UI_Map_Temp",          "大地图活动与奖励" },
    { "UI_Map_Settlement|UI_Map_Province|UI_Map_Zone",     "大地图据点与省份" },
    { "UI_MAP_Icon|UI_Map_icon",                           "大地图图标（变体）" },
    { "UI_Map_Mission|UI_Map_MissionObjective|UI_Map_MissionSquareHollow|UI_Map_MissionOpportunity",
                                                           "大地图任务图标" },
    { "GRN_HUD_MiniMapSkin|GRN_HUD_MiniMap|GRN_HUD_MiniMapJammer",
                                                           "小地图底图与干扰" },
    { "UI_Map_BeaconGeneric|UI_Map_BeaconIcon|UI_Map_Redeploy|UI_Map_FactionState|UI_HUD_Worldicon",
                                                           "信标 / 重部署 / 阵营" },
    { "UI_Map_cursorDefault|UI_Map_cursorCenter|UI_Map_CursorLine",
                                                           "大地图光标" },
    { "UI_HUD_Map|UI_Map_BGGradient|UI_Map_DirtBg|UI_Map_WhiteDirtBg|UI_Map_DifficultyIcon",
                                                           "大地图界面元素" },
    /* Appended with the generator, never inserted: the indices in
     * hud_icon_ids.h are this array's positions, so a new kind goes last. */
    { "UI_COM_Mouse|UI_COM_Key",                           "键鼠提示" },
};

#define NICONS ((int)(sizeof(g_icons) / sizeof(g_icons[0])))

/* Cumulative hits per kind since the last time a switch was touched. A switch
 * that looks like it does nothing has two very different causes - the kind is
 * not on screen, or its ids are not in the table - and only a count tells them
 * apart. Kept across passes so the five-per-second sampling cannot hide one. */
static int g_kindHits[NICONS];
static int g_rawHits;

/* The generated table carries the count its kinds were built with; a mismatch
 * means someone edited one list and not the other, and every id would land on
 * the wrong switch. */
typedef char hc_icon_kind_count_check[(NICONS == HC_ICON_KIND_COUNT) ? 1 : -1];

static int g_iconOff[NICONS];        /* index-aligned with g_icons */

/* ids the ini named by hand, applied on top of the table - an icon whose
 * resource is not in the generated table still gets a switch if its id is
 * known, and the census prints every id it meets */

static volatile LONG g_iconOn;       /* anything off at all? */
static volatile LONG g_iconFrame;
/* icon_hide= is gone: VISIBLE is the only property written. */

static void IconStateSync(void) {
    int i, any = 0;
    for (i = 0; i < NICONS; i++) if (g_iconOff[i]) any = 1;
    InterlockedExchange(&g_iconOn, any ? 1 : 0);
}

/* icon_hide= used to pick scale / alpha / both. Cleared VISIBLE replaced all of
 * them on 2026-10-23, so the key is accepted and ignored. */
static void LoadIconMode(const char *v) {
    Log("ini: icon_hide=%s is ignored - hiding is always VISIBLE=0 now", v);
}

/* The id SH_P_IMAGE holds is a hex string, "0x00106DEDDE40". Returns 0 for
 * anything that is not one - a widget whose image is a real name, if any. */
static int ParseResId(const char *s, unsigned long long *out) {
    unsigned long long v = 0;
    int n = 0;

    if (!s || !s[0]) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s; s++) {
        int c = *s, d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = (v << 4) | (unsigned long long)d;
        if (++n > 16) return 0;
    }
    if (n < 4) return 0;
    *out = v;
    return 1;
}

/* The generated table is sorted by id: one binary search per icon widget, and
 * it runs five times a second over a few hundred widgets. */
static int KindOfId(unsigned long long id) {
    int lo = 0, hi = (int)(sizeof(g_iconIds) / sizeof(g_iconIds[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_iconIds[mid].id == id) return (int)g_iconIds[mid].kind;
        if (g_iconIds[mid].id < id) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}



/* "A|B" against one name: case insensitive, anchored, and every alternative
 * tried, because the archive is not consistent about which spelling it uses
 * (UI_HUD_MiniMap_ next to UI_HUD_Minimap_). */
static int MatchAny(const char *spec, const char *name) {
    const char *p = spec;
    size_t nl = strlen(name);
    while (*p) {
        const char *e = strchr(p, '|');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l && nl >= l && _strnicmp(p, name, l) == 0) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* Which switch an image belongs to. The id is the identity; the prefix test is
 * only a fallback for the day a widget hands back a real name instead. */
/* Is there any reason to walk the icon scenes at all? With every icon switch on
 * and no id named by hand, there is not - and the walk is the most expensive
 * thing this plugin does. */
static int IconAnyOff(void) {
    int i;

    for (i = 0; i < NICONS; i++) if (g_iconOff[i]) return 1;
    return 0;
}

/* ---- 敌人：仅视野内 ------------------------------------------------------
 *
 * The game darkens a world mark when the target is behind cover, which means it
 * knows the answer already; the HUD only has to read it back. The signal is the
 * "_Occluded" twin texture: the mark swaps its icon for the occluded variant
 * while the target is not visible.
 *
 * Resource census 2026-10-25, from all 34 forge listings in
 * third_party/WildlandsToolkit/_analysis (158 Occluded entries, 20 of them
 * Mark-related). The complete set of mark twins is below. Note that Santa
 * Blanca has no twin of its own: the ordinary SB soldier is drawn with a tinted
 * mark rather than a Mark_* icon, so this row can only act on the factions and
 * archetypes the archives actually ship a twin for - Unidad, rebel friendlies,
 * HVT, sniper, heavy gunner, the DLC10 archetypes and the DLC30 ones.
 */
static const unsigned long long g_occIds[] = {
    /* ---- the ten the archive census turned up on 2026-10-25 ---- */
    0x007752BF8D67ULL,   /* UI_HUD_MarkIcon_Unidad_Occluded */
    0x007AD5200B80ULL,   /* UI_HUD_MarkIcon_Rebellion_Friendly_Occluded */
    0x007C56F15D41ULL,   /* UI_HUD_MarkADV_Occluded */
    0x0079B37F8E2AULL,   /* UI_HUD_Mark_Sniper_Occluded */
    0x0079B37F8E2CULL,   /* UI_HUD_Mark_HeavyGunner_Occluded */
    0x0079B37F8E2EULL,   /* UI_HUD_Mark_HVT_Occluded */
    0x0111475C5D5DULL,   /* DLC10_UI_HUD_Mark_Camo_Occluded */
    0x0111475C5D5EULL,   /* DLC10_UI_HUD_Mark_Jammer_Occluded */
    0x0111475C5D5FULL,   /* DLC10_UI_HUD_Mark_Sniper_Occluded */
    0x0111475C5D60ULL,   /* DLC10_UI_HUD_Mark_Armored_Occluded */
    0x024916ABABCBULL,   /* UI_HUD_Mark_DLC30_LAZ_Soldier_Occluded */
    0x024916ABABCFULL,   /* UI_HUD_Mark_DLC30_LAZ_Veteran_Occluded */
    0x024916ABB0DEULL,   /* UI_HUD_Mark_DLC30_LAZ_Tripwire_Occluded */
    0x024916ABC911ULL,   /* UI_HUD_Mark_DLC30_LAZ_Beartrap_Occluded */
    /* ---- world-intel twins: same mechanism, kept for the map pass ---- */
    0x0042DF91872EULL,
    0x0042DF918735ULL,
    0x0042DF91873CULL,
    0x00782AA12D0DULL,
    0x00782AA12D27ULL,
    0x00782AA12D28ULL,
    0x00782AA12D2DULL,
};
static int g_viewOnly;
static int g_viewOnlyPrev;
static volatile LONG g_needMarkRestore;
static int g_restoreTries;
static int g_occHits, g_occPasses;
static int g_viewIdle;   /* passes in a row with nothing changing */
static int g_lastHid;    /* hide count of the previous pass */
static unsigned long long g_occSample;    /* the twin we matched, for the log */
static int  g_occSlots;   /* Occluded slots in the sampled marker */
static int  g_occLit;     /* ...of which VISIBLE says on */
static int  g_alphaLit;   /* ...of which alpha says on */
static int  g_litMarks;   /* markers whose twin slot VISIBLE says on */
static int  g_alphaMarks; /* markers whose twin slot has alpha > 0 */
static int  g_occToldOnce; /* the first live hit has been logged */
/* Which texture the engine actually lit, tallied over one log window. This is
 * the whole question the row rests on: if a target the player cannot see is
 * drawn with UI_HUD_MarkIconEmpty and a visible one with UI_HUD_MarkIcon, the
 * drawn slot IS the criterion. */
static int g_drawEmpty, g_drawSolid, g_drawTwin, g_drawOther, g_drawUnknown;
static int g_shownFlips;   /* hidden markers brought back this window */
static int g_resetMarks;   /* our scale=0 was undone by the engine */
static int g_maxHollowRun; /* longest hollow run seen this window */
static char g_drawSample[400];
/* The dimming the player describes is not in the widget tree at all: a session
 * of 20 markers read identically (colour 255/55/0 on every icon, 235/50 on
 * every Sniper slot, 120/140 on every HVT slot), so the engine dims a hidden
 * target in the material, not in the controls. The one thing the HUD can still
 * read is the marker's own alpha, so this measures it and only acts when it
 * actually varies across the screen - if every marker reports the same alpha
 * there is nothing to separate and the row stays out of the way. */
static int  g_visBlind;   /* ...whose VISIBLE could not be read */
static char g_viewSample[640];            /* slot list of the first enemy mark */

/* ---- what the engine itself writes to VISIBLE ---------------------------
 *
 * The icon layer answers no property call for eleven of its fifteen slots, so
 * "which slot is drawn" cannot be asked. It can be overheard: the probe on the
 * engine's VISIBLE setter (below) sees every write it makes - 4.3k calls and
 * 1.2k "set to 1" in three seconds, measured live 2026-10-25 15:56. Storing
 * the last value per handle in a small hash turns that into a readable "is
 * this slot lit" without touching the widget at all.
 *
 * The hash is written from whatever thread the engine sets properties on and
 * read from the frame body. A miss costs a stale answer, never a bad address,
 * and a handle collision is caught by the stored key, so a wrong answer is
 * impossible - only "not seen recently". */
#define PV_BITS   14                      /* 16384 buckets */
#define PV_SIZE   (1 << PV_BITS)
static uint64_t g_pvKey[PV_SIZE];
static int8_t   g_pvVal[PV_SIZE];
static volatile LONG g_pvWrites;          /* writes seen, for the log */

static void PvNote(uint64_t h, int v) {
    uint32_t i;

    if (!h) return;
    i = (uint32_t)(((h >> 4) * 2654435761u) >> (32 - PV_BITS)) & (PV_SIZE - 1);
    g_pvKey[i] = h;
    g_pvVal[i] = (int8_t)(v ? 1 : 0);
    InterlockedIncrement(&g_pvWrites);
}

/* 1 = the engine last wrote VISIBLE=1 here, 0 = it wrote 0, -1 = never seen.
 *
 * No expiry. A generation window was tried on 2026-10-25 16:17 and it switched
 * the whole row off: the engine does not rewrite these slots every frame, it
 * writes them when a marker changes, so a value that is correct can sit there
 * for minutes - and a two-pass window threw it away and reported "no answer"
 * for every marker. The keys are compared in full, so a recycled handle can
 * only ever hand back that handle's own last write. */
static int PvLast(uint64_t h) {
    uint32_t i;

    if (!h) return -1;
    i = (uint32_t)(((h >> 4) * 2654435761u) >> (32 - PV_BITS)) & (PV_SIZE - 1);
    return (g_pvKey[i] == h) ? g_pvVal[i] : -1;
}

static int IsOccludedId(unsigned long long id) {
    int i;

    for (i = 0; i < (int)(sizeof(g_occIds) / sizeof(g_occIds[0])); i++)
        if (g_occIds[i] == id) return 1;
    return 0;
}

/* The icon layer of one marker - the 15-slot container that holds the whole
 * per-archetype icon set at once.
 *
 * Census 2026-10-25, read live off the running game with Cheat Engine, one
 * marker at a time: whatever a marker is drawn as, it carries every twin
 * preloaded in one container -
 *
 *   UI_HUD_MarkIconEmpty   UI_HUD_MarkIcon   MarkAdd
 *   UI_HUD_Mark_Sniper            UI_HUD_Mark_Sniper_Occluded
 *   UI_HUD_Mark_HeavyGunner       UI_HUD_Mark_HeavyGunner_Occluded
 *   UI_HUD_VIPIcon                UI_HUD_VIPIcon_Occluded
 *   UI_HUD_Mark_HVT               UI_HUD_Mark_HVT_Occluded
 *   UI_HUD_ObjectiveIcon   HUD_Marker_SyncShot ...
 *
 * Each pair is preloaded and the engine lights exactly one half, so "the
 * _Occluded twin is the visible one" is the game itself saying the target is
 * behind cover - the same thing the player sees as the mark going dim. This is
 * the signal the row reads. Screen position is not involved anywhere: a mark
 * behind the camera is culled by the engine, not darkened by it.
 *
 * The layer is found by shape, not by index, because a marker is a pooled
 * widget: the slot that held a sniper is handed to an HVT without a rebuild,
 * and the index that was right for one marker is wrong for the next. Walking
 * the whole subtree for a matching name - what this used to do - cost a string
 * read per node per marker per pass and still missed, because the name lives in
 * the widget's own image property, not on a child of its own. */
/* The image name of one widget, read straight out of its private block.
 *
 * SH_P_IMAGE is registered in the engine's property table for one widget class
 * only, and a marker's icon layer is not that class: measured live 2026-10-25,
 * pWidgetGetS answered 0 for all 15 slots, which is why the first version of
 * this row never matched anything. The name is still there - the private block
 * holds a pointer to the usual {len, cap, refcount, chars} string block - it
 * just sits at a different offset per class:
 *
 *   icon-layer slot    +0x110   "UI_HUD_Mark_Sniper_Occluded", "UI_HUD_MarkIcon"
 *   marker's own icon  +0x400   "UI_HUD_Mark_Sniper"
 *
 * So try every offset actually seen, and accept a hit only when it is a
 * printable string starting with "UI_". At most five reads per slot, and reads
 * are nanoseconds - the property calls this replaces were the expensive part. */
/* One offset, not a list. Census 2026-10-25 on a 20-marker session: every
 * slot of a marker's icon layer is the same class and its private blocks are
 * 0x240 apart, so the image name is at +0x110 for all fifteen - and 0x350 is
 * exactly 0x110 of the NEXT slot, which is how an earlier list of offsets
 * ended up reporting one slot's twin twice and the neighbour's never. */
#define IMG_OFF  0x110

static int WidSane(uint64_t w);          /* defined with the widget helpers */

/* Is [a, a+n) committed and readable right now?
 *
 * A pointer lifted out of a private block is not trustworthy on its own. A
 * field that happens to hold a float passes every cheap "looks like an
 * address" test and then faults when dereferenced - that is exactly the crash
 * of 2026-10-25 12:43, "read of 000000003F800000" (float 1.0) at
 * MoreHudControl+0xF593, inside this function: the value cleared the 0x10000
 * floor and the __try around it did not save the frame. VirtualQuery answers
 * the question without touching the page, at the cost of one cheap call, so a
 * bad field is skipped instead of faulted. */
static int RangeReadable(uint64_t a, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;

    if (a < 0x10000ULL || n == 0) return 0;
    if (a + n < a) return 0;                       /* overflow */
    if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY)))
        return 0;
    return (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize >= a + n;
}

static int WidgetImageName(uint64_t w, char *out, int n) {
    uint64_t priv = 0, blk;
    char buf[96];
    int len, k;

    if (!out || n < 8) return 0;
    out[0] = 0;
    if (!WidSane(w)) return 0;
    __try {
        priv = *(volatile uint64_t *)(uintptr_t)(w + 0x20);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (!RangeReadable(priv + IMG_OFF, 8)) return 0;
    blk = *(volatile uint64_t *)(uintptr_t)(priv + IMG_OFF);
    if (!RangeReadable(blk, 12)) return 0;
    len = *(volatile int *)(uintptr_t)blk;
    if (len < 3 || len >= (int)sizeof(buf)) return 0;
    if (!RangeReadable(blk + 12, (size_t)len)) return 0;
    memcpy(buf, (void *)(uintptr_t)(blk + 12), (size_t)len);
    buf[len] = 0;

    /* No "UI_" gate: the set is {UI_HUD_Mark_X, MarkAdd, Sniper_Add,
     * HeavyGunner_Add, HVT_Add, ...} and rejecting the short ones is what sent
     * the reader onto the neighbour's offset. Every real name is a run of
     * letters, digits and underscores starting with a letter. */
    if (!((buf[0] >= 'A' && buf[0] <= 'Z') || (buf[0] >= 'a' && buf[0] <= 'z')))
        return 0;
    for (k = 0; k < len; k++) {
        char c = buf[k];

        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    strncpy(out, buf, (size_t)n - 1);
    out[n - 1] = 0;
    return 1;
}


static uint64_t MarkerIconLayer(uint64_t m) {
    int i, n = pChildCount ? pChildCount(m) : 0;

    if (n < 2 || n > 40) return 0;
    for (i = 0; i < n; i++) {
        uint64_t c = pChildAt(m, i);
        int k = pChildCount ? pChildCount(c) : 0;

        if (k >= 5 && k <= 20) return c;
    }
    return 0;
}

/* ---- per-marker slot cache ---------------------------------------------
 *
 * The fifteen handles of an icon layer and their texture names do not change
 * while that layer lives, yet the first version re-read both on every pass:
 * 22 markers x 15 slots of VirtualQuery plus a string copy, measured at 2.7 ms
 * per pass. Reading them once per marker leaves the pass with a handful of hash
 * lookups. The cache is keyed on the marker handle and rebuilt whenever the
 * layer handle changes, because the pool does hand a live marker a new layer.
 */
/* The row samples ten times a second while it is on. The idle rate is not used
 * here: the decision needs a fixed number of samples per second to count runs
 * of them, and a slower cadence would silently double every timeout. */
#define VIEW_FRAME_BUSY 6

/* A marker changes state only when the last few passes agree.
 *
 * Chasing each pass - or "three in a row" - was still not enough: a marker the
 * player is looking at still produced runs of hollow passes, so it went down,
 * came back a second later and went down again. What it needs is a debounce
 * that a short burst in either direction cannot win: keep the last eight
 * answers as a ring of bits and move only when three quarters of them agree.
 *
 *   >= 75% hollow -> hide     <= 25% hollow -> show     otherwise -> leave it
 *
 * An alternating signal can never reach either side, so a noisy marker keeps
 * the state it already has instead of blinking between the two.
 */
/* One-way confirmation: hiding waits, showing never does.
 *
 * Every version between 16:42 and 19:19 added memory to BOTH directions and
 * the blinking only got slower and more visible. The reason it never helped is
 * in the logs: the markers that blink are not flipping between "visible" and
 * "occluded" at all, they are flipping between "exactly one slot lit" and "two
 * slots lit", and holding a wrong hide is what turned a 100 ms glitch into a
 * visible one.
 *
 * So a marker is hidden only after three passes in a row say so, and it is put
 * back the moment a pass says anything else. A wrong hide can then never last
 * longer than a third of a second, and the row can never blink: the show side
 * has no delay to fight with.
 */
#define HIDE_AFTER_HOLLOW 3

#define MS_MAX    64
#define MS_SLOTS  20
enum { MSK_UNKNOWN = 0, MSK_HOLLOW, MSK_SOLID, MSK_OTHER };

typedef struct {
    uint64_t    m;
    uint64_t    layer;
    int         n;            /* slots cached; 0 = this layer has none */
    int         built;        /* an attempt has been made for this layer */
    int         age;          /* passes since that attempt */
    int         unsure;       /* passes in a row without a single lit slot */
    int         hollowRun;    /* unbroken passes reading hollow */
    uint64_t    slot[MS_SLOTS];
    signed char kind[MS_SLOTS];
} MarkSlots;

static MarkSlots g_ms[MS_MAX];
static int       g_msNext;

/* Runs are counted per pass while the row is on; switching it off leaves them
 * frozen mid-count, so starting it again must not inherit "three hollow passes
 * in a row" from a minute ago and hide a marker on the first pass back. */
static void MsResetRuns(void) {
    memset(g_ms, 0, sizeof(g_ms));
    g_msNext = 0;
}

static MarkSlots *MsSlot(uint64_t m) {
    int i;
    MarkSlots *ms = NULL;

    for (i = 0; i < MS_MAX && !ms; i++)
        if (g_ms[i].m == m) ms = &g_ms[i];
    for (i = 0; i < MS_MAX && !ms; i++)
        if (!g_ms[i].m) ms = &g_ms[i];
    if (!ms) {
        ms = &g_ms[g_msNext];        /* pool full: reuse in turn */
        g_msNext = (g_msNext + 1) % MS_MAX;
    }
    if (ms->m != m) {                /* first use, or a recycled entry */
        memset(ms, 0, sizeof(*ms));
        ms->m = m;
    }
    return ms;
}

static void MsBuild(MarkSlots *ms, uint64_t m, uint64_t layer) {
    int i, n = layer ? (pChildCount ? pChildCount(layer) : 0) : 0;

    memset(ms, 0, sizeof(*ms));
    ms->m = m;
    ms->layer = layer;
    ms->built = 1;                   /* even a failure is an answer: do not
                                      * retry the walk on every pass */
    if (n <= 0 || n > MS_SLOTS) return;
    for (i = 0; i < n; i++) {
        uint64_t s = pChildAt(layer, i);
        char img[96];

        ms->slot[i] = s;
        ms->kind[i] = MSK_OTHER;
        if (!WidgetImageName(s, img, sizeof(img))) continue;
        /* Same two families the working build keyed on: the hollow ring and
         * the per-archetype _Occluded twins all mean "not drawing it normally",
         * the plain icons mean "drawing it". */
        if (ContainsCI(img, "MarkIconEmpty") || ContainsCI(img, "Occluded"))
            ms->kind[i] = MSK_HOLLOW;
        else if (ContainsCI(img, "MarkIcon"))
            ms->kind[i] = MSK_SOLID;
    }
    ms->n = n;
}

/* Which single slot the engine last lit for this marker, or -1.
 *
 * This is the rule the build that worked used, only without re-reading fifteen
 * texture names per marker per pass: "exactly one slot lit" is what separates a
 * marker's two drawing states, and anything else - nothing lit, or two lit -
 * answers -1 and nothing is hidden on it. The name of that one slot is what the
 * row decides on. Reading the names once per marker (they do not change while
 * the layer lives) is the only change here.
 */
static int MarkerDrawnSlot(uint64_t m, char *detail, int detailN,
                           int *kindOut, int *unsureOut, int *actOut) {
    MarkSlots *ms;
    int i, lit = -1, used = 0, kind = MSK_OTHER;
    int hollowLit = 0, solidLit = 0, hollowAt = -1, solidAt = -1;

    if (detail && detailN > 8) detail[0] = 0;
    if (kindOut) *kindOut = MSK_OTHER;
    ms = MsSlot(m);
    /* Finding the layer means asking every child of the marker how many
     * children it has - eight property calls, twenty-odd markers, ten times a
     * second. A cached layer is trusted while its own child count still
     * matches the cached one, and for five passes at most: the pool does hand
     * a live marker a new set of children, and a name read once and kept for
     * forty passes can end up describing a different slot than the handle it
     * is paired with. */
    if (ms->built && ms->layer) {
        int k = pChildCount ? pChildCount(ms->layer) : ms->n;

        if (k == ms->n && ms->age < 5) {
            ms->age++;
        } else {
            MsBuild(ms, m, MarkerIconLayer(m));
            ms->age = 0;
        }
    } else if (ms->built && ms->age < 10) {
        /* No layer was found last time. Look again in a moment rather than on
         * every pass - a marker without a 5..20 child container would otherwise
         * pay the full child walk ten times a second, forever. */
        ms->age++;
    } else {
        MsBuild(ms, m, MarkerIconLayer(m));
        ms->age = 0;
    }
    if (ms->n == 0) return -1;

    for (i = 0; i < ms->n; i++) {
        int v = PvLast(ms->slot[i]);

        if (detail && detailN > 8 && used < detailN - 24) {
            int w = _snprintf(detail + used, (size_t)(detailN - used), "%s%d:%d",
                              used ? " " : "", i, v);

            if (w > 0) used += w;
        }
        if (v != 1) continue;
        if (ms->kind[i] == MSK_HOLLOW) { hollowLit++; hollowAt = i; }
        else if (ms->kind[i] == MSK_SOLID) { solidLit++; solidAt = i; }
    }
    /* Exactly one lit slot is the answer - the rule the 16:25 build used, and
     * the one that measured clean. Two lit slots is not a state this trail can
     * be trusted to name (a marker the player can see lights its icon AND its
     * "+N" layer), so it is "no answer", and no answer always means "show".
     *
     * Counting the families instead - what 18:09 did - quietly moved a whole
     * group of markers from "always answered unknown, therefore always shown"
     * into "hollow, therefore hidden", and those are the markers that blinked:
     * their second slot comes and goes, so the answer came and went with it.
     * The log shows the move plainly: unknown fell 140 -> 20 per window and
     * solid went 0 -> 112 in the same step. */
    (void)hollowAt;
    (void)solidAt;
    if (hollowLit + solidLit != 1) {
        ms->hollowRun = 0;
        if (unsureOut) *unsureOut = 0;
        if (actOut) *actOut = 2;          /* show, always, without delay */
        return -1;
    }
    kind = (hollowLit == 1) ? MSK_HOLLOW : MSK_SOLID;
    lit = (hollowLit == 1) ? hollowAt : solidAt;
    if (unsureOut) *unsureOut = 0;
    if (kindOut) *kindOut = kind;

    if (kind == MSK_HOLLOW) {
        if (ms->hollowRun < 100000) ms->hollowRun++;
        if (ms->hollowRun > g_maxHollowRun) g_maxHollowRun = ms->hollowRun;
        if (actOut) *actOut = (ms->hollowRun >= HIDE_AFTER_HOLLOW) ? 1 : 0;
    } else {
        ms->hollowRun = 0;
        if (actOut) *actOut = 2;          /* show, always, without delay */
    }
    return lit;
}


static void MarkerViewOnlyLog(int seen) {
    if (++g_occPasses < 20) return;
    g_occPasses = 0;
    Log("view-only: %d marker(s) | hollow=%d solid=%d other=%d unknown=%d -> "
        "%d hidden, %d brought back, %d scale undone, longest hollow run %d "
        "| VISIBLE writes=%ld", seen, g_drawEmpty, g_drawSolid, g_drawTwin,
        g_drawUnknown, g_occHits, g_shownFlips, g_resetMarks, g_maxHollowRun,
        (long)g_pvWrites);
    if (g_drawSample[0])
        Log("view-only: slot trail (index:last write) = %s", g_drawSample);
    g_occHits = 0;
    g_occSample = 0;
    g_occSlots = g_occLit = g_alphaLit = g_visBlind = 0;
    g_litMarks = g_alphaMarks = 0;
    g_viewSample[0] = 0;
    g_drawEmpty = g_drawSolid = g_drawTwin = g_drawOther = g_drawUnknown = 0;
    g_shownFlips = g_resetMarks = 0;
    g_maxHollowRun = 0;
    InterlockedExchange(&g_pvWrites, 0);
    g_drawSample[0] = 0;    /* re-sample next window */
}

/* Does the row that owns `kind` want this image gone, in this context?
 *
 * The kind arrives already looked up. This function used to take only the
 * string and search the 1.5k-entry id table three times for it - once for the
 * minimap rule, once for "the minimap page owns these kinds", once inside
 * KindIsOff - on top of the parse the caller had already done for its own
 * counters. Nothing about the decision needs more than one lookup, and the
 * caller is the one that has the id in hand.
 *
 * The rules below are the switch semantics, unchanged:
 *   minimap page  vehicles are kinds 7 and 23, collectibles 0 and 13; those
 *                 four answer only to mm_vehicle / mm_collect, never to the
 *                 [main] table - falling through is what once let a map-page
 *                 vehicle row hide the minimap's cars.
 *   anywhere else  the [main] table decides, which is g_iconOff[kind]. */
static int IconHidden(const char *img, int ctx, int hasId, int kind) {
    int i;

    if (hasId) {
        if (ctx == HC_CTX_MINIMAP) {
            if (!g_state.mm_vehicle && (kind == 7 || kind == 23)) return 1;
            if (!g_state.mm_collect && (kind == 0 || kind == 13)) return 1;
            if (kind == 7 || kind == 23 || kind == 0 || kind == 13) return 0;
        }
        return (kind >= 0 && kind < NICONS) ? g_iconOff[kind] : 0;
    }
    /* An image that is a real name rather than a resource id (never seen, but
     * the table has prefixes for exactly this case): only the names can say. */
    for (i = 0; i < NICONS; i++)
        if (g_iconOff[i] && MatchAny(g_icons[i].match, img)) return 1;
    return 0;
}

/* What a census line calls one id: its kind, or "-" when the archives named
 * nothing under that id. */
static const char *IdLabel(unsigned long long id) {
    int k = KindOfId(id);
    if (!id) return "-";                       /* the name was not an id */
    return (k >= 0 && k < NICONS) ? g_icons[k].zh : "UNKNOWN";
}

/* Census: the distinct images one scene draws, with the resource id and one
 * sample colour each. The colour matters as much as the id: some hud icons are
 * a white texture tinted per kind, so two kinds can share one SH_P_IMAGE and
 * differ only in SH_P_COLOUR. */
typedef struct {
    char name[80];
    unsigned long long id;
    int count, r, g, b;
} IconSeen;


/* Names already reported, so a marker that stays on screen for ten minutes is
 * one log line and not ten thousand. */
static char g_reported[64][80];
static int  g_nReported;
static int  ReportOnce(const char *img) {
    int i;
    for (i = 0; i < g_nReported; i++)
        if (strcmp(g_reported[i], img) == 0) return 0;
    if (g_nReported >= 64) return 1;
    strncpy(g_reported[g_nReported], img, sizeof(g_reported[0]) - 1);
    g_reported[g_nReported][sizeof(g_reported[0]) - 1] = 0;
    g_nReported++;
    return 1;
}

/* The same for the restore side, but it has to fire again every time a kind is
 * switched back on - four icons of one kind are four widgets to put back and
 * one line to read. Reset when a pass restores nothing. */
static char g_back[32][80];
static int  g_nBack;
static int  ReportBackOnce(const char *img) {
    int i;
    for (i = 0; i < g_nBack; i++)
        if (strcmp(g_back[i], img) == 0) return 0;
    if (g_nBack >= 32) return 1;
    strncpy(g_back[g_nBack], img, sizeof(g_back[0]) - 1);
    g_back[g_nBack][sizeof(g_back[0]) - 1] = 0;
    g_nBack++;
    return 1;
}

/* ---- hide by scale, and put it back ------------------------------------
 *
 * SH_P_ALPHA was the first try and it does not work on the game's own icons:
 * measured 2026-10-20 - the write is accepted, the icon stays. The icon's
 * alpha is part of what the marker system recomputes, so a value written
 * between frames is overwritten before the draw. Scale is layout: it survives
 * until something rebuilds the widget, which is what makes it usable - and it
 * is also why every hidden widget has to be remembered and restored, or an
 * icon would stay collapsed after its switch went back on.
 *
 * SH_P_SCALE (0,0,0) rather than a tiny value: the quad collapses to a point,
 * so nothing is submitted, and a single property covers both axes. */
/* 4096, not 1024: one 3D marker is thirty-odd widgets and a busy firefight
 * draws twenty to thirty markers, so the marker side alone measured 646 records
 * on 2026-10-22 - close enough to the old ceiling that a full table would start
 * dropping records again, and a dropped record is a marker widget that stays
 * collapsed for good. Records are 40 bytes, so the table costs 160 KB of .bss
 * and nothing at runtime. */
/* ---- what we hid, and how to put it back --------------------------------
 *
 * Hidden means SCALE = 0, and that is the only thing this plugin writes to hide
 * anything. VISIBLE is dead data on a HUD tree (measured live 2026-10-25: every
 * child of a 3D marker reported VISIBLE = 0 while the marker kept drawing), and
 * on the widgets where it did work it had to be pressed again and again because
 * the engine recomputes it every frame. Scale is layout: the quad collapses to a
 * point, nothing is submitted, and the value survives until something rebuilds
 * the widget - which is why every collapsed widget is remembered here and put
 * back from the record, never blindly set to 1.
 *
 * The record therefore carries the scale that was there before the collapse:
 * a marker root and a minimap blip are both 1, but a map icon is laid out by
 * the map and need not be, so the read value is the only honest restore.
 *
 * The table is open addressing keyed by handle. It used to be an array scanned
 * linearly on every lookup, which is O(n) per widget inside a walk, and the
 * walks touch hundreds of widgets per pass.
 */
#define ZERO_SLOTS   4096              /* power of two */
#define Z_SLOT_FREE  0
#define Z_SLOT_USED  1
#define Z_SLOT_DEAD  2

/* The only thing a record has to say about itself is which sweep puts it back:
 * the marker pass owns its own roots, everything else is an icon the walk owns. */
#define MODE_ICON    0
#define MODE_MARKER  0x10

typedef struct {
    uint64_t      h;
    unsigned char used;      /* Z_SLOT_* */
    unsigned char mode;      /* MODE_ICON / MODE_MARKER */
    unsigned char kind;      /* icon kind, or 0xFF for "not an icon" */
    int           stamp;     /* pass number it was last seen in */
    float         sc[3];     /* the scale to put back - 1,1,1 when unknown */
} Zeroed;

static Zeroed g_zeroed[ZERO_SLOTS];
static int    g_nZeroed;         /* live records */
static int    g_zeroPass;

static unsigned ZeroHash(uint64_t h) {
    h >>= 4;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (unsigned)(h & (ZERO_SLOTS - 1));
}

static void ZeroCompact(void) {
    static Zeroed tmp[ZERO_SLOTS];
    int i;

    memset(tmp, 0, sizeof(tmp));
    for (i = 0; i < ZERO_SLOTS; i++) {
        unsigned k;
        if (g_zeroed[i].used != Z_SLOT_USED) continue;
        k = ZeroHash(g_zeroed[i].h);
        while (tmp[k].used == Z_SLOT_USED) k = (k + 1) & (ZERO_SLOTS - 1);
        tmp[k] = g_zeroed[i];
    }
    memcpy(g_zeroed, tmp, sizeof(tmp));
}

static int ZeroFind(uint64_t h) {
    unsigned i = ZeroHash(h), n = 0;

    while (n++ < ZERO_SLOTS) {
        Zeroed *z = &g_zeroed[i];
        if (z->used == Z_SLOT_FREE) return -1;
        if (z->used == Z_SLOT_USED && z->h == h) return (int)i;
        i = (i + 1) & (ZERO_SLOTS - 1);
    }
    return -1;
}

/* A tombstone, not a shift: the probe chains of every other handle stay valid. */
static void ZeroDrop(int i) {
    if (i < 0 || i >= ZERO_SLOTS) return;
    if (g_zeroed[i].used != Z_SLOT_USED) return;
    g_zeroed[i].used = Z_SLOT_DEAD;
    if (g_nZeroed > 0) g_nZeroed--;
}

/* Record a collapse. `sc` is the scale that was read before the write, or NULL
 * when the caller knows the widget sits at 1 (every marker root does). The
 * record keeps the first value it was given: a second call for a handle already
 * in the table describes a widget that is collapsed right now, so its "before"
 * value is the one already stored, not what it reads at this moment. */
static Zeroed *ZeroAdd(uint64_t h, int mode, const float *sc) {
    unsigned i = ZeroHash(h), n = 0;
    Zeroed *z = NULL;

    /* A handle that is already recorded is updated, never inserted twice: two
     * records for one widget would each have to be dropped separately, and the
     * copy left behind would claim a handle the pool has since given away. */
    {
        int old = ZeroFind(h);

        if (old >= 0) {
            g_zeroed[old].mode = (unsigned char)mode;
            g_zeroed[old].stamp = g_zeroPass;
            return &g_zeroed[old];
        }
    }
    if (g_nZeroed * 4 >= ZERO_SLOTS * 3) ZeroCompact();
    while (n++ < ZERO_SLOTS) {
        Zeroed *c = &g_zeroed[i];
        if (c->used != Z_SLOT_USED) { z = c; break; }
        i = (i + 1) & (ZERO_SLOTS - 1);
    }
    if (!z) return NULL;
    z->h = h;
    z->used = Z_SLOT_USED;
    z->mode = (unsigned char)mode;
    z->kind = 0xFF;
    z->stamp = g_zeroPass;
    z->sc[0] = sc ? sc[0] : 1.0f;
    z->sc[1] = sc ? sc[1] : 1.0f;
    z->sc[2] = sc ? sc[2] : 1.0f;
    g_nZeroed++;
    return z;
}

/* Every live record, for the sweeps that have to look at all of them. The
 * table is a hash, so "the array" is not contiguous any more. */
static int ZeroFirst(void) {
    int i;
    for (i = 0; i < ZERO_SLOTS; i++)
        if (g_zeroed[i].used == Z_SLOT_USED) return i;
    return -1;
}

static int ZeroNext(int i) {
    for (i = i + 1; i < ZERO_SLOTS; i++)
        if (g_zeroed[i].used == Z_SLOT_USED) return i;
    return -1;
}



/* Crash guard.
 *
 * This plugin writes into engine widgets, and a handle can go stale between two
 * passes: the game destroys and recycles widgets while a map is open. Writing
 * through a dead handle is an access violation inside the game process, which
 * is a crash to desktop with no explanation and nothing in the log.
 *
 * So the risky work is wrapped in SEH (structured exception handling, available
 * in plain C). A fault is caught, logged and the plugin drops every record it
 * holds - the next pass starts clean instead of hammering the same handle. If
 * faults keep coming, the icon filter switches itself off for the session
 * rather than turning the game into a crash loop.
 *
 * Writes are also preceded by a cheap sanity check on the handle, so the common
 * case (handle recycled) never even reaches the engine. */
static volatile LONG g_guardFaults;
static int           g_iconDisabled;

/* Readable vtable? A recycled or freed handle usually still points at memory,
 * so this is not proof - it only rejects the obvious garbage. */
static int WidSane(uint64_t w) {
    uint64_t vt = 0;

    if (w < 0x10000ULL) return 0;
    __try {
        vt = *(volatile uint64_t *)w;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return vt > 0x10000ULL;
}


/* Which resource ids have been hidden, so they can be put back even after the
 * widget that carried them has been destroyed and recycled.
 *
 * Handles cannot do this job on their own. The map creates icon widgets while
 * it is open and drops them when it closes; a handle record that expires in
 * that gap leaves the next widget of the same icon invisible with nothing left
 * to restore it from - which is exactly what happened after the first run that
 * worked: the icons went away and never came back when the switches did.
 * Ids survive the round trip because the id is what the archive calls the
 * texture, not what the engine happens to have allocated. */
#define HID_IDS 256
static unsigned long long g_hidIds[HID_IDS];
static int                g_nHidIds;
static unsigned char      g_hidDone[HID_IDS];     /* restored during this pass */

static int HidIdFind(unsigned long long id) {
    int i;
    for (i = 0; i < g_nHidIds; i++)
        if (g_hidIds[i] == id) return i;
    return -1;
}

static void HidIdAdd(unsigned long long id) {
    if (!id || HidIdFind(id) >= 0) return;
    if (g_nHidIds >= HID_IDS) return;
    g_hidIds[g_nHidIds++] = id;
}

static void HidIdDrop(int i) {
    if (i < 0 || i >= g_nHidIds) return;
    memmove(&g_hidIds[i], &g_hidIds[i + 1],
            (size_t)(g_nHidIds - i - 1) * sizeof(g_hidIds[0]));
    g_nHidIds--;
}

/* The two scales this plugin ever writes. SCALE (0,0,0) rather than a tiny
 * value: the quad collapses to a point, so nothing is submitted. */
static const float kNoScale[3]  = { 0.0f, 0.0f, 0.0f };
static const float kOneScale[3] = { 1.0f, 1.0f, 1.0f };

/* Put a widget back the one way this plugin hides anything. Used when the
 * record is gone but the resource id still says "this was hidden" - a rebuilt
 * widget wears a new handle. Read first so a widget that is not collapsed is
 * left alone; writing 1 unconditionally would flatten a size the layout owns. */
static void ShowOne(uint64_t w) {
    float sc[3] = { 1.0f, 1.0f, 1.0f };

    if (!WidSane(w) || !pWidgetGetV || !pWidgetSetV) return;
    if (pWidgetGetV(w, SH_P_SCALE, sc, 3) && sc[0] < 0.01f)
        pWidgetSetV(w, SH_P_SCALE, kOneScale, 3);
}

static void GuardFault(const char *where, unsigned code) {
    LONG n = InterlockedIncrement(&g_guardFaults);

    memset(g_zeroed, 0, sizeof(g_zeroed));   /* forget every handle */
    g_nZeroed = 0;
    g_nHidIds = 0;
    if (n >= 5) {
        if (!g_iconDisabled) {
            g_iconDisabled = 1;
            Log("guard: %d faults, icon filter disabled for this session; "
                "reload with the switches on to try again", (int)n);
        }
    } else {
        Log("guard: fault 0x%08X in %s - records dropped, continuing",
            code, where);
    }
}

/* Hide one widget: read its scale, collapse it, remember what it was.
 *
 * The read is both the capability test and the "who did this" test. A widget
 * whose class does not answer SH_P_SCALE cannot be collapsed and is left alone;
 * one that is already at 0 was hidden by the game (a faded blip, a marker that
 * is out of range) and is never claimed, or restoring later would draw
 * something the game had decided not to. The id and kind arrive from the walk:
 * it has already parsed them out of the same image string, and parsing them a
 * second and third time here bought nothing. */
static void HideOne(uint64_t w, const char *img, unsigned long long id,
                    int kind) {
    float sc[3] = { 1.0f, 1.0f, 1.0f };
    int ok = 0;

    if (!WidSane(w)) {
        if (ReportOnce("stale-handle-hide"))
            Log("icon hide: skipped a handle that no longer looks like a widget; "
                "the game recycled it between passes");
        return;
    }
    if (!pWidgetGetV || !pWidgetGetV(w, SH_P_SCALE, sc, 3)) {
        if (ReportOnce("no-scale-property"))
            Log("icon hide: %s answers no SH_P_SCALE - not hideable, skipped",
                img && img[0] ? img : "a widget");
        return;
    }
    if (sc[0] < 0.01f) return;               /* already down, not ours */
    ok = pWidgetSetV ? pWidgetSetV(w, SH_P_SCALE, kNoScale, 3) : 0;
    {
        Zeroed *z = ZeroAdd(w, MODE_ICON, sc);

        if (z && kind >= 0 && kind < 256) z->kind = (unsigned char)kind;
    }
    if (ReportOnce(img))
        Log("icon hide %s (%s): scale=0 wrote %d, was %g", img, IdLabel(id), ok,
            sc[0]);
}

/* A collapsed widget comes back by its scale - the value the record holds.
 *
 * Measured live 2026-10-25 through the engine's own property setters: on a
 * HUD_3D_Marker tree VISIBLE is dead data (every child reported VISIBLE=0 while
 * the marker kept drawing) and ALPHA=0 changed nothing on screen, while SCALE
 * 0/0/0 made the marker vanish and 1/1/1 brought it back. */
static void RestoreOne(uint64_t w, const Zeroed *z) {
    float sc[3] = { 1.0f, 1.0f, 1.0f };

    /* Read first: a handle the pool recycled does not answer, and writing a
     * scale to it would resize some other widget. Every record we hold came
     * from a widget that answered this read, so a failure means "gone". */
    if (!WidSane(w) || !pWidgetGetV || !pWidgetSetV) return;
    if (!pWidgetGetV(w, SH_P_SCALE, sc, 3)) return;
    pWidgetSetV(w, SH_P_SCALE, z->sc, 3);
}

/* Depth 6 and 96 children a level: a HUD scene is root -> container ->
 * rows -> icons, and the map screen nests one level more. */
#define ICON_DEPTH  6
#define ICON_KIDS   96

/* How much of a frame the walk may spend. The old limit was a flat 800
 * widgets and it is why every map icon switch did nothing: MENU_Map_Icons is
 * about 800 widgets on its own, so any pass that started with the world HUD
 * scenes ran out of budget before reaching the map, and the census (which
 * runs when the map is open and the world scenes are unloaded) saw 17 icons
 * where the enforcing pass saw 2. The count is now larger than the biggest
 * tree measured (~2.5k for HUD_3D_Marker) and a wall clock guards the
 * pathological case, so "did not reach it" stops being possible. */
#define ICON_BUDGET    40000
/* Measured from the first run with a clock: about 10 microseconds per widget,
 * so 4 ms is only 400 of them and every pass ended TIME-CUT before reaching the
 * map. One whole icon scene is ~2.5k widgets = ~25 ms, which is why a pass now
 * walks what it can inside 25 ms and the next pass resumes at the scene after
 * the one it stopped in. Census is a one-off F9 sweep, so it gets 300 ms. */
/* 40 ms, not 25: a scene must be walked to the end, because the rotation
 * advances per scene and a tree that is cut in half would never see its tail.
 * This only runs while a switch is on, so the cost is paid when it is useful. */
#define ICON_WALK_MS   8

/* Per pass counters. Without them a switch that matches nothing is
 * indistinguishable from a switch that is broken: the enforcement side wrote a
 * line only when it hid something, so "no line" meant either "no icon of that
 * kind on screen" or "the walk never saw an icon at all". */
static int g_passWidgets, g_passImages, g_passMatched, g_passHidden;
static uint64_t g_walkDeadline;   /* NowUs() value the walk must stop at */
static uint64_t NowUs(void);      /* defined below; the walk's clock */
static int      g_walkCalls, g_walkTrunc;
/* Which scene the next pass starts at. A pass that runs out of time stops
 * mid-tree, and without a rotating start the scenes after it would never be
 * visited - which is how the map switch did nothing for hours. */
static int      g_sceneStart;
/* Set when a switch is touched: run the next frame instead of waiting out the
 * sampling interval, so the effect is visible immediately. */
static int      g_iconKick;
/* When the frame callback last ran. The menu pauses the game, so a switch
 * thrown inside it does nothing until the player closes the menu and the game
 * renders again - and that silence looked exactly like a broken switch. */
static uint64_t g_lastFrameTick;

static int WalkIcons(uint64_t w, uint64_t parent, int depth, int *budget,
                     int ctx) {
    char img[96];
    int i, n;

    if (!w || depth > ICON_DEPTH || *budget <= 0) return 0;
    (*budget)--;
    /* Checked once every 16 widgets, against QueryPerformanceCounter.
     *
     * The clock is the point: GetTickCount64 ticks every 15.6 ms, so an 8 ms
     * budget written against it can only be seen as exceeded at a tick
     * boundary - a pass ran to ~15.6 ms before the cut, and every cut then
     * logged "16ms" whether the work took 800 us or 15 ms. Measured
     * 2026-10-26: 182 of 297 passes "TIME-CUT at 16ms" while the QPC perf line
     * for the same windows said 816 us to 10.7 ms. With the deadline in us the
     * budget is the budget and the number in the log means something. */
    if ((g_walkCalls++ & 0x0F) == 0 && NowUs() > g_walkDeadline) {
        *budget = 0;
        g_walkTrunc = 1;
        return 0;
    }
    g_passWidgets++;
    /* Leaves first. SH_P_IMAGE is a string property: the framework copies up to
     * 96 bytes through a vtable lookup, ~20us measured, and asking it of a
     * container can only fail. On the HUD that was 692 wasted reads a pass for
     * 14 real ones - 16 ms for 706 widgets. The child count is one cheap call:
     * only a widget with no children can carry a drawn image (every icon in
     * every census so far is `ImageWidget kids=0`). */
    n = pChildCount ? pChildCount(w) : 0;
    if (n <= 0 && pWidgetGetS &&
        pWidgetGetS(w, SH_P_IMAGE, img, sizeof(img)) && img[0]) {
        {
            unsigned long long id2 = 0;
            int hidden, zi, kidx, hi, hasId;

            g_passImages++;
            /* One parse and one table search, shared by every use below:
             * the counters, the hide decision and the record's kind. HideOne
             * used to parse the same string twice more for its record and its
             * log line. */
            hasId = ParseResId(img, &id2);
            kidx = hasId ? KindOfId(id2) : -1;
            hidden = IconHidden(img, ctx, hasId, kidx);
            zi = ZeroFind(w);
            if (hidden) {
                g_passMatched++;
                if (kidx >= 0 && kidx < NICONS) g_kindHits[kidx]++;
                else g_rawHits++;      /* hidden as a single id, not a kind */
                HidIdAdd(id2);
                if (zi < 0) {
                    g_passHidden++;
                    HideOne(w, img, id2, kidx);
                } else {
                    g_zeroed[zi].stamp = g_zeroPass;
                }
            } else {
                /* The kind is back on (or never was off): put back anything
                 * this build took away, whether or not the handle is the one
                 * that was hidden. */
                if (zi >= 0) {
                    RestoreOne(w, &g_zeroed[zi]);
                    if (ReportBackOnce(img)) Log("icon shown again: %s", img);
                    ZeroDrop(zi);
                }
                hi = id2 ? HidIdFind(id2) : -1;
                if (hi >= 0) {
                    ShowOne(w);
                    g_hidDone[hi] = 1;
                    if (ReportBackOnce(img))
                        Log("icon restored by id: %s (handle was not the one hidden)", img);
                }
            }
        }
    } else {
        /* A container (or a leaf that answers no image at all): it may be one
         * we collapsed earlier, so keep the record alive. The ZeroFind is a
         * hash probe, ~nothing next to the string read we just skipped. */
        int zi = ZeroFind(w);
        if (zi >= 0) g_zeroed[zi].stamp = g_zeroPass;
    }
    if (n > ICON_KIDS) n = ICON_KIDS;
    for (i = 0; i < n && *budget > 0; i++)
        WalkIcons(pChildAt(w, i), w, depth + 1, budget, ctx);
    return 1;
}

/* Which scenes carry icons. Not every drawn scene is walked: the front end and
 * the loading screens have their own trees, and 60 of them a second is not
 * something to spend on a HUD filter. */
static int ContainsCI(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return 0;
    for (; *hay; hay++)
        if (_strnicmp(hay, needle, n) == 0) return 1;
    return 0;
}

/* Which page owns a scene. Exact names; there is deliberately no "contains
 * map" catch-all - that fallback is how the map's rules reached the world
 * markers and the crosshair, hiding icons no row had asked about. */
static int HcSceneKind(const char *nm) {
    if (!nm || !nm[0]) return HC_CTX_NONE;
    if (strcmp(nm, "HUD_3D_Marker") == 0) return HC_CTX_WORLD;
    if (ContainsCI(nm, "Minimap")) return HC_CTX_MINIMAP;
    /* The kill marks are drawn over the crosshair; that scene carries no map
     * or minimap meaning whatever else its name says. */
    if (ContainsCI(nm, "CrossHair") || ContainsCI(nm, "HitFeedback") ||
        ContainsCI(nm, "KillFeedback")) return HC_CTX_FX;
    return HC_CTX_NONE;
}

/* ---- the minimap: one scan, one table -----------------------------------
 *
 * Every minimap switch reads the same table, and that table is filled by one
 * pass in which each node costs ONE identity read, chosen by shape:
 *
 *   kids == 2      detection clouds   -> no row owns them; walk straight through
 *   kids 6..14     enemy blip         -> the tint owner's colour
 *   kids == 0      texture icon       -> the image id
 *   anything else  container          -> recurse only
 *
 * Class names, colours and image ids are never all read for the same node:
 * that tripling is what made the old three-pass version expensive, and reading
 * a texture id from a dot (which has none) was pure waste.
 *
 * Between scans only SCALE is read - one vec3 property - which is how a slot
 * the engine puts back is caught without walking.
 */

enum { MM_BLIP = 1, MM_ICON };

#define MM_ELEMS 192

typedef struct {
    uint64_t      h;        /* the widget a hide is written to */
    int           fam;      /* MM_* */
    int           style;    /* blip style index, or icon kind for MM_ICON */
    unsigned char on;       /* we have written the hide */
    float         sc[3];    /* the scale to put back */
} MmElem;

static MmElem   g_mmEl[MM_ELEMS];
static int      g_nMmEl;
/* Last scan's table. Rebuilding used to lose the fact that a widget was already
 * pressed down by this plugin, so the next scan read a collapsed scale and
 * decided "the game is holding that one" - the record vanished and turning the
 * row back on could never restore it. A handle found again here keeps its on
 * state and the scale that was there before we collapsed it. */
static MmElem   g_mmPrev[MM_ELEMS];
static int      g_nMmPrev;
static int      g_mmOn;          /* elements held down right now */
static int      g_mmScanAge;
static int g_mmVerify = 3;
static int g_mmDotVerify = 3;
static int g_mmQuiet;   /* verify passes in a row that had to re-press nothing */
/* A cheap fingerprint of the blip pool.
 *
 * The full walk is what costs: 183 nodes, each a child count plus a colour or
 * class read, measured at 4.8-6.9 ms per walk on 2026-10-25 19:51 - and it ran
 * once a second whether or not anything had changed. Three child counts answer
 * "did the pool change shape" for tens of microseconds, so the walk only runs
 * when the answer is yes: a blip appeared, a slot was refilled, or a recorded
 * handle died.
 */
static uint64_t g_mmRoot, g_mmLayer, g_mmSlot7;
static uint64_t g_mmScanRoot;   /* the scene being walked right now */
static int      g_mmCntRoot, g_mmCntLayer, g_mmCntSlot7;
static int      g_mmSinceScan;      /* verify passes since the last full walk */
static int      g_mmDirty = 1;   /* a switch moved: act on the next frame */
static int      g_mmLogTold;     /* the hidden/back line has been written */
static int      g_mmRehits;
static int      g_mmStale;       /* handles the pool had already thrown away */
static int      g_mmScans;       /* full scans since load, for the summary */
static int      g_walkNodes;     /* widgets the last full scan visited */
static int      g_mmScenes;
static int      g_mmFresh;   /* a full scan just finished: log one line */
static int      g_mmBlind;   /* the class answers no SH_P_SCALE: not hideable */
static int      g_mmFound[MM_ICON + 1];
static float    g_blipOdd[8][3]; /* blip tints that matched no faction */
static int      g_nBlipOdd;
static uint64_t g_syncDeadline;

/* Timing for the perf line. GetTickCount64 ticks in 15.6ms steps, which made
 * every reading read as 0/16/32 - useless for work measured in microseconds.
 * QueryPerformanceCounter is the clock for this. */
static uint64_t g_markUs;
static uint64_t g_syncUsMax;
static int      g_perfPass;

static uint64_t NowUs(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;

    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    if (!freq.QuadPart) return 0;
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000ULL) / (uint64_t)freq.QuadPart);
}

static void PerfLine(uint64_t iconUs) {
    if ((g_perfPass % 60) != 0) return;
    Log("perf: marker pass %lluus, minimap sync %lluus (worst of 60 frames), "
        "icon walk %lluus", (unsigned long long)g_markUs,
        (unsigned long long)g_syncUsMax, (unsigned long long)iconUs);
    g_syncUsMax = 0;
}

/* Faction of one minimap blip, by hue rather than by palette distance: the tint
 * is scaled by the blip's own alpha and clipped at 255, so the same faction
 * reads 255,80,20 in one frame and 180,105,255 in another. Ratios to the max
 * channel hold across all of them. */
static int DotStyleOf(const float *col) {
    float c[3], n[3], mx;
    int i, allUnit = 1;

    for (i = 0; i < 3; i++) if (col[i] > 1.5f) allUnit = 0;
    for (i = 0; i < 3; i++) {
        c[i] = col[i] * (allUnit ? 255.0f : 1.0f);
        if (c[i] < 0.0f) c[i] = 0.0f;
    }
    mx = c[0];
    for (i = 1; i < 3; i++) if (c[i] > mx) mx = c[i];
    if (mx < 20.0f) return -1;
    for (i = 0; i < 3; i++) n[i] = c[i] / mx;
    if (n[0] >= 1.0f && n[1] <= 0.60f && n[2] <= 0.45f) return 0;  /* red */
    if (n[2] >= 1.0f && n[0] <= 0.85f && n[1] <= 0.55f) return 1;  /* purple */
    if (n[1] >= 1.0f && n[0] <= 0.80f && n[2] <= 0.80f) return 2;  /* green */
    if (n[2] >= 1.0f && n[1] >= 0.45f && n[0] <= 0.55f) return 3;  /* blue */
    if (n[0] > 0.55f && n[1] > 0.55f) return 4;                    /* white */
    return -1;
}

/* The colour owner of a blip, or 0 when this widget is not a blip.
 * It was child 0 in the dump, but the pool rebuilds its slots and the order is
 * not part of any contract - look at the first three. */
static uint64_t MapBlipOwner(uint64_t c) {
    uint64_t ch;
    char cls[56];
    float col[3] = {-1.0f, -1.0f, -1.0f};
    int kids, k;

    if (!c || !pChildCount || !pChildAt) return 0;
    kids = pChildCount(c);
    if (kids < 6 || kids > 14) return 0;
    cls[0] = 0;
    if (!pWidgetClass || !pWidgetClass(c, cls, sizeof(cls))) return 0;
    if (!strstr(cls, "Container")) return 0;
    for (k = 0; k < kids && k < 3; k++) {
        ch = pChildAt(c, k);
        if (!ch || pChildCount(ch) != 2) continue;
        cls[0] = 0;
        if (!pWidgetClass || !pWidgetClass(ch, cls, sizeof(cls))) continue;
        if (!strstr(cls, "Container")) continue;
        if (!pWidgetGetV || !pWidgetGetV(ch, SH_P_COLOUR, col, 3)) continue;
        if (col[0] < 0.05f && col[1] < 0.05f && col[2] < 0.05f) continue;
        return ch;
    }
    return 0;
}

/* The census's shape table: for each coloured container the walk meets, how
 * many children it has and what tint it draws with. Printing it is how the blip
 * shapes were identified when the tree was first mapped (2026-10-22); the rows
 * themselves no longer use it. */




/* Which switch owns an element. Read from the live state, never from a mask:
 * one row, one owner, no page driving another page's kinds. */
static int MmShouldHide(int fam, int style) {
    switch (fam) {
    case MM_BLIP:
        /* Blue and green are the player's own side; no row controls them any
         * more, so they are never pressed. */
        if (style == 2 || style == 3) return 0;
        /* 0 = on, 1 = view-only (a world concept; a blip stays), 2 = off */
        return !g_state.mm_enemy;
    case MM_ICON:
        /* By kind, per scene: 7 and 23 are vehicles on the minimap, 0 and 13
         * are collectibles. Consulting mm_vehicle / mm_collect directly is
         * what stops a map-page vehicle row turning the minimap's off too. */
        if (style == 7 || style == 23) return !g_state.mm_vehicle;
        if (style == 0 || style == 13) return !g_state.mm_collect;
        return g_iconOff[style] != 0;
    }
    return 0;
}

static int MmAnyOff(void) {
    if (!g_state.mm_enemy) return 1;
    if (!g_state.mm_vehicle || !g_state.mm_collect) return 1;
    if (g_iconOff[0] || g_iconOff[23]) return 1;
    return 0;
}

static int MmCountOn(void) {
    int i, n = 0;

    for (i = 0; i < g_nMmEl; i++) if (g_mmEl[i].on) n++;
    return n;
}

/* What this node is, or 0. Exactly one identity property is read. */
static int MmIdentOf(uint64_t w, int kids, int slot, int *styleOut) {
    float col[3] = {-1.0f, -1.0f, -1.0f};
    unsigned long long id = 0;
    char img[96];
    int k;

    *styleOut = -1;
    /* Slot first. The icon layer is a fixed 20-slot strip, and slot 7 is the
     * enemy group - measured 2026-10-25 with Cheat Engine: eight six-to-
     * fourteen child containers there, the player's own group in slot 6. A
     * colour read has to guess the faction from a tint the engine may rewrite,
     * and gives up entirely on a tint that is not in the table; a slot cannot
     * be repainted. The colour test below stays as the fallback for the frames
     * where the layer index is not known. */
    if (slot == 7 && kids >= 6 && kids <= 14) { *styleOut = 0; return MM_BLIP; }
    if (kids >= 6 && kids <= 14) {           /* enemy blip */
        uint64_t owner = MapBlipOwner(w);

        if (!owner) return 0;
        if (!pWidgetGetV || !pWidgetGetV(owner, SH_P_COLOUR, col, 3)) return 0;
        k = DotStyleOf(col);
        if (k < 0) {
            int i, seen = 0;

            for (i = 0; i < g_nBlipOdd; i++)
                if (col[0] - g_blipOdd[i][0] < 0.03f &&
                    g_blipOdd[i][0] - col[0] < 0.03f &&
                    col[1] - g_blipOdd[i][1] < 0.03f &&
                    g_blipOdd[i][1] - col[1] < 0.03f &&
                    col[2] - g_blipOdd[i][2] < 0.03f &&
                    g_blipOdd[i][2] - col[2] < 0.03f)
                    seen = 1;
            if (!seen && g_nBlipOdd < (int)(sizeof(g_blipOdd) / sizeof(g_blipOdd[0]))) {
                g_blipOdd[g_nBlipOdd][0] = col[0];
                g_blipOdd[g_nBlipOdd][1] = col[1];
                g_blipOdd[g_nBlipOdd][2] = col[2];
                g_nBlipOdd++;
                Log("   blip tint not a faction: %g,%g,%g", col[0], col[1], col[2]);
            }
            return 0;
        }
        *styleOut = k;
        return MM_BLIP;
    }
    if (kids == 0) {                         /* leaf: a texture icon */
        /* With both icon rows on, this read cannot change one thing.
         *
         * MmShouldHide answers every MM_ICON style out of those two flags:
         * 7 and 23 from mm_vehicle, 0 and 13 from mm_collect, and every other
         * style from g_iconOff - which HcApplyState only ever sets at 0 and
         * 23, i.e. the same two rows. So with both on, the answer is "not
         * mine" for every kind that exists, and the string read (the expensive
         * call in this walk, ~10-20us, against ~1us for the child count) buys
         * nothing. It is paid for every leaf of the icon pool - a few hundred
         * of them, of which three carry a texture - so skipping it is what
         * makes a scan while only the enemy row is off cost hundreds of
         * microseconds instead of milliseconds.
         *
         * Nothing is lost by not recording them: an element is only ever
         * written to when MmShouldHide says hide, and the moment one of these
         * rows goes off the table is rebuilt (g_mmDirty). A row that goes back
         * on is served by MmDropLeftovers, which hands back every element the
         * previous table had pressed down. */
        if (g_state.mm_vehicle && g_state.mm_collect) return 0;
        if (!pWidgetGetS || !pWidgetGetS(w, SH_P_IMAGE, img, sizeof(img)) ||
            !img[0])
            return 0;
        if (!ParseResId(img, &id)) return 0;
        k = KindOfId(id);
        if (k < 0) return 0;
        *styleOut = k;
        return MM_ICON;
    }
    return 0;
}

/* Hide, keep down, or put back - one element. Scale is the only mechanism:
 * read it, collapse it, and remember what it was. A widget that already reads 0
 * was hidden by the game (an off-screen blip, a faded one) and is never
 * claimed, or restoring later would draw something the game had decided to
 * hide; one whose class answers no SH_P_SCALE cannot be collapsed at all and is
 * counted instead, so "the row does nothing here" is visible in the log. */
static void MmApplyElem(MmElem *e) {
    float sc[3] = { 1.0f, 1.0f, 1.0f };
    int hide;

    if (!e->h) return;
    if (!WidSane(e->h)) { e->h = 0; e->on = 0; return; }
    hide = MmShouldHide(e->fam, e->style);
    if (!pWidgetSetV) return;

    if (hide) {
        if (e->on) return;                     /* already ours */
        if (!pWidgetGetV || !pWidgetGetV(e->h, SH_P_SCALE, sc, 3)) {
            g_mmBlind++;
            return;
        }
        if (sc[0] < 0.01f) return;             /* down, but not by us */
        pWidgetSetV(e->h, SH_P_SCALE, kNoScale, 3);
        e->sc[0] = sc[0];
        e->sc[1] = sc[1];
        e->sc[2] = sc[2];
        e->on = 1;
    } else if (e->on) {
        pWidgetSetV(e->h, SH_P_SCALE, e->sc, 3);
        e->on = 0;
    }
}

/* One new table entry, carrying over what the previous scan knew about this
 * handle. Without the carry-over a widget we had already collapsed read back as
 * scale 0, looked like the game's own doing, and lost its record - so the row
 * could never put it back. */
static void MmNewElem(uint64_t w, int fam, int style) {
    MmElem *e;
    int j;

    if (!w || g_nMmEl >= MM_ELEMS) return;
    e = &g_mmEl[g_nMmEl++];
    e->h = w;
    e->fam = fam;
    e->style = style;
    e->on = 0;
    e->sc[0] = e->sc[1] = e->sc[2] = 1.0f;
    for (j = 0; j < g_nMmPrev; j++) {
        if (g_mmPrev[j].h == w && g_mmPrev[j].on) {
            e->on = 1;                       /* still ours */
            e->sc[0] = g_mmPrev[j].sc[0];
            e->sc[1] = g_mmPrev[j].sc[1];
            e->sc[2] = g_mmPrev[j].sc[2];
            break;
        }
    }
    MmApplyElem(e);
}

/* Handles the previous scan pressed down and this one did not see at all: the
 * group was rebuilt, or the row no longer wants them. Give them back so nothing
 * is left collapsed with no record pointing at it.
 *
 * No read-back gate: the scale in the record is the value this plugin wrote
 * over, so putting it back is unconditional. The old version asked VISIBLE
 * first, which is never 0 on a scale-collapsed widget - every such leftover
 * stayed collapsed, and if the pool handed that widget to another faction the
 * new blip was invisible. */
static void MmDropLeftovers(void) {
    int i, j, n = 0;

    for (i = 0; i < g_nMmPrev; i++) {
        MmElem *p = &g_mmPrev[i];
        int found = 0;

        if (!p->h || !p->on) continue;
        for (j = 0; j < g_nMmEl; j++)
            if (g_mmEl[j].h == p->h) { found = 1; break; }
        if (found) continue;
        if (MmShouldHide(p->fam, p->style)) continue;
        if (WidSane(p->h) && pWidgetSetV) {
            pWidgetSetV(p->h, SH_P_SCALE, p->sc, 3);
            n++;
        }
    }
    if (n) Log("minimap: %d leftover element(s) handed back", n);
}

static void MmWalk(uint64_t w, int depth, int *budget, int slot) {
    int i, kids, style, fam;

    if (!w || depth > 8 || *budget <= 0 || g_nMmEl >= MM_ELEMS) return;
    (*budget)--;
    g_walkNodes++;
    kids = pChildCount ? pChildCount(w) : 0;
    /* Only the scene the fingerprint belongs to may set these. Taking them
     * from whichever scene happened to be walked last meant two scans could
     * describe two different scenes, the fingerprint never matched, and every
     * pool change cost two walks instead of one. */
    if (kids == 20 && g_mmScanRoot == g_mmRoot) g_mmLayer = w;
    fam = MmIdentOf(w, kids, slot, &style);
    if (fam) {
        MmNewElem(w, fam, style);
        g_mmFound[fam]++;
        return;                              /* a dot has no dots inside it */
    }
    if (kids > 64) kids = 64;
    for (i = 0; i < kids && *budget > 0; i++) {
        uint64_t ch = pChildAt(w, i);

        /* Slot 7 of the icon strip is the enemy group; remembering it turns
         * "did a blip appear" into one child-count read. */
        if (kids == 20 && i == 7 && g_mmScanRoot == g_mmRoot) g_mmSlot7 = ch;
        MmWalk(ch, depth + 1, budget, kids == 20 ? i : -1);
    }
}

static void MmLogs(void) {
    int hidden = MmCountOn();

    if (hidden > 0 && !g_mmLogTold) {
        Log("minimap: %d element(s) hidden - blips %d, icons %d, "
            "%d widget(s) walked (%d scene(s))", hidden,
            g_mmFound[MM_BLIP], g_mmFound[MM_ICON], g_walkNodes, g_mmScenes);
        g_mmLogTold = 1;
    } else if (hidden == 0 && g_mmLogTold) {
        Log("minimap: every hidden element is back");
        g_mmLogTold = 0;
    }
    /* One line per ten scans while any row is off: what the walk saw, and how
     * much of it it is holding down. A row that "does nothing" is then either
     * "found 0" or "found some, held none", and those need different fixes. */
    if (g_mmFresh && g_mmScenes && (g_dbg || MmAnyOff() || (g_mmScans % 60) == 0)) {
        g_mmFresh = 0;
        Log("minimap scan: blips=%d icons=%d -> "
            "%d hidden, %d stale, %d blind, %d widget(s) walked "
            "(sig %d/%d/%d)",
            g_mmFound[MM_BLIP], g_mmFound[MM_ICON],
            hidden, g_mmStale, g_mmBlind, g_walkNodes,
            g_mmCntRoot, g_mmCntLayer, g_mmCntSlot7);
    }
    if (g_mmRehits && (g_mmRehits % 500) == 1 && g_dbg)
        Log("minimap: a slot had to be pushed down again (%d so far)",
            g_mmRehits);
}

/* A full scan once a second; between scans only the cached elements are
 * verified, every third frame, one property read each. */
/* 0 = the pool looks exactly like it did after the last walk. */
static int MmPoolChanged(void) {
    if (!g_mmRoot || !pChildCount) return 1;
    if (pChildCount(g_mmRoot) != g_mmCntRoot) return 1;
    if (g_mmLayer && pChildCount(g_mmLayer) != g_mmCntLayer) return 1;
    if (g_mmSlot7 && pChildCount(g_mmSlot7) != g_mmCntSlot7) return 1;
    return 0;
}

static void SyncMinimapDots(void) {
    int i, n, repress = 0;

    if (!pSceneCount || !pSceneAt || !pSceneName || !pSceneRoot) return;
    if (!pChildCount || !pChildAt) return;
    /* The icon scene is not drawn at all: there is nothing in it to press. */
    if (!g_state.mm_icons) { g_mmOn = 0; return; }
    /* Nothing switched off, nothing recorded: there is nothing to do now or
     * later. This used to idle for 60 passes and then fall through to a full
     * scan on every single frame, which is where "minimap sync 5479us" came
     * from in a session where every row was on. */
    if (!g_mmOn && !g_mmDirty && !MmAnyOff()) return;
    if (g_mmScanAge > 0 && !g_mmDirty) {
        /* Still ours to watch: a cheap re-press every third frame. */
        g_mmScanAge--;
        if (--g_mmDotVerify > 0) return;
        g_mmSinceScan++;
        /* A collapsed scale is not a flag the engine flips back the way it
         * flips VISIBLE, so the re-press pass can run far less often - which
         * is the point of using it. Once a dozen passes in a row have needed
         * no re-press at all, drop to one check a second: the loop then costs
         * an occasional read instead of a read per element twice a frame. */
        g_mmDotVerify = g_mmQuiet > 12 ? 60 : 15;
        /* Walk again only when the pool changed shape, or after a long quiet
         * stretch as a safety net. One frame later is soon enough. */
        if (g_mmSinceScan >= 600 || MmPoolChanged()) {
            g_mmScanAge = 0;          /* next frame: full walk */
            return;
        }
        /* The pool is unchanged, so the walk would find exactly the table we
         * already have - and it costs 5-7 ms. Keep this window alive instead
         * of letting the age run out into a scan. */
        g_mmScanAge = 60;
        for (i = 0; i < g_nMmEl; i++) {
            MmElem *e = &g_mmEl[i];
            float sc[3] = { 1.0f, 1.0f, 1.0f };

            if (!e->h) continue;
            if (!e->on) { MmApplyElem(e); continue; }
            /* One read per element, and it is also the liveness test: a handle
             * the pool threw away stops answering, and the next pass rescans.
             * No WidSane() here - a VirtualQuery per element every pass is
             * exactly the sort of cost this row is trying to shed. */
            if (!pWidgetGetV || !pWidgetGetV(e->h, SH_P_SCALE, sc, 3)) {
                e->h = 0;
                e->on = 0;
                g_mmDirty = 1;
                g_mmStale++;
                continue;
            }
            if (sc[0] > 0.01f) {
                pWidgetSetV(e->h, SH_P_SCALE, kNoScale, 3);
                g_mmRehits++;
                repress++;
            }
        }
        if (repress) g_mmQuiet = 0;
        else if (g_mmQuiet < 1000) g_mmQuiet++;
        g_mmOn = MmCountOn();
        MmLogs();
        return;
    }
    g_mmScanAge = 60;
    g_mmDirty = 0;
    g_mmScans++;
    g_mmScenes = 0;
    g_walkNodes = 0;
    g_mmBlind = 0;
    /* Keep last scan's table as the memory of what we pressed. Without it every
     * rebuild turned our own presses into "the game hid that", and the row could
     * not hand them back. */
    memcpy(g_mmPrev, g_mmEl, sizeof(MmElem) * (size_t)g_nMmEl);
    g_nMmPrev = g_nMmEl;
    g_nMmEl = 0;
    memset(g_mmFound, 0, sizeof(g_mmFound));
    g_syncDeadline = GetTickCount64() + 8;
    g_mmRoot = g_mmLayer = g_mmSlot7 = 0;
    n = pSceneCount();
    if (n > 48) n = 48;
    for (i = 0; i < n; i++) {
        uint64_t sc = pSceneAt(i), root;
        char nm[64];
        int budget = 4000;       /* per scene: one shared budget starved the
                                  * last scene, so half the dots were never
                                  * reached and "teammates" looked broken */

        if (!pSceneName(sc, nm, sizeof(nm))) continue;
        /* Only the icon layer. HUD_Minimap is the map canvas - a 3072x3072
         * surface with its own copy of the icon strip - and nothing on this
         * page controls it: walking it pulled the canvas's own dots into the
         * icon table and spent the node budget the real icon layer needed. */
        if (!ContainsCI(nm, "MinimapIcons")) continue;
        root = pSceneRoot(sc);
        if (!root) continue;
        if (!g_mmRoot) g_mmRoot = root;   /* fingerprint follows the first one */
        g_mmScanRoot = root;
        MmWalk(root, 0, &budget, -1);
        g_mmScenes++;
    }
    g_mmScanRoot = 0;
    g_mmCntRoot = (g_mmRoot && pChildCount) ? pChildCount(g_mmRoot) : 0;
    g_mmCntLayer = (g_mmLayer && pChildCount) ? pChildCount(g_mmLayer) : 0;
    g_mmCntSlot7 = (g_mmSlot7 && pChildCount) ? pChildCount(g_mmSlot7) : 0;
    g_mmSinceScan = 0;
    MmDropLeftovers();
    g_mmOn = MmCountOn();
    g_mmFresh = 1;
    MmLogs();
}

/* Which faction a colour belongs to. The game keeps the marker tint in a style
 * resource as R,G,B in 0..255 (hud_marker_colours.h is generated from those),
 * while a widget's SH_P_COLOUR hands back 0..1, so scale first. The tolerance
 * is 24 per channel: the six styles are far further apart than that, and the
 * game does not blend these tints - a marker is Santa Blanca red or it is not.
 * Returns NULL when the colour is none of them, which is the interesting case:
 * it means the tint the marker draws with is not a widget colour at all. */
static const char *MarkerStyleOf(const float *col, unsigned char *rgbOut) {
    float c[3];
    int i, best = -1, allUnit = 1;
    long bestD = 0x7FFFFFFFL;
    /* Decide the scale for the whole triple, never per channel: a mixed one
     * like (1.0, 71.6, 0) is not a colour, and scaling its 1.0 to 255 turned
     * it into a bogus Santa Blanca red in the 16:06 run. */
    for (i = 0; i < 3; i++)
        if (col[i] > 1.5f) allUnit = 0;
    for (i = 0; i < 3; i++) {
        c[i] = col[i] * (allUnit ? 255.0f : 1.0f);
        if (c[i] < 0.0f) c[i] = 0.0f;
        if (c[i] > 255.0f) c[i] = 255.0f;
    }
    for (i = 0; i < MARKER_COLOUR_COUNT; i++) {
        long dr = (long)(c[0] + 0.5f) - kMarkerColours[i].r;
        long dg = (long)(c[1] + 0.5f) - kMarkerColours[i].g;
        long db = (long)(c[2] + 0.5f) - kMarkerColours[i].b;
        long d = dr * dr + dg * dg + db * db;
        if (d < bestD) { bestD = d; best = i; }
    }
    if (rgbOut) {
        rgbOut[0] = (unsigned char)(c[0] + 0.5f);
        rgbOut[1] = (unsigned char)(c[1] + 0.5f);
        rgbOut[2] = (unsigned char)(c[2] + 0.5f);
    }
    if (best < 0 || bestD > 3L * 24L * 24L) return NULL;
    return kMarkerColours[best].style;
}

/* The 3D marker objects are not widgets, so the colour cannot be asked for the
 * way an image widget's can. But if a marker draws in a faction colour, that
 * colour has to be inside the object somewhere - so read the object and look
 * for the palette. Read only, and only on F9; nothing here writes. */

/* Phoenix objects carry MSVC RTTI, so a pointer can be told apart from a
 * number before anything is called through it: vtable -> complete object
 * locator -> type descriptor -> ".?AVClassName@@" all have to land inside the
 * game's own image. That check is what makes it safe to try the widget calls
 * on a pointer found inside an object of unknown type. */
static uint64_t g_peBase, g_peEnd;

static void ResolveImage(void) {
    HMODULE mod;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;

    if (g_peEnd) return;
    mod = GetModuleHandleA(NULL);
    if (!mod) return;
    dos = (IMAGE_DOS_HEADER *)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    nt = (IMAGE_NT_HEADERS *)((char *)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    g_peBase = (uint64_t)(uintptr_t)mod;
    g_peEnd = g_peBase + nt->OptionalHeader.SizeOfImage;
}

static int InImage(uint64_t p, int n) {
    return p >= g_peBase && p + (uint64_t)n <= g_peEnd;
}

/* "ImageWidget" for a real phoenix object, 0 for anything else. */
static int ObjectClass(uint64_t obj, char *out, int n) {
    uint64_t vt, loc, td;
    uint32_t rva;
    char name[96];
    const char *e;

    ResolveImage();
    if (!g_peEnd || !SafeRead(&vt, obj, 8) || !InImage(vt, 8)) return 0;
    if (!SafeRead(&loc, vt - 8, 8) || !InImage(loc, 16)) return 0;
    if (!SafeRead(&rva, loc + 12, 4) || !rva) return 0;
    td = g_peBase + rva;
    if (!InImage(td, 24)) return 0;
    if (!SafeRead(name, td + 16, (int)sizeof(name) - 1)) return 0;
    name[sizeof(name) - 1] = 0;
    if (strncmp(name, ".?AV", 4) != 0) return 0;
    e = strchr(name + 4, '@');
    if (!e || e == name + 4 || e - (name + 4) >= n) return 0;
    memcpy(out, name + 4, (size_t)(e - (name + 4)));
    out[e - (name + 4)] = 0;
    return 1;
}

/* The marker objects answer no widget call, so look inside them for the widget
 * instance they might hold. Everything on this path is a plain read through
 * SafeRead - no engine call is made on a pointer until it is known to be a
 * widget.
 *
 * That restriction is not caution for its own sake: the first version of this
 * probe called ShWidgetGetS on any pointer that survived the RTTI check, and
 * the game died at GRW.exe+0x32F1478 on `mov rax,[rdi+0x130]` with rdi taken
 * from [obj+0x20]. A Phoenix "WidgetPrivate" is a real C++ object with real
 * RTTI, but it is not a widget, and [obj+0x20] is not its private pointer. So
 * the class name is the only thing read here; the getters come later, once a
 * name says ImageWidget. */

/* Walk what the marker container really holds. The 28 objects under
 * HUD_3D_Marker are not widgets themselves - [obj+0] is not a vtable - but
 * [obj+0x20] is a DynamicContainerWidgetPrivate, which is exactly what
 * ShWidgetChildCount already reads (it returned 14 for one of them). So the
 * child array at priv+0x1A8 is the engine's own list, and every pointer in it
 * is a real widget handle: class name, child count and SH_P_IMAGE can be asked
 * for on those without guessing.
 *
 * Read only; the engine calls are made on engine-owned child pointers only,
 * the same ones WalkIcons already walks. */


/* ---- one switch per faction, for the 3D markers -------------------------
 *
 * The 28 objects under HUD_3D_Marker are one per marker drawn. Each is a
 * ContainerWidget holding the icon, the ring and the labels, and none of them
 * answers SH_P_IMAGE - so the texture id cannot separate the factions here.
 * What does is the colour the widgets report, which is the style resource the
 * marker template depends on (see hud_marker_colours.h): Santa Blanca red,
 * Unidad purple, the player's own side blue, HVT white, loot gold.
 *
 * So there is one row per style, and a marker is collapsed when the style found
 * anywhere in its subtree has its bit set. Text labels are skipped when
 * classifying: their colour is the text tint, not the faction. Blue (the squad)
 * has a row like everything else - the player decides what stays on screen, and
 * the master row still means "enemies only", so the squad is not swept up by it.
 *
 * Two things had to be fixed before this could be trusted, both from the first
 * build where some markers never came back:
 *
 *   - the collapse table was 64 entries while one marker is a dozen widgets,
 *     so it filled up, the oldest records were dropped, and those widgets
 *     stayed at scale 0 for good. It is 1024 now;
 *   - records expired after twenty passes whether or not they had been
 *     restored. Marker records are never expired while the switch is on, and
 *     restoring walks the scene instead of trusting the handles: a widget that
 *     is no longer in the tree is gone, and writing its old scale would be a
 *     write into freed memory. */
#define MARK_BIT(style)   (1L << (style))
/* Three hostile colours are one switch: Santa Blanca (red), Unidad (purple)
 * and the HVT / neutral grey. The minimap has a single enemy dot texture for
 * all three, so a per-faction split there was never possible - the player asked
 * for one row that means "enemies", and one row is what the data can honour.
 *
 * Green and blue are the player's own side and have no row at all: this single
 * row must never reach them. Gold is never worn by a faction marker in this
 * build, so it has no row either; markers_off= still accepts it by name. */
#define MARK_ENEMY_MASK   (MARK_BIT(0) | MARK_BIT(1) | MARK_BIT(4))

static int g_marksTold;
static int g_restoreTries;

/* Menu rows no longer come from the colour table: the page is one fixed row
 * (the hostile group), built in EnsureMenu. */

/* The style a single widget reports, plus its brightness ratio.
 *
 * Matching is on chromaticity, not on the raw triple: the channel values the
 * game writes are not always the archive's exact numbers (Santa Blanca reads
 * 255,54,0 where the style resource says 235,50,0), and a colour scaled down by
 * anything - distance fade, a transition, a material multiplier - keeps its
 * chromaticity while losing brightness. Comparing absolute values matched one
 * marker out of twenty-eight; comparing normalised triples matches all of them.
 *
 * The ratio is reported for diagnosis only. It is NOT a view or occlusion
 * signal: three separate attempts to tie it to one were disproved, so no switch
 * is built on it.
 *
 * Labels are skipped: their colour is the text tint, not the faction. */
static int MarkerStyleEx(const float *col, float *ratioOut, unsigned char *rgbOut) {
    float v[3], n[3], m = 0.0f, bestD = 1e9f;
    int i, k, best = -1;

    for (i = 0; i < 3; i++) v[i] = col[i] < 0.0f ? 0.0f : col[i];
    for (i = 0; i < 3; i++) if (v[i] > m) m = v[i];
    if (m > 0.0f && m <= 1.5f) {                /* a 0..1 colour after all */
        for (i = 0; i < 3; i++) v[i] *= 255.0f;
        m *= 255.0f;
    }
    if (m < 8.0f) return -1;                    /* black: nothing to read */
    for (i = 0; i < 3; i++) n[i] = v[i] / m;
    for (i = 0; i < MARKER_COLOUR_COUNT; i++) {
        float p[3], pm = (float)kMarkerColours[i].r, d = 0.0f;
        if ((float)kMarkerColours[i].g > pm) pm = (float)kMarkerColours[i].g;
        if ((float)kMarkerColours[i].b > pm) pm = (float)kMarkerColours[i].b;
        if (pm < 1.0f) continue;
        p[0] = kMarkerColours[i].r / pm;
        p[1] = kMarkerColours[i].g / pm;
        p[2] = kMarkerColours[i].b / pm;
        for (k = 0; k < 3; k++) {
            float t = n[k] - p[k];
            d += t * t;
        }
        if (d < bestD) { bestD = d; best = i; }
    }
    if (best < 0 || bestD > 0.08f * 0.08f * 3.0f) return -1;
    if (rgbOut) {
        rgbOut[0] = (unsigned char)(v[0] + 0.5f);
        rgbOut[1] = (unsigned char)(v[1] + 0.5f);
        rgbOut[2] = (unsigned char)(v[2] + 0.5f);
    }
    if (ratioOut) {
        float pm = (float)kMarkerColours[best].r;
        if ((float)kMarkerColours[best].g > pm) pm = (float)kMarkerColours[best].g;
        if ((float)kMarkerColours[best].b > pm) pm = (float)kMarkerColours[best].b;
        *ratioOut = (pm > 0.0f) ? (m / pm) : 0.0f;
    }
    return best;
}

static int MarkerStyleAt(uint64_t w, float *ratioOut) {
    char cls[48];
    float col[3];

    cls[0] = 0;
    if (pWidgetClass) pWidgetClass(w, cls, sizeof(cls));
    if (strstr(cls, "Label")) return -1;
    if (!pWidgetGetV || !pWidgetGetV(w, SH_P_COLOUR, col, 3)) return -1;
    return MarkerStyleEx(col, ratioOut, NULL);
}

static int MarkerSubtreeStyle(uint64_t w, int depth, float *ratioOut) {
    int i, kids, st;
    float r = 1.0f;

    if (!w || depth > 3) return -1;
    st = MarkerStyleAt(w, &r);
    if (st >= 0) {
        if (ratioOut) *ratioOut = r;
        return st;
    }
    kids = pChildCount ? pChildCount(w) : 0;
    if (kids <= 0 || kids > 64) return -1;
    for (i = 0; i < kids; i++) {
        st = MarkerSubtreeStyle(pChildAt(w, i), depth + 1, ratioOut);
        if (st >= 0) return st;
    }
    return -1;
}



/* Root of the marker scene and how many markers it holds, or -1 if it is not
 * drawing right now. */
/* The marker scene, found once.
 *
 * This used to walk the scene list and strcmp every name on every pass - about
 * forty string compares to answer a question whose answer changes only when the
 * game rebuilds the HUD. The root is cached and validated by three cheap reads
 * (child count in range, scene count unchanged, and a rescan every few hundred
 * passes), so a stale handle cannot point the pass at the wrong tree for long.
 * The scan cap also went from 24 to 48: the map screen pushes the marker scene
 * past 24, and the pass then silently reported "not drawing". */
static uint64_t g_markRootC;
static int      g_markScenesC = -1;
static int      g_markScanAge;

static void MarkerCollapseAll(void);      /* defined below, called by MarkerTick */

static int MarkerSceneKids(uint64_t *rootOut) {
    int i, n, kids = -1;

    if (!pSceneCount || !pSceneAt || !pSceneName || !pSceneRoot) return -1;
    n = pSceneCount();
    if (g_markRootC && ++g_markScanAge < 300 && n == g_markScenesC &&
        pChildCount) {
        kids = pChildCount(g_markRootC);
        if (kids > 0 && kids <= 64) { *rootOut = g_markRootC; return kids; }
    }
    g_markRootC = 0;
    g_markScanAge = 0;
    if (n > 48) n = 48;
    for (i = 0; i < n; i++) {
        uint64_t sc = pSceneAt(i);
        char nm[64];
        if (!pSceneName(sc, nm, sizeof(nm))) continue;
        if (strcmp(nm, "HUD_3D_Marker") != 0) continue;
        *rootOut = pSceneRoot(sc);
        kids = pChildCount ? pChildCount(*rootOut) : -1;
        if (kids > 0 && kids <= 64) {
            g_markRootC = *rootOut;
            g_markScenesC = n;
        }
        return kids;
    }
    return -1;
}

/* ---- marker pass cadence ------------------------------------------------
 *
 * A full pass is about a millisecond, so it cannot run every frame; but the old
 * 30-frame gate meant an enemy marker that spawned just after a pass stayed on
 * screen for up to half a second (that is the "red dot that appears and only
 * then blinks out" the row was reported for). The trigger is the tree's own
 * shape - a new marker changes a slot's child count - which costs a handful of
 * property reads, and the 30-frame tick stays as the safety net for a marker
 * the engine re-laid-out without changing the count. */
static int g_markFp = -1;
static int g_markTick;
static int g_markIdle;

static int MarkerShape(uint64_t root, int kids) {
    int k, sum = kids;

    if (!pChildAt || !pChildCount) return -kids;
    for (k = 0; k < kids && k < 64; k++) {
        uint64_t slot = pChildAt(root, k);
        int subs = slot ? pChildCount(slot) : 0;

        sum = sum * 31 + (subs & 0xFF);
    }
    return sum;
}

static void MarkerTick(void) {
    uint64_t root = 0;
    int kids, fp;
    uint64_t t0;

    if (!InterlockedCompareExchange(&g_marksOn, 0, 0)) return;
    /* The shape probe is 2 reads per slot (~56 on a full screen). Every third
     * frame is ~30 Hz on this build (the frame callback measures 91 Hz): a
     * third of the frames, ~19 reads per frame on average, and 33 ms to react -
     * which is still finer than a marker appearing can be followed by eye. The
     * cheaper side of the trade, as agreed: cost first, and nothing is bought
     * back with cost that the eye cannot see. */
    if ((++g_markTick % 3) != 0) return;
    kids = MarkerSceneKids(&root);
    if (kids <= 0 || kids > 64) { g_markFp = -1; g_markIdle = 0; return; }
    fp = MarkerShape(root, kids);
    /* Unchanged shape: still pass once every ~0.5 s (15 probes at 30 Hz),
     * because the engine can put a marker's scale back, and 视野盲区 can flip a
     * marker's hollow/solid state, without either of them touching the tree. */
    if (fp == g_markFp && ++g_markIdle < 15) return;
    g_markIdle = 0;
    g_markFp = fp;
    t0 = NowUs();
    MarkerCollapseAll();
    g_markUs = NowUs() - t0;
}

/* What the labels of one marker actually say. The marker templates name their
 * label instances (DistanceSanta, DistanceUnidad, DistanceHVT, DistanceAdd,
 * Objective for the hostiles; playerName and distance for the squad), so the
 * text is either a distance like "251m" or, for a teammate, their name - which
 * is a second way to tell the factions apart when the colour cannot be read. */
static void MarkerLabelLine(uint64_t w, int depth, char *out, int n) {
    int i, kids, used = (int)strlen(out);
    char txt[48];

    if (!w || depth > 3 || used >= n - 2) return;
    txt[0] = 0;
    if (pWidgetGetS && pWidgetGetS(w, SH_P_TEXT, txt, sizeof(txt)) && txt[0]) {
        int l = (int)strlen(txt);
        if (used + l + 2 < n) {
            if (used) out[used++] = '|';
            memcpy(out + used, txt, (size_t)l);
            used += l;
            out[used] = 0;
        }
    }
    kids = pChildCount ? pChildCount(w) : 0;
    if (kids <= 0 || kids > 64) return;
    for (i = 0; i < kids; i++) MarkerLabelLine(pChildAt(w, i), depth + 1, out, n);
}

/* One marker is a ContainerWidget whose first child is the icon ImageWidget,
 * carrying the faction colour, followed by the ring container and the labels.
 * A slot under the scene root holds SEVERAL of them (the engine pools marker
 * widgets), which is why judging a slot as one marker collapsed whole batches
 * and matched one style out of eight.
 *
 * The dim the game draws on an out-of-view target is read as well: the marker
 * container's own alpha drops while the icon keeps full alpha, so both are
 * reported and the threshold is applied to the container. */
static int MarkerIsOne(uint64_t m) {
    uint64_t icon;
    char cls[48];
    int kids;

    if (!m) return 0;
    kids = pChildCount ? pChildCount(m) : 0;
    if (kids < 4 || kids > 40) return 0;
    icon = pChildAt(m, 0);
    if (!icon) return 0;
    cls[0] = 0;
    if (!pWidgetClass || !pWidgetClass(icon, cls, sizeof(cls))) return 0;
    return strstr(cls, "Image") != NULL;
}

/* style, brightness ratio of the icon, container alpha, icon alpha. */
static int MarkerInfo(uint64_t m, float *ratioOut, float *alphaOut,
                      float *iconAlphaOut) {
    uint64_t icon = pChildAt(m, 0);
    float col[3], r = 0.0f;
    int st = -1;

    if (ratioOut) *ratioOut = 0.0f;
    if (alphaOut) *alphaOut = -1.0f;
    if (iconAlphaOut) *iconAlphaOut = -1.0f;
    if (pWidgetGetF) pWidgetGetF(m, SH_P_ALPHA, alphaOut);
    if (icon && pWidgetGetV && pWidgetGetV(icon, SH_P_COLOUR, col, 3))
        st = MarkerStyleEx(col, &r, NULL);
    if (iconAlphaOut && icon && pWidgetGetF)
        pWidgetGetF(icon, SH_P_ALPHA, iconAlphaOut);
    if (ratioOut) *ratioOut = r;
    return st;
}

/* The alphas of every image child of one marker, in order.
 *
 * Every marker icon has an "_Occluded" twin in the archive (UI_HUD_Mark_Sniper
 * and UI_HUD_Mark_Sniper_Occluded are separate image widgets, both present in
 * the template), so the blocked-line-of-sight state is a binary swap between
 * two widgets, not an opacity fade. Printing which of them is switched on is
 * what tells the two apart: a marker behind a wall should show a different
 * pattern here than the same marker in the open. */
/* The icon layer as text: "UI_HUD_Mark_Sniper:off ...". Used by the debug
 * lines; the row itself asks MarkerOccluded, which reads the same slots. */
static void MarkerSlotList(uint64_t m, char *out, int n) {
    uint64_t layer = MarkerIconLayer(m);
    int i, k, used = 0;

    out[0] = 0;
    if (!layer) return;
    k = pChildCount(layer);
    for (i = 0; i < k && used < n - 40; i++) {
        uint64_t s = pChildAt(layer, i);
        char img[96];
        unsigned vis = 0;
        int r, w;

        if (!WidgetImageName(s, img, sizeof(img))) continue;
        r = (pWidgetGetU && pWidgetGetU(s, SH_P_VISIBLE, &vis)) ? 1 : 0;
        w = _snprintf(out + used, (size_t)(n - used), "%s%s:%s",
                      used ? " " : "", img, r ? (vis ? "on" : "off") : "?");
        if (w < 0) break;
        used += w;
    }
}

static int MarkerRestoreWalk(uint64_t w, int depth);

/* Is the marker still hidden?
 *
 * One property read, and it is the read that pays for the whole optimisation:
 * the engine resets these slots constantly (the minimap log counted 16501
 * re-applies in one session), so "a record exists" is not proof of anything.
 * Reading the flag back costs a microsecond; re-writing the thirty widgets of a
 * marker costs thirty writes plus thirty record lookups, every pass, for every
 * marker on screen - which is what made the switch cost frames. */
static int MarkerStillHidden(uint64_t m, int zi) {
    float sc[3] = { 1.0f, 1.0f, 1.0f };

    (void)zi;
    if (pWidgetGetV && pWidgetGetV(m, SH_P_SCALE, sc, 3)) return sc[0] < 0.01f;
    return 0;
}

/* No record, but the marker is hidden anyway (the engine fades a target out
 * before the switch ever sees it). */
static int MarkerAlreadyDown(uint64_t m) {
    float sc[3] = { 1.0f, 1.0f, 1.0f };

    if (pWidgetGetV && pWidgetGetV(m, SH_P_SCALE, sc, 3) && sc[0] < 0.01f) return 1;
    return 0;
}

/* Hide a marker: one scale write on its own root.
 *
 * This used to walk the whole subtree writing VISIBLE=0 on every node, which
 * cost tens of property calls per marker per pass and - as the live test showed
 * - did not hide anything on this scene at all. Collapsing the root's scale is
 * one write, and it is the one the renderer obeys. */
static void MarkerCollapseRoot(uint64_t m) {
    if (!m || !pWidgetSetV) return;
    if (pWidgetSetV(m, SH_P_SCALE, kNoScale, 3))
        ZeroAdd(m, MODE_MARKER, NULL);
}

static void MarkerCollapseAll(void) {
    uint64_t root = 0;
    int k, kids, j, subs, seen = 0, hid = 0, slots = 0;
    int hist[MARKER_COLOUR_COUNT + 1];   /* one bucket per style, last = none */
    int noneShown = 0;
    LONG mask = InterlockedCompareExchange(&g_markOff, 0, 0);

    /* No mask is the normal case for an enemy row that is on - the per-marker
     * decision still has to run, or 仅视野内 can never act. Only skip when
     * there is neither a mask nor a view-only pass to make. */
    if (!mask && !g_viewOnly) return;
    memset(hist, 0, sizeof(hist));
    kids = MarkerSceneKids(&root);
    if (kids <= 0 || kids > 64) {
        if (!g_marksTold) {
            Log("marker switch: HUD_3D_Marker is not drawing right now");
            g_marksTold = 1;
        }
        return;
    }
    for (k = 0; k < kids; k++) {
        uint64_t slot = pChildAt(root, k);
        if (!slot) continue;
        slots++;
        subs = pChildCount ? pChildCount(slot) : 0;
        if (subs <= 0 || subs > 200) continue;
        for (j = 0; j < subs; j++) {
            uint64_t m = pChildAt(slot, j), icon;
            float col[3] = {-1.0f, -1.0f, -1.0f}, ratio = 0.0f;
            int st, off, zi, keep;

            if (!MarkerIsOne(m)) continue;
            seen++;
            keep = 0;
            icon = pChildAt(m, 0);
            /* One getter, not three: the alphas and the position are only for
             * the debug line below. */
            st = (icon && pWidgetGetV && pWidgetGetV(icon, SH_P_COLOUR, col, 3))
                     ? MarkerStyleEx(col, &ratio, NULL) : -1;
            if (st < 0) {
                /* The icon's own colour matched nothing. The faction tint can
                 * sit on another node of the same marker (the ring or a
                 * wrapper), so look through the subtree before giving up - a
                 * marker that reads "no style" is a marker no switch can hide,
                 * which is how Santa Blanca disappeared while Unidad stayed. */
                float r2 = 1.0f;
                int st2 = MarkerSubtreeStyle(m, 0, &r2);
                if (st2 >= 0) { st = st2; ratio = r2; }
            }
            hist[st < 0 ? MARKER_COLOUR_COUNT : st]++;
            /* Debug only. This ran on every pass with no throttle, which is
             * 13k identical lines in one session and a 7 MB log nobody reads.
             * The same information is in the styles line below. */
            if (g_dbg && st < 0 && noneShown < 2 && icon && pWidgetGetV &&
                pWidgetGetV(icon, SH_P_COLOUR, col, 3)) {
                float mx = col[0];
                int q;

                for (q = 1; q < 3; q++) if (col[q] > mx) mx = col[q];
                if (mx <= 0.0f) mx = 1.0f;
                Log("   marker with no style: icon colour %g,%g,%g -> "
                    "%d,%d,%d (normalised)", col[0], col[1], col[2],
                    (int)(col[0] / mx * 255.0f + 0.5f),
                    (int)(col[1] / mx * 255.0f + 0.5f),
                    (int)(col[2] / mx * 255.0f + 0.5f));
                noneShown++;
            }
            if (g_dbg && seen <= 6 && (g_marksTold < 2 || (g_zeroPass % 10) == 0)) {
                float ca = -1.0f, ia = -1.0f, pos[3] = {0.0f, 0.0f, 0.0f};
                char imgs[320];

                if (pWidgetGetF) pWidgetGetF(m, SH_P_ALPHA, &ca);
                if (icon && pWidgetGetF) pWidgetGetF(icon, SH_P_ALPHA, &ia);
                if (pWidgetGetV) pWidgetGetV(m, SH_P_POSITION, pos, 3);
                MarkerSlotList(m, imgs, sizeof(imgs));
                Log("   marker %d: style=%-7s iconRatio=%.2f contA=%.2f "
                    "iconA=%.2f pos=%.0f,%.0f imgs=%s%s", seen,
                    st >= 0 ? kMarkerColours[st].style : "?", ratio, ca, ia,
                    pos[0], pos[1], imgs[0] ? imgs : "-",
                    (st >= 0 && (int)((mask >> st) & 1)) ? "  -> collapsed" : "");
            }
            off = (st >= 0) && (int)((mask >> st) & 1);
            /* 仅视野内: an enemy marker whose one lit slot is the hollow
             * mark, i.e. the game is drawing the state it uses for a target it
             * is not showing normally. */
            if (!off && g_viewOnly && st >= 0 &&
                ((MARK_ENEMY_MASK >> st) & 1)) {
                char trail[320];
                int kind = MSK_OTHER, sample, di, unsure = 0, act = 0;

                trail[0] = 0;
                sample = (g_drawSample[0] == 0);
                di = MarkerDrawnSlot(m, sample ? trail : NULL,
                                     sample ? (int)sizeof(trail) : 0, &kind,
                                     &unsure, &act);

                if (di < 0) g_drawUnknown++;
                else if (kind == MSK_HOLLOW) g_drawEmpty++;
                else if (kind == MSK_SOLID) g_drawSolid++;
                else g_drawTwin++;

                /* act: 1 hide, 2 show, 0 leave the marker exactly as it is.
                 *
                 * Deliberately asymmetric. Hiding waits for three passes in a
                 * row that say hollow; showing happens on the first pass that
                 * says anything else. A wrong hide therefore lasts at most a
                 * third of a second, and the row cannot blink: there is no
                 * delay on the show side to fight against. */
                if (act == 1) {
                    off = 1;
                    g_occHits++;
                    if (!g_occToldOnce) {
                        g_occToldOnce = 1;
                        Log("view-only: slot %d hollow for %d passes in a row "
                            "-> hiding it (trail %s)", di, HIDE_AFTER_HOLLOW,
                            trail[0] ? trail : "-");
                    }
                } else if (act == 0) {
                    /* Hollow, but not for long enough yet: leave the marker
                     * exactly as it is for these two passes. */
                    keep = 1;
                } else if (ZeroFind(m) >= 0) {
                    g_shownFlips++;     /* it was down and is going back up */
                }
            }
            if (!off && !keep) {
                /* The engine pools marker widgets: one that was collapsed for
                 * Santa Blanca can be handed to a marker of another faction
                 * without being rebuilt. If this marker still carries one of
                 * our records, put it back - otherwise a colour the player never
                 * asked to hide would stay collapsed until the switch goes off. */
                if (ZeroFind(m) >= 0 ||
                    (pChildAt && ZeroFind(pChildAt(m, 0)) >= 0))
                    MarkerRestoreWalk(m, 0);
                continue;
            }
            zi = ZeroFind(m);
            if (zi >= 0 && !(g_zeroed[zi].mode & MODE_MARKER)) {
                ZeroDrop(zi);            /* a recycled handle, not our record */
                zi = -1;
            }
            if (zi >= 0 && MarkerStillHidden(m, zi)) {
                g_zeroed[zi].stamp = g_zeroPass;   /* already down: nothing to do */
                continue;
            }
            if (zi >= 0) {
                /* Recorded as hidden, but the widget is not at scale 0 any
                 * more: the engine put it back. If this climbs, the blink is
                 * not our state machine at all - it is the game re-laying the
                 * marker out and overwriting the scale we wrote. */
                g_resetMarks++;
            }
            if (zi < 0 && MarkerAlreadyDown(m)) {
                /* The engine had it hidden before this switch ever touched it,
                 * so there is no record to check - and recursing into it would
                 * cost a record lookup per widget, every pass, for nothing. */
                continue;
            }
            MarkerCollapseRoot(m);
            hid++;
        }
    }
    if (hid == g_lastHid) {
        if (g_viewIdle < 1000) g_viewIdle++;
    } else {
        g_viewIdle = 0;
        g_lastHid = hid;
    }
    if (g_viewOnly) MarkerViewOnlyLog(seen);
    if (g_marksTold < 2 || hid) {
        Log("marker switch: %d marker(s) in %d slot(s), %d collapsed (mask 0x%lX)",
            seen, slots, hid, (unsigned long)mask);
        Log("   marker styles on screen: Red=%d Purple=%d Green=%d Blue=%d "
            "White=%d Gold=%d none=%d", hist[0], hist[1], hist[2], hist[3],
            hist[4], hist[5], hist[MARKER_COLOUR_COUNT]);
        g_marksTold++;
    } else if ((g_zeroPass % 60) == 0) {
        Log("   marker styles on screen: Red=%d Purple=%d Green=%d Blue=%d "
            "White=%d Gold=%d none=%d", hist[0], hist[1], hist[2], hist[3],
            hist[4], hist[5], hist[MARKER_COLOUR_COUNT]);
    }
}

/* Put back only what is still in the tree. Anything recorded for a widget the
 * game has since destroyed is dropped without writing: the handle may already
 * belong to another allocation. */
static int MarkerRestoreWalk(uint64_t w, int depth) {
    int i, kids, zi, n = 0;

    if (!w || depth > 3) return 0;
    zi = ZeroFind(w);
    if (zi >= 0 && (g_zeroed[zi].mode & MODE_MARKER)) {
        RestoreOne(g_zeroed[zi].h, &g_zeroed[zi]);
        ZeroDrop(zi);
        n++;
    }
    kids = pChildCount ? pChildCount(w) : 0;
    if (kids <= 0 || kids > 64) return n;
    for (i = 0; i < kids; i++)
        n += MarkerRestoreWalk(pChildAt(w, i), depth + 1);
    return n;
}

/* Put back the values a marker widget had before it was collapsed.
 *
 * The first version of this sweep wrote scale 1 on every widget and called it
 * "the visible state". It is not: a marker's subtree carries per-node scales
 * well below 1 - that is how the ring is drawn smaller than the container that
 * holds it - so forcing 1 everywhere brought every marker back several times its
 * normal size (screenshot 2026-10-22 16:52, rings the width of a truck).
 *
 * The only honest restore is the value that was read before the collapse, which
 * is exactly what a record holds - scale, alpha and the visible flag. A widget
 * with no record was never touched, and this walk deliberately writes nothing on
 * it: forcing VISIBLE = 1 "just in case" also un-hides the nodes the game keeps
 * invisible on purpose (every marker carries an "_Occluded" twin image that is
 * switched off until the target is behind a wall). */
static void MarkerShowTree(uint64_t w, int depth) {
    int i, kids, zi;

    if (!w || depth > 3 || !WidSane(w)) return;
    zi = ZeroFind(w);
    if (zi >= 0 && (g_zeroed[zi].mode & MODE_MARKER)) {
        RestoreOne(w, &g_zeroed[zi]);
        ZeroDrop(zi);
    }
    kids = pChildCount ? pChildCount(w) : 0;
    if (kids <= 0 || kids > 64) return;
    for (i = 0; i < kids; i++) MarkerShowTree(pChildAt(w, i), depth + 1);
}

/* 1 = swept, 0 = the marker scene is not drawing so nothing could be reached.
 *
 * The records are kept on 0. They are the only thing that knows which widgets
 * this plugin collapsed, so dropping them on an unlucky frame would leave those
 * markers collapsed with nothing left to restore them from. */
static int MarkerShowAll(void) {
    uint64_t root = 0;
    int k, j, kids, subs, n = 0;

    kids = MarkerSceneKids(&root);
    if (kids <= 0 || kids > 64) {
        Log("marker switch off: scene is not drawing, keeping %d record(s)",
            g_nZeroed);
        return 0;
    }
    for (k = 0; k < kids; k++) {
        uint64_t slot = pChildAt(root, k);
        if (!slot) continue;
        subs = pChildCount ? pChildCount(slot) : 0;
        if (subs <= 0 || subs > 200) continue;
        for (j = 0; j < subs; j++) {
            uint64_t m = pChildAt(slot, j);
            if (!MarkerIsOne(m)) continue;
            MarkerShowTree(m, 0);
            n++;
        }
    }
    for (k = ZeroFirst(); k >= 0; k = ZeroNext(k))
        if (g_zeroed[k].mode & MODE_MARKER) ZeroDrop(k);
    Log("marker switch off: %d marker(s) restored", n);
    return 1;
}

static void MarkersRestore(int final) {
    uint64_t root = 0;
    int k, kids, n = 0;

    kids = MarkerSceneKids(&root);
    if (kids > 0 && kids <= 64)
        for (k = 0; k < kids; k++)
            n += MarkerRestoreWalk(pChildAt(root, k), 0);
    if (n) {
        Log("marker switch off: %d widget(s) restored", n);
        g_restoreTries = 0;
        return;
    }
    /* The scene is not drawing (menu, loading, looking at nothing). Keep the
     * records for a while - the widgets may come back - then forget them, so a
     * stale handle is never written to. */
    if (!final && g_restoreTries < 60) return;
    for (k = ZeroFirst(); k >= 0; k = ZeroNext(k))
        if (g_zeroed[k].mode & MODE_MARKER) ZeroDrop(k);
    g_restoreTries = 0;
}

static int MarkerMaskFromIni(char *v) {
    char *tok, *ctx = NULL;
    LONG mask = 0;
    int i;

    for (tok = strtok_s(v, ", ", &ctx); tok; tok = strtok_s(NULL, ", ", &ctx)) {
        int hit = 0;
        if (tok[0] == '1' && tok[1] == 0) { mask = MARK_ENEMY_MASK; continue; }
        if (tok[0] == '0' && tok[1] == 0) { mask = 0; continue; }
        for (i = 0; i < MARKER_COLOUR_COUNT; i++)
            if (_stricmp(kMarkerColours[i].style, tok) == 0) {
                mask |= MARK_BIT(i);
                hit = 1;
            }
        if (!hit) Log("markers_off: unknown style \"%s\"", tok);
    }
    return (int)mask;
}

static void OnMarkRow(uint32_t menu, uint32_t item, int value, void *user) {
    int idx = (int)(intptr_t)user;
    LONG mask = InterlockedCompareExchange(&g_markOff, 0, 0);
    char line[128];
    int i, used = 0;

    (void)menu; (void)item;
    if (idx < 0) mask = value ? (mask | MARK_ENEMY_MASK) : (mask & ~MARK_ENEMY_MASK);
    else         mask = value ? (mask | MARK_BIT(idx)) : (mask & ~MARK_BIT(idx));
    InterlockedExchange(&g_markOff, mask);
    InterlockedExchange(&g_marksOn, (mask || g_viewOnly) ? 1 : 0);
    g_marksTold = 0;
    if (mask) {
        SafeMarkerCollapseAll();                 /* take effect now */
    } else {
        g_restoreTries = 0;
        MarkerShowAll();          /* last switch off: sweep, do not trust records */
    }
    line[0] = 0;
    for (i = 0; i < MARKER_COLOUR_COUNT; i++)
        if ((mask >> i) & 1) {
            int l = (int)strlen(kMarkerColours[i].style);
            if (used + l + 2 >= (int)sizeof(line)) break;
            if (used) line[used++] = ',';
            memcpy(line + used, kMarkerColours[i].style, (size_t)l);
            used += l;
            line[used] = 0;
        }
    WriteIniKey("markers_off", line);
    Log("marker switch -> mask 0x%lX (%s)", (unsigned long)mask,
        line[0] ? line : "none");
}

/* ---- minimap opacity and scale (first experiment of its kind) -----------
 *
 * The write whitelist has always allowed three properties - ALPHA, VISIBLE and
 * SCALE - and until now only SCALE was used, to collapse things. This tests the
 * other two as real controls: the minimap's scene root is a plain
 * ContainerWidget, so setting its alpha should fade the whole layer and setting
 * its scale should grow or shrink it, without touching any layer switch.
 *
 * Both scenes are driven: HUD_Minimap is the frame around the map, and
 * HUD_MinimapIcons is the icons drawn on top of it. Scaling only the first would
 * leave the icons at their old size.
 *
 * Values are percent. 100 = untouched, which is also the only value that stops
 * the per-pass writer, so a game that resets the property each frame cannot
 * fight back when the player has not asked for anything. */

/* The scene root is the full UI canvas (0,0 at 1920x1080 in the log), not the
 * minimap: scaling it shrinks the whole layout, and a layer anchored bottom
 * right therefore slides up-left with the canvas. The map's own frame is the
 * first ContainerWidget child that has children - that one has the map's real
 * position and size, so scaling it moves only the map and the compensation
 * below means something. */
static uint64_t MmTarget(uint64_t root, const char **whatOut) {
    int i, kids = pChildCount ? pChildCount(root) : 0;

    if (kids > 0 && kids <= 64) {
        for (i = 0; i < kids && i < 16; i++) {
            uint64_t c = pChildAt(root, i);
            char cls[48];
            int k2;
            if (!c) continue;
            cls[0] = 0;
            ObjectClass(c, cls, sizeof(cls));
            k2 = pChildCount ? pChildCount(c) : 0;
            if (strstr(cls, "Container") && k2 > 0) {
                if (whatOut) *whatOut = "map container";
                return c;
            }
        }
    }
    if (whatOut) *whatOut = "scene root";
    return root;
}

/* Cache of the two minimap scenes and the containers the fx writes to: the
 * scene -> root -> map-container chain only changes when the game rebuilds the
 * HUD. */
typedef struct { uint64_t root, target; char nm[32]; } MmCached;
static MmCached g_mmCache[4];
static int      g_nMmCache;
static int      g_mmScenes = -1;
static int      g_mmRebuild;
static int      g_mmWriteAge;
static int      g_mmForce = 1;

static void MmRebuildCache(void) {
    int i, n, found = 0;

    g_nMmCache = 0;
    if (!pSceneCount || !pSceneAt || !pSceneName || !pSceneRoot) return;
    n = pSceneCount();
    if (n > 24) n = 24;
    for (i = 0; i < n && found < 4; i++) {
        uint64_t sc = pSceneAt(i), root;
        char nm[64];
        if (!pSceneName(sc, nm, sizeof(nm))) continue;
        if (strcmp(nm, "HUD_Minimap") != 0 && strcmp(nm, "HUD_MinimapIcons") != 0)
            continue;
        root = pSceneRoot(sc);
        if (!root) continue;
        g_mmCache[found].root = root;
        g_mmCache[found].target = MmTarget(root, NULL);
        _snprintf(g_mmCache[found].nm, sizeof(g_mmCache[found].nm), "%s", nm);
        found++;
    }
    g_nMmCache = found;
    g_mmScenes = pSceneCount();
}

static void ApplyMinimapFx(void) {
    int i, applied = 0, forced;

    /* The forced flag has to win here: a slider dragged back to 100% is a
     * change like any other, and this early return used to skip the write that
     * would have put the map back (measured, the map stayed at 60% opacity
     * until the HUD was rebuilt). */
    if (!g_mmForce && g_mmAlpha == 100 && g_mmScale == 100 && !g_mmVerify)
        return;
    if (!pSceneCount || !pSceneAt || !pSceneName || !pSceneRoot) return;
    /* The scene -> root -> map-container chain used to be re-walked every
     * frame; it only changes when the game rebuilds the HUD, so it is cached
     * and refreshed when the scene count moves or every five seconds. */
    if (g_mmScenes != pSceneCount() || ++g_mmRebuild > 300)
        MmRebuildCache();
    if (!g_nMmCache) {
        if (!g_mmTold) {
            Log("minimap fx: HUD_Minimap is not drawing right now");
            g_mmTold = 1;
        }
        return;
    }
    /* Both values stick in the engine, so rewriting them 60 times a second was
     * cost without effect. Ten hertz survives a scene rebuild; a setting change
     * forces the very next frame. */
    forced = g_mmForce;
    if (!forced && ++g_mmWriteAge < 6) return;
    g_mmForce = 0;
    g_mmWriteAge = 0;
    /* A changed setting is exactly when the answer matters, so the line below
     * gets to speak again: g_mmTold is also set by the "not drawing right now"
     * branch above, which used to silence the (set=0) report for the whole
     * session - the one line that would have shown the opacity write being
     * refused. */
    if (forced) {
        g_mmTold = 0;
        g_mmFailTold = 0;     /* a new setting: report a failure again */
    }
    for (i = 0; i < g_nMmCache; i++) {
        int okA = 0, okS = 0;

        /* Fading the scene root works for every layer; scaling happens on the
         * map's own container, never on the 1920x1080 canvas. POSITION is never
         * written: the container is anchored bottom-right, so the engine keeps
         * the margins while the map scales. */
        /* Written even at 100%: that write is what undoes an earlier fade. */
        if (pWidgetSetF)
            okA = pWidgetSetF(g_mmCache[i].root, SH_P_ALPHA,
                              (float)g_mmAlpha / 100.0f);
        if (pWidgetSetV) {
            float k = (float)g_mmScale / 100.0f;
            float v[3] = {k, k, k};
            okS = pWidgetSetV(g_mmCache[i].target, SH_P_SCALE, v, 3);
        }
        applied++;
        if (!g_mmTold)
            Log("minimap fx: %s root=%llX alpha=%d%% (set=%d) scale=%d%% (set=%d)",
                g_mmCache[i].nm, (unsigned long long)g_mmCache[i].root,
                g_mmAlpha, okA, g_mmScale, okS);
        if (!okA && !g_mmFailTold) {
            Log("minimap fx: the ALPHA write to %s (root %llX) was refused, and "
                "the scale write beside it %s", g_mmCache[i].nm,
                (unsigned long long)g_mmCache[i].root,
                okS ? "went through - so the handle is live and only the root "
                      "was refused" : "was refused too - the whole handle is "
                      "considered dead");
            g_mmFailTold = 1;
        }
    }
    if (applied) g_mmTold = 1;

    /* Read the layout back after the engine has run: the check that the margins
     * really did not move. */
    if (g_mmVerify > 0 && applied && --g_mmVerify == 0) {
        for (i = 0; i < g_nMmCache && pWidgetGetV; i++) {
            float pos[3] = {0, 0, 0}, sz[2] = {0, 0};
            pWidgetGetV(g_mmCache[i].target, SH_P_POSITION, pos, 3);
            if (!pWidgetGetV(g_mmCache[i].target, SH_P_CONTSIZE, sz, 2))
                pWidgetGetV(g_mmCache[i].target, SH_P_SIZE, sz, 2);
            Log("minimap fx verify: %s scale=%d%% pos=%.0f,%.0f size=%.0f,%.0f",
                g_mmCache[i].nm, g_mmScale, pos[0], pos[1], sz[0], sz[1]);
        }
    }
}

/* Where the current percent sits in its option table. */
static int MmIndex(int pct, int lo, int step, int n) {
    int i;

    if (step <= 0 || n <= 1) return 0;      /* a one-step row has one index */
    i = (pct - lo) / step;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return i;
}

/* ---- property-setter probe ---------------------------------------------
 *
 * Every property read on a marker's icon layer answers 0: the view-only line
 * "UI_HUD_MarkIcon:-1" is pWidgetGetU failing, and it fails the same way on
 * all eleven non-twin slots while the four _Occluded slots answer fine. So
 * which of the fifteen preloaded slots the engine draws cannot be asked of the
 * widget itself. What can be done without changing anything is to watch the
 * engine's own setters - if it flips VISIBLE or ALPHA on a slot, that write
 * passes through here first.
 *
 * The addresses come out of the property table through the framework
 * (ShPropCount / ShPropAt), never from a build offset. Each detour does one
 * increment and at most one store into a six-entry ring, no logging and no
 * property calls, so a hot setter stays cheap; a summary is written every 300
 * frames. This observes only - the engine's value is always passed through. */
static uint64_t g_setVisAddr;
static void (*g_origSetVis)(uint64_t, uint32_t);

/* The engine's own VISIBLE setter. Not a probe: PvNote() records which slot the
 * game last lit, and that record is what 视野盲区 reads (PvLast). Everything the
 * hook used to count for diagnostics is gone; it is now one store and a tail
 * call. */
static void SetVisDetour(uint64_t h, uint32_t v) {
    PvNote(h, (int)v);
    g_origSetVis(h, v);
}

static void ResolveSetters(void) {
    char cls[48];
    uint32_t id;
    int type, i, n;

    if (!pPropCount || !pPropAt) {
        Log("setter probe: property table not reachable");
        return;
    }
    n = pPropCount();
    for (i = 0; i < n; i++) {
        uint64_t set = 0, get = 0;

        if (!pPropAt(i, cls, sizeof(cls), &id, &type, &set, &get)) continue;
        if (!set) continue;
        if (id == SH_P_VISIBLE) g_setVisAddr = set;
    }
    Log("visible hook: %d properties, VISIBLE set=%llX", n,
        (unsigned long long)g_setVisAddr);
    if (g_setVisAddr) {
        int rc = ShHookAddAt("hcvis", (void *)(uintptr_t)g_setVisAddr,
                             "VISIBLE setter probe", (void *)SetVisDetour,
                             (void **)&g_origSetVis);

        Log("setter probe: VISIBLE hook rc=%d orig=%p", rc, (void *)g_origSetVis);
        if (rc != 0) g_setVisAddr = 0;
    }
}



/* ---- the scenes the walk actually visits, remembered --------------------
 *
 * Asking the engine for the scene list is not free: pSceneCount, then a
 * pSceneAt/pSceneName pair per scene, then a pSceneRoot for the ones that
 * survive the filter - 30 to 45 calls of ~10us each per pass, measured from
 * the 2026-10-26 14:52 log (167 widgets cost 5.1ms, 360 cost 8.3ms: the slope
 * is the walk, the intercept is this enumeration). And the answer only changes
 * when a scene opens or closes: the map, the menu, a load. So the walk keeps
 * its own copy - root, context and name for the handful of scenes it walks,
 * not for all 13 to 16 that exist - and rebuilds it when
 *
 *   - the scene count changed (every open/close moves it),
 *   - a switch kicked this pass (that is the frame the player is watching),
 *   - or ten passes went by anyway.
 *
 * The last one is the safety net for a scene destroyed and rebuilt at the same
 * count: a stale root answers no child count, costs two failed calls, and the
 * next rebuild is at most ~3s away. Everything else about the pass is
 * unchanged: same filter (HcSceneKind, no map canvas), same rotation, same
 * per-scene budget. */
#define HC_WALK_MAX 24

typedef struct {
    uint64_t      root;
    unsigned char ctx;
    char          nm[32];
} WalkScene;

static WalkScene g_walkList[HC_WALK_MAX];
static int       g_nWalk;        /* entries in the list */
static int       g_walkFrom;     /* scene count the list was built from */
static int       g_walkAge;      /* passes since the last rebuild */

/* `nNow` is the scene count the caller just read - passed in so a rebuild does
 * not ask for it twice. */
/* ---- per scene: is this walk still buying anything? ----------------------
 *
 * One scene in a real session is enormous and answers nothing: measured
 * 2026-10-27, 550 widgets for the pass at ~10ms, 0 images, 0 matches, and
 * 202 of 570 passes ended TIME-CUT before they finished. Walking it twenty
 * times a second to learn the same "nothing" is what kept the pass from ever
 * completing.
 *
 * So a scene that is big and produces nothing is walked once every DRY_SKIP
 * passes instead of every pass. The conditions that end the holiday are the
 * ones that make it safe: any image at all, a widget count that moved (the
 * pool changed shape), or any switch being touched - a switch change has to
 * be answered on the next pass, not in six seconds.
 */
/* 80, not 150: the instance that goes dry starts at about 150 widgets and
 * grows with the session (330 by the end of one, measured), while the small
 * one that actually carries the icons sits at 42-88. Eighty separates them
 * with room on both sides, and 40 passes（about 13 seconds at 3 a second）
 * spreads the one walk it does get thinner than 20 did. */
#define DRY_WIDGETS 80
#define DRY_SKIP    40
#define WSTAT_MAX   8

typedef struct {
    char     nm[24];
    uint64_t root;  /* two scenes can share a name: the big and the small
                     * HUD_MinimapIcons do, and one of them reporting images
                     * must not lift the other one's holiday (measured
                     * 2026-10-27: the "is dry" line fired twice a second
                     * apart with 166 and 198 widgets, which is how the
                     * shared slot gave itself away) */
    int      widgets;
    int      images;
    int      skip;      /* passes still to leave this scene out; 0 = walk it */
    int      warned;
} WalkStat;

static WalkStat g_wstat[WSTAT_MAX];
static int      g_nWstat;

static WalkStat *WStatFind(const char *nm, uint64_t root, int make) {
    int i;

    for (i = 0; i < g_nWstat; i++)
        if (g_wstat[i].root == root && strcmp(g_wstat[i].nm, nm) == 0)
            return &g_wstat[i];
    if (!make || g_nWstat >= WSTAT_MAX) return NULL;
    memset(&g_wstat[g_nWstat], 0, sizeof(g_wstat[0]));
    g_wstat[g_nWstat].root = root;
    _snprintf(g_wstat[g_nWstat].nm, sizeof(g_wstat[0].nm), "%s", nm);
    return &g_wstat[g_nWstat++];
}

static void WalkStatsReset(void) {
    int i;

    for (i = 0; i < g_nWstat; i++) g_wstat[i].skip = 0;
}

static void WalkListBuild(int nNow) {
    int i, n;

    g_nWalk = 0;
    g_walkFrom = nNow;
    g_walkAge = 0;
    n = nNow;
    if (n > 48) n = 48;
    for (i = 0; i < n && g_nWalk < HC_WALK_MAX; i++) {
        uint64_t sc = pSceneAt(i), root;
        char nm[64];
        int ctx;

        if (!pSceneName(sc, nm, sizeof(nm)) || !nm[0]) continue;
        ctx = HcSceneKind(nm);
        if (ctx == HC_CTX_NONE || ctx == HC_CTX_WORLD) continue;
        /* The FX scenes (CrossHair / HitFeedback / KillFeedback) are the last
         * leftover of the 击杀反馈 row, which was removed on 2026-10-25: they
         * exist here only because that row once asked for them, and since then
         * they have answered nothing. Measured with debug_dump=1 on
         * 2026-10-26 15:06-15:14: 576 walks of CrossHair, 58 widgets each,
         * 0 images and 0 matches - every single time. 0.9ms a pass for a
         * feature this build no longer has. */
        if (ctx == HC_CTX_FX) continue;
        /* HUD_Minimap is the map canvas - the 3072x3072 surface the minimap
         * shows a crop of - and it carries its own copy of the icon strip,
         * with a few hundred slots of which two or three are ever drawn. The
         * icons the player sees are HUD_MinimapIcons, drawn on top of it
         * (census 2026-10-22: that scene holds exactly two kinds of image
         * widget, the vehicle icon and the collectible dot), and the map
         * page's own rows are gone. So walking the canvas bought nothing for
         * either of these two rows and cost a string read on every one of its
         * empty leaves. SyncMinimapDots skips it for the same reason; the walk
         * is older than that decision and never got the same treatment.
         *
         * What it does drop: vehicle and collectible icons inside the big map
         * are no longer collapsed behind the map page's back. No row of that
         * page exists any more, so nothing promised it. */
        if (strcmp(nm, "HUD_Minimap") == 0) continue;
        root = pSceneRoot(sc);
        if (!root) continue;
        g_walkList[g_nWalk].root = root;
        g_walkList[g_nWalk].ctx = (unsigned char)ctx;
        _snprintf(g_walkList[g_nWalk].nm, sizeof(g_walkList[0].nm), "%s", nm);
        g_nWalk++;
    }
}

static void IconFrameBody(void *user) {
    if (InterlockedExchange(&g_needMarkRestore, 0)) {
        if (InterlockedCompareExchange(&g_markOff, 0, 0)) {
            SafeMarkerCollapseAll();
        } else if (MarkerShowAll()) {
            g_restoreTries = 0;
        } else if (++g_restoreTries < 1200) {
            InterlockedExchange(&g_needMarkRestore, 1);   /* retry next frame */
        } else {
            g_restoreTries = 0;
            Log("marker switch off: scene never came back, restore abandoned");
        }
    }
    int i, n, kicked, budget = ICON_BUDGET, start, last = -1;
    uint64_t t0 = NowUs(), iconT0 = 0;

    (void)user;
    if (!pSceneCount || !pSceneAt || !pSceneName || !pSceneRoot) return;
    g_lastFrameTick = GetTickCount64();
    /* the minimap experiment is independent of the icon filter */
    ApplyMinimapFx();
    /* The minimap markers are their own pass, on every frame: the blip pool is
     * about twenty widgets, and a marker the engine re-lays-out would otherwise
     * flicker back for the 30 frames between two icon passes. It returns at once
     * unless a marker switch is on or something of ours is still hidden. */
    {
        uint64_t s0 = NowUs(), d;

        SyncMinimapDots();
        d = NowUs() - s0;
        if (d > g_syncUsMax) g_syncUsMax = d;    /* reported by the perf line */
    }
    /* The marker pass runs on its own cadence, before the icon gate below: an
     * enemy marker that spawns while 敌人 is off has to go on the next frame,
     * not on the next icon pass. */
    MarkerTick();
    /* Also run while something is still collapsed, even with every switch back
     * on: that is the pass that puts the scale back. */
    if (!g_iconOn && !g_marksOn && !g_nZeroed &&
        !g_nHidIds &&
        !g_mmOn && g_mmAlpha == 100 && g_mmScale == 100) {
        g_nBack = 0;      /* idle again: the next switch-on reports afresh */
        return;
    }
    /* Twice a second is enough for icons that appear and disappear on a map,
     * and the walk is the expensive part of this plugin (~10us a widget). A
     * switch change kicks a pass straight away, so the interval is only about
     * catching icons the game spawns on its own. */
    kicked = InterlockedExchange(&g_iconKick, 0);
    if (!kicked &&
        (InterlockedIncrement(&g_iconFrame) %
         (g_viewOnly ? VIEW_FRAME_BUSY : 30)) != 0) return;
    if (!pChildCount || !pChildAt) return;

    iconT0 = NowUs();
    g_zeroPass++;
    g_passWidgets = g_passImages = g_passMatched = g_passHidden = 0;
    g_walkCalls = 0;
    g_walkTrunc = 0;
    /* One budget for every pass, kicked or not. An earlier build raised it to
     * 24 ms so that a switch covered the whole HUD in a single pass; that is
     * responsiveness bought with cost, and with the leaf-first walk a whole
     * pass already fits inside the normal budget. */
    g_walkDeadline = t0 + (uint64_t)ICON_WALK_MS * 1000ULL;
    /* Marker work does not need the icon walk: with every icon switch on and no
     * raw id named, that walk cannot hide anything, and it is the single most
     * expensive thing here (1100 widgets x a class lookup each). */
    if (!IconAnyOff()) {
        /* The marker pass already ran above; running it twice per frame was
         * one of the things the perf line kept showing as avoidable. */
        g_perfPass++;
        PerfLine(0);           /* minimap sync already ran above this gate */
        return;
    }
    /* One cheap read instead of the enumeration: the cached list is only
     * rebuilt when the set of scenes moved. An empty list is rebuilt every
     * pass - it means the scenes are still coming up, and a pass with nothing
     * to walk costs nothing anyway. */
    n = pSceneCount();
    if (kicked) WalkStatsReset();   /* a switch moved: answer it this pass */
    if (kicked || g_nWalk == 0 || ++g_walkAge >= 10 || n != g_walkFrom)
        WalkListBuild(n);
    start = g_nWalk ? g_sceneStart % g_nWalk : 0;
    for (i = 0; i < g_nWalk && budget > 0; i++) {
        int si = (start + i) % g_nWalk;

        last = si;
        {
            /* Per scene, not just in total: "the pass matched nothing" and "the
             * pass never reached the map" look the same in the summary line,
             * and telling them apart was the whole difficulty of this bug. */
            int w0 = g_passWidgets, i0 = g_passImages, m0 = g_passMatched;
            WalkStat *st = WStatFind(g_walkList[si].nm, g_walkList[si].root, 1);

            if (st && st->skip > 0) {
                st->skip--;
                continue;             /* dry for now - see WalkStat above */
            }
            /* WalkIcons decrements the budget through the pointer; subtracting
             * its return value as well charged every widget twice. */
            WalkIcons(g_walkList[si].root, 0, 0, &budget, g_walkList[si].ctx);
            if (st) {
                st->widgets = g_passWidgets - w0;
                st->images  = g_passImages - i0;
                if (st->images > 0) {
                    st->skip = 0;     /* it answers: keep walking it */
                    st->warned = 0;
                } else if (st->widgets >= DRY_WIDGETS && st->skip == 0) {
                    st->skip = DRY_SKIP - 1;
                    if (!st->warned) {
                        st->warned = 1;
                        Log("icon walk: %s is dry (%d widgets, 0 images, %d "
                            "matched) - walking it once every %d passes until "
                            "that changes", st->nm, st->widgets,
                            g_passMatched - m0, DRY_SKIP);
                    }
                }
            }
            if (g_dbg)
                Log("   icon scene %-22s widgets %4d images %3d matched %3d",
                    g_walkList[si].nm, g_passWidgets - w0, g_passImages - i0,
                    g_passMatched - m0);
        }
    }
    if (g_nWalk && last >= 0) g_sceneStart = (last + 1) % g_nWalk;
    for (i = g_nHidIds - 1; i >= 0; i--)
        if (g_hidDone[i]) HidIdDrop(i);
    memset(g_hidDone, 0, sizeof(g_hidDone));
    /* records not seen for twenty passes describe widgets the game has since
     * destroyed: drop them rather than write to a dead handle later. Marker
     * records are exempt while a marker switch is on - a marker that leaves the
     * screen for a few seconds must still be restored when it comes back. */
    for (i = ZeroFirst(); i >= 0; ) {
        int nxt = ZeroNext(i);

        if (!((g_zeroed[i].mode & MODE_MARKER) && g_marksOn) &&
            g_zeroPass - g_zeroed[i].stamp > 20)
            ZeroDrop(i);
        i = nxt;
    }
    /* Switch just went off but the scene was not drawing: keep trying for a
     * few seconds, then forget the handles rather than risk a stale write. */
    if (!g_marksOn && g_nZeroed && (g_zeroPass % 5) == 0 && g_restoreTries < 60) {
        g_restoreTries++;
        MarkersRestore(g_restoreTries >= 60);
    }
    /* What the switches actually cost, in microseconds. */
    g_perfPass++;
    PerfLine(NowUs() - iconT0);
    /* Once every ten passes while enforcing: did the walk see anything at all?
     * "images 0" means the switch had nothing to act on, "matched 0" means the
     * icons on screen were another kind - two very different answers for a
     * switch that looks like it did nothing. */
    if ((g_iconOn || g_nHidIds) && (g_dbg || (g_zeroPass % 10) == 0)) {
        /* "scene N of M" counts the scenes this pass actually walks, not the
         * whole list: M used to be every scene the game has (13-16) and most
         * of them were filtered out unvisited, so the number never described
         * the walk. */
        Log("icon pass: widgets %d, images %d, matched %d, hidden %d, "
            "collapsed %d, %s, %.1fms from scene %d of %d",
            g_passWidgets, g_passImages, g_passMatched,
            g_passHidden, g_nZeroed,
            g_walkTrunc ? "TIME-CUT (raise ICON_WALK_MS)"
                        : (budget <= 0 ? "COUNT-CUT (raise ICON_BUDGET)" : "complete"),
            (double)(NowUs() - t0) / 1000.0, start, g_nWalk);
        IconHitsLog();
    }
}

/* Which switches are actually doing work, by resource family. Names are cut at
 * the first '|' so the line stays ASCII and short: the ini spells the same
 * family the same way, so a count can be matched to a row by eye. */
static void IconHitsLog(void) {
    char line[640], buf[64];
    int i, used = 0, any = 0;

    line[0] = 0;
    for (i = 0; i < NICONS; i++) {
        const char *p2 = g_icons[i].match;
        int l = 0;
        if (!g_kindHits[i]) continue;
        while (p2[l] && p2[l] != '|' && l < 30) l++;
        _snprintf(buf, sizeof(buf), "%s%.*s=%d", used ? ", " : "", l, p2,
                  g_kindHits[i]);
        if (used + (int)strlen(buf) + 1 >= (int)sizeof(line)) break;
        memcpy(line + used, buf, strlen(buf) + 1);
        used += (int)strlen(buf);
        any = 1;
    }
    if (g_rawHits) {
        _snprintf(buf, sizeof(buf), "%s(raw ids)=%d", used ? ", " : "",
                  g_rawHits);
        if (used + (int)strlen(buf) + 1 < (int)sizeof(line)) {
            memcpy(line + used, buf, strlen(buf) + 1);
            used += (int)strlen(buf);
            any = 1;
        }
    }
    if (any) Log("icon kinds hit: %s", line);
}

static void IconFrame(void *user) {
    if (g_iconDisabled) return;
    __try {
        IconFrameBody(user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        GuardFault("icon pass", GetExceptionCode());
    }
}

/* The two menu callbacks touch widgets from the menu thread, so they get a
 * guard of their own rather than relying on the frame callback's. */
static void SafeMarkerCollapseAll(void) {
    if (g_iconDisabled) return;
    __try {
        MarkerCollapseAll();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        GuardFault("marker pass", GetExceptionCode());
    }
}

static void SafeApplyMinimapFx(void) {
    __try {
        ApplyMinimapFx();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        GuardFault("minimap fx", GetExceptionCode());
    }
}


/* Which icon scenes exist this instant. A switch ticked in the world cannot
 * affect the map screen, and without this the only evidence was a silent
 * "matched 0" that reads exactly like a broken switch. */




/* ---- widget tree probe (F7) --------------------------------------------- */

static int SafeRead(void *dst, uint64_t src, int n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!src || n <= 0) return 0;
    if (VirtualQuery((LPCVOID)(uintptr_t)src, &mbi, sizeof(mbi)) != sizeof(mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    if (src + (uint64_t)n > (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize) return 0;
    memcpy(dst, (const void *)(uintptr_t)src, (size_t)n);
    return 1;
}

static void DumpBlock(uint64_t p, int len) {
    unsigned char buf[0x300];
    char out[200];
    int i, k, used;

    if (len > (int)sizeof(buf)) len = (int)sizeof(buf);
    if (!SafeRead(buf, p, len)) {
        Log("  %llX unreadable", (unsigned long long)p);
        return;
    }
    for (i = 0; i < len; i += 16) {
        used = _snprintf(out, sizeof(out), "  %04X ", i);
        for (k = 0; k < 16 && i + k < len; k++)
            used += _snprintf(out + used, sizeof(out) - used, "%02X ", buf[i + k]);
        if (used > 0) Log("%s", out);
    }
    /* slots that look like pointers into readable memory: show the text there */
    for (i = 0; i + 8 <= len; i += 8) {
        uint64_t v;
        char s[40];
        int j, ok = 1;
        memcpy(&v, buf + i, 8);
        if (v < 0x10000 || v > 0x7FFFFFFFFFFFULL) continue;
        if (!SafeRead(s, v, 32)) continue;
        for (j = 0; j < 32 && s[j]; j++)
            if (s[j] < 32 || s[j] > 126) { ok = 0; break; }
        if (ok && s[0]) Log("  +%03X -> %llX  \"%s\"", i, (unsigned long long)v, s);
    }
}

/* Dumps the marker scene's widget chain. Layout measured on 2026-10-20:
 *
 *   root+0x00  vtable
 *   root+0x20  first node, 0x28 bytes each:
 *              { A, common1, common2, common3, B }
 *
 * The three commons are identical for every node (all markers are one widget
 * type); A is a transform/colour block, B a resource index. Knowing how many
 * nodes exist and which A/B they carry is what makes two dumps comparable -
 * flipping a native marker option should make some nodes disappear. */
/* The minimap icon layer, node by node.
 *
 * 2026-10-22: the census says HUD_MinimapIcons draws exactly two kinds of image
 * widget - the vehicle icon and the collectible dot - while the minimap plainly
 * shows red enemy dots. So either the dots are widgets that carry no SH_P_IMAGE,
 * or the radar draws them itself. This dump prints every node with its class,
 * colour, alpha, visible flag and scale, which is what separates the two cases:
 * a dot-ish class carrying a faction colour is hideable by colour, a tree of
 * plain containers is not hideable at all. */



/* ---- menu --------------------------------------------------------------- */

/* Everything a verification pass needs, in one place. Testing a row used to
 * mean reading five scattered lines - which scenes are drawing, whether the
 * switch was applied, what the icon walk counted, what the markers did - and
 * the answers were minutes apart in the log. */



/* ---- the 9 layers the new pages own -------------------------------------
 *
 * One layer, one page. These were rows on the old "core" page; now they are
 * the rows of 小地图 / the top of the root page, and the advanced page must
 * not offer them a second time - two switches for one layer is two states
 * that disagree by the end of the session.
 */


/* The icon kinds the 小地图 page owns, driven from the preset state. */

/* Push one layer's want by name. A layer the poll has not seen yet parks in
 * g_hidePending - the list AddScene reads when the layer finally draws - so a
 * switch works before its layer has ever been on screen. */
static void LayerWant(const char *nm, int want) {
    int i;

    for (i = 0; i < g_nSeen; i++) {
        if (strcmp(g_name[i], nm) != 0) continue;
        g_want[i] = want ? 1 : 0;
        Push(nm, want);
        return;
    }
    for (i = 0; i < g_nHidePending; i++)
        if (strcmp(g_hidePending[i], nm) == 0) break;
    if (!want && i == g_nHidePending && g_nHidePending < HC_MAX) {
        strncpy(g_hidePending[g_nHidePending], nm, HC_NAME - 1);
        g_hidePending[g_nHidePending][HC_NAME - 1] = 0;
        g_nHidePending++;
    } else if (want && i < g_nHidePending) {
        memmove(g_hidePending[i], g_hidePending[i + 1],
                sizeof(g_hidePending[0]) * (size_t)(g_nHidePending - i - 1));
        g_nHidePending--;
    }
}

/* State -> engine. Everything the state says is pushed here, so there is one
 * place that knows what a setting means; the menu only edits the struct. */
static void HcApplyState(void) {
    LONG mask;

    /* 3D markers. Bit set = hidden, which is how the marker walk reads it.
     * 敌人 is a plain on/off now: off sets every enemy bit, on clears them and
     * leaves the per-marker decision to 仅视野内. */
    /* Watch the edge: with 敌人 on the mask is 0, so the pass stops running the
     * moment this row goes off - and a marker it had collapsed would stay
     * collapsed until the pool rebuilt it. The frame body does one full restore
     * sweep on the falling edge, whatever path turned the row off. */
    {
        int viewOnly = g_state.enemy && !g_state.enemy_blind;

        if (g_viewOnlyPrev && !viewOnly)
            InterlockedExchange(&g_needMarkRestore, 1);
        if (!g_viewOnlyPrev && viewOnly)
            MsResetRuns();                /* fresh confirmation, not an old one */
        g_viewOnlyPrev = viewOnly;
        g_viewOnly = viewOnly;
    }
    mask = g_state.enemy ? 0 : MARK_ENEMY_MASK;
    InterlockedExchange(&g_markOff, mask);
    /* g_viewOnly has to keep the pass alive on its own: with 敌人 on the mask is
     * 0, and without this the walk never runs and the row does nothing. */
    InterlockedExchange(&g_marksOn, (mask || g_viewOnly) ? 1 : 0);
    g_marksTold = 0;

    /* Rebuild the minimap table only when a row that decides it actually
     * changed. The old code set this on every call, and every menu action calls
     * here - measured 2026-10-26: 132 actions, 47 full scans (5-6 ms each),
     * every one inside the action, none while idle. */
    {
        static int prevMm = -1;
        int now = (g_state.mm_icons << 3) | (g_state.mm_vehicle << 2) |
                  (g_state.mm_collect << 1) | (g_state.mm_enemy ? 1 : 0);

        if (now != prevMm) { prevMm = now; g_mmDirty = 1; }
    }

    /* Icon kinds the new pages drive. A kind is set from exactly one of
     * these lines, never from two pages. */
    g_iconOff[23] = g_state.mm_vehicle ? 0 : 1;   /* UI_HUD_Vehicle_Icon */
    g_iconOff[0]  = g_state.mm_collect ? 0 : 1;   /* MiniMapDot_Collectible */
    /* Kinds 6 / 8 / 9 / 10 / 17 / 27 have no row of their own any more: a key
     * with no menu entry could hide an icon for good, so they stay shown. */
    g_iconOff[8] = g_iconOff[27] = g_iconOff[6] = 0;
    g_iconOff[9] = g_iconOff[10] = g_iconOff[17] = 0;
    /* Same rule as the minimap table: a pass costs milliseconds, so it is only
     * worth kicking when what the walk decides from actually moved. The old
     * condition stayed true for as long as a row was off, so dragging a slider
     * kicked a full icon pass per step. */
    {
        static int prevIcon = -1;
        int nowIcon = (mask ? 4 : 0) | (g_state.mm_vehicle ? 0 : 2) |
                      (g_state.mm_collect ? 0 : 1);

        if (nowIcon != prevIcon) {
            prevIcon = nowIcon;
            InterlockedExchange(&g_iconKick, 1);
        }
    }

    /* Scene-level layers. */
    LayerWant("HUD_Minimap",            g_state.mm_bg);
    LayerWant("HUD_MinimapIcons",       g_state.mm_icons);
    LayerWant("HUD_DamageDetection",    g_state.hit_indicator);
    LayerWant("HUD_FactionState_Notification", g_state.wanted);
    LayerWant("HUD_Tutorial",           g_state.tutorial);
    LayerWant("HUD_Drone",              g_state.oth_drone);
    LayerWant("HUD_Binocular",          g_state.oth_bino);


    g_mmAlpha = g_state.mm_alpha;
    g_mmScale = g_state.mm_scale;
    g_mmLogTold = 0;
    g_mmForce = 1;

    /* The icon pass returns early when nothing of ours is off; that flag has to
     * be rebuilt here or the minimap/map rows would set a kind and the walk
     * would never run to apply it. */
    IconStateSync();
}

/* ---- the menu, built from the state -------------------------------------
 *
 * Rows register as they are made (menu handle, label, state key), so a preset
 * switch can put every row back on the value the state holds without a second
 * list to keep in step. The labels live here because ShMenuSetValue and
 * ShMenuDisable both match on them.
 */
enum { RK_TOGGLE = 0, RK_INDEX };

typedef struct {
    uint32_t    menu;
    const char *label;
    const char *key;      /* state key, or NULL for an action row */
    unsigned char kind;   /* RK_* */
    int         lo, step, n;   /* RK_INDEX only */
} HcReg;
static HcReg g_reg[192];
static int   g_nReg;

static void HcUpdateDisabled(void);
static void HcRefreshMenu(void);
static int  MmIndex(int pct, int lo, int step, int n);

static void RegRow(uint32_t menu, const char *label, const char *key, int kind) {
    if (g_nReg >= (int)(sizeof(g_reg) / sizeof(g_reg[0]))) return;
    g_reg[g_nReg].menu = menu;
    g_reg[g_nReg].label = label;
    g_reg[g_nReg].key = key;
    g_reg[g_nReg].kind = (unsigned char)kind;
    g_reg[g_nReg].lo = 0;
    g_reg[g_nReg].step = 0;
    g_reg[g_nReg].n = 0;
    g_nReg++;
}

/* key -> value, and back. Two chains of strcmp, run only on a menu event. */
static int StateGet(const char *key) {
    if (!key) return 0;
#define G(f) if (strcmp(key, #f) == 0) return (int)g_state.f;
    G(enemy) G(enemy_blind)
    G(mm_bg) G(mm_icons) G(mm_enemy)
    G(mm_vehicle) G(mm_collect)
    G(hit_indicator) G(wanted) G(tutorial)
    G(oth_drone) G(oth_bino)
#undef G
    if (strcmp(key, "mm_scale") == 0) return (int)g_state.mm_scale;
    if (strcmp(key, "mm_alpha") == 0) return (int)g_state.mm_alpha;
    return 0;
}

static void StateSet(const char *key, int v) {
    if (!key) return;
#define S(f) if (strcmp(key, #f) == 0) { g_state.f = (unsigned char)v; return; }
    S(enemy) S(enemy_blind)
    S(mm_bg) S(mm_icons) S(mm_enemy)
    S(mm_vehicle) S(mm_collect)
    S(hit_indicator) S(wanted) S(tutorial)
    S(oth_drone) S(oth_bino)
#undef S
    if (strcmp(key, "mm_scale") == 0) { g_state.mm_scale = (unsigned short)v; return; }
    if (strcmp(key, "mm_alpha") == 0) { g_state.mm_alpha = (unsigned short)v; return; }
}

/* Put back every icon record whose kind is switched on again - right now,
 * instead of waiting for a walk that only happens when the widget draws. The
 * record carries the handle and the value it had, so this needs no scene and
 * no walk. */
static void HcRestoreIcons(void) {
    int k = ZeroFirst(), n = 0;

    while (k >= 0) {
        int nxt = ZeroNext(k);
        Zeroed *z = &g_zeroed[k];

        if (z->kind != 0xFF && !g_iconOff[z->kind]) {
            RestoreOne(z->h, z);
            ZeroDrop(k);
            n++;
        }
        k = nxt;
    }
    if (n) Log("icon restore: %d widget(s) put back immediately", n);
}

/* A two-option row: index 0 is 开启, 1 is 关闭. */
/* ---- duplicate menu events ----------------------------------------------
 *
 * Measured 2026-10-26 (and the same in the previous session, on the sliders):
 * one key press produces two callbacks carrying the SAME value, about 120 ms
 * apart. The framework fires once per press - the second one is the row being
 * re-fired, and it is pure duplicate work: another state write, another ini
 * flush, another marker pass, another icon restore, another FX write.
 *
 * Dropping an identical (key, value) inside 250 ms cannot lose a real action:
 * a toggle press changes its value, and a slider only re-fires the value it
 * already has when the key is released - which is exactly the case that needs
 * no work. The window is generous on purpose; a human cannot toggle the same
 * row twice in a quarter of a second and mean it.
 */
static char     g_dupKey[40];
static int      g_dupVal = -1;
static uint64_t g_dupTick;
static int      g_dupSkip;

static int DupMenuEvent(const char *key, int value) {
    uint64_t now = GetTickCount64();

    if (key && g_dupVal == value && now >= g_dupTick &&
        now - g_dupTick < 250 && strcmp(g_dupKey, key) == 0) {
        if (++g_dupSkip <= 5)
            Log("menu: %s = %d ignored (same event again within 250ms)",
                key, value);
        return 1;
    }
    if (key) {
        _snprintf(g_dupKey, sizeof(g_dupKey), "%s", key);
        g_dupKey[sizeof(g_dupKey) - 1] = 0;
    }
    g_dupVal = value;
    g_dupTick = now;
    return 0;
}

static void OnHcToggle(uint32_t menu, uint32_t item, int value, void *user) {
    int i = (int)(intptr_t)user;
    int on;

    (void)menu; (void)item;
    if (i < 0 || i >= g_nReg) return;
    if (DupMenuEvent(g_reg[i].key, value)) return;
    on = (value == 0) ? 1 : 0;
    StateSet(g_reg[i].key, on);
    HcApplyState();
    HcWriteSlot(&g_state, g_slotCur);
    HcUpdateDisabled();
    if (g_reg[i].key && (strcmp(g_reg[i].key, "enemy") == 0 ||
                         strcmp(g_reg[i].key, "enemy_blind") == 0)) {
        if (InterlockedCompareExchange(&g_markOff, 0, 0)) SafeMarkerCollapseAll();
        else MarkerShowAll();
    }
    HcRestoreIcons();
    Log("menu: %s = %d (slot %d)", g_reg[i].key, on, g_slotCur + 1);
}

/* The two minimap sliders hand over a step index; the state keeps percent. */
static void OnHcIndex(uint32_t menu, uint32_t item, int value, void *user) {
    int i = (int)(intptr_t)user;
    int pct;

    (void)menu; (void)item;
    if (i < 0 || i >= g_nReg) return;
    if (value < 0) value = 0;
    if (value >= g_reg[i].n) value = g_reg[i].n - 1;
    pct = g_reg[i].lo + value * g_reg[i].step;
    /* Deduplicated in percent, not in knob index: the log then says the same
     * thing as the line that did the work ("85%", not "knob 13"). */
    if (DupMenuEvent(g_reg[i].key, pct)) return;
    StateSet(g_reg[i].key, pct);
    HcWriteSlot(&g_state, g_slotCur);
    /* A slider changes the minimap's own picture and nothing else. Pushing the
     * whole state here made every step of a drag pay for an icon pass and a
     * table rebuild it could not possibly affect. */
    if (strcmp(g_reg[i].key, "mm_alpha") == 0) g_mmAlpha = pct;
    else g_mmScale = pct;
    g_mmForce = 1;                 /* write on the next frame, not in five */
    SafeApplyMinimapFx();
    Log("menu: %s = %d%%", g_reg[i].key, pct);
}

/* Rows that cannot do anything while a master switch is off are dimmed, not
 * hidden: the player keeps seeing what the setting is and what it was left
 * at, and it comes back the moment the master does. */
static void HcUpdateDisabled(void) {
    int i;

    if (!pMenuDisable) return;
    for (i = 0; i < g_nReg; i++) {
        const char *k = g_reg[i].key;
        int off = 0;

        if (!k) continue;
        /* Minimap page: "icons" is the master - with it off the whole icon
         * scene is not drawn, blips included, so the rows that read that layer
         * go inert. "background" is not a master (it hides the map face only),
         * and the two sliders act on the container itself, so they keep
         * working. */
        if (g_reg[i].menu == g_pgMm && !g_state.mm_icons) {
            if (strcmp(k, "mm_enemy") == 0 ||
                strcmp(k, "mm_vehicle") == 0 || strcmp(k, "mm_collect") == 0)
                off = 1;
        }
        /* The view-only row belongs to 敌人: with the enemy row off there is
         * nothing left to narrow, so it is dimmed and inert - its own value is
         * untouched and comes back the moment the parent row does. */
        if (strcmp(k, "enemy_blind") == 0 && !g_state.enemy) off = 1;
        pMenuDisable(g_reg[i].menu, g_reg[i].label, off);
    }
}

/* Put every row on the state's value, without firing anything. */
static void HcRefreshMenu(void) {
    int i;

    if (!pMenuSetValue) return;
    for (i = 0; i < g_nReg; i++) {
        if (!g_reg[i].key) continue;
        if (g_reg[i].kind == RK_INDEX) {
            int pct, idx;

            /* Named, not assumed: the old "anything that is not alpha must be
             * scale" would silently feed a wrong value to a third slider. */
            if (strcmp(g_reg[i].key, "mm_alpha") == 0) pct = g_state.mm_alpha;
            else if (strcmp(g_reg[i].key, "mm_scale") == 0) pct = g_state.mm_scale;
            else continue;
            idx = MmIndex(pct, g_reg[i].lo, g_reg[i].step, g_reg[i].n);
            pMenuSetValue(g_reg[i].menu, g_reg[i].label, idx);
        } else if (g_reg[i].kind == RK_TOGGLE) {
            pMenuSetValue(g_reg[i].menu, g_reg[i].label,
                          StateGet(g_reg[i].key) ? 0 : 1);
        } else {
            pMenuSetValue(g_reg[i].menu, g_reg[i].label,
                          StateGet(g_reg[i].key));
        }
    }
    HcUpdateDisabled();
}

/* Reset: every switch that is off goes back on, and the HUD is told so in the
 * same frame. It walks the registry - the rows of the main page and its
 * subpages - and then the two minimap sliders, which go back to 100%: leaving
 * the map faded is not a reset. The action rows (调试 / 诊断 / 属性倾印) are
 * not switches and were never part of it. */
static void OnPresetReset(uint32_t menu, uint32_t item, int value, void *user) {
    int i, n = 0;

    (void)menu; (void)item; (void)value; (void)user;
    if (DupMenuEvent("reset", 0)) return;
    for (i = 0; i < g_nReg; i++) {
        if (g_reg[i].kind != RK_TOGGLE || !g_reg[i].key) continue;
        if (StateGet(g_reg[i].key)) continue;      /* already on */
        StateSet(g_reg[i].key, 1);
        n++;
    }
    /* The two minimap sliders are settings, not switches, but a reset that
     * leaves the map at 60% opacity is not a reset: they go back to 100% too.
     * Their menu rows are refreshed by HcRefreshMenu below, which reads the
     * state this loop has just written. */
    for (i = 0; i < g_nReg; i++) {
        if (g_reg[i].kind != RK_INDEX || !g_reg[i].key) continue;
        if (strcmp(g_reg[i].key, "mm_alpha") != 0 &&
            strcmp(g_reg[i].key, "mm_scale") != 0)
            continue;
        if (StateGet(g_reg[i].key) == 100) continue;
        /* HcApplyState below copies the state into g_mmAlpha/g_mmScale and
         * raises g_mmForce, so this only has to move the value. */
        StateSet(g_reg[i].key, 100);
        n++;
    }
    HcWriteSlot(&g_state, g_slotCur);
    HcApplyState();                       /* layers, icon kinds, minimap table */
    HcRestoreIcons();                     /* icons collapsed by kind */
    InterlockedExchange(&g_needMarkRestore, 1);   /* 3D markers put back */
    SafeApplyMinimapFx();                 /* opacity/scale back to 100% now */
    HcRefreshMenu();
    Log("reset: %d row(s) back to default", n);
}

/* Every switch is a left/right row, two options, no checkboxes: one control
 * shape for the whole menu is easier to use with a pad than a mix of ticks and
 * arrows, and it matches the three-state rows. The two option strings are text
 * ids like every label, so the framework translates them on every draw - which
 * is also what makes a language switch take effect without rebuilding the
 * menu. */
static const char *const g_onOff[2] = { "@mhc.on", "@mhc.off" };

/* Labels are text ids, resolved by the framework against the tables declared in
 * ShLangDeclare below. Nothing here copies or caches the translated string: the
 * id is also what ShMenuSetValue/ShMenuDisable match on, so a row can be
 * refreshed from the state in any language. */
static void HcToggle(uint32_t menu, const char *id, const char *key) {
    int me;

    RegRow(menu, id, key, RK_TOGGLE);
    me = g_nReg - 1;
    if (!pMenuList || !menu) return;
    /* The state keeps 1 = shown; the list index is 0 = 开启 by convention, so
     * the row shows index (!value). */
    pMenuList(menu, id, g_onOff, 2,
              StateGet(key) ? 0 : 1, OnHcToggle, (void *)(intptr_t)me);
}

/* The three-state row: 0 on, 1 reserved, 2 off. The middle option is listed
 * because it is the shape the feature will have; until the view-cone pass
 * exists it acts as off, which is what a player picking it is asking for. */


/* A percent row, shown as steps because that is what the engine writes. The
 * option strings are "80%" and need no translation; the row label is an id. */
static void HcSlider(uint32_t menu, const char *id, const char *key,
                     const char *const *opts, int n, int lo, int step) {
    int pct = StateGet(key);
    int me;

    RegRow(menu, id, key, RK_INDEX);
    me = g_nReg - 1;
    g_reg[me].lo = lo;
    g_reg[me].step = step;
    g_reg[me].n = n;
    if (pMenuList && menu)
        pMenuList(menu, id, opts, n, MmIndex(pct, lo, step, n), OnHcIndex,
                  (void *)(intptr_t)me);
}

/* ---- the plugin's own text ----------------------------------------------
 *
 * Every row label, page title, hint and on/off word is a text id, and the text
 * lives in these two tables. This is the same shape every other plugin in this
 * repo ships (TimeWeatherControl, AmmoControl, Ballistics ...): a zh-CN and an
 * en-US baseline compiled in, declared once each, so the menu has both
 * languages without a lang.ini on disk and follows a language change on the
 * next frame. ShLangDeclare is called with the plugin's own module name, which
 * is the owner the framework derives for this menu (see OwnerFromAddress).
 *
 * Keys are stable: an id is never renamed for wording, and the ids are also
 * what ShMenuSetValue/ShMenuDisable match on. A row added later gets a new key
 * appended in BOTH tables - a key present in one language only falls back to
 * en-US, which is how a missing translation shows up as English, never as a
 * blank row.
 *
 * Percent options ("80%") are not here: a number needs no translating. */
static const ShText kMhcZh[] = {
    { "@mhc.page",         "更多HUD显示控制" },
    { "@mhc.hint",         "更多的HUD显示控制开关" },
    { "@mhc.enemy",        "敌人" },
    { "@mhc.enemy_blind",  "视野盲区的敌人" },
    { "@mhc.mm",           "小地图" },
    { "@mhc.hit",          "受击指示" },
    { "@mhc.wanted",       "通缉等级" },
    { "@mhc.tutorial",     "其他教程" },
    { "@mhc.other",        "其他" },
    { "@mhc.reset",        "重置" },
    { "@mhc.scale",        "缩放" },
    { "@mhc.alpha",        "不透明度" },
    { "@mhc.bg",           "背景" },
    { "@mhc.icons",        "图标" },
    { "@mhc.mmenemy",      "敌人" },
    { "@mhc.vehicle",      "载具" },
    { "@mhc.collect",      "弹药箱" },
    { "@mhc.drone",        "无人机" },
    { "@mhc.bino",         "望远镜" },
    { "@mhc.on",           "开启" },
    { "@mhc.off",          "关闭" },
};

static const ShText kMhcEn[] = {
    { "@mhc.page",         "More HUD Control" },
    { "@mhc.hint",         "More control over what the HUD shows" },
    { "@mhc.enemy",        "Enemies" },
    { "@mhc.enemy_blind",  "Enemies out of view" },
    { "@mhc.mm",           "Minimap" },
    { "@mhc.hit",          "Damage indicator" },
    { "@mhc.wanted",       "Wanted level" },
    { "@mhc.tutorial",     "Other tutorials" },
    { "@mhc.other",        "Other" },
    { "@mhc.reset",        "Reset" },
    { "@mhc.scale",        "Scale" },
    { "@mhc.alpha",        "Opacity" },
    { "@mhc.bg",           "Background" },
    { "@mhc.icons",        "Icons" },
    { "@mhc.mmenemy",      "Enemies" },
    { "@mhc.vehicle",      "Vehicles" },
    { "@mhc.collect",      "Ammo boxes" },
    { "@mhc.drone",        "Drone" },
    { "@mhc.bino",         "Binoculars" },
    { "@mhc.on",           "On" },
    { "@mhc.off",          "Off" },
};

static void EnsureMenu(void) {
    /* Made on the first poll, on whatever screen the player is on. It used to
     * wait for ShIsInGame, on the reasoning that the HUD layers only exist
     * inside the world - but the scene list arrives in the main menu too (72
     * rows measured there), so there was nothing to wait for. */
    if (g_menu || !pMenuCreate) return;

    g_menu = pMenuCreate("@mhc.page");
    if (!g_menu) return;
    g_pgRoot = g_menu;
    /* The slider option strings ("80%") are built here, once: the row list
     * holds pointers into these buffers, and a pointer that was never filled
     * shows as an empty value column. */
    BuildMmLabels();
    if (pMenuHint)
        pMenuHint(g_menu, "@mhc.hint");

    HcToggle(g_menu, "@mhc.enemy", "enemy");
    HcToggle(g_menu, "@mhc.enemy_blind", "enemy_blind");

    if (pMenuSub) {
        g_pgMm  = pMenuSub(g_menu, "@mhc.mm");
    }

    /* 受击指示 = HUD_DamageDetection, the direction arrows when YOU are hit -
     * and the blood screen is drawn by that same layer, so it has no switch of
     * its own and a second row for it only ever lied. */
    HcToggle(g_menu, "@mhc.hit", "hit_indicator");
    /* Both were advanced-page rows until 2026-10-26: measured, the first is the
     * wanted-level panel in the top right and the second is the small tip block
     * at mid-left, so each gets a name of its own and a row on the main page. */
    HcToggle(g_menu, "@mhc.wanted", "wanted");
    HcToggle(g_menu, "@mhc.tutorial", "tutorial");

    if (pMenuSub)
        g_pgOth = pMenuSub(g_menu, "@mhc.other");

    /* Last row of the menu: the one that undoes everything above it. */
    if (pMenuAction)
        pMenuAction(g_menu, "@mhc.reset", OnPresetReset, NULL);

    /* ---- 小地图 ---- */
    if (g_pgMm) {
        HcSlider(g_pgMm, "@mhc.scale", "mm_scale", g_mmScaleOpts,
                 MM_SCALE_N, MM_SCALE_LO, MM_SCALE_STEP);
        HcSlider(g_pgMm, "@mhc.alpha", "mm_alpha", g_mmAlphaOpts,
                 MM_ALPHA_N, MM_ALPHA_LO, MM_ALPHA_STEP);
        HcToggle(g_pgMm, "@mhc.bg", "mm_bg");
        HcToggle(g_pgMm, "@mhc.icons", "mm_icons");
        HcToggle(g_pgMm, "@mhc.mmenemy", "mm_enemy");
        HcToggle(g_pgMm, "@mhc.vehicle", "mm_vehicle");
        HcToggle(g_pgMm, "@mhc.collect", "mm_collect");
    }

    /* ---- 其他 ---- */
    if (g_pgOth) {
        HcToggle(g_pgOth, "@mhc.drone", "oth_drone");
        HcToggle(g_pgOth, "@mhc.bino", "oth_bino");
    }

    HcUpdateDisabled();
    Log("menu created (pages root=%u mm=%u other=%u, rows %d)",
        g_pgRoot, g_pgMm, g_pgOth, g_nReg);
}

/* ---- poll --------------------------------------------------------------- */

static void Poll(void) {
    static char buf[HC_LIST];
    int off = 0;

    if (!pScenes) return;

    if (pScenes(buf, sizeof(buf)) > 0) {
        while (buf[off]) {
            char nm[HC_NAME];
            int l = 0;
            while (buf[off] && buf[off] != ',' && l < HC_NAME - 1)
                nm[l++] = buf[off++];
            nm[l] = 0;
            if (buf[off] == ',') off++;
            if (l) AddScene(nm, 1);
        }
    }
    EnsureMenu();
}


static DWORD WINAPI Worker(LPVOID param) {
    DWORD last = 0;
    (void)param;

    for (;;) {
        DWORD now = GetTickCount();
        if (now - last >= HC_POLL_MS) {
            last = now;
            Poll();
        }
        Sleep(50);
    }
    return 0;
}

/* ---- startup ------------------------------------------------------------ */

static void Resolve(void) {
    HMODULE di = GetModuleHandleA("dinput8.dll");
    if (!di) {
        Log("dinput8.dll not found - nothing to talk to");
        return;
    }
    *(FARPROC *)&pScenes     = GetProcAddress(di, "ShGameHudScenes");
    *(FARPROC *)&pAllScenes  = GetProcAddress(di, "ShGameScenes");
    *(FARPROC *)&pShow       = GetProcAddress(di, "ShGameHudSceneShow");
    *(FARPROC *)&pVisible    = GetProcAddress(di, "ShGameHudSceneVisible");
    *(FARPROC *)&pMenuCreate = GetProcAddress(di, "ShMenuCreate");
    *(FARPROC *)&pMenuToggle = GetProcAddress(di, "ShMenuToggle");
    *(FARPROC *)&pMenuAction = GetProcAddress(di, "ShMenuAction");
    *(FARPROC *)&pMenuHint   = GetProcAddress(di, "ShMenuHint");
    *(FARPROC *)&pMenuNumber = GetProcAddress(di, "ShMenuNumber");
    *(FARPROC *)&pMenuList   = GetProcAddress(di, "ShMenuList");
    *(FARPROC *)&pLogPath    = GetProcAddress(di, "ShLogPath");
    *(FARPROC *)&pMenuSub    = GetProcAddress(di, "ShMenuSub");
    *(FARPROC *)&pSceneCount = GetProcAddress(di, "ShGameSceneCount");
    *(FARPROC *)&pSceneAt    = GetProcAddress(di, "ShGameSceneAt");
    *(FARPROC *)&pSceneName  = GetProcAddress(di, "ShGameSceneName");
    *(FARPROC *)&pSceneRoot  = GetProcAddress(di, "ShSceneRoot");
    *(FARPROC *)&pChildCount = GetProcAddress(di, "ShWidgetChildCount");
    *(FARPROC *)&pChildAt    = GetProcAddress(di, "ShWidgetChildAt");
    *(FARPROC *)&pWidgetClass = GetProcAddress(di, "ShWidgetClass");
    *(FARPROC *)&pWidgetGetS = GetProcAddress(di, "ShWidgetGetS");
    *(FARPROC *)&pWidgetGetF = GetProcAddress(di, "ShWidgetGetF");
    *(FARPROC *)&pWidgetGetV = GetProcAddress(di, "ShWidgetGetV");
    *(FARPROC *)&pWidgetSetF = GetProcAddress(di, "ShWidgetSetF");
    *(FARPROC *)&pWidgetSetU = GetProcAddress(di, "ShWidgetSetU");
    *(FARPROC *)&pWidgetGetU = GetProcAddress(di, "ShWidgetGetU");
    *(FARPROC *)&pWidgetSetV = GetProcAddress(di, "ShWidgetSetV");
    *(FARPROC *)&pFrameRegister = GetProcAddress(di, "ShRegisterFrameCallback");
    *(FARPROC *)&pCfgGetStr  = GetProcAddress(di, "ShPluginConfigGetStr");
    *(FARPROC *)&pCfgGetInt  = GetProcAddress(di, "ShPluginConfigGetInt");
    *(FARPROC *)&pCfgSetStr  = GetProcAddress(di, "ShPluginConfigSetStr");
    *(FARPROC *)&pCfgSetInt  = GetProcAddress(di, "ShPluginConfigSetInt");
    *(FARPROC *)&pMenuSetValue = GetProcAddress(di, "ShMenuSetValue");
    *(FARPROC *)&pMenuDisable  = GetProcAddress(di, "ShMenuDisable");
    *(FARPROC *)&pPropCount    = GetProcAddress(di, "ShPropCount");
    *(FARPROC *)&pPropAt       = GetProcAddress(di, "ShPropAt");

    if (!pScenes || !pShow) {
        Log("this dinput8.dll has no per-scene HUD calls - rebuild the framework");
        pScenes = NULL;
        pShow = NULL;
    }
    ResolveSetters();
}

/* The plugin's own name, taken from the module file: the log, the ini and the
 * text owner all follow it, so a folder renamed in the package keeps working
 * without a line of code changing. It has to match what the framework derives
 * for this menu (OwnerFromAddress in scripthook_menu.c: the module base name
 * with the .asi stripped), or the text ids would resolve to nothing. */
static void NameFromModule(HINSTANCE inst) {
    char mod[MAX_PATH];
    char *base, *dot;
    size_t n;

    g_owner[0] = 0;
    if (!inst || !GetModuleFileNameA(inst, mod, sizeof(mod))) return;
    base = strrchr(mod, '\\');
    base = base ? base + 1 : mod;
    dot = strrchr(base, '.');
    n = dot ? (size_t)(dot - base) : strlen(base);
    if (n == 0 || n >= sizeof(g_owner)) { g_owner[0] = 0; return; }
    memcpy(g_owner, base, n);
    g_owner[n] = 0;
}

static void ResolveDir(void) {
    char path[MAX_PATH];
    char *slash;

    if (!GetModuleFileNameA(g_inst, path, sizeof(path))) return;
    slash = strrchr(path, '\\');
    if (!slash) return;
    *slash = 0;                                  /* ...\plugins\MoreHudControl */
    slash = strrchr(path, '\\');
    if (!slash) return;
    *slash = 0;                                  /* ...\plugins */
    slash = strrchr(path, '\\');
    if (!slash) return;
    *slash = 0;                                  /* the game folder */
    strncpy(g_dir, path, sizeof(g_dir) - 1);
}

static DWORD WINAPI InitThread(LPVOID param) {
    int i;
    (void)param;
    NameFromModule(g_inst);
    ResolveDir();
    Resolve();
    OpenLog();          /* before LoadConfig: its config: line is worth having */
    /* Both languages, compiled in: the menu is readable in either without a
     * lang.ini on disk, and a language change takes effect on the next frame
     * because every label is a text id rather than a copied string. */
    if (g_owner[0]) {
        ShLangDeclare(g_owner, "en-US", kMhcEn,
                      (int)(sizeof(kMhcEn) / sizeof(kMhcEn[0])));
        ShLangDeclare(g_owner, "zh-CN", kMhcZh,
                      (int)(sizeof(kMhcZh) / sizeof(kMhcZh[0])));
    } else {
        Log("text: the module name could not be read - the menu falls back to "
            "its ids");
    }
    LoadConfig();
    CfgSelfCheck();
    Log("--- MoreHudControl: markers, minimap, layers ---");
    Log("build " __DATE__ " " __TIME__);
    if (!pScenes) Log("per-scene HUD calls missing; nothing will happen");
    /* One line that says which entry points this build actually got. Twice now
     * a feature was dead because its GetProcAddress line never made it into
     * the source, and nothing said so. */
    Log("api: scenes=%d show=%d menu=%d toggle=%d sub=%d number=%d list=%d "
        "widget=%d setter=%d", pScenes != 0, pShow != 0, pMenuCreate != 0,
        pMenuToggle != 0, pMenuSub != 0, pMenuNumber != 0, pMenuList != 0,
        pWidgetGetS != 0, pWidgetSetV != 0);
    if (!pWidgetSetF || !pWidgetSetU || !pWidgetSetV)
        Log("icon filter: this dinput8.dll has no ShWidgetSet* - rebuild the "
            "framework, or the icon page will not hide anything");
    else
        Log("icon filter: ready");

    /* The icon filter needs the game thread, not this worker: it writes widget
     * properties, and the engine's setters expect to be called from its own
     * thread. One frame callback, five passes a second, nothing at all while
     * every icon kind is on. */
    if (pFrameRegister) {
        if (pFrameRegister(IconFrame, NULL))
            Log("icon filter: frame callback registered");
        else
            Log("icon filter: no frame callback slot left - icon switches "
                "will not apply this session");
    }

    /* The roster first, then whatever the ini added: every row exists before
     * its layer has ever drawn, which is what lets a turret, a drone or a
     * swimming layer be switched off without having sat in one first. */
    if (g_preset)
        for (i = 0; i < NLABELS; i++) AddScene(g_labels[i].id, 0);
    Log("prefilled %d row(s): roster %d%s",
        (int)g_nSeen, g_preset ? NLABELS : 0, g_preset ? "" : " (off)");
    return Worker(NULL);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = inst;
        DisableThreadLibraryCalls(inst);
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
    }
    return TRUE;
}
