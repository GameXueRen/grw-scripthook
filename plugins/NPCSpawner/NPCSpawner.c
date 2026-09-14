/* Native NPC spawner.
 *
 * A behaviour-equivalent re-implementation of the third-party
 * plugin plugins\NPCSpawner\NPCSpawner.asi, recovered by
 * disassembly.  The original is a C++ plugin that late-binds
 * this framework's ScriptHook exports; this file keeps that
 * structure but is plain C so it builds with the rest of the
 * plugin tree.
 *
 * What it does
 * ------------
 *   NPC Group     Santa Blanca / Unidad / Rebels / Civilians / Special
 *   NPC Number    a three row spinner that walks the group's list
 *   Spawn Distance  the formation centre placed this far ahead
 *   Spawn Count   1 / 3 / 5 at once
 *   Formation     Line / Spread / Semicircle / Circle / Random
 *   Facing        Face Player / Face Forward
 *   Spawn Selected / Undo Last Spawn
 *
 * The group filter, the formation maths and the batch spawn now
 * live in the framework - ShNpcGroupOfArchetype,
 * ShNpcGroupSize, ShNpcAtInGroup, ShNpcPlanFormation and
 * ShNpcSpawnFormation - so this plugin keeps only what is its
 * own: the menu, the spinner, the tracked list and the undo.
 * Another plugin gets the same summon by calling those exports;
 * see scripthook.h and docs/npcspawner-reverse.md.
 *
 * Deviations from the original, all deliberate (see the report):
 *   - the tracked list is pruned of dead handles before the
 *     spawn limit is tested; the original only ever removed one
 *     entry on undo, so dead NPCs permanently ate the 50 budget;
 *   - the NPC Number row is snapped back to its middle entry
 *     with ShMenuSetValue instead of clearing and rebuilding the
 *     whole menu;
 *   - status lines go through ShMenuStatusF rather than a local
 *     sprintf buffer;
 *   - the F field of the spawn status is how many of the batch
 *     did not appear, where the original counted failed facing
 *     transforms, which were almost always zero.
 *
 * Logs to <gamedir>\logs\
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scripthook.h"
#include "log.h"

/* ---- catalogue limits and defaults ------------------------------ */

#define TRACK_MAX     128
#define SPAWN_LIMIT   50     /* total tracked, as in the original  */

/* Same value the original's status line prints as the total
 * denominator (0x32). */
#define REPORT_LIMIT  50

/* ---- late binding ----------------------------------------------- */

typedef int      (*GetVersion_t)(void);
typedef int      (*GetGameState_t)(void);
typedef int      (*GetEntityKind_t)(uint64_t);
typedef int      (*GetHealthEntity_t)(uint64_t, uint32_t *, uint32_t *);
typedef int      (*Despawn_t)(uint64_t);
typedef int      (*LastError_t)(void);
typedef uint32_t (*MenuCreate_t)(const char *);
typedef int      (*MenuList_t)(uint32_t, const char *, const char **,
                               int, int, ShMenuFn, void *);
typedef int      (*MenuAction_t)(uint32_t, const char *,
                                 ShMenuFn, void *);
typedef int      (*MenuStatus_t)(uint32_t, const char *);
typedef int      (*MenuStatusF_t)(uint32_t, const char *, ...);
typedef int      (*MenuSetValue_t)(uint32_t, const char *, int);
typedef int      (*NpcGroupSize_t)(int);
typedef int      (*NpcAtInGroup_t)(int, int, ShNpcArchetype *);
typedef int      (*SpawnFormation_t)(const ShNpcSpawnRequest *,
                                     uint64_t *, int);
typedef uint32_t (*SpawnBegin_t)(const ShNpcSpawnRequest *);
typedef int      (*SpawnPoll_t)(uint32_t, uint64_t *, int, int *);
typedef int      (*SpawnCancel_t)(uint32_t);
typedef int      (*SpawnEnd_t)(uint32_t);

/* The summon itself - the layout, the spawn and the facing - is
 * the framework's job now, reached through the last three.  This
 * plugin binds only what its own menu needs. */
