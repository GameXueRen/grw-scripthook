/* Which play mode this session is in, read from the game itself.
 *
 * The mode is the mode object. CreateGameMode is handed a description of
 * the mode it is to build, and the first field of that description points
 * into the game's own mode table - one entry per mode, stable across
 * sessions:
 *
 *   38DC7F0   the campaign    the base story, Narco Road,
 *                             Fallen Ghosts, The Last Rites
 *   38DCD80   Ghost Mode      幽灵/魅影模式, the permadeath campaign
 *   38DD178   Mercenaries     the eight player PvPvE mode
 *   38DCF80   Guerrilla       游击战, camp defence
 *   3908D98   Ghost War       the 4v4 PvP mode
 *
 * GameModeManager::SetCurrentGameMode is hooked as well and its argument
 * is logged with every mode, but it is not what identifies the mode. It
 * looked like it at first - 3 for Ghost War, 2 for the three modes that
 * shared it, 0 for the campaign - until Fallen Ghosts arrived with 2 and
 * the campaign's object: the same mode content can come with 0 or with
 * 2. What that number means is not known; a session class (offline,
 * online, PvP) fits everything seen so far. So it is recorded, and the
 * object decides. An object that is not in the table is left undecided
 * and logged, never guessed at - with one exception, measured twice and
 * never once contradicted: argument 3, which has only ever named Ghost
 * War, is believed even when the object is new, because dropping it is
 * not caution but a wrong answer. It is believed **once**: the watcher
 * consumes the argument as soon as it has settled a mode, so a number the
 * game set earlier can never decide a mode that arrives later.
 *
 * 0 is not believed any more. It used to answer "campaign", and the
 * campaign's object is in the table either way; what the exception bought
 * was nothing, and what it cost was the report of 2026-09-17: a 0 left
 * over from an earlier call met an unrecognised PvP object, was answered
 * "campaign", and every plugin stayed on in Ghost War. 2 stays undecided
 * throughout: three modes share it and only the object separates them.
 *
 * The engine's own name table at 0x390CCF0 (MP / TG / MERC / empty / SP
 * / COOP) is not used for this: it is not indexed by the argument (3 is
 * Ghost War and slot 3 there is empty), and the function that reads it
 * is not on the path these sessions took.
 *
 * The class is not reflected, so a name cannot find it: 312 methods of
 * the front end objects resolved against a 24000 name dictionary came
 * back with nothing but the scene interface, and the hashes of this
 * class's own method names match no method table entry at all. Its
 * address came from the other side - scanning the image for what
 * references its own log line ("[mGameModeManager][IsMaster=%d]
 * SetCurrentGameMode(%u, %u)" at rva 0x391E078), then asking the
 * exception directory (.pdata) for the enclosing function, which is
 * 0x9BA0140. That same check runs live before the hook is armed: if the
 * bytes there do not reference that line, this is not the build the
 * address came from and nothing is hooked.
 *
 * Two things about that are load bearing, both learned the hard way:
 *
 *   - The detour stores the argument and calls through, and does nothing
 *     else. An earlier version logged from inside the hook and took the
 *     client down three sessions running, each time on the first sixteen
 *     byte store of a logging function's 1024 byte line buffer (movdqa
 *     [rbp+0x3E0], xmm0 - the same instruction in the framework's logger
 *     and in the probe's), because the mode manager is called on a
 *     thread whose stack is already deep. Formatting happens on the
 *     thread below instead.
 *   - The check is retried rather than trusted once. This DLL is a
 *     dinput8 proxy, so it is loaded before the game's own entry point
 *     runs, and the section this function lives in (.link) is encrypted
 *     on disk - read off the file it is not code at all. Early on it can
 *     still be encrypted in memory, and a failed check there means "not
 *     yet", not "wrong build".
 *
 * There is deliberately no second source. The first version of this
 * module watched the main menu and treated a left click on the Ghost War
 * item as the answer, which worked, and it is gone on the owner's call:
 * it was mouse only (a pad confirm never moves the pointer), it needed a
 * calibrated point, and a different resolution or UI scale - or the menu
 * rearranging - silently made it wrong. A mode that is read from the
 * game has none of those failure modes, and a fallback that can be
 * quietly wrong is worse than an honest "not yet": ShSelectedPlayMode
 * says NONE until the game has said something, and
 * ShPlayModeHookArmed tells a caller whether it ever will.
 *
 * What is still not covered, in full: the values above are measured, not
 * documented - 3 is Ghost War on this build, 2 is shared by the three
 * modes the object separates, and 0 is not taken as the campaign any more
 * (its object is in the table; the argument on its own now decides
 * nothing but Ghost War). A build whose layout differs will not arm the
 * hook, and the answer stays NONE rather than becoming a guess.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "scripthook_tick.h"
#include "log.h"
#include "image.h"
#include "third_party/minhook/include/MinHook.h"

/* GameModeManager::SetCurrentGameMode and ::CreateGameMode, and the log
 * line inside each that is used as proof of identity. */
