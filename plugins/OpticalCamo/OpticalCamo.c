/* Optical camo enhancement.
 *
 * A behaviour-equivalent re-implementation of the third-party plugin
 * plugins\OpticalCamo\OpticalCamo.asi, recovered by disassembly - see
 * docs/opticacamo-reverse.md (not shipped in the repository) for every
 * address behind what is below.
 * The original is a C++ plugin that late-binds this framework's exports
 * (GetModuleHandleA("dinput8.dll") + GetProcAddress) and votes on a
 * part-level flag to decide whether the player's optical camo is doing
 * anything; this file keeps that behaviour, drops the compatibility
 * machinery, and is plain C like the rest of the tree.
 *
 * What it does
 * ------------
 *   Optical Camo (crouch)   [off] / [on]  - the master switch, off by
 *                           default
 *   Camo Visibility         0.1x .. 0.9x  - the multiplier put on the
 *                           player while the camo is live (smaller =
 *                           harder to detect), 0.5x by default
 *
 * The switch is a real switch: while it is off nothing is applied, the
 * thread parks on an event (it wakes on the switch, the step or a
 * play-mode change, with a one-second backstop) and the game is not read
 * at all - no player, no parts.  Turning it on starts the watching;
 * turning it off restores the normal value once and parks again.
 *
 * While it is on, a 20 Hz round decides whether the camo is live.  That
 * vote starts with ShIsInGame(), so outside play - front end, loading
 * screen, map - it stops before touching anything; the cost in game is
 * eight guarded reads against the player's own node list.
 *
 * The status line is pushed only while this page is the one on screen
 * (ShMenuIsShowing): nothing is written to a menu nobody is looking at,
 * and the line is refreshed the moment the page comes up - see the
 * status part of Pump.
 *
 * The effect is the framework's: ShSetVisibility scales the detection /
 * awareness term the engine computes, the same term the trainer's
 * stealth toggle drives.  This plugin only decides WHEN that multiplier
 * is applied, and with which step.
 *
 * How "the camo is live" is known (the original's whole trick, kept)
 * -----------------------------------------------------------------
 * Not from an inventory and not from an ability object, but from the
 * player's own parts:
 *
 *   ShIsInGame() -> ShGetPlayer() -> ShGetEntityNodes(entity, nodes, 64)
 *   -> ShReadBytes(node + 0x54, &u16, 2) for every part
 *   -> count bit 7 of that halfword
 *
 *   every part flagged   -> the camo is NOT live   (Inactive)
 *   every part clear     -> the camo IS live       (Active)
 *   unreadable or mixed  -> cannot tell            (State unavailable)
 *
 * The byte at +0x54 is engine state, not ours: the offset, the bit and
 * the vote are what the disassembly proves (report sections 4 and 9).
 * The vote is logged whenever it is not a clean "all one way", which is
 * how that reading stays checkable in the field.
 *
 * Deviations from the original, all deliberate (see the report):
 *   - a master switch and eight steps (default off / 0.5x) instead of
 *     the original's four steps and always-on write: the original had no
 *     way to leave the engine alone, and no way to be truly off;
 *   - while off, the thread parks and the game is not read;
 *   - the status line is only written while this page is the one on
 *     screen (ShMenuIsShowing);
 *   - no shared-memory bus (W_VisibilityBus_v2 / "VIS2") and no Linked
 *     channel: there is no consumer inside this framework, and with no
 *     consumer the original applied the factor itself anyway - that is
 *     the half kept here;
 *   - no ShGetVersion gate and no PE build fingerprint (TimeDateStamp
 *     0x6A7C5143 / SizeOfImage 0x18B09000): the framework validates its
 *     own injection site, so a plugin has nothing to gate on;
 *   - no late binding: the exports used here are linked directly;
 *   - the status line drops the "Linked |" / "Direct |" prefix, and
 *     "State unavailable" is printed without a factor;
 *   - the play-mode declaration is explicit (the original stayed silent,
 *     which lands in the framework's default group: Ghost War and
 *     Mercenaries), and a plugin blocked mid-session puts the multiplier
 *     back to normal instead of leaving it on.
 */