static GetVersion_t        pGetVersion;
static GetGameState_t      pGetGameState;
static GetEntityKind_t     pGetEntityKind;
static GetHealthEntity_t   pGetHealthEntity;
static Despawn_t           pDespawn;
static LastError_t         pLastError;
static MenuCreate_t        pMenuCreate;
static MenuList_t          pMenuList;
static MenuAction_t        pMenuAction;
static MenuStatus_t        pMenuStatus;
static MenuStatusF_t       pMenuStatusF;
static MenuSetValue_t      pMenuSetValue;
static NpcGroupSize_t      pNpcGroupSize;
static NpcAtInGroup_t      pNpcAtInGroup;
static SpawnFormation_t    pSpawnFormation;
/* Optional: only the self test row needs the batch handle. */
static SpawnBegin_t        pSpawnBegin;
static SpawnPoll_t         pSpawnPoll;
static SpawnCancel_t       pSpawnCancel;
static SpawnEnd_t          pSpawnEnd;

/* ---- option labels (the original's .rdata tables) --------------- */

static const char *g_formationOpts[5] = {
    "Line", "Spread", "Semicircle", "Circle", "Random"
};
static const char *g_facingOpts[2] = { "Face Player", "Face Forward" };
static const char *g_groupOpts[5] = {
    "Santa Blanca", "Unidad", "Rebels", "Civilians", "Special"
};
static const char *g_distanceOpts[6] = {
    "10 m", "20 m", "30 m", "50 m", "75 m", "100 m"
};
static const char *g_countOpts[3] = { "1", "3", "5" };

/* The real numbers behind those two list rows. */
static const float g_distanceM[6] = { 10.0f, 20.0f, 30.0f,
                                      50.0f, 75.0f, 100.0f };
static const int   g_countN[3]    = { 1, 3, 5 };

/* ---- state ------------------------------------------------------ */

static uint32_t g_menu = 0;

static int g_group     = SH_NPC_GROUP_SANTA_BLANCA;
static int g_sel[SH_NPC_GROUP_MAX] = { 1, 1, 1, 1, 1 }; /* 1 based */
static int g_distIdx   = 2;      /* 30 m   */
static int g_countIdx  = 0;      /* 1      */
static int g_formation = SH_NPC_FORMATION_RANDOM;
static int g_facing    = SH_NPC_FACING_PLAYER;

static int g_groupCount = 0;     /* size of the current group */

/* The three spinner labels, rewritten in place.  The framework
 * borrows the pointers, so a rewrite shows up on the next
 * capture without touching the menu. */
static char g_numLabel[3][16];
static const char *g_numOpts[3] = {
    g_numLabel[0], g_numLabel[1], g_numLabel[2]
};

/* Tracked spawns and the undo target. */
static CRITICAL_SECTION g_trackLock;
static volatile LONG g_locksInit = 0;
static uint64_t g_track[TRACK_MAX];
static int      g_trackN = 0;
static uint64_t g_last = 0;

/* Busy flag: a spawn or undo is in flight. */
static volatile LONG g_busy = 0;

/* Held while the self test thread runs, so two clicks do not
 * stack up. */
static volatile LONG g_selfRun = 0;
/* [Settings] selftest from our own ini; shows the row when on. */
static int g_selfOn = 0;

/* ---- our own ini ------------------------------------------------ */

static HINSTANCE g_inst = NULL;
static char      g_iniPath[MAX_PATH];

/* plugins\<name>\<name>.ini, derived from our own module file
 * name, so the pairing survives a rename.  Same shape as
 * firstperson's and GhostNoWipe's. */
static void ResolveIniPath(void) {
    char mod[MAX_PATH];
    const char *base, *dot;
    size_t n;

    g_iniPath[0] = 0;
    if (!g_inst || !GetModuleFileNameA(g_inst, mod, sizeof(mod))) return;

    /* Only a dot in the file name counts: a folder may hold one. */
    base = strrchr(mod, '\\');
    dot = strrchr(mod, '.');
    if (dot && base && dot < base) dot = NULL;

    n = dot ? (size_t)(dot - mod) : strlen(mod);
    if (n >= sizeof(g_iniPath)) n = sizeof(g_iniPath) - 1;
    memcpy(g_iniPath, mod, n);
    g_iniPath[n] = 0;
    strncat(g_iniPath, ".ini", sizeof(g_iniPath) - n - 1);
}

static int IniInt(const char *key, int def) {
    if (!g_iniPath[0]) return def;
    return GetPrivateProfileIntA("Settings", key, def, g_iniPath);
}

/* The archetype id tables and the classifier that used them now
 * live in the framework (scripthook_npc.c), reached through
 * ShNpcGroupOfArchetype.  Keeping one copy here as well is how
 * the two drift apart.
 */

/* ---- helpers ---------------------------------------------------- */

