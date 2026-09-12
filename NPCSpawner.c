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
 * The interesting part is the group filter: the framework's
 * ShNpcArchetype carries only {id, kind} with no name, so the
 * original hardcodes four archetype id tables (plus one lone
 * Unidad id) and falls back to the engine's kind value.  Those
 * tables are reproduced verbatim below; the evidence for them
 * is in docs/npcspawner-reverse.md.
 *
 * Deviations from the original, all deliberate (see the report):
 *   - the tracked list is pruned of dead handles before the
 *     spawn limit is tested; the original only ever removed one
 *     entry on undo, so dead NPCs permanently ate the 50 budget;
 *   - the NPC Number row is snapped back to its middle entry
 *     with ShMenuSetValue instead of clearing and rebuilding the
 *     whole menu;
 *   - status lines go through ShMenuStatusF rather than a local
 *     sprintf buffer.
 *
 * Logs to <gamedir>\logs\
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "scripthook.h"
#include "log.h"

/* ---- catalogue limits and defaults ------------------------------ */

#define GROUP_MAX     5
#define PLAN_MAX      8      /* the menu only ever asks for 1..5 */
#define TRACK_MAX     128
#define SPAWN_LIMIT   50     /* total tracked, as in the original  */

/* Same value the original's status line prints as the total
 * denominator (0x32). */
#define REPORT_LIMIT  50

enum {
    G_SANTA_BLANCA = 0, G_UNIDAD, G_REBELS, G_CIVILIANS, G_SPECIAL
};

enum {
    F_LINE = 0, F_SPREAD, F_SEMICIRCLE, F_CIRCLE, F_RANDOM
};

enum {
    FACE_PLAYER = 0, FACE_FORWARD
};

/* ---- late binding ----------------------------------------------- */

typedef int      (*GetVersion_t)(void);
typedef int      (*GetGameState_t)(void);
typedef int      (*GetPlayer_t)(ShPlayer *);
typedef int      (*GetPlayerPosition_t)(ShVec3 *);
typedef int      (*GetEntityTransform_t)(uint64_t, ShVec3 *,
                                         float *, float *, float *);
typedef int      (*GetEntityKind_t)(uint64_t);
typedef int      (*GetHealthEntity_t)(uint64_t, uint32_t *, uint32_t *);
typedef int      (*QueueTransform_t)(uint64_t, const ShVec3 *,
                                     float, float, float);
typedef int      (*NpcCount_t)(void);
typedef const ShNpcArchetype *(*NpcAt_t)(int);
typedef uint64_t (*SpawnNpc_t)(uint64_t, const ShVec3 *);
typedef int      (*Despawn_t)(uint64_t);
typedef uint32_t (*MenuCreate_t)(const char *);
typedef int      (*MenuList_t)(uint32_t, const char *, const char **,
                               int, int, ShMenuFn, void *);
typedef int      (*MenuAction_t)(uint32_t, const char *,
                                 ShMenuFn, void *);
typedef int      (*MenuStatus_t)(uint32_t, const char *);
typedef int      (*MenuStatusF_t)(uint32_t, const char *, ...);
typedef int      (*MenuSetValue_t)(uint32_t, const char *, int);

static GetVersion_t        pGetVersion;
static GetGameState_t      pGetGameState;
static GetPlayer_t         pGetPlayer;
static GetPlayerPosition_t pGetPlayerPosition;
static GetEntityTransform_t pGetEntityTransform;
static GetEntityKind_t     pGetEntityKind;
static GetHealthEntity_t   pGetHealthEntity;
static QueueTransform_t    pQueueTransform;
static NpcCount_t          pNpcCount;
static NpcAt_t             pNpcAt;
static SpawnNpc_t          pSpawnNpc;
static Despawn_t           pDespawn;
static MenuCreate_t        pMenuCreate;
static MenuList_t          pMenuList;
static MenuAction_t        pMenuAction;
static MenuStatus_t        pMenuStatus;
static MenuStatusF_t       pMenuStatusF;
static MenuSetValue_t      pMenuSetValue;

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

static int g_group     = G_SANTA_BLANCA;
static int g_sel[GROUP_MAX] = { 1, 1, 1, 1, 1 };  /* 1 based, per group */
static int g_distIdx   = 2;      /* 30 m   */
static int g_countIdx  = 0;      /* 1      */
static int g_formation = F_RANDOM;
static int g_facing    = FACE_PLAYER;

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

