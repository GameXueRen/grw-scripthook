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
/* The tick a fov transition is measured in. MEASURE 2026-10-01: the walk
 * itself is no longer a per-tick fraction of the distance - see RampOnce, which
 * spreads a change over a fixed number of these ticks with a smoothstep on it.
 * RAMP_STEP (an exponential 35 % a tick) lived here and is what made a wide
 * override read as "no transition": it is removed rather than tuned, because
 * the shape was wrong, not the rate. */
#define RAMP_MS     16
/* rad: close enough to call a fov settled, for the paths that only compare. */
#define RAMP_MIN    0.002f
/* While the override is on, which fov is in force depends on the view,
 * so a switch between first and third person has to land at once rather
 * than on the next slow tick. The same beat reads the view mode, and a
 * poll that often is also what keeps the head measurement it is derived
 * from alive.
 */
#define VIEW_MS     250
/* How long after an aim the view is left alone. A reading inside this window
 * is the aim's own settling, not a change of view.
 *
 * The framework used to report first person for the engine's own aim camera -
 * which sits on the head bone - for a beat after the aim came down, and this
 * window was measured against that hand over. The structure has since
 * changed: the frame is never handed to the engine's aim camera, and the
 * framework no longer takes its POSITION either (the eye is the only position
 * source - see g_aimArm in scripthook_fpx.c), so the view is first person
 * throughout an aim and after it either way. The constant and the wait are
 * kept as they are: a reading taken inside an aim is still the aim, not a
 * view change, and the window is what keeps the fov from being re-read
 * against a camera that is still settling. */
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
/* How far above the framework's 0.5 line the engine's value is followed, on
 * an aim with No zoom off: only a magnified optic's pull reaches down here,
 * and it is the one place the framework's own handover can land a step. */
#define OPTIC_MARGIN 0.10f
/* MEASURE 2026-10-01: HOLD_CALM and HOLD_MS lived here. They timed the
 * engine's own fov to decide whether the sights were up, because a plugin had
 * no dependable aim state to read. It has one now - the engine's own byte,
 * through ShFp2Gate - so the wait is gone, and with it the 120 ms of the pull
 * that used to happen before No zoom took hold. The detector's full reasoning
 * is in the note at GateAds. */

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
/* The fov No zoom holds, frozen at the frame an aim starts. See where it is
 * written in TickThread: WantedRad() cannot serve this, because while an aim
 * is up it returns the engine's own (narrowed) value. */
static volatile float g_holdRad = 0.0f;
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
/* For the diagnostic only: the latched aim, published so the log can show it
 * beside `aim`. See the note in DiagState for why the log kept telling us the
 * wrong story without it. */
static volatile int   g_aimHeldDbg = 0;
/* Which of the three value paths the last tick took: 1 following the engine,
 * 2 holding a no-zoom target, 3 settling on a view's fov. Published for the
 * diagnostic, so a log can say which branch produced a number rather than
 * leaving it to be inferred from the number. */
static volatile int   g_pathDbg = 0;

/* The framework's live gate bytes, resolved by name because a dinput8 older
 * than this plugin will not have them; see GateAds. `ads` is the byte the
 * ENGINE writes at its own two aim sites - the edge itself, not a reading of
 * the fov - which is what this plugin needs to tell an iron sight from a
 * magnified optic without waiting to see which one settles.
 */
typedef void (*Fp2Gate_t)(int *menu, int *drone, int *ads, int *fresh);
static Fp2Gate_t g_gate;

/* 1 while an aim is up, from the engine's own byte.
 *
 * This replaced a time-based hold detector that watched the fov: the value
 * had to sit inside [0.50, 0.75) within HOLD_CALM of where that run started
 * for HOLD_MS before an aim was believed. Two things were wrong with it, both
 * reported 2026-10-01.
 *
 * It is LATE. 120 ms of the engine's own pull happens before the hold is
 * believed, and in that window No zoom was not yet holding - so the camera
 * followed the engine down and was walked back when the hold engaged. That is
 * the "it still zooms and then immediately returns" the field reported, and
 * it is the detector's own latency showing on screen.
 *
 * And it is LATE ON PURPOSE, which is why it cannot simply be shortened: it
 * existed to keep a magnified optic's pull through the same band from being
 * taken for an aim (reported 2026-09-21: "why does it affect scopes?"), and
 * only time told the two apart.
 *
 * The engine's byte has neither problem. It is the edge, so nothing is late,
 * and it is 1 only for an aim and never for an optic's pull, so there is
 * nothing to tell apart. A framework without the gate answers 0 and the fov
 * test below is used instead - the old behaviour, minus the claim that it is
 * the right one.
 */
static int GateAds(void) {
    int ads = 0;

    if (!g_gate) {
        /* No gate: fall back to the fov, and be honest in the log about it. */
        static int said;
        float e = ShFovEngine();

        if (!said) { said = 1; Log("fov: no ShFp2Gate - aim from the fov"); }
        return e > OPTIC_RAD && e < SIGHT_HI;
    }
    g_gate(NULL, NULL, &ads, NULL);
    return ads ? 1 : 0;
}

static int OverrideOn(void) {
    return InterlockedCompareExchange(&g_on, 0, 0) ? 1 : 0;
}

/* How long the tick should wait before the next step of a running sweep. Set by
 * RampOnce and read by the tick's cadence, so the sweep advances by real elapsed
 * time whatever beat the loop happens to be on - which is what makes the travel
 * the same length at 250 ms a tick as at 16 ms, and it is why this is not a
 * per-tick fraction (audited 2026-10-01: the old fraction meant 2.5 s of
 * transition at the slow beat against an engine whose own travel is 0.4 s). */
static volatile DWORD g_rampDue = 60;
/* The batch a sweep is divided into: it wants its next step at this delay, and
 * the tick will not sleep longer than this while the batch stands. */
#define RAMP_BATCH_MS 60u