static void EnsureLocks(void) {
    while (!g_locksInit) {
        if (InterlockedCompareExchange(&g_locksInit, 2, 0) == 0) {
            InitializeCriticalSection(&g_trackLock);
            LogInit("scripthook_npcspawner.log");
            InterlockedExchange(&g_locksInit, 1);
        }
    }
}

static void SetStatus(const char *text) {
    if (g_menu && pMenuStatus) pMenuStatus(g_menu, text);
}

static int TryBusy(void) {
    return InterlockedCompareExchange(&g_busy, 1, 0) == 0;
}

static void ReleaseBusy(void) {
    InterlockedExchange(&g_busy, 0);
}

/* The two states the original refuses to spawn in. */
static int WorldBusy(void) {
    int s;
    if (!pGetGameState) return 0;
    s = pGetGameState();
    return s == SH_STATE_LOADING || s == SH_STATE_RELOADING;
}

/* Count the group, wrap this group's spinner back into range,
 * then return the catalogue index and archetype id it points at
 * (or -1).  sel wraps rather than clamps, exactly as the
 * original's 0x180001340 does, so the spinner is endless.  The
 * walk itself is the framework's.
 */
static int GroupScan(int group, int *outCount, uint64_t *outId) {
    ShNpcArchetype a;
    int c, idx;

    if (outCount) *outCount = 0;
    if (outId) *outId = 0;
    if (!pNpcGroupSize || !pNpcAtInGroup) return -1;

    c = pNpcGroupSize(group);
    if (c <= 0) return -1;

    if (g_sel[group] < 1) g_sel[group] = c;
    else if (g_sel[group] > c) g_sel[group] = 1;

    memset(&a, 0, sizeof(a));
    idx = pNpcAtInGroup(group, g_sel[group] - 1, &a);

    if (outCount) *outCount = c;
    if (outId) *outId = a.id;
    return idx;
}

/* Rewrite the three spinner labels: previous / current / next. */
static void UpdateNumberLabels(void) {
    int sel = g_sel[g_group];
    int count = g_groupCount > 0 ? g_groupCount : 1;
    int lo = sel - 1 < 1 ? count : sel - 1;
    int hi = sel + 1 > count ? 1 : sel + 1;

    snprintf(g_numLabel[0], sizeof(g_numLabel[0]), "%d", lo);
    snprintf(g_numLabel[1], sizeof(g_numLabel[1]), "%d", sel);
    snprintf(g_numLabel[2], sizeof(g_numLabel[2]), "%d", hi);
}

/* The line under the menu, matching the original's 0x180001340. */
static void RefreshStatus(void) {
    int count = 0, idx;
    uint64_t id = 0;

    if (!g_menu) return;
    idx = GroupScan(g_group, &count, &id);
    g_groupCount = count;
    UpdateNumberLabels();

    if (count <= 0) { SetStatus("@np.st.noentries"); return; }
    if (idx < 0) { SetStatus("@np.st.nosel"); return; }
    if (pMenuStatusF)
        pMenuStatusF(g_menu, "@np.st.count",
                     g_sel[g_group], count, idx, REPORT_LIMIT);
    /* The row the user just moved snaps back to the middle, so
     * the row keeps showing current - 1 / current / current + 1. */
    if (pMenuSetValue) pMenuSetValue(g_menu, "@np.number", 1);
}

/* Drop tracked handles whose entity is gone, so the 50 budget
 * is not permanently eaten by corpses. */
static void PruneTrack(void) {
    int i, w = 0;

    EnsureLocks();
    EnterCriticalSection(&g_trackLock);
    for (i = 0; i < g_trackN; i++) {
        uint32_t c = 0, m = 0;
        if (pGetHealthEntity(g_track[i], &c, &m)) g_track[w++] = g_track[i];
    }
    if (w != g_trackN) Log("npcspawner: pruned %d of %d", g_trackN - w, g_trackN);
    g_trackN = w;
    LeaveCriticalSection(&g_trackLock);
}

static void TrackRemove(uint64_t e) {
    int i, w = 0;

    EnterCriticalSection(&g_trackLock);
    for (i = 0; i < g_trackN; i++)
        if (g_track[i] != e) g_track[w++] = g_track[i];
    g_trackN = w;
    if (g_last == e) g_last = 0;
    LeaveCriticalSection(&g_trackLock);
}

/* ---- the spawn worker (mirrors 0x180002070) --------------------- */