/* ---- the archetype group tables (verbatim from the binary) ------ */

/* Never spawned: blacklisted by the original. @0x180007C00 */
static const uint64_t g_blacklist[44] = {
    0x5325BD7A52ULL, 0x78FADA79FFULL, 0x5325BD7A4DULL, 0x78FADA4D05ULL,
    0x5325BD7A48ULL, 0x31B65512F7ULL, 0x10F2A192C3DULL, 0x10F2A192C35ULL,
    0x3D66E0ABDFULL, 0x2309B3C694ULL, 0x5325BD7627ULL, 0x2FD455483EULL,
    0x237A1CBFBAULL, 0x237A1CBFB9ULL, 0x10F2A192C39ULL, 0x4738A95B71ULL,
    0x51021FED77ULL, 0x4FEB645A8DULL, 0x4FEB645A8EULL, 0x4AC59FEDD9ULL,
    0x4FEB647516ULL, 0x4FEB6459B2ULL, 0x4AC59FEDD7ULL, 0x4FEB6459EBULL,
    0x4D8AB38F5AULL, 0x4FEB6459ECULL, 0x4AC59FEDD6ULL, 0x4FEB647517ULL,
    0x4AC59FEDD8ULL, 0x4D8AB38F5BULL, 0x4AC59FEDDAULL, 0x4FEB6459B1ULL,
    0x7C33CC49CAULL, 0x3456303A13ULL, 0x5B708516B0ULL, 0x3F73BD8D99ULL,
    0x3F4892BDFDULL, 0x8AFE25F47FULL, 0x6E164F05B3ULL, 0x3F4892BCF7ULL,
    0x3456303D78ULL, 0x2EC3CD6993ULL, 0x2DF374C80CULL, 0xBBE631D833ULL
};

/* Special, explicitly listed. @0x180007D60 */
static const uint64_t g_special[76] = {
    0x1A987A8752DULL, 0x1AFB8794FE6ULL, 0x1B155F83D72ULL, 0x1AFB8751E8EULL,
    0x1A987A5256FULL, 0x1B155F6628CULL, 0x1AFB8794FEAULL, 0x1A987A5FFD7ULL,
    0x183A1B02363ULL, 0x185B81997B0ULL, 0x18173A2EB26ULL, 0x18173A5399AULL,
    0x187334C4DB7ULL, 0x185B81B4750ULL, 0x187E427E5C5ULL, 0x18173A30AC6ULL,
    0x18173A2FCA4ULL, 0x18173A3102FULL, 0x197A932C062ULL, 0x18F3D2B2FA2ULL,
    0x1A16C520991ULL, 0x18D7BD5327EULL, 0x1994707A751ULL, 0x8A9482DAC2ULL,
    0x7C0B092643ULL,  0x8A9482DACCULL, 0x14397E627AEULL, 0x154BBBC37D0ULL,
    0x154BBB495E1ULL, 0x537991063FULL, 0x4AC59FCA66ULL, 0x5379910630ULL,
    0x537991063EULL,  0x4AC59FCA65ULL, 0x5379910631ULL, 0x4AC59FCA64ULL,
    0x68EB25F118ULL,  0x433A9B6E26ULL, 0x68EB25EB16ULL, 0x7D662A1D27ULL,
    0x78C9B348CFULL,  0x45F1E58279ULL, 0x792C60E200ULL, 0x18B72EE403DULL,
    0x198B997684CULL, 0x71CBB77732ULL, 0x71CBB77725ULL, 0x71CBB77721ULL,
    0x71CBB77722ULL,  0x71CBB7772DULL, 0x71CBB77720ULL, 0x71CBB77739ULL,
    0x7929219EF8ULL,  0x45F1E58223ULL, 0x71CBB77729ULL, 0x71CBB77724ULL,
    0x71CBB77727ULL,  0x71CBB77736ULL, 0x147CD1A13C3ULL, 0x82AF1233BCULL,
    0x71CBB77735ULL,  0x71CBB77731ULL, 0x71CBB7772CULL, 0x71CBB77728ULL,
    0x71CBB7772BULL,  0x71CBB77726ULL, 0x71CBB77734ULL, 0x71CBB7772AULL,
    0x71CBB77730ULL,  0x71CBB7773AULL, 0x71CBB77738ULL, 0x7926908976ULL,
    0x71CBB77737ULL,  0xCA9DCD4408ULL, 0x71CBB77723ULL, 0x7C33CCA452ULL
};

