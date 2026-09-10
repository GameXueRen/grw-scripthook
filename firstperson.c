/* First person. The camera sits at the player's eye and the
 * head is hidden, so the body and weapon stay drawn.
 */
/* The eye is not computed here and the head is not hidden
 * from here. Both live in the ScriptHook's engine path
 * (ShFp2*): the eye is the engine's own answer for the head,
 * written where the camera position goes, and the head is
 * held down by the engine's own visibility call, once a
 * frame, from inside the camera frame.
 *
 * What this plugin owns is the part a player touches: the
 * on/off state, the eye offset, the hotkey, the menu, and
 * telling the player what is happening.
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

/* The eye offset, in centimetres, in world axes. The engine's
 * head position is already the eye, so the useful range is
 * small: these are the final centimetres of taste, not a
 * distance to walk forwards.
 */
#define OFF_DEF     0.0f
#define OFF_MIN     -100.0f
#define OFF_MAX     100.0f
#define OFF_STEP    1.0f

typedef int (*IsInGame_t)(void);
typedef int (*GameState_t)(void);
typedef int (*FirstPerson_t)(float, float);
typedef void (*Release_t)(uint32_t);
typedef int (*SetBlur_t)(int);
typedef void (*HandoverClear_t)(void);
typedef uint32_t (*ToastEx_t)(const char *, uint32_t, uint32_t);
typedef uint32_t (*ToastSet_t)(uint32_t, const char *, uint32_t,
                               uint32_t);
typedef uint32_t (*MenuCreate_t)(const char *);
typedef int (*MenuToggle_t)(uint32_t, const char *, int,
                            ShMenuFn, void *);
typedef int (*MenuNumber_t)(uint32_t, const char *, float, float,
                            float, float, ShMenuFn, void *);
typedef int (*MenuList_t)(uint32_t, const char *, const char **,
                          int, int, ShMenuFn, void *);
typedef int (*MenuSetValue_t)(uint32_t, const char *, int);
typedef int (*MenuStatus_t)(uint32_t, const char *);
typedef int (*MenuStatusF_t)(uint32_t, const char *, ...);
typedef int (*MenuIsOpen_t)(void);
typedef int (*MenuHint_t)(uint32_t, const char *);
typedef int (*LogPath_t)(const char *, char *, int);

/* The engine side of the view. Optional: a ScriptHook without
 * them has no first person at all, and the plugin says so
 * rather than pretending. */
typedef int      (*Fp2Install_t)(void);
typedef int      (*Fp2Extras_t)(uint32_t);
typedef int      (*Fp2Ready_t)(void);
typedef uint32_t (*Fp2Missing_t)(void);
typedef void     (*Fp2Enable_t)(int);
typedef void     (*Fp2SetOffset_t)(float, float, float);
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
static FirstPerson_t g_fp;
static Release_t    g_release;
static ToastEx_t    g_toastEx;
static ToastSet_t   g_toastSet;
static HandoverClear_t g_handoverClear;
static MenuIsOpen_t g_menuIsOpen;
static MenuSetValue_t g_menuSetValue;
static MenuStatus_t g_status;
static MenuStatusF_t g_statusF;
static SetBlur_t    g_setBlur;

static Fp2Install_t   g_fpxInstall;
static Fp2Extras_t    g_fpxExtras;
static Fp2Ready_t     g_fpxReady;
static Fp2Missing_t   g_fpxMissing;
static Fp2Enable_t    g_fpxEnable;
static Fp2SetOffset_t g_fpxSetOff;
static Fp2Gate_t      g_fpxGate;
static Fp2HeadOk_t    g_fpxHeadOk;
static Fp2HeadShow_t  g_fpxHeadShow;
static Fp2Bow_t       g_fpxBow;
static Fp2Age_t       g_fpxAge;
static int            g_fpxUp = 0;   /* engine sites are patched */

static uint32_t g_menu = 0;
static volatile int   g_on = 0;
/* [Settings] engine_extras takes the body and shoulder patches,
 * one bit each: 1 the body position hook, 2 body visibility,
 * 4 the shoulder swap, 8 the wall push. All four by default -
 * they are what the table does, and a build that disagrees with
 * one of them can leave it out bit by bit. */
static volatile int   g_extras = 15;

/* The eye offset, cm, world axes: the only camera numbers the
 * player tunes. The old per stance and per vehicle sets went
 * with the old placement - the engine's own head position
 * already handles slopes, vehicles and respawns, so one set
 * of final centimetres is all that is left to want. */