#include <windows.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "scripthook.h"
#include "log.h"

/* plugins\OpticalCamo\OpticalCamo.ini: the original's section, plus the
 * switch this rewrite adds.  Visibility keeps its original meaning (the
 * multiplier as text). */
#define INI_SECTION  "OpticalCamo"
#define INI_KEY_ON   "Enabled"      /* 0 off (default), 1 on           */
#define INI_KEY_STEP "Visibility"   /* 0.20 .. 0.90, default "0.50"    */

#define POLL_MS      50          /* the original's round, Sleep(0x32)  */
#define IDLE_MS      1000        /* parked: backstop wake-up           */
#define PART_LIMIT   64          /* ShGetEntityNodes(..., 64)          */
#define PART_FLAG    0x54        /* what the original read per part    */
#define PART_BIT     0x80        /* ... and the bit it counted         */
#define EPSILON      0.0025f     /* the original's compare slack       */
/* How long a clean ACTIVE reading keeps the factor applied after the vote
 * says otherwise. The engine's per-part flag is not steady while the camo
 * shimmers - see the note in ReadCamoState - and a factor that is released
 * between two flickers of the flag is a factor nobody can feel. */
#define CAMO_HOLD_MS 2000

/* The nine steps by index: no 1.0x, because the switch is what turns the
 * plugin off, and no 0.0x, because a hard zero is the trainer's stealth
 * toggle rather than a camo strength.  Out-of-range snaps to the default
 * (0.5x, the value the original shipped as its middle step). */
#define STEP_COUNT   9
#define STEP_DEFAULT 4           /* 0.50x                              */

static const float kStep[STEP_COUNT] = {
    0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f
};
static const char *kStepName[STEP_COUNT] = {
    "0.1x", "0.2x", "0.3x", "0.4x", "0.5x", "0.6x", "0.7x", "0.8x", "0.9x"
};

/* 0 unknown, 1 inactive, 2 active - the original's three states. */
enum { CAMO_UNKNOWN = 0, CAMO_INACTIVE = 1, CAMO_ACTIVE = 2 };

static volatile LONG g_on      = 0;   /* the master switch: off by default */
static volatile LONG g_step    = STEP_DEFAULT;  /* 0.50x                  */
static volatile LONG g_force   = 1;   /* redraw the status line         */
static volatile LONG g_blocked = 0;   /* set by the framework when we
                                         must not run (PvP modes)      */
static volatile LONG g_live    = 0;   /* a factor below 1.0 is applied  */
static int           g_wroteEver = 0; /* ... and we have written at all */
static float         g_wroteF  = 1.0f;/* the value we last wrote        */
static uint32_t      g_menu    = 0;
static HINSTANCE     g_inst    = NULL;
static HANDLE        g_wake    = NULL;/* switch / step / mode change    */
static char          g_iniPath[MAX_PATH];

static int StepClamped(int step) {
    if (step < 0) return 0;
    if (step > STEP_COUNT - 1) return STEP_DEFAULT;
    return step;
}

static int StepNow(void)   { return StepClamped((int)InterlockedCompareExchange(&g_step, 0, 0)); }
static int OnNow(void)     { return (int)InterlockedCompareExchange(&g_on, 0, 0); }

/* Nothing to watch: the switch is off and nothing is applied.  The
 * framework's stealth hook may be installed by the user's own toggle,
 * but that is not ours to hold - we neither read nor write anything. */
static int IdleNow(void) {
    return !OnNow() && !InterlockedCompareExchange(&g_live, 0, 0);
}

/* ---- config ---------------------------------------------------------
 * The step keeps the original's key and its "snap to the nearest step and
 * write it back" behaviour, so the file always shows one of the eight.
 * The switch is new, and defaults to off. */