#define GM_SITE_RVA    0x9BA0140u
#define GM_PROOF_RVA   0x391E078u
#define CM_SITE_RVA    0x9B985E8u
#define CM_PROOF_RVA   0x391DFF0u
/* How far into a function the proof of identity is looked for. It has
 * to cover SetCurrentGameMode's (a reference at +0x25) and
 * CreateGameMode's, which is much further in at +0x92 - a window of 0x80
 * missed it, the check failed for ever, and nothing was armed. */
#define GM_PROBE       0x180

/* The arguments that matter, measured (see the header). 2 is the
 * ambiguous one: Ghost Mode and Mercenaries both set it. */
#define GM_TYPE_MERCENARIES 2
#define GM_TYPE_GHOST_WAR   3

/* The mode objects, which is what separates the two when the argument
 * does not. CreateGameMode is handed a description of the mode to build
 * and its first field is a pointer into the game's own mode table - one
 * entry per mode, and stable across sessions (Mercenaries was 38DD178 in
 * all three of them). RVAs are for this build, and the same identity
 * check that guards the hooks guards the table: a build with a different
 * layout never reaches it.
 *
 *   38DCD80  Ghost Mode    the permadeath campaign       (3 sessions)
 *   38DD178  Mercenaries                                 (3 sessions)
 *   38DCF80  Guerrilla     游击战, camp defence           (1 session)
 *   3908D98  Ghost War                                   (1, arg 3 anyway)
 *   38DC7F0  the campaign   the story mode, and Narco     (3, arg 0 anyway)
 *                           Road, which is campaign
 *                           content rather than a mode
 *                           of its own: same argument,
 *                           same object, measured
 *
 * A mode object that is not in the table is logged, and for an argument
 * of 2 - where three modes share the argument and none of them is the
 * ordinary case - that leaves the mode undecided rather than guessed at.
 * Guerrilla arrived exactly that way: argument 2, an object the table did
 * not have, one logged line, one row added.
 */
static const struct {
    uint32_t    rva;
    int         mode;
    const char *name;
} g_fp[] = {
    { 0x38DCD80u, SH_PLAYMODE_GHOST_MODE,  "Ghost Mode" },
    { 0x38DD178u, SH_PLAYMODE_MERCENARIES, "MERCENARIES" },
    { 0x38DCF80u, SH_PLAYMODE_GUERRILLA,   "Guerrilla" },
    /* Measured 2026-09-17, entering Ghost War from the front end: arg 3
     * with this object. It sits 0xE0 below the row recorded for Ghost War
     * years-of-notes ago, which is what the other table region's spacing
     * looks like - Ghost War's entry is not in the 0x38DDxxx block the
     * other four share. That older row is kept: it may well be the entry
     * itself, with this being the description 0xE0 before it. */
    { 0x3908CB8u, SH_PLAYMODE_GHOST_WAR,   "Ghost War" },
    { 0x3908D98u, SH_PLAYMODE_GHOST_WAR,   "Ghost War" },
    { 0x38DC7F0u, SH_PLAYMODE_CAMPAIGN,    "campaign" }
};
#define GM_FP_N ((int)(sizeof(g_fp) / sizeof(g_fp[0])))

/* The check is retried for about a minute: the section it reads can
 * still be encrypted while this DLL is starting. */
#define GM_TRIES       30
#define GM_TRY_MS      2000