static volatile float g_offX = OFF_DEF;
static volatile float g_offY = OFF_DEF;
static volatile float g_offZ = OFF_DEF;

/* A hotkey flips first/third person while playing: the same
 * toggle as the menu's Enabled row. The key edge is polled on
 * its own thread so the flip answers regardless of what the
 * tick is doing. Index 0 is "None", meaning the hotkey is
 * off. */
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

/* ---- the camera ------------------------------------------- */

/* The offset is all this plugin contributes to the eye: the
 * ScriptHook does the rest inside the engine's frame. Taking
 * the camera is still a camera call, because that is what
 * marks the position as ours. */
static void PushCamera(void) {
    if (g_fpxSetOff)
        g_fpxSetOff(g_offX / 100.0f, g_offY / 100.0f,
                    g_offZ / 100.0f);
    /* Still the camera call that claims the position: the bit
     * it sets is what routes the manager's frame to the engine
     * path, and on a build without those sites the old camera
     * state is what keeps the plugin from doing harm. */
    if (g_fp) g_fp(0.0f, 0.0f);
}

/* The hook reapplies the eye every frame until it is given
 * back, so a screen the player opens has to release it. */
static volatile int g_held = 0;

static void Hold(int want) {
    if (want == g_held) return;
    g_held = want;
    if (want) {
        if (g_fpxUp && g_fpxEnable) g_fpxEnable(1);
        PushCamera();
    } else {
        if (g_fpxUp && g_fpxEnable) g_fpxEnable(0);
        if (g_release) g_release(SH_CAM_POS);
    }
}

/* Handing the head back. The engine side shows it on its own
 * when first person lets go - ShFp2Enable(0) opens a show
 * window and ShFp2HeadShow(1) says it directly - so all this
 * does is make sure, the way the old node scan never had to
 * because it was the only writer. There is no scan any more:
 * the engine's own visibility call is the one writer, and a
 * frame where it cannot name the head is a frame the log
 * explains. */
static void ShowHead(void) {
    if (g_fpxUp && g_fpxHeadShow) g_fpxHeadShow(1);
}

/* … and taking it back. Until this is said the engine path
 * only shows the head, so the other half of every hand-over
 * has to be said too: menus, the drone and the engine's own
 * views show it, and the frame after they close hides it
 * again. */
static void HideHead(void) {
    if (g_fpxUp && g_fpxHeadShow) g_fpxHeadShow(0);
}

/* ---- what the bar says -----------------------------------
 * One line across the top of the screen, so nothing here has
 * to be guessed at from the view alone. Only a state that
 * changed is said, so a wait that runs for seconds does not
 * announce itself on every one of those ticks.
 */
enum {
    SAY_NONE = 0,
    SAY_FP_ON,         /* first person on                     brief */
    SAY_FP_AWAY,       /* the view belongs to the engine      hold  */
    SAY_TP             /* third person                        brief */
};
static volatile int      g_said = SAY_NONE;
static uint32_t          g_toastId = 0;

/* Green once it is done, amber while something is being waited
 * for, plain white for a plain change of view. */
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
     * moves on must not stack up. A line whose time is up is
     * gone and cannot be set, so it is simply made again -
     * not noticing that lost every message after the first. */
    if (g_toastId && g_toastSet &&
        g_toastSet(g_toastId, text, rgb, ms))
        return;
    g_toastId = g_toastEx(text, rgb, ms);
}

static void Report(void);
static void SaveIni(void);

/* Turn first person on or off. Runs on both the menu worker
 * (Enabled toggle) and the hotkey thread. Everything here is
 * camera state: the engine path resolves nothing and scans
 * nothing, so neither thread can stall the other. */