/* @0x180007FC0 */
static const uint64_t g_santaBlanca[2] = {
    0xF645ED5E6DULL, 0x154BBBC8ADAULL
};

/* @0x180007FE8 */
static const uint64_t g_civilians[3] = {
    0x1AFB8765956ULL, 0x7DECAB61CDULL, 0x7DECAB1E1DULL
};

/* The one explicitly listed Unidad archetype. */
#define NPC_ID_UNIDAD 0x1A987A7937CULL

static int InTable(const uint64_t *t, int n, uint64_t v) {
    int i;
    for (i = 0; i < n; i++) if (t[i] == v) return 1;
    return 0;
}

/* Mirrors the original's classifier at 0x1800011C0: the tables
 * win, the engine's kind is the fallback.
 */
static int NpcInGroup(int group, const ShNpcArchetype *a) {
    uint64_t id;
    int kind;

    if (!a) return 0;
    id = a->id;
    kind = a->kind;

    if (InTable(g_blacklist, 44, id)) return 0;
    if (InTable(g_santaBlanca, 2, id)) return group == G_SANTA_BLANCA;
    if (id == NPC_ID_UNIDAD) return group == G_UNIDAD;
    if (InTable(g_civilians, 3, id)) return group == G_CIVILIANS;
    if (InTable(g_special, 76, id)) return group == G_SPECIAL;

    switch (group) {
    case G_SANTA_BLANCA: return kind == 3;
    case G_UNIDAD:       return kind == 5;
    case G_REBELS:       return kind == 6 || kind == 7;
    case G_CIVILIANS:    return kind <= 1;
    case G_SPECIAL:      return kind == 4;
    }
    return 0;
}

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

/* Walk the catalogue and count this group, then return the
 * catalogue index and archetype id of the g_sel[group]-th entry
 * of it (or -1).  sel wraps rather than clamps, exactly as the
 * original's 0x180001340 does, so the spinner is endless.
 */