/* The layout, the spawn and the facing all belong to
 * ShNpcSpawnFormation now.  This thread exists only so the menu
 * callback does not block on it: a batch of five waits on five
 * entities in turn, and the first spawn of an archetype streams
 * its assets in.
 */
static DWORD WINAPI SpawnWorker(LPVOID p) {
    ShNpcSpawnRequest req;
    uint64_t ent[SH_NPC_SPAWN_MAX];
    int got = 0, i, count, alive;

    if (!p) { ReleaseBusy(); return 0; }
    req = *(ShNpcSpawnRequest *)p;
    free(p);

    count = req.count;
    got = pSpawnFormation(&req, ent, SH_NPC_SPAWN_MAX);

    Log("npcspawner: spawn id=%llx n=%d form=%d face=%d dist=%.0f got=%d",
        (unsigned long long)req.id, count, req.formation, req.facing,
        req.distance, got);

    EnterCriticalSection(&g_trackLock);
    for (i = 0; i < got; i++) {
        if (g_trackN < TRACK_MAX) g_track[g_trackN++] = ent[i];
        g_last = ent[i];
    }
    LeaveCriticalSection(&g_trackLock);

    ReleaseBusy();

    if (WorldBusy()) return 0;

    if (got == 0) {
        if (pLastError && pLastError() == SH_ERR_NO_POSITION)
            SetStatus("@np.st.noplayer");
        else
            SetStatus("@np.st.spawnfail");
        return 0;
    }

    EnterCriticalSection(&g_trackLock);
    alive = g_trackN;
    LeaveCriticalSection(&g_trackLock);

    if (got == count) {
        RefreshStatus();
        return 0;
    }
    /* F is what did not turn up; the original counted failed
     * facing transforms here instead. */
    if (pMenuStatusF)
        pMenuStatusF(g_menu, "@np.st.spawned",
                     got, count, count - got, alive, REPORT_LIMIT);
    return 0;
}

/* ---- the undo worker (mirrors 0x180002490) ---------------------- */

static DWORD WINAPI UndoWorker(LPVOID p) {
    uint64_t e;
    int kind, ok;
    uint32_t cur = 0, max = 0;
    (void)p;

    EnterCriticalSection(&g_trackLock);
    e = g_last;
    g_last = 0;
    LeaveCriticalSection(&g_trackLock);

    if (!e) {
        SetStatus("@np.st.noundo");
        ReleaseBusy();
        return 0;
    }
    if (WorldBusy()) { ReleaseBusy(); return 0; }

    kind = pGetEntityKind(e);
    if (!kind) {
        TrackRemove(e);
        SetStatus("@np.st.undounavail");
        ReleaseBusy();
        return 0;
    }
    if (!pGetHealthEntity(e, &cur, &max)) {
        TrackRemove(e);
        SetStatus("@np.st.undounavail");
        ReleaseBusy();
        return 0;
    }
    if (cur == 0) {
        SetStatus("@np.st.dead");
        ReleaseBusy();
        return 0;
    }

    ok = pDespawn(e);
    if (ok) TrackRemove(e);
    ReleaseBusy();
    if (WorldBusy()) return 0;

    Log("npcspawner: undo ent=%llx ok=%d", (unsigned long long)e, ok);
    if (!ok) SetStatus("@np.st.undofail");
    else RefreshStatus();
    return 0;
}

/* ---- menu callbacks --------------------------------------------- */

static void OnNumber(uint32_t menu, uint32_t item, int value, void *user) {
    int count = g_groupCount;
    int before;
    (void)menu; (void)item; (void)user;

    if (count <= 0) return;
    before = g_sel[g_group];
    if (value == 0) {
        if (--g_sel[g_group] < 1) g_sel[g_group] = count;   /* wrap down */
    } else if (value == 2) {
        if (++g_sel[g_group] > count) g_sel[g_group] = 1;   /* wrap up */
    }
    if (g_sel[g_group] != before)
        Log("npcspawner: number %d/%d", g_sel[g_group], count);
    RefreshStatus();
}