static void RampDue(DWORD ms) {
    if (ms < 1u) ms = 1u;
    if (ms > RAMP_BATCH_MS) ms = RAMP_BATCH_MS;
    g_rampDue = ms;
}

/* Whether an aim byte could be about to arrive. Used only for the tick's
 * cadence - see the note at the sleep. */
static int CouldBeAiming(float eng) {
    return (eng >= 0.05f && eng < 1.2f) ? 1 : 0;
}

/* A fov transition of a fixed length, eased at both ends.
 * MEASURE 2026-10-01, and the whole of the "the raise and lower is a step"
 * report. The walk used to be `g_shown += (goal - g_shown) * RAMP_STEP` - an
 * exponential approach at 35 % a tick. That is not a transition curve: the
 * first tick takes a third of the distance, the second takes a third of what
 * is left, and by the third the picture has covered 73 % of the travel. Over
 * the ~0.3 rad an unmodified game moves, that read as smooth enough. Over the
 * 0.7 rad an override at 80 degrees moves it read as a single step - and that
 * is exactly the split the field reported: cases 1 and 2 (override off, the
 * engine's own narrow travel) smooth, cases 3 and 4 (override on, the wide
 * travel) "no transition". The curve was the same in all four; only the
 * distance differed, and an exponential only looks like a transition while
 * the distance is short.
 *
 * So the travel is spread over a fixed number of ticks with a smoothstep on
 * it: slow at the start, fast through the middle, slow into the target. The
 * length is the engine's own ADS transition, so the fov arrives with the
 * weapon rather than before or after it.
 *
 * The goal is sampled when it CHANGES - not every tick. An aim's own travel
 * moves it every tick, and re-sampling each one would either restart the
 * sweep forever or leave `from` chasing the value it is sweeping towards, so
 * the travel would collapse. One aim, one sweep: when the goal moves by more
 * than a hair, the sweep restarts from what the camera carries and runs to
 * the new goal; while it holds, the picture settles and stays.
 *
 * TWO LENGTHS, because there are two jobs. A value of OURS settling on a
 * target of OURS has to be long enough to read as a transition. A value
 * tracking one the ENGINE is moving has to keep up with that movement - and
 * "keep up" means the same order as the movement itself, not as fast as
 * possible. MEASURE 2026-10-01, in two steps, because the first fix for this
 * was wrong in the other direction:
 *
 *   - chasing the engine at the settling length left our value far above the
 *     engine's when the framework's 0.5 handover took the channel back, so
 *     the handover landed as a step. "The scope has no transition."
 *   - chasing it fast enough to guarantee arrival before that handover made
 *     the whole travel finish in about 60 ms, which is not a transition
 *     either: nobody can see it. "Still far too fast, no visible transition
 *     at all."
 *
 * The engine's own ADS travel runs about 400 ms, which is the thing being
 * watched - so that is the length, and the handover is met by being close
 * rather than by being early. The sweep's length is a duration, not a race.
 */
#define RAMP_MS_STEPS 28      /* settle: 28 x 16 ms, the engine's own raise */

/* How long the channel stays ours after an aim ends, so the lowering sweep
 * finishes under our own pin. Must exceed the sweep (RAMP_MS_STEPS x RAMP_MS
 * = about 450 ms) with room for a slow tick or two. See the ownership note in
 * TickThread - releasing on the aim-end frame is what made the raise animate
 * and the lower jump. */
#define SETTLE_MS     900u

/* How far what we are about to write may sit from the engine's own value before
 * we pin it. Under this the engine's value passes the framework's own test and
 * ours is either the same value or a jitter away from it, so pinning would only
 * suppress a camera the engine is legitimately driving. See the note at the pin
 * in TickThread - this one number is what separates "our transition must land"
 * from "let the engine's magnification through". */
#define PIN_GAP       0.02f

/* One fixed-length sweep, started by a caller that knows the moment the value
 * has to move. MEASURE 2026-10-01, and the fix for the last piece: the
 * engine's own fov does not ramp at all. Measured on the 2026-10-01 build, the
 * ADS edge and the engine's value:
 *
 *     22:09:25.417  ads: 0 -> 1
 *     22:09:25.431  fov=0.4916      <- 14 ms later, already at the sight's value
 *
 * It is a STEP. Every round of this before now tried to FOLLOW that value -
 * at various rates, with a sweep, with an exponential - and following a step
 * is a step: the override faithfully reproduced the shape it was told to
 * track, which is exactly what "there is no transition" looked like. The
 * animation the unmodified game shows in first person is the weapon model
 * coming up, not the fov.
 *
 * So the transition has to be OURS: when the aim starts, take the value the
 * camera carries and sweep it to the engine's over RAMP_MS_STEPS ticks, once,
 * with an end point sampled at that moment. It does not track anything after
 * that, so a step in the engine cannot become a step on screen.
 */