static void ResolveIniPath(void) {
    char mod[MAX_PATH];
    const char *dot;
    size_t n;

    g_iniPath[0] = 0;
    if (!g_inst || !GetModuleFileNameA(g_inst, mod, sizeof(mod))) return;
    dot = strrchr(mod, '.');
    n = dot ? (size_t)(dot - mod) : strlen(mod);
    if (n >= sizeof(g_iniPath)) n = sizeof(g_iniPath) - 1;
    memcpy(g_iniPath, mod, n);
    g_iniPath[n] = 0;
    strncat(g_iniPath, ".ini", sizeof(g_iniPath) - n - 1);
}

static void SaveStep(int step) {
    char buf[16];
    if (!g_iniPath[0]) return;
    step = StepClamped(step);
    snprintf(buf, sizeof(buf), "%.2f", kStep[step]);
    if (!WritePrivateProfileStringA(INI_SECTION, INI_KEY_STEP, buf, g_iniPath))
        Log("could not save %s=%s", INI_KEY_STEP, buf);
}

static void SaveOn(int on) {
    if (!g_iniPath[0]) return;
    if (!WritePrivateProfileStringA(INI_SECTION, INI_KEY_ON, on ? "1" : "0",
                                    g_iniPath))
        Log("could not save %s=%d", INI_KEY_ON, on);
}

/* Nearest step to whatever the ini holds.  Text that is not a number,
 * NaN and infinities fall back to the default (0.5x).  A value exactly
 * between two steps goes to the higher one: the weaker effect. */
static int NearestStep(float v) {
    int best = STEP_DEFAULT, i;
    float bestD = 1e9f;

    if (!(v == v) || v > 1e6f || v < -1e6f) return STEP_DEFAULT;
    for (i = 0; i < STEP_COUNT; i++) {
        float d = v - kStep[i];
        if (d < 0.0f) d = -d;
        if (d <= bestD) { bestD = d; best = i; }
    }
    return best;
}

static void LoadSettings(void) {
    char buf[64];
    char *end = NULL;
    int step, on;

    if (!g_iniPath[0]) return;

    on = GetPrivateProfileIntA(INI_SECTION, INI_KEY_ON, 0, g_iniPath) ? 1 : 0;
    InterlockedExchange(&g_on, on);

    if (!GetPrivateProfileStringA(INI_SECTION, INI_KEY_STEP, "0.50", buf,
                                  sizeof(buf), g_iniPath))
        strcpy(buf, "0.50");
    {
        double v = strtod(buf, &end);
        /* Text that is not a number parses as 0.0, which would land on the
         * strongest step (0.1x) instead of the documented default 0.5x. */
        step = (end == buf) ? STEP_DEFAULT : NearestStep((float)v);
    }
    InterlockedExchange(&g_step, step);

    Log("ini: %s=%d, %s=%s -> step %d (%s)",
        INI_KEY_ON, on, INI_KEY_STEP, buf, step, kStepName[step]);
    SaveStep(step);
}

/* ---- the state vote (the original's core) ---------------------------- */

typedef struct {
    int parts;    /* how many parts the entity reported                */
    int flagged;  /* parts with bit 7 set at +0x54                     */
    int clear;    /* parts with it clear                               */
    int failed;   /* parts ShReadBytes could not read                  */
} CamoVote;

/* The vote above is the original's, unchanged. What follows is the fix
 * for the field report of 2026-09-17: "0.1x and 0.5x both feel like
 * nothing, enemies still spot me instantly at range".
 *
 * That session's log is the whole explanation. The parts never disagree -
 * there is not one mixed or "state unavailable" line in it - but the vote
 * flips cleanly between Active and Inactive about once a second, all
 * session long. A vote that alternates that fast hands the multiplier to
 * the engine for roughly half a second at a time, which is the same as
 * never applying it: by the time the detection term has been scaled down,
 * the next reading has put it back.
 *
 * So an Active reading is latched: the factor stays applied for
 * CAMO_HOLD_MS, and only a clean Inactive that outlives that window turns
 * it off again - one reading cannot undo what another just decided. A
 * reading that cannot be made holds the decision that stands, rather than
 * releasing the factor on evidence that does not exist; the old path fell
 * straight to 1.0x there, a second way the effect could vanish mid-crouch.
 *
 * The latch only ever extends how long the factor is applied, never how
 * strong it is, so the menu's promise ("crouch to trigger, then this
 * multiplier") still holds - it just holds still long enough to be felt.
 * Every change of the latched state is logged with the raw parts behind
 * it, so the next session can read exactly what the engine said and when.
 *
 * Outside a round the latch is bypassed on purpose: leaving a factor on
 * the player while standing in the front end is not something a "crouch
 * to trigger" switch may do. */
