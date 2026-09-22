/* Field of view, changed from the menu and held every
 * frame by the ScriptHook's camera override.
 *
 * Five rows: the override switch, a fov for each view, the no-zoom switch,
 * and the way back to the game's own value. A frame with no iron sight up
 * carries its view's fov.
 *
 * No zoom on iron sights is the one part about the aiming camera rather
 * than a view, and it holds in both views at once: what it keeps is the fov
 * the frame already had, read from whichever view is on screen. With it off
 * the engine's own aim value goes through untouched - the game's own small
 * narrowing - and the magnified optics and the binoculars are left alone
 * either way (their values are under 0.5 rad, which the framework passes
 * through unless the pin is set, and the pin is only ever set for the iron
 * sights).
 *
 * The two fovs and the two switches live in fov_changer.ini beside the .asi
 * and are restored on the next launch.
 */
/* Linked against the ScriptHook, so the API is called
 * directly. See src/README.md for how that works.
 */
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>

#include "scripthook.h"
#include "log.h"

/* What this plugin needs of the framework: nothing newer than the first
 * version of the plugin API, so any ScriptHook that carries the API at all can
 * load this (see SH_REQUIRES_API). Name the last thing you use, not the header
 * you happened to build against. */
SH_REQUIRES_API(1);

#define DEG2RAD     0.01745329252f
#define RAD2DEG     57.2957795f

/* The engine's own vertical fov is about 0.815 rad, which
 * is the fallback if we never manage to read it.
 */
#define FALLBACK    0.815f
/* 30 to 120 in steps of one: the game's own value is about 47, and a step
 * of one is what lets two rows be set to exactly the same number. */
#define DEG_MIN     30.0f
#define DEG_MAX     120.0f
#define DEG_STEP    1.0f
#define TICK_MS     500
/* No zoom polls the fov faster than the menu's cadence: the sights come
 * up in a frame or two, not in half a second. */
#define NOZOOM_MS   16
/* A fov change is walked rather than snapped: about a sixth of a second
 * from one value to the next, on the same beat No zoom uses. A hold has
 * nothing to walk - its target is the value already on screen. */
#define RAMP_MS     16
#define RAMP_STEP   0.35f
#define RAMP_MIN    0.002f     /* rad: close enough to settle on the target */
/* While the override is on, which fov is in force depends on the view,
 * so a switch between first and third person has to land at once rather
 * than on the next slow tick. The same beat reads the view mode, and a
 * poll that often is also what keeps the head measurement it is derived
 * from alive.
 */
#define VIEW_MS     250
/* How long after an aim the view is left alone. The framework reports first
 * person for the engine's own aim camera on the head bone, and goes on doing
 * it for a beat after the aim comes down; a reading inside that window is
 * the hand over, not a view. */
#define HANDOVER_MS 800
/* Where an iron sight's fov sits. Measured live on the 2026-09 build,
 * the aim fov of every sight in the game: iron sights 0.69, 1x / 2.5x /
 * 3x 0.49, 2x 0.40 and 0.34, the 1x step of the dual lens 0.30, 3.5x
 * 0.28, 4x 0.25 and 0.24, 4.5x 0.23 and 0.19, 5x and 5.5x 0.20, 6x 0.17;
 * the hip is about 0.81. The iron sight is the only aim in this band -
 * the hip is above it and every magnified optic well below - so it is
 * recognised by the value alone. That matters: the aim state a plugin
 * can read is not dependable, and the whole feature would sit idle on a
 * frame where it read wrong.
 *
 * Measured again with the override at 121 deg (2026-09-21): the engine's
 * own values do not move with it - the hip still computes 0.81 rad and
 * the iron sights 0.69 rad - because the replacement happens after the
 * engine computes, at the store in scripthook_fov.c. The band above is
 * therefore right as it stands; what an override changes is the camera
 * alone, never what this reads.
 */
/* The hip's band, and the value an iron sight computes: the two things the
 * aim detection below turns on. Measured live on the 2026-09 build, the
 * iron sight is 0.69 and the hip about 0.81.
 */
#define SIGHT_HI    0.75f
/* The framework's own line between a gameplay fov and a zoom optic
 * (scripthook_fov.c passes anything under it straight through unless the
 * pin is set). Narrower than this is a scope, and its magnification is not
 * ours to take. */
#define OPTIC_RAD   0.50f
/* How far above the framework's 0.5 line the engine's value is followed.
 * Only a magnified optic's pull reaches down here; an iron sight's 0.69 is
 * well clear of it, which is what keeps the hold from being followed (and
 * No zoom from showing the pull it exists to hide). */