static void OnSpawnSelected(uint32_t menu, uint32_t item, int value,
                            void *user) {
    ShNpcSpawnRequest *r;
    HANDLE h;
    int count = 0, idx, alive;
    uint64_t id = 0;
    (void)menu; (void)item; (void)value; (void)user;

    if (!TryBusy()) { SetStatus("@np.st.busy"); return; }

    PruneTrack();

    EnterCriticalSection(&g_trackLock);
    alive = g_trackN;
    LeaveCriticalSection(&g_trackLock);

    if (alive + g_countN[g_countIdx] > SPAWN_LIMIT) {
        if (pMenuStatusF)
            pMenuStatusF(g_menu, "@np.st.limit", alive, REPORT_LIMIT);
        ReleaseBusy();
        return;
    }

    idx = GroupScan(g_group, &count, &id);
    if (count <= 0 || idx < 0 || !id) {
        SetStatus(count <= 0 ? "@np.st.noentries" : "@np.st.nosel");
        ReleaseBusy();
        return;
    }

    r = (ShNpcSpawnRequest *)malloc(sizeof(*r));
    if (!r) {
        SetStatus("@np.st.spawnreq");
        ReleaseBusy();
        return;
    }
    r->id = id;
    r->distance = g_distanceM[g_distIdx];
    r->count = g_countN[g_countIdx];
    r->formation = g_formation;
    r->facing = g_facing;

    h = CreateThread(NULL, 0, SpawnWorker, r, 0, NULL);
    if (!h) {
        free(r);
        SetStatus("@np.st.worker");
        ReleaseBusy();
        return;
    }
    CloseHandle(h);
}

static void OnUndo(uint32_t menu, uint32_t item, int value, void *user) {
    HANDLE h;
    (void)menu; (void)item; (void)value; (void)user;

    if (!TryBusy()) { SetStatus("@np.st.busy"); return; }

    h = CreateThread(NULL, 0, UndoWorker, NULL, 0, NULL);
    if (!h) {
        SetStatus("@np.st.worker");
        ReleaseBusy();
    } else {
        CloseHandle(h);
    }
}

static void OnDistance(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    if (value >= 0 && value < 6) g_distIdx = value;
    RefreshStatus();
}

static void OnCount(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    if (value >= 0 && value < 3) g_countIdx = value;
    RefreshStatus();
}

static void OnFormation(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    if (value >= 0 && value < 5) g_formation = value;
    RefreshStatus();
}

static void OnFacing(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    if (value >= 0 && value < 2) g_facing = value;
    RefreshStatus();
}

static void OnGroup(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    if (value < 0 || value >= SH_NPC_GROUP_MAX) return;
    g_group = value;
    Log("npcspawner: group %d", g_group);
    RefreshStatus();
}

/* ---- the self test ----------------------------------------------- */

/* A scripted walk of the batch API, off by default and toggled by
 * [Settings] selftest in this plugin's own ini.  It exists to be
 * read back in the log: every expected value is printed beside
 * what actually came back.  It spawns real NPCs and deliberately
 * leaves them out of the undo list, so a diagnostic never spends
 * the 50 budget the menu is tracking.
 */