static volatile LONG  g_gmType = -1;     /* RDX of the last call seen */
static volatile LONG  g_gmCalls;
static volatile LONG  g_gmLogged = -2;   /* argument the watcher logged */
static volatile LONG  g_gmFpLogged;      /* mode object the watcher logged */
static volatile LONG  g_gmCmCalls;       /* CreateGameMode calls seen */
/* How many calls the watcher writes a line for. Everything past this is
 * still resolved; it just stops filling the log - and mode changes are
 * logged whatever the count is. */
#define CALL_LOG_MAX 40
static volatile LONG  g_gmCallsLogged;   /* calls the watcher has logged */
static volatile LONG  g_gmCmLogged;      /* (set gm, then create mode) */
static volatile LONG  g_gmTries;
static volatile LONG  g_gmArmed;         /* 1 armed, -1 given up */
static volatile LONG  g_gmCmArmed;
static volatile void  *g_gmDesc;         /* RDX of CreateGameMode */
/* The description g_gmDesc points at belongs to one of those CreateGameMode
 * calls, and is only read while that call is fresh (M2 of the pre-release
 * audit, 2026-09-17). The game builds a description for the mode it is about
 * to create and may release it once the mode is up, so re-reading that memory
 * every 200 ms for the rest of the session eventually reads whatever moved
 * in - a value that gets past FingerprintOf's range check often enough to be
 * believed, which rewrites the mode under a running match. That is the
 * "sometimes blocked, sometimes not" shape of the report, and the session
 * that can reach it is the one that switches mode without restarting: the
 * campaign back to the front end and into a PvP mode (the PvP modes
 * themselves need a restart, so they cannot change under themselves).
 *
 * Held as a count of ticks rather than a timestamp because the hook that
 * hands the description over must not call out to anything - one locked
 * store is all it gets to spend (see the note at the top of this file). */
#define GM_FP_FRESH     30              /* ticks (6 s): a fresh desc is read */
static volatile LONG  g_gmDescFresh;     /* ticks left to read g_gmDesc */
static volatile LONG  g_gmFpKept;        /* what was read while it was fresh */
static volatile LONG  g_gmFp;            /* mode object, as an RVA */
static volatile LONG  g_gmMode;          /* resolved mode, NONE until known */
static void          *g_gmOrig;
static void          *g_cmOrig;
static volatile LONG  g_started;
static volatile LONG  g_enabled = 1;

typedef void *(*GmFn)(void *, void *, void *, void *);

/* ReadProcessMemory rather than a plain read: the page can be gone. */
static int GmRead(uint64_t addr, void *out, size_t n) {
    SIZE_T got = 0;

    if (addr < 0x10000) return 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)addr,
                             out, n, &got) && got == n;
}

/* The function is the one that logs its own line when the first few
 * instructions load the address of that line, rip relative. */
static int RefVerified(uint64_t at, uint64_t want) {
    unsigned char b[GM_PROBE];
    int i;

    if (!GmRead(at, b, sizeof(b))) return 0;
    for (i = 0; i + 7 <= (int)sizeof(b); i++) {
        int32_t d;

        if (b[i] != 0x48 && b[i] != 0x4C) continue;
        if (b[i + 1] != 0x8D || (b[i + 2] & 0xC7) != 0x05) continue;
        memcpy(&d, b + i + 3, 4);
        if ((uint64_t)((int64_t)(at + (uint64_t)i + 7) + d) == want)
            return 1;
    }
    return 0;
}

/* Store and call through. Nothing else may happen here - see the note at
 * the top about what formatting in this hook cost. */
static void *GmDetour(void *self, void *type, void *a3, void *a4) {
    LONG v = (LONG)(uintptr_t)type;

    if (v >= 0 && v < 4096) g_gmType = v;
    g_gmCalls++;
    return ((GmFn)g_gmOrig)(self, type, a3, a4);
}

/* CreateGameMode(manager, description, ...). The description is only
 * kept as a pointer: reading it happens on the watcher thread, because
 * a memory read is not something to do from inside this hook. */
static void *CmDetour(void *self, void *desc, void *a3, void *a4) {
    g_gmCmCalls++;
    if (desc) {
        /* The window first, then the pointer: the watcher must never pair a
         * fresh description with the previous call's window, which is what
         * decides whether it is still allowed to read that memory. The
         * answer read from the previous description is dropped here too, so
         * the new one is read on its own merits - the two can be the same
         * address, the allocator reuses it, which is exactly why the pointer
         * itself cannot be what says "same mode". */
        InterlockedExchange(&g_gmDescFresh, GM_FP_FRESH);
        InterlockedExchange(&g_gmFpKept, 0);
        g_gmDesc = desc;
    }
    return ((GmFn)g_cmOrig)(self, desc, a3, a4);
}