#define OPTIC_MARGIN 0.10f
/* What counts as the sights being up: the engine's own value sitting inside
 * the sights' band, within HOLD_CALM of where that run started, for
 * HOLD_MS. Measured against the alternative - the engine's pull travels
 * about 0.12 rad through this range in a couple of hundred milliseconds, so
 * a traveller leaves HOLD_CALM within a tick or two whatever the cadence,
 * while a held aim stays inside it (its own sway is a fraction of a
 * degree). See the aim test in TickThread. */
#define HOLD_CALM   0.02f
#define HOLD_MS     120u

/* Config convention, the same every plugin follows: the .ini sits beside
 * the .asi and takes its base name, so fov_changer.asi pairs with
 * fov_changer.ini. The name comes from the module file rather than a
 * literal, so the pairing survives a rename.
 */
static HINSTANCE g_inst = NULL;
static char      g_name[64];
static char      g_iniPath[MAX_PATH];
/* A slider row fires on every change, and one save is three
 * read-modify-write passes of the whole file. Dragging one produced that
 * per tick, so the rows only mark the file dirty and the tick writes it
 * once the burst is over. */
static volatile LONG g_iniDirty = 0;

static uint32_t g_menu = 0;
static volatile LONG  g_on = 0;      /* read by the tick, written by the menu */
/* Set on unload. Only a flag: the release belongs to the tick thread, because
 * DllMain runs under the loader lock, where a framework call is forbidden. */
static volatile LONG  g_stop = 0;
/* One fov per view, in degrees. The aiming camera has no value of its own:
 * it carries the view it was raised from (No zoom), or the engine's own.
 */
static volatile float g_fpDeg = 0.0f;
static volatile float g_tpDeg = 0.0f;
static volatile float g_defaultRad = 0.0f;
/* Which view the frame is showing, refreshed on the tick. A LONG rather
 * than an int because the tick writes it and the menu thread reads it;
 * UNKNOWN until the first read, and every reader has to treat that as
 * "not first person" (scripthook.h is explicit about why).
 */
static volatile LONG  g_view = SH_VIEW_UNKNOWN;
/* The fov a frame with no iron sight up carries, in radians, and the value
 * last handed to the camera - the ramp walks the second towards the first.
 *
 * No zoom holds the first of these rather than asking which view is on
 * screen. It cannot ask: while an aim is up the framework reports first
 * person for the engine's own aim camera, which sits on the head bone, so
 * a third person aim would read the first person row. That is what was
 * reported on 2026-09-21 - with the third person row set the sights still
 * narrowed, while the first person row left alone behaved.
 */
static volatile float g_lookRad = 0.0f;
static volatile float g_shown = 0.0f;
/* What the ini asked for, applied once the menu exists. */
static int g_initOverride = 0;
static int g_initNozoom = 0;
/* No zoom on iron sights: the toggle, the fov the hip last had, the value
 * the engine computed on the last frame, the value the render camera really
 * carries, and whether the last pass found the sights. The last three are
 * what the status line reports - engine against camera is what tells a
 * replacement that landed from one the engine wrote over.
 */
static volatile LONG  g_nozoom = 0;
static volatile float g_hipRad = 0.0f;
static volatile float g_engRad = 0.0f;
static volatile float g_camRad = 0.0f;
static volatile float g_engPrev = 0.0f;
static volatile LONG  g_aim = 0;

static int OverrideOn(void) {
    return InterlockedCompareExchange(&g_on, 0, 0) ? 1 : 0;
}

/* ---- settings -------------------------------------------- */

/* Resolve <gamedir>\plugins\<name>\<name>.ini from the plugin's own file
 * name, once - the framework knows the folders, this knows its own name. */
static void ResolveIniPath(HMODULE m) {
    char mod[MAX_PATH];
    const char *base, *dot;
    size_t len;

    g_name[0] = 0;
    g_iniPath[0] = 0;
    if (!m || !GetModuleFileNameA(m, mod, sizeof(mod))) return;
    base = strrchr(mod, '\\');
    base = base ? base + 1 : mod;
    dot = strrchr(base, '.');
    len = dot ? (size_t)(dot - base) : strlen(base);
    if (len == 0 || len >= sizeof(g_name)) return;
    memcpy(g_name, base, len);
    g_name[len] = 0;
    if (!ShPluginIniPath(g_name, g_iniPath, sizeof(g_iniPath)))
        g_iniPath[0] = 0;
}

static int IniInt(const char *key, int def) {
    if (!g_iniPath[0]) return def;
    return GetPrivateProfileIntA("Settings", key, def, g_iniPath);
}

/* The settings are optional: with no file, or a key missing, the built-in
 * defaults stand.
 *
 * A file written before the three rows existed carries one `fov` key: it
 * seeds BOTH views, so a widened first person view stays as wide as it
 * was, and the aiming row keeps following them. That is what makes an
 * existing config run exactly as it ran before.
 */