static int LatchCamoState(int raw, const CamoVote *vote) {
    static int      held = CAMO_UNKNOWN;
    static uint32_t activeAt = 0;
    static int      lastRaw = -1;
    uint32_t now = GetTickCount();
    int was = held;

    if (raw == CAMO_ACTIVE) {
        held = CAMO_ACTIVE;
        activeAt = now;
    } else if (raw == CAMO_INACTIVE) {
        if (held != CAMO_ACTIVE || now - activeAt > CAMO_HOLD_MS)
            held = CAMO_INACTIVE;
    } else if (held == CAMO_UNKNOWN) {
        held = CAMO_UNKNOWN;
    }

    if (raw != lastRaw || held != was) {
        Log("vote: %d part(s), %d flagged, %d clear, %d unreadable -> "
            "%s (latched %s)", vote->parts, vote->flagged, vote->clear,
            vote->failed,
            raw == CAMO_ACTIVE ? "active" :
            raw == CAMO_INACTIVE ? "inactive" : "unknown",
            held == CAMO_ACTIVE ? "active" :
            held == CAMO_INACTIVE ? "inactive" : "unknown");
        lastRaw = raw;
    }
    return held;
}

static int ReadCamoState(CamoVote *vote) {
    ShPlayer pl;
    uint64_t nodes[PART_LIMIT];
    int n, i, failed = 0, flagged = 0, clear = 0;
    int raw;

    memset(vote, 0, sizeof(*vote));

    /* Not in play: front end, loading, map.  Nothing is read and nothing
     * is held - see the note on LatchCamoState for why this one case must
     * release the factor rather than remember it. */
    if (!ShIsInGame()) return CAMO_UNKNOWN;

    memset(&pl, 0, sizeof(pl));
    if (!ShGetPlayer(&pl) || !pl.entity)
        return LatchCamoState(CAMO_UNKNOWN, vote);

    memset(nodes, 0, sizeof(nodes));
    n = ShGetEntityNodes(pl.entity, nodes, PART_LIMIT);
    if (n < 1 || n > PART_LIMIT)
        return LatchCamoState(CAMO_UNKNOWN, vote);

    for (i = 0; i < n; i++) {
        uint16_t v = 0;
        if (ShReadBytes(nodes[i] + PART_FLAG, &v, 2)) {
            if (v & PART_BIT) flagged++;
            else             clear++;
        } else {
            failed++;
        }
    }

    vote->parts   = n;
    vote->flagged = flagged;
    vote->clear   = clear;
    vote->failed  = failed;

    if (failed)            raw = CAMO_UNKNOWN;   /* a part we could not read */
    else if (flagged == n) raw = CAMO_INACTIVE;  /* every part flagged   */
    else if (clear == n)   raw = CAMO_ACTIVE;    /* every part clear     */
    else                   raw = CAMO_UNKNOWN;   /* the parts disagree   */

    return LatchCamoState(raw, vote);
}

/* ---- apply + status -------------------------------------------------
 * One writer (the poll thread) and one value: the multiplier is written
 * only when the wanted value moves, the status line only while this page
 * is up and only when what it shows has moved.  The original had the
 * first guard, for the same reason - this runs twenty times a second
 * while the game is drawing.
 *
 * Two things are deliberately new: with the switch off and nothing
 * written yet, the framework is never asked to do anything at all; and
 * nothing is written into a menu nobody can see - the line is pushed
 * when the menu opens instead. */