static void SetFp(int on) {
    if (on) {
        g_on = 1;
        Hold(1);
        /* Whatever the head was doing - shown through a menu,
         * handed back by an earlier switch - first person
         * takes it again from here. */
        HideHead();
        if (g_setBlur) g_setBlur(0);
        Say(SAY_FP_ON, "第一人称已开启", SAY_RGB_DONE,
            SH_TOAST_MS_DEFAULT);
    } else {
        g_on = 0;
        /* Turning first person off is a change of view, not a
         * camera handed to the engine for an aim, so the grace
         * that keeps the head hidden across an aim has to go
         * with it. */
        if (g_handoverClear) g_handoverClear();
        ShowHead();
        if (g_setBlur) g_setBlur(1);
        Hold(0);
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

/* The callbacks must not touch the player: everything heavy
 * lives in the ScriptHook now, but the rule costs nothing to
 * keep and the tick thread still owns the clock. */
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
}

/* One offset, three axes. user carries the axis (0 X, 1 Y,
 * 2 Z). */
static void OnOffset(uint32_t menu, uint32_t item, int value,
                     void *user) {
    int axis = (int)(intptr_t)user;
    (void)menu; (void)item;

    switch (axis) {
    case 0: g_offX = (float)value; break;
    case 1: g_offY = (float)value; break;
    case 2: g_offZ = (float)value; break;
    default: return;
    }
    SaveIni();
    /* Only while we already own it, or dragging a slider would
     * take the camera back during a screen. */
    if (g_on && g_held) PushCamera();
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

/* The row is printed from a template, and the template is
 * translated before it is filled in - so the English below is a
 * key in [zh_cn.First person] rather than something the player
 * ever reads.
 */
static void SayStatus(const char *tmpl, const char *why,
                      float x, float y, float z) {
    char line[128];

    if (g_statusF) {
        g_statusF(g_menu, tmpl, why, x, y, z);
        return;
    }
    snprintf(line, sizeof(line), tmpl, why, x, y, z);
    g_status(g_menu, line);
}

#define STATUS_OFF     "off"
#define STATUS_ON      "on, head hidden [%+.0f %+.0f %+.0f]"
#define STATUS_AWAY    "on, view taken by the engine (%s) [%+.0f %+.0f %+.0f]"
#define STATUS_NOSITE  "on, engine sites missing - no first person"

static void Report(void) {
    if (!g_status) return;
    if (!g_on) {
        if (g_statusF) g_statusF(g_menu, STATUS_OFF);
        else           g_status(g_menu, STATUS_OFF);
        return;
    }
    if (!g_fpxUp) {
        g_status(g_menu, STATUS_NOSITE);
        return;
    }
    SayStatus(STATUS_ON, "", g_offX, g_offY, g_offZ);
}

static void ReportAway(const char *why) {
    if (!g_status) return;
    SayStatus(STATUS_AWAY, why, g_offX, g_offY, g_offZ);
}

/* ---- the tick -------------------------------------------- */

static DWORD WINAPI TickThread(LPVOID p) {
    int dPlay = -1, dAway = -1;
    uint64_t lastBeat = 0, menuHeldAt = 0;
    int menuStuckSaid = 0;
    (void)p;

    for (;;) {
        int playing, menu = 0, drone = 0, ads = 0, fresh = 0;
        int away;
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

        /* A menu count that never comes back down leaves the
         * gates open and no eye is placed until something
         * decrements it - the engine pairs those increments
         * with decrements, so a count held with no menu on
         * screen is the pairing having been missed. Nothing
         * here can safely write a byte the engine owns, so it
         * is said instead of fixed. */
        if (playing && menu > 0 &&
            (!g_menuIsOpen || !g_menuIsOpen())) {
            if (!menuHeldAt) menuHeldAt = nowMs;
            else if (!menuStuckSaid && nowMs - menuHeldAt > 10000) {
                menuStuckSaid = 1;
                Diag("gate: menu count stuck at %d with no menu "
                     "open (bow=%d)", menu,
                     g_fpxBow ? g_fpxBow() : -1);
            }
        } else {
            menuHeldAt = 0;
            menuStuckSaid = 0;
        }

        if (nowMs - lastBeat >= 1000) {
            lastBeat = nowMs;
            Diag("beat: play=%d on=%d held=%d menu=%d drone=%d "
                 "ads=%d bow=%d age=%u headok=%d "
                 "off=%.0f,%.0f,%.0f",
                 playing, g_on, g_held, menu, drone, ads,
                 g_fpxBow ? g_fpxBow() : -1,
                 g_fpxAge ? g_fpxAge() : 0u,
                 g_fpxHeadOk ? g_fpxHeadOk() : -1,
                 g_offX, g_offY, g_offZ);
        }

        /* Off the camera, or a load screen: everything goes
         * back, and the engine path shows the head on its way
         * (the show window opened by ShFp2Enable(0) runs on the
         * camera frame, so this needs no per frame help). */
        if (!g_on || !HoldThroughScreens()) {
            if (g_held) {
                Hold(0);
                Report();
            }
            continue;
        }

        Hold(1);

        /* A menu, the drone or a cutscene is a view the engine
         * drives, and the head is meant to show in it. The
         * engine path already declines to place the eye there;
         * the head needs both halves of the hand-over said:
         * shown on the way in, handed back to the engine path
         * on the way out. Saying only the first half is a head
         * that stays visible for the rest of the session. */
        away = menu || drone || EngineView();
        if (away != dAway) {
            dAway = away;
            Diag("away: menu=%d drone=%d engine=%d",
                 menu, drone, EngineView());
            if (away) {
                ReportAway(menu ? "menu"
                         : drone ? "drone" : "engine view");
                ShowHead();
            } else {
                HideHead();
                Report();
            }
        }
    }
    return 0;
}

/* The flip hotkey lives on its own thread so the flip answers
 * regardless of what the tick is doing. SetFp is safe from
 * here: it flips state and queues camera calls for the game
 * thread.
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
    if (!g_iniPath[0]) return;
    g_diagOn = IniBool(g_iniPath, "diag", 0);
    g_extras = IniInt(g_iniPath, "engine_extras", 15);
    if (g_extras < 0) g_extras = 0;
    if (g_extras > 15) g_extras = 15;
    g_offX = ClampF(IniFloat(g_iniPath, "offset_x_cm", OFF_DEF),
                    OFF_MIN, OFF_MAX);
    g_offY = ClampF(IniFloat(g_iniPath, "offset_y_cm", OFF_DEF),
                    OFF_MIN, OFF_MAX);
    g_offZ = ClampF(IniFloat(g_iniPath, "offset_z_cm", OFF_DEF),
                    OFF_MIN, OFF_MAX);
    g_hotKey = IniInt(g_iniPath, "hotkey_key", g_hotKey);
    if (g_hotKey < 0 || g_hotKey >= HOTKEYS) g_hotKey = 0;
}

/* Write the current settings back to <name>.ini. "Enabled" is
 * a live state, not a setting, so it is deliberately not saved
 * and always starts off. */
static void SaveIni(void) {
    char buf[64];

    if (!g_iniPath[0]) return;
    snprintf(buf, sizeof(buf), "%.1f", g_offX);
    WritePrivateProfileStringA("Settings", "offset_x_cm", buf,
                               g_iniPath);
    snprintf(buf, sizeof(buf), "%.1f", g_offY);
    WritePrivateProfileStringA("Settings", "offset_y_cm", buf,
                               g_iniPath);
    snprintf(buf, sizeof(buf), "%.1f", g_offZ);
    WritePrivateProfileStringA("Settings", "offset_z_cm", buf,
                               g_iniPath);
    snprintf(buf, sizeof(buf), "%d", g_hotKey);
    WritePrivateProfileStringA("Settings", "hotkey_key", buf,
                               g_iniPath);
    snprintf(buf, sizeof(buf), "%d", g_diagOn);
    WritePrivateProfileStringA("Settings", "diag", buf,
                               g_iniPath);
    snprintf(buf, sizeof(buf), "%d", g_extras);
    WritePrivateProfileStringA("Settings", "engine_extras", buf,
                               g_iniPath);
}

/* ---- bind ------------------------------------------------ */

static DWORD WINAPI BindThread(LPVOID p) {
    HMODULE m = NULL;
    MenuCreate_t menuCreate = NULL;
    MenuToggle_t menuToggle = NULL;
    MenuNumber_t menuNumber = NULL;
    MenuList_t  menuList = NULL;
    MenuHint_t  menuHint = NULL;
    (void)p;

    while (!m) {
        m = GetModuleHandleA("dinput8.dll");
        if (!m) Sleep(500);
    }
    *(FARPROC *)&g_logPath = GetProcAddress(m, "ShLogPath");
    *(FARPROC *)&g_inGame = GetProcAddress(m, "ShIsInGame");
    *(FARPROC *)&g_state = GetProcAddress(m, "ShGetGameState");
    *(FARPROC *)&g_fp = GetProcAddress(m, "ShCameraFirstPerson");
    *(FARPROC *)&g_release =
        GetProcAddress(m, "ShCameraReleaseFields");
    /* Optional: the status line. An older dinput8 without it
     * simply says nothing. */
    *(FARPROC *)&g_toastEx = GetProcAddress(m, "ShToastEx");
    *(FARPROC *)&g_toastSet = GetProcAddress(m, "ShToastSet");
    *(FARPROC *)&g_handoverClear =
        GetProcAddress(m, "ShCameraHandoverClear");
    /* Optional: an older dinput8 just keeps the blur. */
    *(FARPROC *)&g_setBlur = GetProcAddress(m, "ShSetCameraBlur");
    *(FARPROC *)&g_menuIsOpen = GetProcAddress(m, "ShMenuIsOpen");
    *(FARPROC *)&g_menuSetValue =
        GetProcAddress(m, "ShMenuSetValue");
    *(FARPROC *)&g_status = GetProcAddress(m, "ShMenuStatus");
    *(FARPROC *)&g_statusF = GetProcAddress(m, "ShMenuStatusF");
    /* The menu builders. These have to be resolved before the
     * gate below, or the gate reads NULL and the whole plugin
     * bows out of binding - no menu, no threads, no first
     * person, and nothing in the log to say why. */
    *(FARPROC *)&menuCreate = GetProcAddress(m, "ShMenuCreate");
    *(FARPROC *)&menuToggle = GetProcAddress(m, "ShMenuToggle");
    *(FARPROC *)&menuNumber = GetProcAddress(m, "ShMenuNumber");
    *(FARPROC *)&menuList = GetProcAddress(m, "ShMenuList");
    *(FARPROC *)&menuHint = GetProcAddress(m, "ShMenuHint");

    /* The engine side of the view. Missing from an older
     * dinput8: the plugin binds, the menu says the sites are
     * missing, and no first person is offered. */
    *(FARPROC *)&g_fpxInstall = GetProcAddress(m, "ShFp2Install");
    *(FARPROC *)&g_fpxExtras =
        GetProcAddress(m, "ShFp2InstallExtras");
    *(FARPROC *)&g_fpxReady = GetProcAddress(m, "ShFp2Ready");
    *(FARPROC *)&g_fpxMissing = GetProcAddress(m, "ShFp2Missing");
    *(FARPROC *)&g_fpxEnable = GetProcAddress(m, "ShFp2Enable");
    *(FARPROC *)&g_fpxSetOff = GetProcAddress(m, "ShFp2SetOffset");
    *(FARPROC *)&g_fpxGate = GetProcAddress(m, "ShFp2Gate");
    *(FARPROC *)&g_fpxHeadOk = GetProcAddress(m, "ShFp2HeadOk");
    *(FARPROC *)&g_fpxHeadShow = GetProcAddress(m, "ShFp2HeadShow");
    *(FARPROC *)&g_fpxBow = GetProcAddress(m, "ShFp2Bow");
    *(FARPROC *)&g_fpxAge = GetProcAddress(m, "ShFp2Age");

    if (!g_inGame || !g_state || !g_fp || !g_release ||
        !menuCreate || !menuToggle || !menuNumber ||
        !g_status) {
        /* A plugin that silently does not bind is the worst
         * kind of failure to chase: no menu, no threads, and
         * nothing anywhere to say why. Say which symbol was
         * missing, and say it whether diag is on or not. */
        g_diagOn = 1;
        Diag("bind failed: ingame=%d state=%d fp=%d release=%d "
             "menu=%d toggle=%d number=%d status=%d",
             g_inGame != NULL, g_state != NULL, g_fp != NULL,
             g_release != NULL, menuCreate != NULL,
             menuToggle != NULL, menuNumber != NULL,
             g_status != NULL);
        return 1;
    }

    if (g_fpxInstall) g_fpxInstall();
    g_fpxUp = g_fpxReady && g_fpxReady();
    ResolveIniPath(m);
    LoadIni();
    /* The body and shoulder patches are the ones that change the
     * engine whether first person is on or not, so they are off
     * until asked for: [Settings] engine_extras. */
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
    /* The eye offset, cm in world axes: the only camera
     * numbers there are to tune. */
    if (menuNumber) {
        menuNumber(g_menu, "Offset X cm", g_offX,
                   OFF_MIN, OFF_MAX, OFF_STEP, OnOffset,
                   (void *)(intptr_t)0);
        menuNumber(g_menu, "Offset Y cm", g_offY,
                   OFF_MIN, OFF_MAX, OFF_STEP, OnOffset,
                   (void *)(intptr_t)1);
        menuNumber(g_menu, "Offset Z cm", g_offZ,
                   OFF_MIN, OFF_MAX, OFF_STEP, OnOffset,
                   (void *)(intptr_t)2);
    }
    if (menuHint)
        menuHint(g_menu,
                 "First person: the eye sits at the engine's own "
                 "head position, the head is hidden by the "
                 "engine's own call. Tune the offset here.");
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