static void LoadIni(void) {
    int v, shared;

    if (!g_iniPath[0]) return;
    shared = IniInt("fov", 0);
    v = IniInt("fov_fp", shared);
    if ((float)v >= DEG_MIN && (float)v <= DEG_MAX) g_fpDeg = (float)v;
    v = IniInt("fov_tp", shared);
    if ((float)v >= DEG_MIN && (float)v <= DEG_MAX) g_tpDeg = (float)v;
    g_initOverride = IniInt("override", 0) ? 1 : 0;
    g_initNozoom = IniInt("nozoom", 0) ? 1 : 0;
}

static void SaveIni(void) {
    char buf[64];

    if (!g_iniPath[0]) return;
    snprintf(buf, sizeof(buf), "%d", OverrideOn());
    WritePrivateProfileStringA("Settings", "override", buf, g_iniPath);
    snprintf(buf, sizeof(buf), "%d", (int)(g_fpDeg + 0.5f));
    WritePrivateProfileStringA("Settings", "fov_fp", buf, g_iniPath);
    snprintf(buf, sizeof(buf), "%d", (int)(g_tpDeg + 0.5f));
    WritePrivateProfileStringA("Settings", "fov_tp", buf, g_iniPath);
    snprintf(buf, sizeof(buf), "%d",
             InterlockedCompareExchange(&g_nozoom, 0, 0) ? 1 : 0);
    WritePrivateProfileStringA("Settings", "nozoom", buf, g_iniPath);
}

static void SaveIniSoon(void) { InterlockedExchange(&g_iniDirty, 1); }

/* Captured while the override is off, so it is the game's value rather than
 * one of ours read back. Both the tick and a menu switch can call this, and
 * the test-and-set was two separate steps: the flag makes exactly one of
 * them the learner. The two writes below are aligned 32-bit, so a reader
 * sees one value or the other - never a mix of two.
 */
static volatile LONG g_learned;

static void LearnDefault(void) {
    ShCamera c;

    if (OverrideOn() || InterlockedCompareExchange(&g_learned, 0, 0)) return;
    if (!ShIsInGame()) return;
    if (!ShGetCamera(&c)) return;
    if (c.fov > 0.05f && c.fov < 3.0f) {
        g_defaultRad = c.fov;
        if (g_fpDeg <= 0.0f) g_fpDeg = c.fov * RAD2DEG;
        if (g_tpDeg <= 0.0f) g_tpDeg = c.fov * RAD2DEG;
        InterlockedExchange(&g_learned, 1);
    }
}

static float DefaultRad(void) {
    return (g_defaultRad > 0.0f) ? g_defaultRad : FALLBACK;
}

/* The fov of the view the frame is showing. Anything but FIRST_PERSON
 * reads the third person row - including SH_VIEW_UNKNOWN, which the
 * header says a consumer must treat as "not first person", and which is
 * what a player who never turns the first person plugin on will see.
 */
static float ViewRad(void) {
    float deg;

    if (InterlockedCompareExchange(&g_view, 0, 0) == SH_VIEW_FIRST_PERSON)
        deg = g_fpDeg;
    else
        deg = g_tpDeg;
    return (deg > 0.0f) ? deg * DEG2RAD : DefaultRad();
}

/* What the next apply carries.
 *
 * The first branch is the frame with an aim up (see the tick, which applies
 * nothing else while one is). With No zoom on, what it holds is the fov the
 * frame had before the aim came up - recorded by the tick rather than read
 * off the view, for the reason at g_lookRad - and in first person and third
 * person alike that is a value that does not move, which is the whole of
 * "does not zoom".
 *
 * With No zoom off it is the engine's own aim value instead, and the point
 * of applying it rather than releasing the channel is that it is then
 * walked to (see the tick's ramp) instead of arriving in one frame. A
 * release handed the camera straight to a value the engine had already
 * computed, so the aim was a snap - and in first person, where the fov is
 * the only thing an aim moves, that was the whole of what the player saw.
 */
static float WantedRad(void) {
    if (InterlockedCompareExchange(&g_aim, 0, 0)) {
        float hold;

        if (OverrideOn() && !InterlockedCompareExchange(&g_nozoom, 0, 0)) {
            float e = ShFovEngine();

            if (e > 0.05f && e < 3.0f) return e;
        }
        hold = g_lookRad;
        if (hold > 0.05f && hold < 3.0f) return hold;
        return (g_hipRad > 0.0f) ? g_hipRad : DefaultRad();
    }
    return ViewRad();
}

/* Whether there is a camera to change at all.
 *
 * This used to push a value here and now, and no longer does: every fov
 * change is walked by the tick, so a row, a switch and a view all arrive
 * the same way - and a value applied from a menu callback would land the
 * jump the walking exists to avoid. What the menu still needs to know is
 * whether the camera is up, which is what the status line reports; a
 * switch turned on before a session simply waits for the tick.
 */