static void Pump(int state) {
    int on = OnNow();
    int step = StepNow();
    int blocked = (int)InterlockedCompareExchange(&g_blocked, 0, 0);
    float want = (on && state == CAMO_ACTIVE && !blocked) ? kStep[step] : 1.0f;
    float shown = 1.0f;
    int need = 0, forced, changed;
    static int   lastState = -1;
    static int   lastStep  = -1;
    static int   lastOn    = -1;
    static float lastShown = 1e9f;
    static int   needPush  = 1;   /* never pushed: the first open shows it */

    if (!g_wroteEver)
        need = (want != 1.0f);   /* nothing applied yet: 1.0x stays out  */
    else if (want > g_wroteF + EPSILON || want < g_wroteF - EPSILON)
        need = 1;                /* something is applied and must move   */

    if (need) {
        static float failLoggedFor = 1e9f;

        if (!ShSetVisibility(want)) {
            /* Once per target: the poll runs at 20 Hz, so a refusal would
             * otherwise write twenty lines a second. g_wroteF is NOT
             * advanced - the send was refused, so the next tick retries. */
            if (failLoggedFor != want) {
                failLoggedFor = want;
                Log("ShSetVisibility(%.3f) failed, error %d",
                    want, ShLastError());
            }
        } else {
            failLoggedFor = 1e9f;
            Log("visibility %.3fx (switch %s, state %s, step %s)",
                want, on ? "on" : "off",
                state == CAMO_ACTIVE ? "active" :
                state == CAMO_INACTIVE ? "inactive" : "unavailable",
                kStepName[step]);
            g_wroteF = want;
            g_wroteEver = 1;
            InterlockedExchange(&g_live, want == 1.0f ? 0 : 1);
        }
    }

    /* The multiplier actually in force, when the framework can say. On a
     * failed query `shown` would keep its initial 1.0 and the status line
     * would report a number nothing had set. */
    if (!ShGetVisibility(&shown)) shown = lastShown;

    forced = (int)InterlockedExchange(&g_force, 0);
    changed = forced || on != lastOn || state != lastState ||
              step != lastStep ||
              shown > lastShown + EPSILON || shown < lastShown - EPSILON;
    if (changed) {
        lastOn    = on;
        lastState = state;
        lastStep  = step;
        lastShown = shown;
        needPush  = 1;
    }

    /* Only while this page is the one on screen: that is the only time
     * the line can be read.  A change made while it is not keeps
     * needPush set, so the moment the page comes up it shows today's
     * value - no refresh is spent on a menu nobody is looking at. */
    if (needPush && g_menu && ShMenuIsShowing(g_menu)) {
        if (!on)
            ShMenuStatusF(g_menu, "@camo.status.off", shown);
        else if (state == CAMO_UNKNOWN)
            ShMenuStatusF(g_menu, "@camo.status.unknown");
        else
            ShMenuStatusF(g_menu, state == CAMO_ACTIVE
                                   ? "@camo.status.active"
                                   : "@camo.status.inactive", shown);
        needPush = 0;
    }
}

/* ---- menu ----------------------------------------------------------- */

static void OnToggle(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    value = value ? 1 : 0;
    if (InterlockedExchange(&g_on, value) == value) return;
    SaveOn(value);
    InterlockedExchange(&g_force, 1);
    if (g_wake) SetEvent(g_wake);       /* leave idle / start watching */
    if (value)
        Log("switch on: watching the camo");
    else
        Log("switch off: no factor, no detection, the thread parks");
}

static void OnStep(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    if (value < 0 || value >= STEP_COUNT) return;
    /* A list row fires twice per change on purpose: once on the step and
     * once on the key release, so what is on screen is what the plugin
     * holds (scripthook_menu.c, r == 1 then r == 2).  The second call
     * carries the same value, and there is nothing to store or redraw for
     * it - only the first call does any work. */
    if (InterlockedExchange(&g_step, value) == value) return;
    SaveStep(value);
    InterlockedExchange(&g_force, 1);
    if (g_wake) SetEvent(g_wake);
    Log("step %d (%s)", value, kStepName[value]);
}