static void SelfTestRun(void) {
    ShNpcSpawnRequest req;
    int count = 0, idx, n, done, i;
    uint64_t id = 0;
    uint32_t job;

    idx = GroupScan(g_group, &count, &id);
    if (count <= 0 || idx < 0 || !id) {
        Log("selftest: no selection in group %d, nothing to do", g_group);
        SetStatus("@np.st.nosel");
        return;
    }
    Log("selftest: start, group %d id %llx", g_group,
        (unsigned long long)id);

    req.id = id;
    req.count = 5;
    req.distance = g_distanceM[g_distIdx];
    req.facing = SH_NPC_FACING_PLAYER;

    /* 1: handle semantics - End refuses while running, Cancel is
     *    safe, End frees, and the id is dead afterwards. */
    req.formation = SH_NPC_FORMATION_RANDOM;
    job = pSpawnBegin(&req);
    Log("selftest: 1 begin -> %u (want non-zero)", (unsigned)job);
    if (!job) goto done;

    Log("selftest: 1 End while running -> %d (want 0)", pSpawnEnd(job));
    Log("selftest: 1 Cancel -> %d (want 1)", pSpawnCancel(job));

    done = 0;
    for (i = 0; i < 60 && !done; i++) {
        Sleep(200);
        pSpawnPoll(job, NULL, 0, &done);
    }
    n = pSpawnPoll(job, NULL, 0, &done);
    Log("selftest: 1 stopped at n=%d done=%d (n should be small)", n, done);
    Log("selftest: 1 End -> %d (want 1)", pSpawnEnd(job));
    Log("selftest: 1 Poll after End -> %d (want -1)",
        pSpawnPoll(job, NULL, 0, NULL));

    /* 2: a batch left alone, so the poll can be watched rising */
    req.formation = SH_NPC_FORMATION_SPREAD;
    job = pSpawnBegin(&req);
    Log("selftest: 2 begin -> %u (want non-zero)", (unsigned)job);
    if (!job) goto done;

    n = 0;
    done = 0;
    /* 30 ms, because the engine is quick once the archetype is
     * streamed: five of them land in about 275 ms, so a coarse
     * sample only ever catches the final count. */
    for (i = 0; i < 200 && !done; i++) {
        int was = n, d2 = 0;
        Sleep(30);
        n = pSpawnPoll(job, NULL, 0, &d2);
        if (n != was || d2 != done)
            Log("selftest: 2 poll n=%d done=%d", n, d2);
        done = d2;
    }
    Log("selftest: 2 final n=%d done=%d (want n=5)", n, done);
    Log("selftest: 2 End -> %d (want 1)", pSpawnEnd(job));

    /* 3: cancel in the middle of a batch big enough to catch */
    req.count = 20;
    job = pSpawnBegin(&req);
    Log("selftest: 3 begin count=20 -> %u (want non-zero)", (unsigned)job);
    if (!job) goto done;

    /* Watch it fill up first, then pull the plug at about half.
     * Twenty take roughly 1.1 s, so cancelling at 1 s sat on the
     * edge of being finished anyway and could report n=20 by
     * luck. */
    n = 0;
    done = 0;
    for (i = 0; i < 5 && !done; i++) {
        int was = n, d2 = 0;
        Sleep(100);
        n = pSpawnPoll(job, NULL, 0, &d2);
        if (n != was) Log("selftest: 3 poll n=%d done=%d", n, d2);
        done = d2;
    }
    Log("selftest: 3 Cancel at ~0.5s -> %d (want 1)", pSpawnCancel(job));

    done = 0;
    for (i = 0; i < 120 && !done; i++) {
        Sleep(200);
        pSpawnPoll(job, NULL, 0, &done);
    }
    n = pSpawnPoll(job, NULL, 0, &done);
    Log("selftest: 3 stopped at n=%d of 20 done=%d (want n below 20)",
        n, done);
    Log("selftest: 3 End -> %d (want 1)", pSpawnEnd(job));

done:
    Log("selftest: finished; the NPCs it made are NOT in the undo list");
    SetStatus("@np.st.selfdone");
}

static DWORD WINAPI SelfTestThread(LPVOID p) {
    (void)p;
    SelfTestRun();
    InterlockedExchange(&g_selfRun, 0);
    return 0;
}

static void OnSelfTest(uint32_t menu, uint32_t item, int value,
                       void *user) {
    HANDLE h;
    (void)menu; (void)item; (void)value; (void)user;

    if (InterlockedCompareExchange(&g_selfRun, 1, 0)) {
        SetStatus("@np.st.selfbusy");
        return;
    }
    h = CreateThread(NULL, 0, SelfTestThread, NULL, 0, NULL);
    if (!h) {
        InterlockedExchange(&g_selfRun, 0);
        SetStatus("@np.st.worker");
        return;
    }
    CloseHandle(h);
    SetStatus("@np.st.selfrun");
}

/* ---- menu build ------------------------------------------------- */

/* ---- text ---------------------------------------------------------
 * The plugin's own text, compiled in: lang.ini beside this source only
 * has to carry what it changes, and the menu reads with or without it.
 * The keys are stable IDs, so rewording a row never breaks a
 * translation. This plugin resolves the framework by name; the tables
 * are declared through the same kind of pointer.
 */
typedef struct { const char *key; const char *text; } TextRow;
typedef int (*LangDeclare_t)(const char *owner, const char *lang,
                             const TextRow *rows, int n);
static LangDeclare_t pLangDeclare;