static int Push(void) {
    return ShCameraReady();
}

/* ---- text ---------------------------------------------------------
 * The menu's own text, compiled in: with no lang.ini anywhere the menu
 * still reads in either language, and a lang.ini only has to carry what
 * it changes. The keys are stable IDs, so rewording a row never breaks
 * a translation - the reason they are not the English literals.
 */
static const ShText kEn[] = {
    { "@fov.page",       "Field of view" },
    { "@fov.override",   "Override" },
    { "@fov.tp",         "Third person fov" },
    { "@fov.fp",         "First person fov" },
    { "@fov.nozoom",     "No zoom on iron sights" },
    { "@fov.reset",      "Back to the game default" },
    { "@fov.status.on",  "%.0f deg, game default %.0f" },
    { "@fov.status.off", "off, game is %.0f deg" },
    { "@fov.status.both.on",
      "fov %.0f deg (game %.0f)\niron sights: engine %.2f, camera %.2f" },
    { "@fov.status.both.off",
      "fov %.0f deg (game %.0f)\nno iron sights, engine %.2f, camera %.2f" },
    { "@fov.notready",   "the camera is not ready" },
    { "@fov.hint",       "Widens the first and third person fov" }
};

static const ShText kZh[] = {
    { "@fov.page",       "延展视野范围" },
    { "@fov.override",   "覆盖游戏视野范围设置" },
    { "@fov.tp",         "第三人称视野范围" },
    { "@fov.fp",         "第一人称视野范围" },
    { "@fov.nozoom",     "机瞄开镜不缩放视野" },
    { "@fov.reset",      "恢复游戏默认视野范围" },
    { "@fov.status.on",  "当前视野范围 %.0f 度，游戏默认 %.0f" },
    { "@fov.status.off", "已关闭，当前视野范围 %.0f 度" },
    { "@fov.status.both.on",
      "视野范围 %.0f 度（游戏默认 %.0f）\n"
      "机瞄中：引擎 %.2f，相机 %.2f" },
    { "@fov.status.both.off",
      "视野范围 %.0f 度（游戏默认 %.0f）\n"
      "未在机瞄：引擎 %.2f，相机 %.2f" },
    { "@fov.notready",   "游戏视野尚未就绪" },
    { "@fov.hint",       "延展第一/第三人称视野范围" }
};

static void FovText(void) {
    static int done;

    if (done) return;
    done = 1;
    ShLangDeclare("fov_changer", "en-US", kEn,
                  (int)(sizeof(kEn) / sizeof(kEn[0])));
    ShLangDeclare("fov_changer", "zh-CN", kZh,
                  (int)(sizeof(kZh) / sizeof(kZh[0])));
}

static void Report(void) {
    float defDeg = DefaultRad() * RAD2DEG;
    float viewDeg = OverrideOn() ? ViewRad() * RAD2DEG : defDeg;

    if (InterlockedCompareExchange(&g_nozoom, 0, 0)) {
        /* The engine's own value and the camera's are both on the line
         * while the feature runs: equal means the replacement landed, and
         * a camera still on the engine's value means something wrote over
         * it after us. */
        if (InterlockedCompareExchange(&g_aim, 0, 0))
            ShMenuStatusF(g_menu, "@fov.status.both.on",
                          viewDeg, defDeg, g_engRad, g_camRad);
        else
            ShMenuStatusF(g_menu, "@fov.status.both.off",
                          viewDeg, defDeg,
                          g_engRad, g_camRad);
    } else if (g_on) {
        ShMenuStatusF(g_menu, "@fov.status.on",
                      viewDeg, defDeg);
    } else {
        ShMenuStatusF(g_menu, "@fov.status.off",
                      defDeg);
    }
}

static void OnToggle(uint32_t menu, uint32_t item, int value,
                     void *user) {
    (void)menu; (void)item; (void)user;

    if (value) {
        LearnDefault();
        /* A push needs a number for every view, including the case where
         * nothing has been learned or typed yet. */
        if (g_fpDeg <= 0.0f) g_fpDeg = DefaultRad() * RAD2DEG;
        if (g_tpDeg <= 0.0f) g_tpDeg = DefaultRad() * RAD2DEG;
        InterlockedExchange(&g_on, 1);
        if (!Push()) {
            InterlockedExchange(&g_on, 0);
            /* The framework's row already flipped to "on"; put it back, or
             * the menu claims an override that is not in force. */
            ShMenuSetValue(g_menu, "@fov.override", 0);
            ShMenuStatus(g_menu, "@fov.notready");
            return;
        }
    } else {
        InterlockedExchange(&g_on, 0);
        /* The no-zoom feature is on the same channel; releasing the
         * override must not hand it back while the sights are held. */
        if (!InterlockedCompareExchange(&g_nozoom, 0, 0))
            ShCameraReleaseFields(SH_CAM_FOV);
    }
    SaveIniSoon();
    Report();
}