/* 1 armed, 0 not verified yet, -1 failed. */
static int ArmOne(uint32_t site, uint32_t proof, void *detour, void **orig,
                  const char *what) {
    void *target = (void *)SH_IMG(site);
    MH_STATUS s;

    if (!RefVerified(SH_IMG(site), SH_IMG(proof))) return 0;
    s = MH_CreateHook(target, detour, orig);
    if (s != MH_OK) {
        LogAlways("playmode: hook %s at %08X failed (%d)", what, site, (int)s);
        return -1;
    }
    s = MH_EnableHook(target);
    if (s != MH_OK) {
        /* Not left half-installed: a created-but-disabled hook still owns
         * the trampoline *orig points at, and this install is retried. */
        LogAlways("playmode: enabling %s at %08X failed (%d)", what, site, (int)s);
        MH_RemoveHook(target);
        *orig = NULL;
        return -1;
    }
    return 1;
}

static void GmInstall(void) {
    LONG n;
    MH_STATUS s;
    int a, b;

    if (g_gmArmed && g_gmCmArmed) return;
    n = InterlockedIncrement(&g_gmTries);
    if (n > GM_TRIES) {
        if (n == GM_TRIES + 1)
            Log("playmode: nothing verified after %ld tries - the mode stays "
                "unknown", (long)GM_TRIES);
        return;
    }
    if (!RefVerified(SH_IMG(GM_SITE_RVA), SH_IMG(GM_PROOF_RVA)) ||
        !RefVerified(SH_IMG(CM_SITE_RVA), SH_IMG(CM_PROOF_RVA))) {
        /* Said once, so a silent log is never the first sign of a
         * check that does not match: the next line is either the armed
         * one or the give up one. */
        if (n == 1)
            Log("playmode: waiting for the game's mode manager to be "
                "verifiable (SetCurrentGameMode %08X, CreateGameMode "
                "%08X)", GM_SITE_RVA, CM_SITE_RVA);
        return;                         /* not yet, or not this build */
    }
    s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        LogAlways("playmode: minhook init failed (%d) - the mode stays unknown",
            (int)s);
        InterlockedExchange(&g_gmArmed, -1);
        InterlockedExchange(&g_gmCmArmed, -1);
        return;
    }
    a = ArmOne(GM_SITE_RVA, GM_PROOF_RVA, (void *)GmDetour, &g_gmOrig,
               "SetCurrentGameMode");
    b = ArmOne(CM_SITE_RVA, CM_PROOF_RVA, (void *)CmDetour, &g_cmOrig,
               "CreateGameMode");
    if (a < 0 || b < 0) {
        InterlockedExchange(&g_gmArmed, -1);
        InterlockedExchange(&g_gmCmArmed, -1);
        return;
    }
    if (a == 1) InterlockedExchange(&g_gmArmed, 1);
    if (b == 1) InterlockedExchange(&g_gmCmArmed, 1);
    LogAlways("playmode: SetCurrentGameMode at %08X and CreateGameMode at %08X "
        "verified and hooked (try %ld) - the mode comes from the game",
        GM_SITE_RVA, CM_SITE_RVA, (long)n);
}

/* The mode object's first field is a pointer into the game's own mode
 * table, and that is what separates the modes which share an argument.
 * Read here, on this thread, rather than in the hook. */