static const TextRow kEn[] = {
    { "@np.page",        "Native NPC Spawner" },
    { "@np.number",      "NPC Number" },
    { "@np.spawn",       "Spawn Selected" },
    { "@np.undo",        "Undo Last Spawn" },
    { "@np.distance",    "Spawn Distance" },
    { "@np.count",       "Spawn Count" },
    { "@np.formation",   "Formation" },
    { "@np.facing",      "Facing" },
    { "@np.group",       "NPC Group" },
    { "@np.selftest",    "Self test" },
    { "@np.st.count",    "%d/%d | Total %d/%d" },
    { "@np.st.limit",    "Limit | Total %d/%d" },
    { "@np.st.spawned",  "Spawn %d/%d | F %d | Total %d/%d" },
    { "@np.st.busy",     "Busy" },
    { "@np.st.worker",   "Could not start worker" },
    { "@np.st.dead",     "Last spawn is dead" },
    { "@np.st.noentries", "No entries" },
    { "@np.st.noundo",   "Nothing to undo" },
    { "@np.st.noplayer", "Player unavailable" },
    { "@np.st.nosel",    "Selection unavailable" },
    { "@np.st.selfbusy", "Self test already running" },
    { "@np.st.selfdone", "Self test done" },
    { "@np.st.selfrun",  "Self test running" },
    { "@np.st.spawnfail", "Spawn failed" },
    { "@np.st.spawnreq", "Spawn request failed" },
    { "@np.st.undofail", "Undo failed" },
    { "@np.st.undounavail", "Undo target unavailable" }
};

static const TextRow kZh[] = {
    { "@np.page",        "原生 NPC 生成器" },
    { "@np.number",      "NPC 编号" },
    { "@np.spawn",       "生成已选 NPC" },
    { "@np.undo",        "撤销上次生成" },
    { "@np.distance",    "生成距离" },
    { "@np.count",       "生成数量" },
    { "@np.formation",   "阵型" },
    { "@np.facing",      "朝向" },
    { "@np.group",       "NPC 阵营" },
    { "@np.selftest",    "自检（异步召唤）" },
    { "@np.st.count",    "%d/%d | 总计 %d/%d" },
    { "@np.st.limit",    "已达上限 | 总计 %d/%d" },
    { "@np.st.spawned",  "已生成 %d/%d | 未出现 %d | 总计 %d/%d" },
    { "@np.st.busy",     "正在处理" },
    { "@np.st.worker",   "无法启动工作线程" },
    { "@np.st.dead",     "上次生成的目标已死亡" },
    { "@np.st.noentries", "无条目" },
    { "@np.st.noundo",   "没有可撤销的生成" },
    { "@np.st.noplayer", "无法获取玩家位置" },
    { "@np.st.nosel",    "无可选条目" },
    { "@np.st.selfbusy", "自检已在运行" },
    { "@np.st.selfdone", "自检完成，详见日志" },
    { "@np.st.selfrun",  "自检进行中，详见日志" },
    { "@np.st.spawnfail", "生成失败" },
    { "@np.st.spawnreq", "生成请求失败" },
    { "@np.st.undofail", "撤销失败" },
    { "@np.st.undounavail", "撤销目标不可用" }
};

static void TextInit(void) {
    static int done;
    HMODULE m;

    if (done) return;
    m = GetModuleHandleA("dinput8.dll");
    if (!m) return;
    if (!pLangDeclare)
        *(FARPROC *)&pLangDeclare = GetProcAddress(m, "ShLangDeclare");
    if (!pLangDeclare) return;
    done = 1;
    pLangDeclare("NPCSpawner", "en-US", kEn,
                 (int)(sizeof(kEn) / sizeof(kEn[0])));
    pLangDeclare("NPCSpawner", "zh-CN", kZh,
                 (int)(sizeof(kZh) / sizeof(kZh[0])));
}

/* The menu the original offers, with this build's own labels. The
 * option lists (spinner, distance, count, formation, facing, group)
 * carry catalogue or numeric labels and are left as they are. */
static uint32_t BuildMenu(void) {
    uint32_t m;

    TextInit();
    m = pMenuCreate("@np.page");
    if (!m) return 0;

    /* Rebuild the spinner labels before the row is added, so the
     * first capture is already correct. */
    UpdateNumberLabels();

    pMenuList(m, "@np.number", g_numOpts, 3, 1, OnNumber, NULL);
    pMenuAction(m, "@np.spawn", OnSpawnSelected, NULL);
    pMenuAction(m, "@np.undo", OnUndo, NULL);
    pMenuList(m, "@np.distance", g_distanceOpts, 6, g_distIdx,
              OnDistance, NULL);
    pMenuList(m, "@np.count", g_countOpts, 3, g_countIdx, OnCount, NULL);
    pMenuList(m, "@np.formation", g_formationOpts, 5, g_formation,
              OnFormation, NULL);
    pMenuList(m, "@np.facing", g_facingOpts, 2, g_facing, OnFacing, NULL);
    pMenuList(m, "@np.group", g_groupOpts, 5, g_group, OnGroup, NULL);

    /* Last, and only when asked for: [Settings] selftest in this
     * plugin's own ini.  It is a diagnostic, not part of the
     * summon the original offered. */
    if (g_selfOn && pSpawnBegin && pSpawnPoll && pSpawnCancel &&
        pSpawnEnd)
        pMenuAction(m, "@np.selftest", OnSelfTest, NULL);

    RefreshStatus();
    return m;
}