static void OnFp(uint32_t menu, uint32_t item, int value,
                 void *user) {
    (void)menu; (void)item; (void)user;
    g_fpDeg = (float)value;
    if (OverrideOn()) Push();
    SaveIniSoon();
    Report();
}

static void OnTp(uint32_t menu, uint32_t item, int value,
                 void *user) {
    (void)menu; (void)item; (void)user;
    g_tpDeg = (float)value;
    if (OverrideOn()) Push();
    SaveIniSoon();
    Report();
}

/* No zoom on iron sights. Turning it off hands the channel back at once
 * rather than on the next pass: with the override off this feature is the
 * only thing holding it, and the tick may be half a second away.
 */
static void OnNoZoom(uint32_t menu, uint32_t item, int value,
                     void *user) {
    (void)menu; (void)item; (void)user;
    InterlockedExchange(&g_nozoom, value ? 1 : 0);
    if (!value) {
        ShFovPin(0);
        if (!OverrideOn()) ShCameraReleaseFields(SH_CAM_FOV);
    }
    SaveIniSoon();
    Report();
}

static void OnReset(uint32_t menu, uint32_t item, int value,
                    void *user) {
    (void)menu; (void)item; (void)value; (void)user;
    g_fpDeg = DefaultRad() * RAD2DEG;
    g_tpDeg = g_fpDeg;
    /* The number rows show the framework's own copy, so they have to be
     * told. */
    ShMenuSetValue(g_menu, "@fov.fp", (int)(g_fpDeg + 0.5f));
    ShMenuSetValue(g_menu, "@fov.tp", (int)(g_tpDeg + 0.5f));
    if (OverrideOn()) Push();
    SaveIniSoon();
    Report();
}

/* A blocked (PvP) mode: the framework takes this page out of the menu, so
 * the override has to let go here - a fov left pushed in Ghost War is
 * exactly what the blacklist exists to prevent. */
static void OnBlocked(int allowed, int blocked, void *user) {
    (void)blocked; (void)user;

    if (!allowed) {
        ShFovPin(0);
        ShCameraReleaseFields(SH_CAM_FOV);
    } else if (OverrideOn()) Push();
}

/* ---- diagnostic log ----------------------------------------------------
 *
 * The engine's value, what the camera carries, what we pushed and the verdict
 * that decided it - on every change, and once a second while it holds. The
 * plugin went without a log until 2026-09-21, and the two symptoms that day (a
 * hold that did not engage, a value that did not land) are exactly the kind
 * that cannot be told apart from the outside: both look like "it still zooms".
 *
 * Written from the tick thread only, so no lock. Off unless [Settings] diag=1
 * asks for it - the same shape firstperson uses, and for the same reason: this
 * is here for the next field report, not for every session.
 *
 * Through the framework's writer, so the file lands in logs\ with the others,
 * obeys [Settings] LogLevel and is kept for the runs before this one - log.h
 * renames the previous run's file aside rather than truncating it. */
static volatile LONG g_diagOn = -1;     /* -1 = not asked yet, 0 = off, 1 = on */
static volatile LONG g_diagMade;

static void DiagOpen(void) {
    /* Asked once and cached: GetPrivateProfileIntA is file I/O, and this runs
     * on the tick path. */
    if (g_diagOn < 0) {
        LONG on = IniInt("diag", 0) ? 1 : 0;

        InterlockedCompareExchange(&g_diagOn, on, -1);
    }
    if (g_diagOn != 1) return;
    if (!g_diagMade && InterlockedCompareExchange(&g_diagMade, 1, 0) == 0)
        LogInitAlways("fov_changer.log");
}

static void DiagState(float eng, float cam, float shown, float want,
                      int aim, int hold, int held, int nz, int ovr) {
    if (g_diagOn != 1) return;
    Log("eng=%.3f cam=%.3f shown=%.3f want=%.3f aim=%d hold=%d held=%d "
        "nz=%d ovr=%d",
        eng, cam, shown, want, aim, hold, held, nz, ovr);
}

/* Entering a session reinstalls the camera hook, so the
 * override is pushed again to survive the transition.
 */