static uint32_t FingerprintOf(void) {
    void *desc = (void *)g_gmDesc;
    uint64_t q = 0, base = SH_IMG(0);
    uint32_t rva;

    if (!desc) return 0;
    if (!GmRead((uint64_t)(uintptr_t)desc, &q, sizeof(q))) return 0;
    if (q < base) return 0;
    rva = (uint32_t)(q - base);
    if (rva < 0x1000 || rva > 0x40000000u) return 0;
    return rva;
    }

    /* The fingerprint of the description we hold - read while it is still ours to
    * read, then kept.
    *
    * The window is the one CmDetour opens: reads happen only inside it, and the
    * first valid one is remembered, so past the window nothing touches that
    * memory again (the game may have released it by then). Keeping the answer
    * also carries the mode through the description going away, which is what
    * happens when a session leaves a mode without entering another one - the
    * mode stays what it was last said to be, which is the truth for that
    * session. A new description (a new CreateGameMode) opens a new window and
    * clears what was kept, so the modes that can follow one another in a single
    * session - the campaign, then a PvP mode - are each read from their own. */
    static uint32_t FreshFingerprint(void) {
    LONG kept = InterlockedCompareExchange(&g_gmFpKept, 0, 0);
    uint32_t fp;

    if (kept) return (uint32_t)kept;
    if (!g_gmDescFresh) return 0;           /* its window has closed */
    fp = FingerprintOf();
    if (fp) InterlockedExchange(&g_gmFpKept, (LONG)fp);
    return fp;
    }

/* 1 when a reading belongs to this row: the entry the table recorded, or
 * the slot the game's description actually points at, which sits
 * MODE_DESC_BACK before it.
 *
 * Measured twice on 2026-09-17, both times to the byte: the campaign
 * arrived as 38DC760 = 38DC7F0 - 0x90, and Mercenaries as 38DD0E8 =
 * 38DD178 - 0x90. Two modes, one distance - and the entries themselves
 * are 0x1F8 or more apart (0x1F8 is the closest pair, Guerrilla to
 * Mercenaries), so a reading can only ever reach the row it belongs to.
 * Before this, both of those sessions read as "unrecognised mode object",
 * the mode stayed NONE and nothing was blocked in either of them. */
#define MODE_DESC_BACK 0x90u
static int FpIsRow(uint32_t fp, uint32_t rva) {
    return fp == rva || fp + MODE_DESC_BACK == rva;
}

static const char *NameOfFp(uint32_t fp) {
    int i;

    for (i = 0; i < GM_FP_N; i++)
        if (FpIsRow(fp, g_fp[i].rva)) return g_fp[i].name;
    return NULL;
}

/* (mode object, argument) -> the mode.
 *
 * The object is the mode, so it is what is looked up first, whatever the
 * argument says: the campaign has arrived with 0 and with 2. An object
 * that is not in the table leaves the mode undecided rather than guessed
 * at. The argument carries one thing on its own, Ghost War's 3, and the
 * watcher hands it over once (see the module header) so that it cannot
 * decide anything the game said later. The log line carries both, which
 * is all a new row needs. */
static int ResolveMode(LONG t, uint32_t fp) {
    int i;

    if (t < 0 && !fp) return SH_PLAYMODE_NONE;
    if (fp) {
        for (i = 0; i < GM_FP_N; i++)
            if (FpIsRow(fp, g_fp[i].rva)) return g_fp[i].mode;
        /* An object the table does not know does not throw away the one
         * argument that has only ever named a single mode. 3 has only ever
         * been Ghost War (measured again on 2026-09-17, entering it from
         * the front end: arg 3, object 03908CB8). 0 is not believed here
         * any more: the campaign is in the table (38DC7F0, and its
         * description 0x90 before it), so believing 0 added nothing for
         * the campaign, while a 0 left over from an earlier call answered
         * "campaign" for a PvP object the table did not know yet - and in
         * that answer's world nothing is blocked, which is how the plugins
         * stayed on in Ghost War and Mercenaries (2026-09-17). 2 stays
         * undecided: Ghost Mode, Mercenaries and Guerrilla all set it, and
         * only the object tells them apart. */
        if (t == GM_TYPE_GHOST_WAR) return SH_PLAYMODE_GHOST_WAR;
        return SH_PLAYMODE_NONE;
    }
    if (t == GM_TYPE_GHOST_WAR) return SH_PLAYMODE_GHOST_WAR;
    return SH_PLAYMODE_NONE;
}

/* Picks up what the detours stored, resolves it, and logs a change once.
 * Logging lives here because this thread has the stack for it - and
 * because reading the mode object is a memory read. */