/* ---- text ---------------------------------------------------------
 * The menu's own text, compiled in. The keys are stable IDs, so
 * rewording a row never breaks a translation in lang.ini.
 */
static const ShText kEn[] = {
    { "@camo.page",             "Optical Camo" },
    { "@camo.enabled",          "Optical Camo (crouch effect)" },
    { "@camo.step",             "Camo Visibility" },
    { "@camo.status.off",       "Off | %.3fx" },
    { "@camo.status.active",    "Active | %.3fx" },
    { "@camo.status.inactive",  "Inactive | %.3fx" },
    { "@camo.status.unknown",   "State unavailable" },
    { "@camo.hint",
      "Wear the optical camo backpack, Future Soldier or John Kozak set; the "
      "effect applies while crouching triggers it." }
};

static const ShText kZh[] = {
    { "@camo.page",             "光学迷彩加强" },
    { "@camo.enabled",          "光学迷彩加强（蹲下触发特效时生效）" },
    { "@camo.step",             "敌人视觉感知" },
    { "@camo.status.off",       "关闭 | %.3fx" },
    { "@camo.status.active",    "激活 | %.3fx" },
    { "@camo.status.inactive",  "未激活 | %.3fx" },
    { "@camo.status.unknown",   "状态不可用" },
    { "@camo.hint",
      "装备光学迷彩背包/未来战士/约翰•科扎克套装，蹲下触发特效时生效。" }
};

static void CamoText(void) {
    static int done;

    if (done) return;
    done = 1;
    ShLangDeclare("OpticalCamo", "en-US", kEn,
                  (int)(sizeof(kEn) / sizeof(kEn[0])));
    ShLangDeclare("OpticalCamo", "zh-CN", kZh,
                  (int)(sizeof(kZh) / sizeof(kZh[0])));
}

static void BuildMenu(void) {
    CamoText();
    g_menu = ShMenuCreate("@camo.page");
    if (!g_menu) {
        Log("ShMenuCreate failed, error %d", ShLastError());
        return;
    }
    ShMenuHint(g_menu, "@camo.hint");
    if (!ShMenuToggle(g_menu, "@camo.enabled", OnNow(),
                      OnToggle, NULL)) {
        Log("ShMenuToggle failed, error %d", ShLastError());
        ShMenuDestroy(g_menu);
        g_menu = 0;
        return;
    }
    if (!ShMenuList(g_menu, "@camo.step", kStepName, STEP_COUNT,
                    StepNow(), OnStep, NULL)) {
        Log("ShMenuList failed, error %d", ShLastError());
        ShMenuDestroy(g_menu);
        g_menu = 0;
    }
}

/* ---- play modes ------------------------------------------------------
 * The original declared nothing, which is the framework's default group:
 * blocked in Ghost War and Mercenaries.  Declared explicitly here, and a
 * plugin blocked mid-session restores normal visibility instead of
 * leaving a multiplier on until the next round. */

static void OnBlocked(int allowed, int blocked, void *user) {
    (void)user;
    InterlockedExchange(&g_blocked, allowed ? 0 : 1);
    InterlockedExchange(&g_force, 1);
    if (g_wake) SetEvent(g_wake);
    if (allowed)
        Log("allowed again: the camo is followed again");
    else
        Log("blocked (%d): the multiplier goes back to 1.0x", blocked);
}

/* ---- threads -------------------------------------------------------- */

/* Set on unload. DllMain only flips it and wakes the poll thread, which
 * does the actual restoring. */
static volatile LONG g_stop;