static DWORD WINAPI TickThread(LPVOID p) {
    int held = 0;      /* the fov channel is ours right now */
    DWORD viewAt = 0;  /* when the view mode was last read */
    DWORD aimAt = 0;   /* when an aim was last up, of any kind */
    /* The hold detector, in time rather than in ticks: the engine's own
     * value inside the sights' band, within HOLD_CALM of where that run
     * started, for HOLD_MS. See the aim test - a value passing through the
     * band is not an aim, and a per-tick delta cannot tell the two apart. */
    static float    holdRef = 0.0f;
    static uint64_t holdAt = 0;
    static int      hold = 0;
    (void)p;

    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        int in = ShIsInGame();
        int allowed = ShPluginAllowed();
        int nz = InterlockedCompareExchange(&g_nozoom, 0, 0) != 0;
        ShCamera cam;
        float eng = ShFovEngine();
        int aim;
        int quiet = 0;     /* the engine's own value did not move this tick */

        LearnDefault();

        /* Which view the frame is showing, which is what picks between the
         * first and third person rows. Read on its own slow beat: the mode
         * is derived from a head measurement that the read itself keeps
         * alive, so a poll per tick would hold that measurement running for
         * nothing, and the view does not change faster than this.
         *
         * Only a frame whose engine value is a plain hip is asked, and that
         * is not an optimisation. With an aim up, and for a beat after it
         * comes down, the framework reports first person for the engine's
         * own aim camera sitting on the head bone - so taking the reading
         * would hand a third person aim the first person row. That is the
         * dip, the pause and the return reported on 2026-09-21, on the iron
         * sights and on every scope alike.
         *
         * So an aim of any kind is remembered here - a scope's value is as
         * much an aim as an iron sight's - and the view is only read once a
         * beat has passed since the last one.
         */
        if (eng > 0.05f && eng < SIGHT_HI) aimAt = GetTickCount();
        if ((DWORD)(GetTickCount() - viewAt) >= VIEW_MS) {
            viewAt = GetTickCount();
            if (eng >= SIGHT_HI && eng < 1.2f &&
                (DWORD)(GetTickCount() - aimAt) >= HANDOVER_MS)
                InterlockedExchange(&g_view, ShCameraViewMode());
        }

        /* What the render camera really carries, beside the engine's own
         * value: the two together say whether a replacement landed. */
        if (ShGetCamera(&cam) && cam.fov > 0.05f && cam.fov < 3.0f)
            g_camRad = cam.fov;

        /* The engine's own value, taken before it is replaced: it is what
         * tells the sights apart from the hip and from a magnified optic. */
        if (eng > 0.05f && eng < 3.0f) g_engRad = eng;
        /* The hip's own fov, learned only from a value that is standing
         * still. A frame halfway through the pull to the sights sits inside
         * this band as well, and taking one as the hip is what left the
         * sights keeping a fov that was already narrowed: measured 0.76 rad
         * held against a hip of 0.81. */
        quiet = fabsf(eng - g_engPrev) < 0.002f;
        if (eng >= SIGHT_HI && eng < 1.2f && quiet) g_hipRad = eng;

        /* Held in the sights' band, or only passing through it? Passing is
         * what a magnified optic's pull does on its way to 0.49 and below.
         * Time is what tells them apart, not a per-tick delta: a held aim
         * sways by a hair from tick to tick, which the delta test read as
         * movement - so the hold never engaged, the plugin kept following the
         * engine, and No zoom did nothing (reported 2026-09-21). A run that
         * stays within HOLD_CALM for HOLD_MS is a hold; a pull leaves that
         * range within a tick or two, which restarts the run. */
        if (eng >= OPTIC_RAD && eng < SIGHT_HI &&
            fabsf(eng - holdRef) <= HOLD_CALM) {
            if (!holdAt) holdAt = GetTickCount64();
            hold = (GetTickCount64() - holdAt) >= HOLD_MS;
        } else {
            holdRef = eng;
            holdAt = 0;
            hold = 0;
        }
        g_engPrev = eng;

        /* An aim, from the frame the engine starts pulling to the one it has
         * let go of. The value alone says it, which keeps the feature
         * independent of an aim state a plugin cannot depend on reading - and
         * it is kept apart from the switch too, because the frame with No
         * zoom off still has to know an aim is up: that is the frame the
         * engine's own value is left alone on.
         *
         * Entering needs the value to be HELD in the band, not merely to be
         * passing through it. A magnified optic's pull sweeps this same range
         * on its way to 0.49 and below (reported 2026-09-21: "why does it
         * affect scopes?"): with the band test alone the plugin took the hold
         * over halfway through the optic's pull, walked the fov back towards
         * the hip, and then let go under the framework's 0.5 line - a jump in
         * both directions that the optic's own zoom had never had. Iron
         * sights are held inside the band (0.69) and an optic is never held
         * in it, so the hold is what tells them apart from the value alone.
         *
         * Leaving keeps the wide edge it always had: with the sights up, the
         * value walks back up through the band, and handing the channel over
         * at the first moving frame would land the camera on the engine's
         * narrow end and jump from there.
         *
         * Never an optic: under the framework's line the camera keeps the
         * engine's value and a scope's magnification is not ours to take -
         * the hold test is what makes that true.
         */
        {
            int was = InterlockedCompareExchange(&g_aim, 0, 0) != 0;

            aim = eng < (was ? SIGHT_HI + 0.06f : SIGHT_HI) &&
                  eng >= OPTIC_RAD &&
                  (was || hold);
        }
        InterlockedExchange(&g_aim, aim);

        if (!allowed || !in) {
            /* Blocked, or off the camera: the menu cannot switch it off
             * from inside a blocked mode, so the hold is let go here and
             * taken again once the mode allows it. */
            if (held) { ShCameraReleaseFields(SH_CAM_FOV); held = 0; }
            ShFovPin(0);
        /* Both switches decide from here, and neither one is the whole of
         * it: with the sights up it is No zoom that says whether the view
         * is held, and with them down it is the override that says whether
         * the view is ours at all. A frame that wants neither falls to the
         * release below, which is what leaves the engine's own value - the
         * game's small iron sight narrowing, and every magnified optic and
         * the binoculars, whose values are under 0.5 rad and pass through
         * unless the pin is set.
         */
        /* Applied while the override holds the view - and while No zoom holds
         * the sights, which it does on its own, with the override off. With
         * the override on and No zoom off an aim is applied to as well: what
         * it is walked to there is the engine's own value (WantedRad), and
         * releasing instead is what made that hand over a snap.
         */
        } else if (OverrideOn() || (aim && nz)) {
            ShCameraOverride o;
            float want = WantedRad();
            /* The engine's own value while it is travelling through the
             * optic range: a magnified optic's pull walks 0.81 down to 0.49,
             * and the framework hands the channel back to the engine at its
             * 0.5 line - so a value of ours parked above that line turns the
             * handover into a step down (reported 2026-09-21: correct with
             * the override off, wrong with it on). Following the engine
             * while it moves makes that line a continuation instead - our
             * value arrives at 0.5 from above, the engine's carries on from
             * below - and the value the override asked for comes back the
             * moment the engine's value is held (see the aim test: a hold,
             * not a still tick - an aim that has arrived still sways a
             * little, and reading that as travel kept the hold from ever
             * engaging).
             */
            /* ... and only close to the framework's line, which is the one
             * place a step can land: an iron sight lives at 0.69 and never
             * goes near 0.5, so following it there buys nothing and costs
             * the thing No zoom is for - the pull stays visible as a dip and
             * a return (reported 2026-09-21: "the view shrinks, then comes
             * back"). With No zoom off there is nothing to hide, so the
             * whole range is followed and the game's own pull shows through.
             */
            int nearLine = eng < OPTIC_RAD + OPTIC_MARGIN;

            int follow = !hold && eng > 0.05f && eng < SIGHT_HI &&
                         (nearLine || !nz);

            if (follow) {
                /* Ramped, not copied: entering the follow at its own top
                 * would drop the camera onto the engine's value in a single
                 * frame - the step this exists to remove, moved up from the
                 * framework's line. What the ramp costs at the line itself
                 * is a fraction of a degree. */
                if (g_shown > 0.05f && g_shown < 3.0f)
                    g_shown += (eng - g_shown) * RAMP_STEP;
                else
                    g_shown = eng;
            } else {
                /* Walked towards the target, never snapped to it, and seeded
                 * from what the camera carries when nothing of ours is on
                 * the channel yet - so a switch just turned on walks up from
                 * the view on screen instead of jumping to its value. A hold
                 * has nothing to walk: its target is already on screen.
                 */
                if (g_shown <= 0.05f && g_camRad > 0.05f && g_camRad < 3.0f)
                    g_shown = g_camRad;
                if (g_shown > 0.05f && g_shown < 3.0f &&
                    fabsf(want - g_shown) > RAMP_MIN)
                    g_shown += (want - g_shown) * RAMP_STEP;
                else
                    g_shown = want;
            }

            memset(&o, 0, sizeof(o));
            o.apply = SH_CAM_FOV;
            o.fov = g_shown;
            if (ShCameraApply(&o)) {
                held = 1;
                /* The sights' value is inside the gameplay range, so the
                 * engine would take the override on its own; the pin is
                 * what holds it there whatever a frame computes. */
                ShFovPin(aim);
                /* What a frame with no sights up carries - the value No
                 * zoom hands back to them (g_lookRad). */
                /* Only a plain hip is recorded here. A frame that is
                 * following the engine mid-pull carries the optic's own
                 * narrowing value, and taking that as "what the hip had" is
                 * what made No zoom hold the sight's own fov - the feature
                 * doing exactly nothing (measured 2026-09-21: want=0.688,
                 * the engine's own aim value, with the hold engaged). */
                if (!aim && !follow && eng >= SIGHT_HI) g_lookRad = g_shown;
            } else {
                held = 0;
                ShFovPin(0);
            }
        } else {
            /* Nothing of ours is on the channel, so the engine's own fov is
             * what a frame with no sights up carries. Recording it is what
             * keeps the no-zoom hold right when the switch is turned on
             * before the override is. */
            /* A plain hip only, for the reason at the recording above: this
             * is what No zoom hands back when the sights come up. */
            if (!aim && eng >= SIGHT_HI && eng < 3.0f) g_lookRad = eng;
            if (held) { ShCameraReleaseFields(SH_CAM_FOV); held = 0; }
            ShFovPin(0);
            g_shown = 0.0f;
        }

        /* The decision, on every change and once a second while it holds. */
        DiagOpen();
        {
            static int lastAim = -1, lastHold = -1;
            static DWORD lastAt;
            DWORD now = GetTickCount();

            if (aim != lastAim || hold != lastHold ||
                (DWORD)(now - lastAt) >= 1000) {
                lastAt = now;
                lastAim = aim;
                lastHold = hold;
                DiagState(eng, g_camRad, g_shown, WantedRad(), aim, hold,
                          held, nz, OverrideOn());
            }
        }

        /* One write per burst of changes, never one per slider notch. */
        if (InterlockedExchange(&g_iniDirty, 0)) SaveIni();

        /* The status line is only refreshed while somebody can read it. */
        if (ShMenuIsOpen()) Report();
        /* Fast while the sights are up, while a change is still being
         * walked, and while the override is on as well: which fov is in
         * force follows the view, and a change has to land on the next beat
         * rather than half a second after the camera moved.
         */
        Sleep((g_shown > 0.05f && g_shown < 3.0f &&
               fabsf(WantedRad() - g_shown) > RAMP_MIN)
                  ? RAMP_MS
                  : (nz ? NOZOOM_MS : (OverrideOn() ? VIEW_MS : TICK_MS)));
    }

    /* Unloading: an override left pushed would be one with no owner left to
     * release it. */
    if (held) ShCameraReleaseFields(SH_CAM_FOV);
    ShFovPin(0);
    return 0;
}