static DWORD WINAPI ModeThread(LPVOID p) {
    DWORD lastTry = 0, now;
    int waited = 0;

    (void)p;
    for (;;) {
        LONG t;
        uint32_t fp;
        int mode;
        const char *name;

        Sleep(200);
        ShTickPing(SH_TICK_PLAYMODE);
        if (g_gmDescFresh > 0) g_gmDescFresh--;     /* the read window ages */
        now = GetTickCount();
        if (!(g_gmArmed && g_gmCmArmed) && now - lastTry >= GM_TRY_MS) {
            lastTry = now;
            GmInstall();
        }
        t = g_gmType;
        fp = FreshFingerprint();
        /* The object is the mode, and it arrives with CreateGameMode -
         * which is called whether or not SetCurrentGameMode ever was. An
         * argument that was never seen is not a reason to stop reading,
         * and this used to be exactly that: the watcher skipped everything
         * until SetCurrentGameMode had been called, so its object was
         * never looked at. Measured 2026-09-17 (this machine, the shipped
         * build): a whole session - the front end into Ghost War and into
         * Mercenaries - in which SetCurrentGameMode was never called once,
         * which left the mode undecided and nothing blocked in either PvP
         * mode. */
        /* Every call, up to a cap. The change-only lines below cannot tell
         * "the manager was never touched" from "it was touched and repeated
         * what it had already said", and that is exactly the question left
         * when a mode that should block does not - 2026-09-17, after the
         * game's September update: Mercenaries blocked correctly, while
         * entering Ghost War left no line anywhere in the log. */
        if ((LONG)g_gmCallsLogged != g_gmCalls && g_gmCalls <= CALL_LOG_MAX) {
            g_gmCallsLogged = g_gmCalls;
            Log("playmode: SetCurrentGameMode call %ld: arg %ld, mode "
                "object %08X", (long)g_gmCalls, (long)t, (unsigned)fp);
        }
        if ((LONG)g_gmCmLogged != g_gmCmCalls && g_gmCmCalls <= CALL_LOG_MAX) {
            g_gmCmLogged = g_gmCmCalls;
            Log("playmode: CreateGameMode call %ld: mode object %08X",
                (long)g_gmCmCalls, (unsigned)fp);
        }
        if (t < 0 && !fp) continue;
        if (fp) waited = 0;
        else if (++waited < 25)
            continue;           /* five seconds: the object is the answer */
        mode = ResolveMode(t, fp);
        /* One decision per argument: it is consumed the moment it has been
         * used, so whatever decides the next mode is the object - which is
         * the answer and cannot go stale. Cleared with a CAS, because the
         * hook stores to this from another thread: an argument the game set
         * while this decision was being made survives, and only the value
         * just used is taken back. The trade-off is deliberate - an object
         * the table does not know, arriving long after the 3 that would
         * have named it, is left undecided rather than believed. */
        if (t >= 0) InterlockedCompareExchange(&g_gmType, -1, t);
        /* A changed object is worth a line of its own even when the mode it
         * resolves to does not change: an object that is not in the table
         * is precisely the case the table needs a row for, and two unknown
         * objects in a row would otherwise go by unlogged. */
        if (t == g_gmLogged && fp == (uint32_t)g_gmFpLogged &&
            mode == g_gmMode)
            continue;
        g_gmLogged = t;
        g_gmFpLogged = (LONG)fp;
        g_gmMode = (LONG)mode;
        g_gmFp = (LONG)fp;
        name = fp ? NameOfFp(fp) : NULL;
        if (t < 0)
            Log("playmode: mode object %08X (%ld calls), no "
                "SetCurrentGameMode seen yet - %s", (unsigned)fp,
                (long)g_gmCalls,
                name ? name : (fp ? "unrecognised mode object"
                                  : "mode object not read yet"));
        else
            Log("playmode: the game set GameModeType %ld with mode object "
                "%08X (%ld calls) - %s", (long)t, (unsigned)fp,
                (long)g_gmCalls,
                name ? name : (fp ? "unrecognised mode object"
                                  : "mode object not read yet"));
    }
    return 0;
}

/* ---- the exports ---------------------------------------------------- */

SH_API int ShSelectedPlayMode(void) {
    return (int)g_gmMode;       /* resolved by the watcher; NONE until then */
}

SH_API int ShIsGhostWarMode(void) {
    return ShSelectedPlayMode() == SH_PLAYMODE_GHOST_WAR;
}

SH_API int ShIsMercenariesMode(void) {
    return ShSelectedPlayMode() == SH_PLAYMODE_MERCENARIES;
}

SH_API int ShIsGhostMode(void) {
    return ShSelectedPlayMode() == SH_PLAYMODE_GHOST_MODE;
}