/* ---- startup ---------------------------------------------------- */

static DWORD WINAPI BindThread(LPVOID p) {
    HMODULE mod = NULL;
    uint32_t menu;
    (void)p;

    EnsureLocks();

    /* The loader starts plugins after dinput8.dll is up, but bind
     * defensively so the plugin also survives other loaders. */
    while (!mod) {
        mod = GetModuleHandleA("dinput8.dll");
        if (!mod) Sleep(500);
    }

    *(FARPROC *)&pGetVersion = GetProcAddress(mod, "ShGetVersion");
    *(FARPROC *)&pGetGameState = GetProcAddress(mod, "ShGetGameState");
    *(FARPROC *)&pGetEntityKind = GetProcAddress(mod, "ShGetEntityKind");
    *(FARPROC *)&pGetHealthEntity = GetProcAddress(mod, "ShGetHealthEntity");
    *(FARPROC *)&pDespawn = GetProcAddress(mod, "ShDespawn");
    *(FARPROC *)&pMenuCreate = GetProcAddress(mod, "ShMenuCreate");
    *(FARPROC *)&pMenuList = GetProcAddress(mod, "ShMenuList");
    *(FARPROC *)&pMenuAction = GetProcAddress(mod, "ShMenuAction");
    *(FARPROC *)&pMenuStatus = GetProcAddress(mod, "ShMenuStatus");
    *(FARPROC *)&pMenuStatusF = GetProcAddress(mod, "ShMenuStatusF");
    *(FARPROC *)&pMenuSetValue = GetProcAddress(mod, "ShMenuSetValue");
    /* Optional: it only tells a missing player apart from a
     * batch that simply would not appear. */
    *(FARPROC *)&pLastError = GetProcAddress(mod, "ShLastError");

    /* The summon API itself.  Without it this menu has nothing to
     * offer, so the plugin stands down rather than adding rows
     * that cannot work. */
    *(FARPROC *)&pNpcGroupSize = GetProcAddress(mod, "ShNpcGroupSize");
    *(FARPROC *)&pNpcAtInGroup = GetProcAddress(mod, "ShNpcAtInGroup");
    *(FARPROC *)&pSpawnFormation =
        GetProcAddress(mod, "ShNpcSpawnFormation");

    if (!pGetVersion || !pGetGameState || !pGetEntityKind ||
        !pGetHealthEntity || !pDespawn || !pMenuCreate || !pMenuList ||
        !pMenuAction || !pMenuStatus || !pNpcGroupSize ||
        !pNpcAtInGroup || !pSpawnFormation) {
        Log("npcspawner: required export missing, giving up");
        return 1;
    }

    /* The API is up once ShGetVersion answers. */
    while (!pGetVersion()) Sleep(500);

    ResolveIniPath();
    g_selfOn = IniInt("selftest", 0) ? 1 : 0;

    /* Optional: the batch handle, which only the self test uses. */
    *(FARPROC *)&pSpawnBegin = GetProcAddress(mod, "ShNpcSpawnBegin");
    *(FARPROC *)&pSpawnPoll = GetProcAddress(mod, "ShNpcSpawnPoll");
    *(FARPROC *)&pSpawnCancel = GetProcAddress(mod, "ShNpcSpawnCancel");
    *(FARPROC *)&pSpawnEnd = GetProcAddress(mod, "ShNpcSpawnEnd");

    Log("npcspawner: up, ini=%s selftest=%d",
        g_iniPath[0] ? g_iniPath : "(none)", g_selfOn);
    if (g_selfOn &&
        (!pSpawnBegin || !pSpawnPoll || !pSpawnCancel || !pSpawnEnd))
        Log("npcspawner: selftest asked for but the batch exports are gone");
    menu = BuildMenu();
    if (!menu) {
        Log("npcspawner: menu create failed");
        return 1;
    }
    g_menu = menu;
    RefreshStatus();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        g_inst = inst;
        CreateThread(NULL, 0, BindThread, NULL, 0, NULL);
    }
    return TRUE;
}