/* All initialization is here, not in DllMain: that runs under the loader
 * lock, where taking the framework's own locks and allocating is what the
 * plugin contract forbids. */
static DWORD WINAPI InitThread(LPVOID p) {
    (void)p;
    g_fpDeg = FALLBACK * RAD2DEG;
    g_tpDeg = g_fpDeg;
    ResolveIniPath(g_inst);
    LoadIni();
    FovText();
    g_menu = ShMenuCreate("@fov.page");
    if (!g_menu) return 0;          /* nothing to drive without a page */
    ShMenuToggle(g_menu, "@fov.override", g_initOverride, OnToggle, NULL);
    ShMenuNumber(g_menu, "@fov.tp", g_tpDeg, DEG_MIN, DEG_MAX,
                 DEG_STEP, OnTp, NULL);
    ShMenuNumber(g_menu, "@fov.fp", g_fpDeg, DEG_MIN, DEG_MAX,
                 DEG_STEP, OnFp, NULL);
    ShMenuToggle(g_menu, "@fov.nozoom", g_initNozoom, OnNoZoom, NULL);
    ShMenuHint(g_menu, "@fov.hint");
    ShMenuAction(g_menu, "@fov.reset", OnReset, NULL);
    /* The rows above only set the menu's copy; the hold itself is taken
     * here. A push before the camera is up simply fails and the tick takes
     * it again once a session is running, so the ini's wish is not lost. */
    if (g_initOverride) {
        InterlockedExchange(&g_on, 1);
        Push();
    }
    if (g_initNozoom) InterlockedExchange(&g_nozoom, 1);
    Report();
    /* A view override is not something a PvP match wants; saying it out
     * loud is what makes the release in the tick deliberate rather than an
     * accident of the default. */
    /* Both calls are checked, and the status line is how this plugin has
     * always reported a refusal (it keeps no log of its own). A refused
     * blacklist means OnBlocked never runs, and that callback is the one
     * thing that gives the FOV back when a PvP match starts. */
    if (!ShPluginBlacklist(SH_MODE_BLACKLIST_GHOST_WAR |
                           SH_MODE_BLACKLIST_MERCENARIES))
        ShMenuStatus(g_menu, "fov: blacklist declaration refused");
    if (!ShPluginOnBlocked(OnBlocked, NULL))
        ShMenuStatus(g_menu, "fov: on-blocked registration refused");
    {
        HANDLE h = CreateThread(NULL, 0, TickThread, NULL, 0, NULL);

        if (h) CloseHandle(h);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = inst;
        DisableThreadLibraryCalls(inst);
        {
            HANDLE h = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);

            if (h) CloseHandle(h);   /* never waited on */
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&g_stop, 1);
    }
    return TRUE;
}