SH_API int ShIsGuerrillaMode(void) {
    return ShSelectedPlayMode() == SH_PLAYMODE_GUERRILLA;
}

/* The mask bit a mode occupies, for the plugin blacklist: one bit per
 * mode, laid out in the order of the enum, so a mask reads the same way
 * the modes are listed above. Anything that is not a mode is 0 bits. */
SH_API uint32_t ShPlayModeBit(int mode) {
    if (mode < SH_PLAYMODE_GHOST_WAR || mode > SH_PLAYMODE_GUERRILLA)
        return 0;
    return 1u << (mode - SH_PLAYMODE_GHOST_WAR);
}

/* The name from the mode table, so the log lines, the settings page and
 * anything else cannot drift apart: "Ghost War", "MERCENARIES",
 * "campaign", "Ghost Mode", "Guerrilla". "" for NONE and for anything
 * that is not a mode. */
SH_API const char *ShPlayModeName(int mode) {
    int i;

    for (i = 0; i < GM_FP_N; i++)
        if (g_fp[i].mode == mode) return g_fp[i].name;
    return "";
}

SH_API int ShPlayModeFingerprint(void) {
    return (int)g_gmFp;
}

SH_API int ShPlayModeEvidence(char *buf, int len) {
    LONG t = g_gmType;
    int  fp = (int)g_gmFp;
    const char *name = fp ? NameOfFp((uint32_t)fp) : NULL;

    if (!buf || len < 1) return 0;
    if (!g_gmArmed || !g_gmCmArmed) {
        snprintf(buf, (size_t)len,
                 "the mode hooks are not armed on this build");
        return 0;
    }
    if (!fp) {
        if (t < 0)
            snprintf(buf, (size_t)len, "no mode object read yet, and %s",
                     g_gmCalls ? "the argument it set has been spent"
                               : "SetCurrentGameMode has not been called");
        else
            snprintf(buf, (size_t)len,
                     "GameModeType %ld, mode object not read yet", (long)t);
        return 0;
    }
    /* The object is what decides the mode, so an argument that is not held is
     * a detail rather than a reason to have no answer - the same reading the
     * watcher takes (2026-09-17: a session went into Ghost War and into
     * Mercenaries without SetCurrentGameMode being called once).
     *
     * "Not held" is no longer the same as "never called": the argument is
     * used once and dropped (ResolveMode and the watcher, 2026-09-17), so a
     * session that called SetCurrentGameMode at every mode change spends its
     * last one and reads here as t < 0. Both readings are in the line now,
     * because the first version said "SetCurrentGameMode not called" for a
     * session whose own playmode log had just said "call 1: arg 3" - seen in
     * the field log of 2026-09-17 and fixed the same day. */
    if (t < 0)
        snprintf(buf, (size_t)len,
                 "mode object %08X (%s); %s", (unsigned)fp,
                 name ? name : "not recognised",
                 g_gmCalls ? "the argument it set was used once"
                           : "SetCurrentGameMode has not been called");
    else
        snprintf(buf, (size_t)len,
                 "GameModeType %ld with mode object %08X (%s)", (long)t,
                 (unsigned)fp, name ? name : "not recognised");
    return ShSelectedPlayMode() != SH_PLAYMODE_NONE;
}

SH_API int ShPlayModeHookArmed(void) {
    return (g_gmArmed == 1 && g_gmCmArmed == 1) ? 1 : 0;
}

/* Called by the loader with the rest of the subsystems. */
void ShPlayModeStart(void) {
    if (InterlockedCompareExchange(&g_started, 1, 0)) return;

    /* The log is opened first: Log drops everything written before it,
     * and the install line is the one worth keeping (it went missing
     * once for exactly this reason - the install ran before the thread
     * that called LogInit). */
    LogInit("scripthook_playmode.log");

    g_enabled = ShConfigGetBool("playmode", "enabled", 1) ? 1 : 0;
    if (!g_enabled) {
        LogAlways("playmode: disabled in scripthook.ini");
        return;
    }
    GmInstall();
    {
        HANDLE h = CreateThread(NULL, 0, ModeThread, NULL, 0, NULL);

        if (!h) LogAlways("playmode: watcher thread failed to start");
        else    CloseHandle(h);   /* never waited on */
    }
}