static DWORD WINAPI PollThread(LPVOID arg) {
    int idleWas = -1;
    (void)arg;

    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        CamoVote vote;
        int state, clean, idle;

        /* Parked while the switch is off and nothing has been applied:
         * the wait is what wakes us, plus a slow backstop.  The switch,
         * the step and a play-mode change all signal the event, so
         * leaving idle is immediate. */
        idle = IdleNow();
        /* With no event (CreateEventA failed) the wait would return at once
         * every time - and this loop would then spin a core at full speed.
         * Sleeping is the honest fallback: the same cadence, no busy wait. */
        if (g_wake) WaitForSingleObject(g_wake, idle ? IDLE_MS : POLL_MS);
        else        Sleep(idle ? IDLE_MS : POLL_MS);

        if (idle != idleWas) {
            idleWas = idle;
            if (idle)
                Log("switch off: idle - no factor, no detection, the game "
                    "is not read");
            else
                Log("switch on: watching the camo (only while in play)");
            InterlockedExchange(&g_force, 1);   /* redraw the status line */
        }

        if (IdleNow()) {
            Pump(CAMO_UNKNOWN);   /* keep the status line honest, no reads */
            continue;
        }

        state = ReadCamoState(&vote);

        /* A clean vote is the normal case and stays out of the log; the
         * rest is worth a line, because that is where the +0x54 reading
         * can still surprise us. */
        clean = vote.parts && vote.failed == 0 &&
                (vote.flagged == vote.parts || vote.clear == vote.parts);
        {
            /* The edge, not every tick: a mixed read is a state, and at
             * 20 Hz a single wobble would write twenty lines. */
            static int uncleanLogged;
            if (!clean && vote.parts) {
                if (!uncleanLogged) {
                    uncleanLogged = 1;
                    Log("vote: parts=%d flagged=%d clear=%d failed=%d -> %s",
                        vote.parts, vote.flagged, vote.clear, vote.failed,
                        state == CAMO_ACTIVE ? "active" :
                        state == CAMO_INACTIVE ? "inactive" : "unavailable");
                }
            } else {
                uncleanLogged = 0;
            }
        }

        Pump(state);
    }

    /* Unloading: hand the normal visibility back, so whatever runs next is
     * not left invisible. Done here rather than in DllMain, which runs
     * under the loader lock and must not call framework APIs. */
    if (InterlockedCompareExchange(&g_live, 0, 0))
        ShSetVisibility(1.0f);
    return 0;
}

static DWORD WINAPI InitThread(LPVOID arg) {
    (void)arg;

    /* Always, not LogInit: a plugin's own log is written at every level
     * except none, so the line about what did not work survives a quiet
     * session. */
    LogInitAlways("OpticalCamo.log");
    Log("--- optical camo ---");
    Log("built " __DATE__ " " __TIME__);

    ResolveIniPath();
    if (!g_iniPath[0]) Log("no ini path: the setting will not persist");
    LoadSettings();

    /* Blocked in the two PvP modes - the group a silent plugin is put in,
     * said out loud. */
    if (!ShPluginBlacklist(SH_MODE_BLACKLIST_GHOST_WAR |
                           SH_MODE_BLACKLIST_MERCENARIES))
        Log("the blacklist declaration was refused, error %d", ShLastError());
    if (!ShPluginOnBlocked(OnBlocked, NULL))
        Log("ShPluginOnBlocked failed, error %d", ShLastError());
    if (!ShPluginAllowed()) {
        InterlockedExchange(&g_blocked, 1);
        Log("blocked in this session: the multiplier stays at 1.0x");
    }

    BuildMenu();
    Log("switch %s, step %d (%s), starting the poll thread",
        OnNow() ? "on" : "off", StepNow(), kStepName[StepNow()]);
    {
        HANDLE h = CreateThread(NULL, 0, PollThread, NULL, 0, NULL);

        if (h) CloseHandle(h);   /* never waited on */
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = inst;
        DisableThreadLibraryCalls(inst);
        g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        {
            HANDLE h = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);

            if (h) CloseHandle(h);   /* never waited on */
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        /* The restore belongs to the poll thread: this runs under the
         * loader lock, where a framework call is what the plugin contract
         * forbids. At process exit the thread freezes with the process and
         * nothing needs restoring. */
        InterlockedExchange(&g_stop, 1);
        if (g_wake) SetEvent(g_wake);
    }
    return TRUE;
}