static float RampOnce(float to, int cancel) {
    static float from = 0.0f, at = 0.0f, lastGoal = 0.0f;
    static uint64_t startAt = 0;
    static int have = 0;
    float u;
    uint64_t now = GetTickCount64();

    if (cancel) { have = 0; lastGoal = 0.0f; return to; }
    if (!(to > 0.05f && to < 3.0f)) return to;

    /* A sweep is restarted by the caller's cancel, AND by its goal changing.
     *
     * MEASURE 2026-10-01: the cancel alone is not enough, and this is finding 4
     * of the audit. In steady state the lowering branch re-arms a fresh sweep
     * as soon as the previous one finishes, so `have` is 1 on nearly every tick
     * with `at` frozen - and a view switch, a slider notch or Reset all change
     * `to` while the running sweep keeps writing the OLD goal until its clock
     * expires. The change then took another full sweep to arrive, so a
     * first/third person switch showed nothing for up to 448 ms and then eased
     * over another 448. Comparing the goal is what the deleted FovEase did
     * wrong (it used a static goal and a shared clock); done here, next to the
     * clock it restarts, it is safe: a changed goal restarts everything
     * together, so there is no state to go stale. */
    if (!have || fabsf(to - lastGoal) > 0.0005f) {
        from = (g_shown > 0.05f && g_shown < 3.0f) ? g_shown : to;
        at = to;
        lastGoal = to;
        startAt = now;
        have = 1;
    }

    u = (float)(now - startAt) / (float)(RAMP_MS_STEPS * RAMP_MS);
    if (u >= 1.0f) { have = 0; from = at; return at; }
    if (u <= 0.0f) u = 0.0f;
    /* Publish the time this sweep has left, so the tick can sleep until its next
     * step rather than to a fixed beat. A sweep has RAMP_MS_STEPS steps; asking
     * for the remainder divided by the steps left keeps every step the same
     * length whatever cadence the rest of the loop is running at. */
    {
        DWORD left = (DWORD)(startAt + (uint64_t)(RAMP_MS_STEPS * RAMP_MS) - now);
        DWORD stepsLeft = (DWORD)((1.0f - u) * (float)RAMP_MS_STEPS) + 1u;

        RampDue(left / stepsLeft);
    }
    return from + (at - from) * (u * u * (3.0f - 2.0f * u));   /* smoothstep */
}