static int GroupScan(int group, int *outCount, uint64_t *outId) {
    int n, i, c = 0, idx = -1, k = 0;
    uint64_t id = 0;

    if (outCount) *outCount = 0;
    if (outId) *outId = 0;
    if (!pNpcCount || !pNpcAt) return -1;

    n = pNpcCount();
    if (n <= 0) return -1;

    for (i = 0; i < n; i++)
        if (NpcInGroup(group, pNpcAt(i))) c++;

    if (c > 0) {
        if (g_sel[group] < 1) g_sel[group] = c;
        else if (g_sel[group] > c) g_sel[group] = 1;

        for (i = 0; i < n; i++) {
            const ShNpcArchetype *a = pNpcAt(i);
            if (!NpcInGroup(group, a)) continue;
            if (++k == g_sel[group]) { idx = i; id = a->id; break; }
        }
    }
    if (outCount) *outCount = c;
    if (outId) *outId = id;
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

    if (count <= 0) { SetStatus("No entries"); return; }
    if (idx < 0) { SetStatus("Selection unavailable"); return; }
    if (pMenuStatusF)
        pMenuStatusF(g_menu, "%d/%d | Total %d/%d",
                     g_sel[g_group], count, idx, REPORT_LIMIT);
    /* The row the user just moved snaps back to the middle, so
     * the row keeps showing current - 1 / current / current + 1. */
    if (pMenuSetValue) pMenuSetValue(g_menu, "NPC Number", 1);
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

static void TrackAdd(uint64_t e) {
    EnterCriticalSection(&g_trackLock);
    if (g_trackN < TRACK_MAX) g_track[g_trackN++] = e;
    g_last = e;
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

/* ---- formation geometry (mirrors 0x180001530 / 0x180001770) ------ */

#define PI_F      3.14159265f
#define TWO_PI_F  6.28318531f
#define INV_2P24  5.9604645e-8f   /* 2^-24, the 24 bit PRNG scale */

/* One point of the formation, in the plane the caller then
 * rotates by the player's yaw.  n <= 1 is a single point at the
 * formation centre.
 */
static void FormationPoint(int formation, int i, int n,
                           unsigned *seed, float *ox, float *oy) {
    float a;

    *ox = 0.0f;
    *oy = 0.0f;
    if (n <= 1) return;

    switch (formation) {
    case F_LINE:
        *ox = ((float)i - (float)(n - 1) * 0.5f) * 3.0f;
        break;

    case F_SPREAD: {
        /* A three column grid, each row centred on its own. */
        int q = i / 3, r = i % 3;
        int rowLen = n - 3 * q;
        int rows = (n + 2) / 3;
        *ox = ((float)r - (float)(rowLen - 1) * 0.5f) * 3.5f;
        *oy = ((float)q - ((float)rows - 1.0f) * 0.5f) * 3.5f;
        break;
    }

    case F_SEMICIRCLE:
        a = PI_F * ((float)i / (float)(n - 1)) - PI_F * 0.5f;
        *ox = 5.0f * cosf(a);
        *oy = 5.0f * sinf(a);
        break;

    case F_CIRCLE:
        a = TWO_PI_F * (float)i / (float)n;
        *ox = 4.0f * cosf(a);
        *oy = 4.0f * sinf(a);
        break;

    default: {
        /* Random: a 24 bit hash of the running seed, giving the
         * angle a jitter inside its slice and the radius a value
         * in [2.5, 7.0). */
        unsigned s = *seed;
        unsigned h1 = (s * 0x19660Du + 0x3C6EF35Fu) & 0xFFFFFFu;
        float r1, r2, rad;

        s = s * 0x17385CA9u + 0x47502932u;
        *seed = s;

        r1 = (float)h1 * INV_2P24;
        r2 = (float)(s & 0xFFFFFFu) * INV_2P24;

        a = (TWO_PI_F / (float)n) * ((float)i + r1);
        rad = 2.5f + 4.5f * r2;
        *ox = rad * cosf(a);
        *oy = rad * sinf(a);
        break;
    }
    }
}

/* Lay the whole formation out: the centre goes `dist` metres
 * along the player's facing, every point is then rotated by the
 * same yaw, and z is the player's own (the original does no
 * ground probe).  Returns how many points were written.
 */
static int PlanFormation(int formation, float dist, int n,
                         const ShVec3 *pp, float yaw, ShVec3 *out) {
    unsigned seed = (unsigned)GetTickCount() ^ (unsigned)(uintptr_t)out;
    float cy = cosf(yaw), sy = sinf(yaw);
    float bx, by;
    int i;

    if (n > PLAN_MAX) n = PLAN_MAX;
    if (n < 1) return 0;

    bx = pp->x + dist * cy;
    by = pp->y + dist * sy;

    for (i = 0; i < n; i++) {
        float ox, oy;
        FormationPoint(formation, i, n, &seed, &ox, &oy);
        out[i].x = bx + ox * sy + oy * cy;
        out[i].y = by - ox * cy + oy * sy;
        out[i].z = pp->z;
    }
    return n;
}

/* ---- the spawn worker (mirrors 0x180002070) --------------------- */

typedef struct {
    uint64_t id;
    float    dist;
    int      count;
    int      formation;
    int      facing;
} SpawnReq;

static DWORD WINAPI SpawnWorker(LPVOID p) {
    SpawnReq *r = (SpawnReq *)p;
    ShPlayer pl;
    ShVec3 pp = { 0.0f, 0.0f, 0.0f };
    ShVec3 tmp;
    ShVec3 pos[PLAN_MAX];
    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
    uint64_t id;
    float dist;
    int count, formation, facing;
    int n, i, spawned = 0, faceFail = 0, alive;

    if (!r) { ReleaseBusy(); return 0; }
    id = r->id;
    dist = r->dist;
    count = r->count;
    formation = r->formation;
    facing = r->facing;
    free(r);

    if (!pGetPlayer(&pl) || !pGetPlayerPosition(&pp) ||
        !pGetEntityTransform(pl.entity, &tmp, &yaw, &pitch, &roll)) {
        Log("npcspawner: player unavailable");
        SetStatus("Player unavailable");
        ReleaseBusy();
        return 0;
    }

    n = PlanFormation(formation, dist, count, &pp, yaw, pos);
    Log("npcspawner: spawn id=%llx n=%d form=%d face=%d dist=%.0f yaw=%.2f",
        (unsigned long long)id, n, formation, facing, dist, yaw);

    for (i = 0; i < n; i++) {
        uint64_t e;

        if (WorldBusy()) break;

        e = pSpawnNpc(id, &pos[i]);
        if (!e) continue;                 /* not counted, as in the original */
        if (WorldBusy()) continue;        /* spawned, but do not track it */

        TrackAdd(e);
        spawned++;

        if (facing == FACE_PLAYER) {
            /* Turn it to look at the player, in radians. */
            float dx = pp.x - pos[i].x;
            float dy = pp.y - pos[i].y;
            if (!pQueueTransform(e, &pos[i], atan2f(dy, dx), 0.0f, 0.0f))
                faceFail++;
        }
    }

    ReleaseBusy();

    if (WorldBusy()) return 0;
    Log("npcspawner: spawned %d/%d, faceFail %d", spawned, count, faceFail);

    if (spawned == 0) {
        SetStatus("Spawn failed");
        return 0;
    }

    EnterCriticalSection(&g_trackLock);
    alive = g_trackN;
    LeaveCriticalSection(&g_trackLock);

    if (spawned == count && faceFail == 0) {
        RefreshStatus();
        return 0;
    }
    if (pMenuStatusF)
        pMenuStatusF(g_menu, "Spawn %d/%d | F %d | Total %d/%d",
                     spawned, count, faceFail, alive, REPORT_LIMIT);
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
        SetStatus("Nothing to undo");
        ReleaseBusy();
        return 0;
    }
    if (WorldBusy()) { ReleaseBusy(); return 0; }

    kind = pGetEntityKind(e);
    if (!kind) {
        TrackRemove(e);
        SetStatus("Undo target unavailable");
        ReleaseBusy();
        return 0;
    }
    if (!pGetHealthEntity(e, &cur, &max)) {
        TrackRemove(e);
        SetStatus("Undo target unavailable");
        ReleaseBusy();
        return 0;
    }
    if (cur == 0) {
        SetStatus("Last spawn is dead");
        ReleaseBusy();
        return 0;
    }

    ok = pDespawn(e);
    if (ok) TrackRemove(e);
    ReleaseBusy();
    if (WorldBusy()) return 0;

    Log("npcspawner: undo ent=%llx ok=%d", (unsigned long long)e, ok);
    if (!ok) SetStatus("Undo failed");
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
    SpawnReq *r;
    HANDLE h;
    int count = 0, idx, alive;
    uint64_t id = 0;
    (void)menu; (void)item; (void)value; (void)user;

    if (!TryBusy()) { SetStatus("Busy"); return; }

    PruneTrack();

    EnterCriticalSection(&g_trackLock);
    alive = g_trackN;
    LeaveCriticalSection(&g_trackLock);

    if (alive + g_countN[g_countIdx] > SPAWN_LIMIT) {
        if (pMenuStatusF)
            pMenuStatusF(g_menu, "Limit | Total %d/%d", alive, REPORT_LIMIT);
        ReleaseBusy();
        return;
    }

    idx = GroupScan(g_group, &count, &id);
    if (count <= 0 || idx < 0 || !id) {
        SetStatus(count <= 0 ? "No entries" : "Selection unavailable");
        ReleaseBusy();
        return;
    }

    r = (SpawnReq *)malloc(sizeof(*r));
    if (!r) {
        SetStatus("Spawn request failed");
        ReleaseBusy();
        return;
    }
    r->id = id;
    r->dist = g_distanceM[g_distIdx];
    r->count = g_countN[g_countIdx];
    r->formation = g_formation;
    r->facing = g_facing;

    h = CreateThread(NULL, 0, SpawnWorker, r, 0, NULL);
    if (!h) {
        free(r);
        SetStatus("Could not start worker");
        ReleaseBusy();
        return;
    }
    CloseHandle(h);
}

static void OnUndo(uint32_t menu, uint32_t item, int value, void *user) {
    HANDLE h;
    (void)menu; (void)item; (void)value; (void)user;

    if (!TryBusy()) { SetStatus("Busy"); return; }

    h = CreateThread(NULL, 0, UndoWorker, NULL, 0, NULL);
    if (!h) {
        SetStatus("Could not start worker");
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
    if (value < 0 || value >= GROUP_MAX) return;
    g_group = value;
    Log("npcspawner: group %d", g_group);
    RefreshStatus();
}

/* ---- menu build ------------------------------------------------- */

/* The row labels below are the translation keys in
 * plugins\NPCSpawner\NPCSpawner.ini, so renaming one here means
 * renaming it there too.  The package the original plugin shipped
 * in had those values shifted up a line (see the report, section 4);
 * the deployed ini is fixed.  The ini is read once per owner, so an
 * edit only shows after a game restart.
 */
static uint32_t BuildMenu(void) {
    uint32_t m;

    m = pMenuCreate("Native NPC Spawner");
    if (!m) return 0;

    /* Rebuild the spinner labels before the row is added, so the
     * first capture is already correct. */
    UpdateNumberLabels();

    pMenuList(m, "NPC Number", g_numOpts, 3, 1, OnNumber, NULL);
    pMenuAction(m, "Spawn Selected", OnSpawnSelected, NULL);
    pMenuAction(m, "Undo Last Spawn", OnUndo, NULL);
    pMenuList(m, "Spawn Distance", g_distanceOpts, 6, g_distIdx,
              OnDistance, NULL);
    pMenuList(m, "Spawn Count", g_countOpts, 3, g_countIdx, OnCount, NULL);
    pMenuList(m, "Formation", g_formationOpts, 5, g_formation,
              OnFormation, NULL);
    pMenuList(m, "Facing", g_facingOpts, 2, g_facing, OnFacing, NULL);
    pMenuList(m, "NPC Group", g_groupOpts, 5, g_group, OnGroup, NULL);

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
    *(FARPROC *)&pGetPlayer = GetProcAddress(mod, "ShGetPlayer");
    *(FARPROC *)&pGetPlayerPosition =
        GetProcAddress(mod, "ShGetPlayerPosition");
    *(FARPROC *)&pGetEntityTransform =
        GetProcAddress(mod, "ShGetEntityTransform");
    *(FARPROC *)&pGetEntityKind = GetProcAddress(mod, "ShGetEntityKind");
    *(FARPROC *)&pGetHealthEntity = GetProcAddress(mod, "ShGetHealthEntity");
    *(FARPROC *)&pQueueTransform = GetProcAddress(mod, "ShQueueTransform");
    *(FARPROC *)&pNpcCount = GetProcAddress(mod, "ShNpcCount");
    *(FARPROC *)&pNpcAt = GetProcAddress(mod, "ShNpcAt");
    *(FARPROC *)&pSpawnNpc = GetProcAddress(mod, "ShSpawnNpc");
    *(FARPROC *)&pDespawn = GetProcAddress(mod, "ShDespawn");
    *(FARPROC *)&pMenuCreate = GetProcAddress(mod, "ShMenuCreate");
    *(FARPROC *)&pMenuList = GetProcAddress(mod, "ShMenuList");
    *(FARPROC *)&pMenuAction = GetProcAddress(mod, "ShMenuAction");
    *(FARPROC *)&pMenuStatus = GetProcAddress(mod, "ShMenuStatus");
    *(FARPROC *)&pMenuStatusF = GetProcAddress(mod, "ShMenuStatusF");
    *(FARPROC *)&pMenuSetValue = GetProcAddress(mod, "ShMenuSetValue");

    if (!pGetVersion || !pGetGameState || !pGetPlayer ||
        !pGetPlayerPosition || !pGetEntityTransform || !pGetEntityKind ||
        !pGetHealthEntity || !pQueueTransform || !pNpcCount || !pNpcAt ||
        !pSpawnNpc || !pDespawn || !pMenuCreate || !pMenuList ||
        !pMenuAction || !pMenuStatus) {
        Log("npcspawner: required export missing, giving up");
        return 1;
    }

    /* The API is up once ShGetVersion answers. */
    while (!pGetVersion()) Sleep(500);

    Log("npcspawner: up");
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
        CreateThread(NULL, 0, BindThread, NULL, 0, NULL);
    }
    return TRUE;
}