/* FovEase lived here, and is deleted rather than kept for the paths that no
 * longer use it. It was the source of two of this file's bugs, both from the
 * same design choice: it inferred when a transition started by comparing the
 * goal against a static lastGoal, and a goal that repeated an earlier value -
 * the view's fov on the way down is the same number a hip settle used a moment
 * before - made it reuse an ancient clock and jump straight to the target.
 * RampOnce replaces it everywhere and is explicit: a transition is started by
 * a caller that knows the edge, not guessed. One primitive, no inference. */

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
    float v;

    if (InterlockedCompareExchange(&g_learned, 0, 0)) return;
    if (!ShIsInGame()) return;

    /* FINDING 10 of the 2026-10-01 audit: the override check that used to be
     * here made a restored session (override=1 in the ini, which sets g_on
     * before the tick has ever run) unable to learn the game's default at all -
     * DefaultRad() stayed on the compiled-in fallback for the whole session, so
     * the status line's "game default" and the "Back to the game default" row
     * both reported a constant instead of the game's value.
     *
     * The engine's own value is the right source while the override is on: it
     * is the fov the game computed, read before our replacement, so it is the
     * default by definition. With the override off the camera carries the same
     * thing, and reading it there keeps the old behaviour (and keeps working on
     * a framework too old for ShFovEngine). */
    v = ShFovEngine();
    if (!(v > 0.05f && v < 3.0f)) {
        if (!ShGetCamera(&c)) return;
        v = c.fov;
    }
    if (v > 0.05f && v < 3.0f) {
        g_defaultRad = v;
        if (g_fpDeg <= 0.0f) g_fpDeg = v * RAD2DEG;
        if (g_tpDeg <= 0.0f) g_tpDeg = v * RAD2DEG;
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
        /* Hand the fov back now rather than waiting up to a tick - the menu
         * cannot see the tick's `held`, and a channel held half a second after
         * the switch went off is a value nobody owns.
         *
         * The pin goes with it ONLY when No zoom is off too: that feature is
         * the other thing that holds this channel, and it holds it with the
         * pin, so clearing the pin under it would leave the sights narrowing
         * again. Releasing the channel while leaving the pin set has its own
         * failure - our value pinned with the engine supposed to be in charge -
         * so the two are decided together, from the same state the tick reads. */
        if (InterlockedCompareExchange(&g_nozoom, 0, 0)) {
            /* No zoom still owns the channel; leave both alone. */
        } else {
            ShFovPin(0);
            ShCameraReleaseFields(SH_CAM_FOV);
        }
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
        /* FINDING 9 of the 2026-10-01 audit: the pin belongs to whichever
         * feature is holding the channel, so it may only be cleared when the
         * override is off too. Clearing it under a still-on override left a
         * window - up to VIEW_MS, since the tick re-pins on its next pass - in
         * which we held the channel with the pin down, and any engine value
         * under 0.5 (a scope, a vehicle zoom) showed through ours. */
        if (!OverrideOn()) {
            ShFovPin(0);
            ShCameraReleaseFields(SH_CAM_FOV);
        }
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

/* The main line, on a change and once a second while it holds.
 *
 * MEASURE 2026-10-01: `shown` here must be the value the CAMERA was given, not
 * the sweep's own variable. An earlier revision zeroed that variable on release
 * and this printed it, so the log reported 0.000 through the very release the
 * exercise was about - three rounds of reading a cleared variable, which is how
 * the transition got argued about from the wrong numbers. The variable is no
 * longer zeroed (see the release branch), but the rule stands: print what went
 * to the camera, and print the inputs the decision was made from.
 *
 * `path` is which of the value paths ran - 1 follow, 2 hold, 3 settle on the
 * view's fov, 4 an aim with nothing of ours to say (the engine's own value
 * written through, which is what a magnified optic's aim under the override
 * is) - so a line says which branch produced the number instead of leaving it
 * to be inferred from the number. The audit of 2026-10-01 (finding 11) pointed
 * out that the previous parameter was named `hold` but never printed, and that
 * the per-tick line's parameter was named `follow` while receiving the path
 * code. */
static void DiagState(float eng, float cam, float shown, float want,
                      int aim, int path, int held, int nz, int ovr) {
    int gateAds = 0;

    if (g_gate) g_gate(NULL, NULL, &gateAds, NULL);
    if (g_diagOn != 1) return;
    Log("eng=%.3f cam=%.3f wrote=%.3f want=%.3f aim=%d latched=%d look=%.3f "
        "hold=%.3f path=%d held=%d nz=%d ovr=%d gate=%d hasgate=%d",
        eng, cam, shown, want, aim, g_aimHeldDbg, g_lookRad,
        g_holdRad, path, held, nz, ovr, gateAds, g_gate ? 1 : 0);
}

/* Entering a session reinstalls the camera hook, so the
 * override is pushed again to survive the transition.
 */
static DWORD WINAPI TickThread(LPVOID p) {
    int held = 0;      /* the fov channel is ours right now */
    DWORD viewAt = 0;  /* when the view mode was last read */
    DWORD aimAt = 0;   /* when an aim was last up, of any kind */
    /* The aim, latched for its whole duration. See the note at the aim test:
     * this is the engine's own byte, held across the stretch where a magnified
     * optic's value is already under the framework's 0.5 line and the fov
     * alone would say "not an aim". */
    int aimHeld = 0;
    /* The same value one tick ago, so the rising edge can be seen. A sweep has
     * to start there - see the note at the follow below. */
    int aimHeldPrev = 0;
    /* How long the channel stays ours after an aim ends. The lowering sweep
     * runs under our own pin - releasing on the first frame of it is what made
     * the raise animate and the lower jump. MEASURE 2026-10-01: the aim-end
     * frame logged path=3 (our settle) with wrote=1.396 while cam went
     * 0.492 -> 1.396 in the same tick, i.e. the engine's step landed on the
     * frame we let go. See the note at the lowering branch. */
    uint64_t aimEndAt = 0;
    /* Which sight the current aim is: 1 an iron sight, 0 a magnified optic, and
     * 0 with no aim up. Latched for the whole aim - see the note at the value
     * decision, where a per-frame test is shown to be wrong. */
    int iron = 0;
    /* The engine's aim byte as read this tick, for the sight latch. See there. */
    int gateUp = 0;
    /* The delay a running sweep asked for before its next step, read at the top
     * of the tick and written by RampOnce during the value decision. A sweep is
     * the one thing here that knows the time it still needs, so it sets the
     * beat; everything else only sets an upper bound. The first tick is
     * immediate, via the initial value. */
    (void)p;

    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        DWORD rampDue = g_rampDue;
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
        /* FINDING 8 of the 2026-10-01 audit: this was never assigned anywhere,
         * so `quiet` compared against a permanent 0.0f and was false on every
         * frame - meaning g_hipRad was never learned at all and both of its
         * fallbacks silently degraded to DefaultRad()/g_shown. The comparison
         * above is against the PREVIOUS frame, so the assignment belongs here. */
        g_engPrev = eng;

        /* An aim, from the frame the engine's own byte says one is up - the
         * edge itself, with no detector and nothing to wait for. See GateAds
         * for what the time-based hold this replaced cost: 120 ms of the pull
         * was read as "not an aim yet", so No zoom arrived after the zoom it
         * exists to prevent.
         *
         * An optic's pull is no longer a risk at all - the byte is set by the
         * engine's own aim sites on entering an aim, and a magnified optic
         * sets it exactly like an iron sight does. What the byte cannot say
         * is which kind of optic it is, and that is the fov's job, kept
         * below: under the framework's 0.5 line the camera keeps the engine's
         * value and a scope's magnification is not ours to take.
         *
         * MEASURE 2026-10-01: `aim` must NOT be the aim state. It was
         * `GateAds() && eng >= OPTIC_RAD`, which folds two different questions
         * into one - is an aim up, and is the engine's value still in the iron
         * sight band. A magnified optic's value falls UNDER the line as its
         * pull runs, so the aim state went false mid-aim, the plugin released
         * the channel on the frame the fov crossed 0.5, and the camera stepped
         * on to whatever the engine had by then. That is the "the scope has no
         * transition" report, and it also killed No zoom whenever an iron
         * sight's value drifted under the line - both of them the same error.
         *
         * So: `inAim` is the aim, latched for its whole duration from the
         * engine's own byte; `aim` stays what the no-zoom hold and the fov
         * reporting need it to be (an aim whose value is in the sight band).
         */
        {
            int gate = GateAds();

            /* Kept for the whole tick: the sight latch below needs to know
             * whether the engine is still pulling, and that is this byte. */
            gateUp = gate;

            if (gate) aimHeld = 1;
            else if (eng >= SIGHT_HI) aimHeld = 0;   /* the byte cleared */

            /* The aim just ended: start the window in which the lowering
             * sweep still owns the channel. */
            if (aimHeldPrev && !aimHeld) aimEndAt = GetTickCount64();

            /* MEASURE 2026-10-01: freeze the fov the frame had BEFORE the aim,
             * at the moment the aim starts, and hold THAT. No zoom captured
             * its target through WantedRad, which returns the ENGINE's value
             * while an aim is up - so the feature locked in the very number it
             * exists to hide, and the sights narrowed exactly as they would
             * have with it off. Measured on the 2026-10-01 logs: with No zoom
             * on, `want` tracked the engine's pull down to 0.688 instead of
             * staying at the 1.396 the frame carried before the aim.
             *
             * The value to hold is g_lookRad - what a plain hip frame carried,
             * recorded on the frames with no aim up - sampled once, here. */
            if (aimHeld && !aimHeldPrev) {
                float hip = g_lookRad;

                /* g_lookRad is what the hip frame carried, and it is preferred
                 * because it is recorded on frames with no aim up. The fallbacks
                 * are for the first aim of a session, before any hip frame has
                 * been recorded: the engine's hip first, then whatever is on
                 * screen. Preferring g_shown over g_hipRad here would sample a
                 * sweep's moving value on a quick re-aim, which is the failure
                 * the recording above exists to avoid. */
                if (!(hip > 0.05f && hip < 3.0f)) hip = g_hipRad;
                if (!(hip > 0.05f && hip < 3.0f)) hip = eng;
                if (!(hip > 0.05f && hip < 3.0f)) hip = g_shown;
                if (hip > 0.05f && hip < 3.0f) g_holdRad = hip;
            }

            aim = aimHeld && eng >= OPTIC_RAD;
            g_aimHeldDbg = aimHeld;
        }
        InterlockedExchange(&g_aim, aim);

        if (!allowed || !in) {
            /* Blocked, or off the camera: the menu cannot switch it off
             * from inside a blocked mode, so the hold is let go here and
             * taken again once the mode allows it. */
            if (held) { ShCameraReleaseFields(SH_CAM_FOV); held = 0; }
            ShFovPin(0);
        /* WHO HOLDS THE CHANNEL, and what to write while holding it.
         *
         * MEASURE 2026-10-01, and the structural error behind the whole
         * exercise: this used to hold the channel only on frames whose aim
         * test happened to pass, so who owned the camera fov flipped with a
         * signal that flips. Every flip is a handover - we let go, the engine
         * puts its own value on the camera, we take it back - and a handover
         * on a frame nobody chose reads as the picture cutting. The aim byte
         * is a real signal, but it goes up and down inside a single aim and
         * across the 0.5 line, and ownership must not be wired to anything
         * that does.
         *
         * So ownership follows the SWITCH:
         *
         *   the override is on  -> the channel is ours for as long as it is
         *                          on, aim or no aim. What changes inside an
         *                          aim is only the VALUE we write.
         *   No zoom is on       -> ours while the aim lasts, because that is
         *                          the feature: the fov must not move.
         *   neither             -> released, and the engine's own value
         *                          stands, which is the unmodified game.
         *
         * The value written is then one of three, and none of them is a
         * handover:
         *
         *   FOLLOW  - an aim with No zoom off: move with the engine's own
         *             travel. Our value has to become the engine's before the
         *             framework's 0.5 line is reached, so this is tracking,
         *             and tracking is short.
         *   HOLD    - an aim with No zoom on: stay on the fov the frame had
         *             before the aim came up. That is the whole feature.
         *   SETTLE  - no aim: ease onto the view's own value, which is the
         *             transition a switch or a slider should have.
         */
        /* WHO HOLDS THE CHANNEL, and what to write while holding it.
         *
         * MEASURE 2026-10-01, in two parts.
         *
         * Ownership follows the SWITCH, not the aim: an aim byte goes up and
         * down inside a single aim, and ownership wired to it flipped the
         * camera back and forth - every flip a handover, every handover a cut.
         *
         * And it outlives the aim by the lowering sweep. Releasing on the frame
         * the aim ENDS is what made the raise animate and the lower jump: that
         * frame's own log is
         *
         *     22:29:21.426  eng=0.757 cam=0.492 wrote=1.396  lat=0 path=3
         *
         * - we wrote 1.396, path 3 is our settle, and cam moved 0.492 -> 1.396
         * inside that one tick, because ShCameraReleaseFields had already
         * cleared the pin and the engine's step landed on it. So the channel is
         * held for SETTLE_MS after an aim goes down, which is longer than a
         * sweep, and the sweep finishes under our own pin.
         *
         * FINDING 2 of the 2026-10-01 audit: that window must NOT open when the
         * override is off. `aimEndAt` is set on every aim end whatever the
         * switches say, so a session that had the override on earlier - or the
         * nozoom-only mode - took the channel for 0.9 s after every aim and
         * forced the SLIDER's fov onto the camera, then snapped back when this
         * branch let go. With the override off there is no value of ours to
         * lower to: the hold's target is the engine's own hip, so releasing on
         * the aim-end frame is correct and invisible.
         */
        } else if (OverrideOn() || (aim && nz) ||
                   (OverrideOn() && GetTickCount64() - aimEndAt < SETTLE_MS)) {
            ShCameraOverride o;
            float want = WantedRad();
            /* Whether this frame's value is OURS and has to be defended, as
             * opposed to the engine's own value passed through. Decided at the
             * pin below, from how far what we are about to write is from what
             * the engine itself has - see the long note there.
             *
             * MEASURE 2026-10-01, and the regression that produced it: the pin
             * is not only how the engine's value is suppressed, it is also what
             * lets a value UNDER the framework's 0.5 line reach the camera at
             * all. The stub replaces the engine's value only when the channel is
             * enabled AND (pinned OR the engine's own value is >= 0.5). An aim
             * drops the engine's value to 0.49, so an unpinned write of ours is
             * discarded and the engine's step lands instead - which is exactly
             * what the raises showed:
             *
             *     00:30:09.021  eng=0.492 cam=0.492 wrote=1.396   <- our sweep
             *     ...                                                  discarded
             *     00:30:09.557  eng=0.492 cam=0.492 wrote=0.492
             *
             * while the LOWERING worked, because by then the engine's value is
             * back above 0.5 and an unpinned write passes the test on its own.
             * One asymmetry, and it is the whole of "the raise has no
             * transition and the lower does". */
            /* ORDER IS THE FEATURE. No zoom is asked for first, and the follow is
             * what is left over - not the other way round.
             *
             * MEASURE 2026-10-02, the last of the four-case matrix: with the
             * override ON and No zoom ON, an iron sight still zoomed. The cause
             * was this pair of conditions in the other order. `follow` was tested
             * first and its test was `(eng >= SIGHT_HI || !nz)`, so a magnified
             * optic could be followed even with No zoom on (that was the fix for
             * finding 1) - but that also let an IRON SIGHT through whenever the
             * engine's value had not yet reached the hip band, and the iron
             * sight's own pull ends at 0.688, which is UNDER SIGHT_HI. So the
             * frame was "followed" to 0.688 and the sight zoomed, with the
             * switch that exists to prevent it sitting right there.
             *
             * With the hold asked first, the two features cannot overlap: No zoom
             * answers whenever it can (an aim inside the framework's band, which
             * is exactly an iron sight), and the follow covers what it cannot -
             * a magnified optic's pull, which is under OPTIC_RAD and is not ours
             * to take. Both test cases then hold at once, which the four-case
             * matrix demanded and no ordering of `follow` alone could give. */
            /* No zoom answers ONLY for an iron sight, which is the measured
             * distinction below: the engine settles at 0.688 for an iron sight
             * and at 0.492 for every magnified optic, and the second of those is
             * under OPTIC_RAD while the first is over it.
             *
             * `aim` alone was too narrow (it goes false the moment a scope - or
             * an iron sight that drifts - passes under OPTIC_RAD, and the hold
             * then released mid-aim), and `aim || Narrowed(eng)` was too broad
             * (it held every scope at the hip fov, which is a scope that never
             * zooms at all). Both of those were in this file, and both are
             * wrong for the same reason: they asked about the FRAMEWORK's line
             * instead of about which sight is actually up. */
            /* WHICH SIGHT IS UP, and this is a measurement, not a guess.
             *
             * MEASURE 2026-10-02, frame by frame at 60 Hz across a weapon
             * switch inside one session:
             *
             *   iron sight   eng = 0.815 -> 0.714 -> 0.688   (a pull, with travel)
             *   magnified    eng = 0.492                      (a step, within 16 ms)
             *
             * The engine's own settled value is therefore the signal, and
             * OPTIC_RAD (0.5, the framework's pass-through line) is exactly the
             * dividing line: an iron sight comes to rest at 0.688, above it, and
             * every magnified optic at or under it. That line had been used all
             * along as "should we suppress the engine" and never as "which sight
             * is this" - and it is both.
             *
             * The catch, and the reason several versions of this were wrong: the
             * line cannot be tested on the frame the aim byte RISES. At that
             * instant an iron sight is still at 0.714 and a scope has already
             * stepped to 0.492, so it takes one tick before the value means what
             * it says. Both settle inside a single tick, so one frame of patience
             * is the whole cost - and none of it is visible, because an iron
             * sight is held from the first frame either way and a scope has not
             * written anything yet.
             *
             * So: optimistic while an aim is up, narrowed only by a value that
             * stands under the line.
             *
             * Gated on the aim byte still being RAISED (`gateUp`), and that gate
             * is the second half of the same lesson. On the way DOWN the
             * engine's value climbs back through 0.75 before the byte clears -
             * 0.640, 0.683, 0.713, 0.734, 0.758 across the frames either side of
             * one of these releases - so `eng >= OPTIC_RAD` reads as "an iron
             * sight" exactly when the aim is ending, and the latch would flip a
             * scope to iron as it lowered and hold the hip fov there instead of
             * letting go. What that produced was `wrote` and `cam` frozen at
             * 1.396 for the whole lower: a scope whose raise animated and whose
             * lower did not, because the lowering had nothing to travel to. The
             * byte being up is what says the engine is still pulling; once it is
             * down the engine value is on its way back to the hip and is not
             * evidence about the sight at all. */
            if (!aimHeld) iron = 0;
            else if (nz && gateUp) {
                if (eng < OPTIC_RAD) iron = 0;      /* an optic, under the line */
                else                 iron = 1;      /* still an iron sight       */
            }

            /* Order: the hold first, so No zoom answers whenever it can and the
             * follow covers what it cannot. */
            if (nz && aimHeld && iron) {
                /* The hold, and it is exactly that: the frozen pre-aim value,
                 * written as it stands. No easing and no ramp - a value that
                 * does not move is the whole feature, and any smoothing here
                 * would move it. See where g_holdRad is frozen.
                 *
                 * `iron` is latched for the whole aim rather than tested per
                 * frame, and that latch is the point. An iron sight's own pull
                 * passes through 0.714 on its way to 0.688, and a per-frame test
                 * would read that first frame as "not yet narrowed", hand the
                 * aim to the follow sweep, and walk the view from the hip DOWN
                 * to 0.714 before the hold could start - a visible narrowing
                 * that No zoom then had to undo, which is exactly the "it still
                 * zooms" the switch was meant to prevent. The latch holds from
                 * the first frame and only gives way if the sight turns out to
                 * be an optic. */
                float hold = g_holdRad;

                if (!(hold > 0.05f && hold < 3.0f)) hold = want;
                g_shown = hold;
                g_pathDbg = 2;
            } else if (aimHeld && OverrideOn() && eng > 0.05f) {
                /* No zoom is off (or an optic is under the framework's band,
                 * which is not ours to hold): walk to the engine's value in one
                 * sweep per aim.
                 *
                 * MEASURE 2026-10-01: the sweep's state outlives a single aim
                 * unless it is reset, and that is what made the transition
                 * intermittent - exactly two of ten raises had one. The reset
                 * used to happen only when the channel was given back, and
                 * with the override on the channel is held for the whole
                 * session: so `have` stayed 1 and `startAt` was still the
                 * previous aim's, `u` came out past 1 on the first frame of
                 * the next aim, and RampOnce returned the target immediately.
                 * The log shows it plainly - a raise whose first tick already
                 * reads the settled value:
                 *
                 *     22:20:41.909  eng=0.492 cam=1.396 wrote=0.492
                 *
                 * against one that started correctly:
                 *
                 *     22:20:34.603  eng=0.492 cam=1.396 wrote=1.396
                 *     22:20:34.655  eng=0.492 cam=1.384 wrote=1.368
                 *
                 * So the edge, not the release, is what starts a sweep. */
                if (aimHeld && !aimHeldPrev) RampOnce(0.0f, 1);
                g_shown = RampOnce(eng, 0);
                g_pathDbg = 1;
            } else {
                /* Three jobs share this branch, and the branch condition above
                 * is what decides which one is running:
                 *
                 *   an aim under the framework's band with No zoom on
                 *                                 -> the LOWERING sweep, from
                 *                                    the held value back to the
                 *                                    view's, over the same fixed
                 *                                    length as the raise;
                 *   an aim whose engine value is above the band and No zoom on
                 *                                 -> not a hold (it is not
                 *                                    narrowing), so the view's
                 *                                    fov is written as it stands;
                 *   no aim at all                 -> the view's fov, walked.
                 *
                 * MEASURE 2026-10-01, two bugs in a row in the sweep.
                 *
                 * First, this used FovEase, whose reset condition is "the goal
                 * changed" against a static lastGoal. The lowering goal is the
                 * view's fov - the same number a hip settle used a moment
                 * earlier - so the comparison said "no change", the elapsed
                 * clock was the ancient one from before the aim, u came out
                 * past 1, and every lowering after the first returned the
                 * target on its first frame.
                 *
                 * Then, with RampOnce, the raise's sweep state was still
                 * standing: nothing cancelled it on the way down, and a
                 * finished sweep has `have` cleared, so the first lowering call
                 * re-seeded from the aim's own value and returned the target
                 * again. A sweep is per-transition, so BOTH edges have to
                 * restart it - the raise's rising edge, and this falling one.
                 *
                 * Cancelling and restarting here makes the lowering identical
                 * in shape to the raise: from the value on screen, to the
                 * view's fov, over RAMP_MS_STEPS ticks.
                 *
                 * MEASURE 2026-10-02: guarded by `!aimHeld`, and that guard is
                 * what lets a magnified optic keep its zoom while the override is
                 * on. A scope's first frame lands here (the hold has already
                 * given it up, and the follow is not the engine's to give yet),
                 * and this branch writes the VIEW's fov - so without the guard
                 * the scope's own step was replaced by the hip fov for that
                 * frame, pinned, which is a visible flicker at the start of
                 * every scope aim. With an aim up and no value of ours to write,
                 * the engine's own value is simply left alone. */
                if (!aimHeld) {
                    if (aimHeldPrev) {
                        RampOnce(0.0f, 1);
                        (void)RampOnce(want, 0);
                    }
                    g_shown = RampOnce(want, 0);
                    g_pathDbg = 3;
                } else {
                    /* An aim with nothing of ours to say about it: hand the
                     * frame's fov back to the engine by writing what the engine
                     * itself computed, unpinned (the pin test below sees no gap
                     * and leaves it down). */
                    g_shown = eng;
                    g_pathDbg = 4;
                }
            }

            /* At the source, through ShCameraApply - which is where a fov has
             * to be taken, and which already does exactly that: it calls the
             * framework's own source write (scripthook_camera.c, SH_CAM_FOV)
             * and leaves the value in the store the camera manager reads,
             * ahead of the camera build and ahead of culling.
             *
             * MEASURE 2026-10-01: an attempt to call that source write
             * directly (ShFovSet) is what made the last round worse - it is
             * NOT an export, so the plugin's call resolved to nothing and the
             * value never landed at all. ShCameraApply is the exported door to
             * the same write.
             *
             * And it has to be called on EVERY frame we hold, not just the
             * first: the enable bit it sets is cleared by ShFovClear when the
             * channel is given back, and ShFovPin only works while that bit is
             * set - see the stub in scripthook_fov.c, which tests the enable
             * first and the pin second. A pin without the enable is a pin
             * nobody reads.
             *
             * The pin is then what holds OUR value against the engine's own
             * step for as long as the hold is wanted - which is what makes a
             * ramp we computed survive to the screen. */
            memset(&o, 0, sizeof(o));
            o.apply = SH_CAM_FOV;
            o.fov = g_shown;
            if (ShCameraApply(&o)) {
                held = 1;
                /* PIN EXACTLY WHEN WE ARE FIGHTING THE ENGINE'S VALUE.
                 *
                 * That is the whole rule, and it is what the four test cases
                 * forced. The stub replaces the engine's value only when the
                 * channel is enabled AND (pinned OR the engine's own value is
                 * >= 0.5 rad). So:
                 *
                 *   an aim puts the engine at 0.49, under that line, and an
                 *   unpinned write of ours is DISCARDED - the engine's step
                 *   lands and our sweep is thrown away. That is the "the raise
                 *   has no transition" report, and the lowering passed only
                 *   because the engine is back above 0.5 by then;
                 *
                 *   but pinning unconditionally (finding 5's first fix, and the
                 *   version before it) suppresses the engine when we are writing
                 *   the engine's own value - which is what a magnified optic's
                 *   aim does on the follow path, and why a scope stopped zooming
                 *   with the override on.
                 *
                 * Comparing what we are about to write against what the engine
                 * has separates those exactly, with no case list:
                 *
                 *   our sweep travelling (1.396 -> 0.49)  -> far from 0.49 -> pin
                 *   following the engine (0.49 -> 0.49)   -> equal        -> no pin
                 *   No zoom holding the hip (0.815 vs 0.49)-> far apart    -> pin
                 *   a hip frame (0.815 vs 0.815)          -> equal        -> no pin
                 *
                 * which is right in every case and leaves the binoculars, the
                 * vehicle zooms and the drone alone, as the plugin's own header
                 * promises. */
                if (fabsf(g_shown - eng) > PIN_GAP) ShFovPin(1);
                else                                ShFovPin(0);
                /* The hip's fov, recorded on a frame with no aim up, and the
                 * SOURCE follows whoever owns the camera. MEASURE 2026-10-02,
                 * and this is what the whole of case 2 turned on:
                 *
                 *   with the override on, what the frame is showing is OUR
                 *   value, not the engine's - and the engine's hip is 0.815
                 *   while ours is 1.396 at an 80-degree row. Recording `eng`
                 *   here therefore taught No zoom the ENGINE's hip, so the hold
                 *   held 0.815: the view did not zoom during the aim, but it
                 *   had already narrowed from 1.396 to 0.815 as the aim began,
                 *   which is exactly what "it still zooms" looks like. The
                 *   hold was working and holding the wrong number.
                 *
                 *   with the override off there is no value of ours, so the
                 *   engine's is the one to record.
                 *
                 * The `!nz` guard that used to be here was the other half: it
                 * made this record nothing at all whenever No zoom was on, so
                 * g_lookRad stayed 0.000 for the whole of case 2 (the log shows
                 * `look=0.000` on every line) and the freeze fell through to
                 * g_shown - a sweep's moving value - which is how the wrong
                 * number got in. What No zoom is set to is irrelevant to "what
                 * did the hip carry"; what matters is that no aim is up. */
                if (!aim) {
                    float hip = OverrideOn()
                                    ? ((g_shown > 0.05f && g_shown < 3.0f)
                                           ? g_shown : eng)
                                    : eng;

                    if (hip > 0.05f && hip < 3.0f) g_lookRad = hip;
                }
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
            /* Release ONLY what we took. Audited 2026-10-01 (finding 6): this
             * used to clear `held` first and then release unconditionally, on
             * every idle tick - which throws away the one fact needed to guard
             * the call. ShCameraReleaseFields clears the SH_CAM_FOV ownership
             * bit, and the framework's contract is "release only what you took,
             * so releasing one field leaves another plugin's running". chaos.c
             * owns SH_CAM_FOV for its fish-eye and tunnel effects, so an idle
             * fov_changer was killing them within a tick. */
            if (held) {
                ShCameraReleaseFields(SH_CAM_FOV);
                held = 0;
            } else {
                ShFovPin(0);
            }
            /* Keep the bookkeeping honest while the channel is not ours: the
             * engine's value is what the camera carries now, so it is what the
             * next sweep has to start from. Audited 2026-10-01 (finding 3):
             * g_shown used to freeze at whatever we last wrote, so the next
             * takeover set from == at and stepped instead of sweeping - which
             * is the "turning Override on does not animate" symptom, and the
             * same frozen value kept the cadence's `moving` test permanently
             * true and the loop at 16 ms forever. */
            if (eng > 0.05f && eng < 3.0f) g_shown = eng;
        }

        /* The decision, on every change and once a second while it holds.
         * MEASURE 2026-10-01: the gate's own byte is a third thing that can
         * change now, and it is the one under investigation - a 1 Hz beat
         * cannot see the edges of a signal that lasts a few hundred ms, which
         * is exactly what made the aim test look intermittently false with no
         * way to tell why. */
        DiagOpen();
        {
            static int lastAim = -1, lastGate = -1, lastHeld = -1;
            static DWORD lastAt;
            DWORD now = GetTickCount();
            int gateNow = 0;

            if (g_gate) g_gate(NULL, NULL, &gateNow, NULL);

            if (aim != lastAim || gateNow != lastGate || aimHeld != lastHeld ||
                (DWORD)(now - lastAt) >= 1000) {
                lastAt = now;
                lastAim = aim;
                lastGate = gateNow;
                lastHeld = aimHeld;
                DiagState(eng, g_camRad, g_shown, WantedRad(), aim, g_pathDbg,
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
        /* The cadence, decided at the END of the tick from the state this tick
         * leaves behind.
         *
         * MEASURE 2026-10-01, and the reason the transition could never be made
         * to work by tuning a rate: a rate is a fraction PER TICK, so it means
         * nothing unless the tick rate is known - and this loop's cadence
         * changes with the switches. With the override on and no sight up it
         * slept VIEW_MS (250 ms), which is 4 Hz, and any per-tick fraction
         * takes ten times longer at 4 Hz than at 40. A follow that walks 55 %
         * of the gap per tick needs about ten ticks; at 4 Hz that is 2.5
         * seconds, against an engine transition of 0.4 - so our value never
         * caught the engine's, the engine's arrived on the camera on its own,
         * and the picture cut.
         *
         * So the fast beat is taken whenever anything of OURS is on the
         * channel and moving, and whenever an aim is up or has just gone:
         * those are the frames a transition lives in. The slow beats are for
         * when nothing is moving, which is what they were for.
         */
        {
            float want = WantedRad();
            int moving = (g_shown > 0.05f && g_shown < 3.0f &&
                          fabsf(want - g_shown) > RAMP_MIN);
            DWORD ms = TICK_MS;

            /* A sweep that is still travelling needs the fast beat, and so does
             * one that has just been started; both are states this tick knows.
             * `moving` catches the first, the block below the second. */
            if (aimHeld || nz || aim || moving ||
                (DWORD)(GetTickCount() - aimAt) < 400)
                ms = NOZOOM_MS;
            /* FINDING 7 of the 2026-10-01 audit: the sleep is chosen from the
             * state at the END of the tick, so with the override on and nothing
             * moving the loop slept VIEW_MS - and an aim that began during that
             * sleep was noticed up to 250 ms late, with the camera keeping the
             * hip fov for that whole time before the sweep even started. There
             * is no way to see an edge that has not happened yet; what this can
             * do is refuse to sleep through the one state an aim can arrive in.
             * A plain hip is exactly that state, and it is also the only state
             * the override has nothing to do in, so the fast beat costs nothing
             * there - an aim byte that never comes costs one cheap read. */
            else if (OverrideOn() && CouldBeAiming(eng))
                ms = NOZOOM_MS;
            else if (OverrideOn())
                ms = VIEW_MS;

            /* Everything else is a slow poll: nothing of ours is on the
             * channel, so there is no value to keep up with. */
            if (rampDue < ms) ms = rampDue;   /* a sweep's next step is due first */

            Sleep(ms);
        }
        /* The aim's edge is for the NEXT tick to compare against - the sweep
         * is started from it at the top of the value decision above. */
        aimHeldPrev = aimHeld;
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
    /* The framework's aim byte, by name. Optional on purpose: this plugin
     * still declares SH_REQUIRES_API(1) and must load on a dinput8 that
     * predates the gate - it then falls back to the fov test in GateAds
     * rather than refusing to load over a missing import. */
    *(FARPROC *)&g_gate = GetProcAddress(GetModuleHandleA("dinput8.dll"),
                                         "ShFp2Gate");
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
