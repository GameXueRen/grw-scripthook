/* Enemy reinforcements.
 *
 * A wave of enemies arrives when the player is shot.
 *
 * What starts a wave
 * ------------------
 * Exactly one signal: the player was HIT BY SOMETHING THAT IS NOT
 * THE PLAYER.  It is the only reading the engine offers that is
 * evidence rather than inference - someone aimed at us and landed
 * one, so that one is an enemy whatever faction the engine thinks
 * it belongs to.
 *
 * What the player shoots is deliberately NOT a trigger: hurting an
 * NPC says nothing about whether it was hostile to the player, and
 * a stray round into a rebel's back is not "a fight".  Neither is
 * a shot that merely passes nearby: only a landed hit counts.
 *
 * ([Settings] fire_combat, off by default) A shot fired at the
 * player from inside the engage radius counts as the same thing.
 * That is the opt in for the enemies' "in combat" state as a
 * trigger, and it is evidence too: ShShot carries the muzzle
 * position and the shooter's kind, so this is a distance test and
 * not a guess.  See OnFire for why the per NPC alert byte was
 * dropped.
 *
 * The hit arrives through the hit hook.  ShHit.kind is the kind of
 * the thing that was HIT - the root - and not of the shooter.  That
 * kind alone already names the local player, since teammates carry
 * SH_KIND_TEAMMATE, and the handles are compared against
 * ShGetPlayer's as well.  The second test is what catches being hit
 * while riding: in a vehicle the root re-parents to the car while
 * the entity stays the soldier (scripthook.h, ShPlayer).
 *
 * Spawn and forget
 * ----------------
 * Nothing is tracked after a wave.  No entity handle is kept, no
 * entity is read back, and no health is touched: the engine owns
 * what it spawned.  All this keeps is the time of its own recent
 * spawns, which is what the cap counts - see the note on spawn
 * count below.  That is deliberate: every per entity call is a
 * component lookup, and the framework documents a missed lookup as
 * a full heap pass of tens of seconds
 * (scripthook_health.c, EntityComponent), which froze the game when
 * this plugin used to harden what it spawned.
 *
 * Which archetype answers is the menu's pick, or - with the random
 * row on - one drawn at random out of the selected group each wave.
 *
 * A respawn or a redeploy stands it down: everything the next wave
 * is decided from is cleared, so a fight cannot follow the player
 * into a new life.  See CheckNewLife.
 *
 * A fight gets a budget of waves, set in the menu; 0 is no budget
 * at all, which is the default.  It is per fight, not per session:
 * the budget opens again once the hits stop for "Fight window s"
 * (20 by default), which is the same window the status line calls
 * "waiting to be hit".  Manual spawns are not counted against it.
 *
 * Which faction answers
 * ---------------------
 * The engine will not say which faction the shooter belongs to:
 * ShHit carries no such field, and the API has no entity to
 * archetype lookup.  So the returning wave is the faction the menu
 * has selected, and each of the five groups remembers its own
 * archetype, so switching group switches what answers.
 *
 * The probe ([Settings] probe) is the experiment that could change
 * that: it dumps the shooter's components and looks for a
 * catalogue archetype id inside them.  A "probe: match" line would
 * be the mapping, and the group it names is the shooter's faction.
 * It samples the shooter only, never the field.  NOT verified: the
 * first runs produced no match at all, so the id is not in the
 * window it reads - see the probe section below.
 *
 * Logs to <gamedir>\logs\scripthook_enemyreinforce.log
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "scripthook.h"
#include "log.h"

/* ---- tuning ----------------------------------------------------- */

#define TICK_MS          500     /* main loop                    */
#define FIGHT_WINDOW_S   20      /* default: a fight is this recent */
#define NEAR_RADIUS      120.0f  /* default scan reach, metres   */
#define SCAN_MIN         20
#define SCAN_MAX         300

/* The cap counts spawns this recent, not spawns still alive: the
 * engine owns the NPCs and nothing is read back, so a wave the
 * player has already cleared keeps its slot until its record ages
 * out.  The window is what turns the cap into a rate: with the
 * defaults - 8, batches of 3, every 8 s - it holds the waves to
 * roughly one every ten seconds instead of letting them pile up. */
#define SPAWN_RING       256
#define SPAWN_WINDOW_MS  30000

/* A formation that comes back empty this fast was turned away by the
 * spawner itself.  Measured: a real spawn takes about 260 ms
 * (22:22:18.519 -> .781) and a refused draw took 4 ms (22:22:27.037
 * -> .041).  A slower zero is the framework's hand off timing out,
 * which says nothing about the archetype. */
#define REFUSE_FAST_MS   50

/* The probe's own limits.  It runs on the hit callback's thread
 * and reads guarded memory, so every one of these is a hard cap:
 * PROBE_MAX_ENTS distinct shooters per session, PROBE_MIN_GAP_MS
 * between two samples, and this much memory per component. */
#define PROBE_MAX_ENTS   8
#define PROBE_MIN_GAP_MS 1000
#define PROBE_MAX_COMP   32
#define PROBE_WINDOW     0x100
#define PROBE_MAX_IDS    1032

/* ---- late binding ----------------------------------------------- */

typedef int      (*GetVersion_t)(void);
typedef int      (*IsInGame_t)(void);
typedef int      (*GetGameState_t)(void);
typedef int      (*GameStateName_t)(char *, int);
typedef int      (*HitHookInstall_t)(void);
typedef int      (*OnHit_t)(ShHitFn, void *, int);
typedef int      (*OnFire_t)(ShFireFn, void *, int);
typedef int      (*GetPlayer_t)(ShPlayer *);
typedef int      (*GetPos_t)(ShVec3 *);
typedef int      (*FindEntities_t)(int, float, uint32_t, ShEntity *, int);
typedef int      (*NpcCount_t)(void);
typedef const ShNpcArchetype *(*NpcAt_t)(int);
typedef int      (*NpcGroupSize_t)(int);
typedef int      (*NpcAtInGroup_t)(int, int, ShNpcArchetype *);
typedef int      (*NpcGroupOf_t)(uint64_t);
typedef const char *(*NpcGroupName_t)(int);
typedef int      (*GetComponents_t)(uint64_t, ShComponent *, int);
typedef uint64_t (*ReadU64_t)(uint64_t, int *);
typedef uint64_t (*FindComponent_t)(uint64_t, uint32_t);
typedef int      (*ReadBytes_t)(uint64_t, void *, uint32_t);
typedef int      (*SpawnFormation_t)(const ShNpcSpawnRequest *,
                                     uint64_t *, int);
typedef uint32_t (*MenuCreate_t)(const char *);
typedef int      (*MenuList_t)(uint32_t, const char *, const char **,
                               int, int, ShMenuFn, void *);
typedef int      (*MenuToggle_t)(uint32_t, const char *, int,
                                 ShMenuFn, void *);
typedef int      (*MenuNumber_t)(uint32_t, const char *, float, float,
                                 float, float, ShMenuFn, void *);
typedef int      (*MenuAction_t)(uint32_t, const char *,
                                 ShMenuFn, void *);
typedef int      (*MenuSetValue_t)(uint32_t, const char *, int);
typedef int      (*MenuStatus_t)(uint32_t, const char *);
typedef int      (*MenuStatusF_t)(uint32_t, const char *, ...);
typedef int      (*ConfigGetStr_t)(const char *, const char *,
                                   const char *, char *, int);
typedef int      (*ConfigSetStr_t)(const char *, const char *,
                                   const char *);

static GetVersion_t       pGetVersion;
static IsInGame_t         pIsInGame;
static GetGameState_t     pGetGameState;
static GameStateName_t    pGetGameStateName;
static HitHookInstall_t   pHitHookInstall;
static OnHit_t            pOnHit;
static OnFire_t           pOnFire;
static GetPlayer_t        pGetPlayer;
static GetPos_t           pGetPos;
static FindEntities_t     pFindEntities;
static NpcCount_t         pNpcCount;
static NpcAt_t            pNpcAt;
static NpcGroupSize_t     pGroupSize;
static NpcAtInGroup_t     pAtInGroup;
static NpcGroupOf_t       pNpcGroupOf;
static NpcGroupName_t     pNpcGroupName;
static GetComponents_t    pGetComponents;
static ReadU64_t          pReadU64;
static FindComponent_t    pFindComponent;
static ReadBytes_t        pReadBytes;
static SpawnFormation_t   pSpawnFormation;
static MenuCreate_t       pMenuCreate;
static MenuList_t         pMenuList;
static MenuToggle_t       pMenuToggle;
static MenuNumber_t       pMenuNumber;
static MenuAction_t       pMenuAction;
static MenuSetValue_t     pMenuSetValue;
static MenuStatus_t       pMenuStatus;
static MenuStatusF_t      pMenuStatusF;
static ConfigGetStr_t     pConfigGetStr;
static ConfigSetStr_t     pConfigSetStr;

/* ---- state ------------------------------------------------------ */

static uint32_t g_menu = 0;

static volatile LONG g_active = 0;      /* reinforcing */
static volatile LONG g_probe  = 1;      /* dump the shooter    */
static volatile LONG g_onFire = 0;      /* nearby fire counts  */
static int  g_group     = SH_NPC_GROUP_SANTA_BLANCA;
static int  g_sel[SH_NPC_GROUP_MAX] = { 1, 1, 1, 1, 1 };  /* 1 based */
static int  g_groupCount = 0;
static int  g_interval  = 8;            /* seconds between waves */
static int  g_batch     = 3;
static int  g_cap       = 8;            /* our spawns in the world */
static int  g_waves     = 0;            /* waves per fight, 0 = no limit */
static volatile LONG g_random = 0;      /* a random archetype each wave */
static int  g_fightS    = FIGHT_WINDOW_S;/* seconds without a hit ends it */
static int  g_scanR     = 120;          /* how far the samples look     */
static int  g_distIdx   = 3;            /* 50 m */
static int  g_formation = SH_NPC_FORMATION_RANDOM;
static volatile LONG g_spawning = 0;    /* a wave is in flight */

/* One remembered archetype PER GROUP.  Picking another group has to
 * answer with that group's own enemy, which one global slot could
 * not do.  Persisted in scripthook.ini as type_id_0 .. type_id_4;
 * the single type_id the first version wrote is read as group 0. */
static uint64_t g_remembered[SH_NPC_GROUP_MAX] = { 0, 0, 0, 0, 0 };

/* The trigger.  A flag, not a timestamp, because the wave is
 * decided from it: a tick that runs late can postpone a wave, but
 * it can never swallow the event. */
static volatile LONG   g_hitPending = 0;
static volatile LONG64 g_lastHit = 0;
static volatile LONG   g_hits = 0;      /* landed on us, for the log */
static unsigned long long g_lastHitLog = 0;
static unsigned long long g_lastFireLog = 0;
static unsigned long long g_lastSkipLog = 0;
static volatile LONG64 g_lastWave = 0;
static volatile LONG   g_waveN = 0;     /* waves sent, this fight */

/* ---- option labels ---------------------------------------------- */

static const char *g_groupOpts[SH_NPC_GROUP_MAX] = {
    "Santa Blanca", "Unidad", "Rebels", "Civilians", "Special"
};
static const char *g_formationOpts[5] = {
    "Line", "Spread", "Semicircle", "Circle", "Random"
};
static const char *g_distOpts[6] = {
    "10 m", "20 m", "30 m", "50 m", "75 m", "100 m"
};
static const float g_distM[6] = { 10.0f, 20.0f, 30.0f,
                                  50.0f, 75.0f, 100.0f };

/* The spinner labels, rewritten in place: the menu borrows the
 * pointers, so a rewrite shows up on the next capture. */
static char g_numLabel[3][16];
static const char *g_numOpts[3] = {
    g_numLabel[0], g_numLabel[1], g_numLabel[2]
};

/* ---- recent spawns ---------------------------------------------- */

/* The cap counts spawns this recent.  Nothing is read back: the
 * engine owns these NPCs, and asking after one means a component
 * lookup, which the framework documents as a full heap pass when it
 * misses.  A ring of timestamps is all this needs, so a wave the
 * player has already cleared still holds a slot until its record
 * ages out - see SPAWN_WINDOW_MS. */

static CRITICAL_SECTION g_lock;
static volatile LONG g_locksInit = 0;
static unsigned long long g_spawnAt[SPAWN_RING];
static int g_spawnNext = 0;

static void EnsureLocks(void) {
    while (!g_locksInit) {
        if (InterlockedCompareExchange(&g_locksInit, 2, 0) == 0) {
            InitializeCriticalSection(&g_lock);
            LogInit("scripthook_enemyreinforce.log");
            InterlockedExchange(&g_locksInit, 1);
        } else if (g_locksInit == 2) {
            /* Another thread is inside the initialisation.  Yield
             * instead of spinning: LogInit opens a file, and that is
             * not instant on a slow or scanned disk. */
            Sleep(1);
        }
    }
}

static void SetStatus(const char *text) {
    if (g_menu && pMenuStatus) pMenuStatus(g_menu, text);
}

/* One record per NPC that arrived.  Runs on the wave's thread, so
 * the ring is under the lock. */
static void RecordSpawns(int n) {
    unsigned long long now = GetTickCount64();
    int i;

    EnterCriticalSection(&g_lock);
    for (i = 0; i < n; i++) {
        g_spawnAt[g_spawnNext] = now;
        if (++g_spawnNext >= SPAWN_RING) g_spawnNext = 0;
    }
    LeaveCriticalSection(&g_lock);
}

/* How many we have spawned inside the window, which is what the cap
 * is measured against. */
static int SpawnCount(void) {
    unsigned long long now = GetTickCount64();
    int i, n = 0;

    EnterCriticalSection(&g_lock);
    for (i = 0; i < SPAWN_RING; i++) {
        if (!g_spawnAt[i]) continue;
        if (now - g_spawnAt[i] < SPAWN_WINDOW_MS) n++;
    }
    LeaveCriticalSection(&g_lock);
    return n;
}

/* Forget the count: the NPCs it stands for are not in the world the
 * player is about to be in. */
static void ClearSpawns(void) {
    EnterCriticalSection(&g_lock);
    memset(g_spawnAt, 0, sizeof(g_spawnAt));
    g_spawnNext = 0;
    LeaveCriticalSection(&g_lock);
}

/* ---- the probe (NOT verified) ----------------------------------- */

/* The experiment behind "the corresponding faction".  The engine
 * keeps an archetype's faction private and the API has no entity
 * to archetype lookup, so the returning wave is the menu's
 * faction.  This is the reading that could change that: dump the
 * shooter's components, and look for a catalogue archetype id
 * inside them.  A "probe: match" line is that mapping - the value
 * is the archetype the shooter came from, and ShNpcGroupOf turns
 * it into the faction.
 *
 * It runs on the hit callback's thread, where API calls are safe
 * but blocking ones are not, so it never blocks and is capped
 * hard: PROBE_MAX_ENTS shooters, PROBE_MIN_GAP_MS apart, and no
 * more than PROBE_WINDOW bytes per component.  The log has no
 * throttle of its own, so the caps are what keep it readable. */

static uint64_t g_ids[PROBE_MAX_IDS];    /* the catalogue, sorted */
static int      g_idn = 0;
static uint64_t g_probedEnt[PROBE_MAX_ENTS];
static volatile LONG g_probedN = 0;
static unsigned long long g_probeLast = 0;

static int IdCmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Sorted once, so a sample costs a binary search per eight bytes
 * instead of a walk of the whole catalogue.  Called from
 * InitThread and never from the callback thread: ShNpcCount waits
 * for the registry on its first call. */
static void BuildCatalogue(void) {
    int i, n;

    g_idn = 0;
    if (!pNpcCount || !pNpcAt) return;

    n = pNpcCount();
    if (n <= 0) return;
    if (n > PROBE_MAX_IDS) n = PROBE_MAX_IDS;

    for (i = 0; i < n; i++) {
        const ShNpcArchetype *a = pNpcAt(i);
        if (a && a->id) g_ids[g_idn++] = a->id;
    }
    if (g_idn > 1)
        qsort(g_ids, (size_t)g_idn, sizeof(g_ids[0]), IdCmp);
}

/* The sorted index of a catalogue id, or -1. */
static int IdFind(uint64_t v) {
    int lo = 0, hi = g_idn - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (g_ids[mid] == v) return mid;
        if (g_ids[mid] < v) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

/* Walk one window eight bytes at a time and name anything that is
 * a catalogue id.  ShReadU64 is a guarded read, so a page that is
 * not there returns 0 with ok clear rather than killing the game. */
static void ProbeWindow(const char *what, uint64_t base, int len) {
    int off;

    if (!base || !pReadU64) return;

    for (off = 0; off + 8 <= len; off += 8) {
        int ok = 0, m, g;
        uint64_t v = pReadU64(base + (uint64_t)off, &ok);

        if (!ok || !v) continue;
        m = IdFind(v);
        if (m < 0) continue;

        g = pNpcGroupOf ? pNpcGroupOf(v) : -1;
        Log("probe: match %s+0x%x = %llx group %d (%s)",
            what, off, (unsigned long long)v, g,
            (g >= 0 && pNpcGroupName) ? pNpcGroupName(g) : "?");
    }
}

/* ---- the alert state (NOT verified) ----------------------------- */

/* Whether the game considers the player to be in combat is not
 * something the API answers.  What an earlier investigation did find
 * is a per NPC component, class hash 0x4736ef45, whose byte +0x60
 * went 0 -> 1 and +0x65 went 1 -> 0 when a faction turned hostile
 * (the old publicenemy.c experiment, diffing one mission against
 * another).  That is a lead, not an answer: the HUD shows three
 * states - unseen, alerted, in combat - and this has to be read once
 * in each of them before anything can key off it.
 *
 * So the window is dumped and nothing acts on it.  Two ways in: the
 * menu row, pressed while standing in each state; and every shot
 * that lands on the player, which samples whoever fired it (see
 * ProbeShooter).  The line is plain hex from ALERT_FROM, so the two
 * offsets from the old note land at characters 0x40 and 0x4a. */

#define ALERT_HASH   0x4736ef45u
#define ALERT_FROM   0x40
#define ALERT_LEN    0x38

static void AlertDump(const char *tag, uint64_t e) {
    uint8_t b[ALERT_LEN];
    char hex[ALERT_LEN * 3];
    uint64_t comp;
    int i, o = 0;

    if (!pFindComponent || !pReadBytes || !e) return;

    comp = pFindComponent(e, (uint32_t)ALERT_HASH);
    if (!comp) {
        Log("alert: %s ent=%llx has no %08x component", tag,
            (unsigned long long)e, ALERT_HASH);
        return;
    }
    memset(b, 0, sizeof(b));
    if (!pReadBytes(comp + ALERT_FROM, b, ALERT_LEN)) {
        Log("alert: %s ent=%llx window unreadable", tag,
            (unsigned long long)e);
        return;
    }
    for (i = 0; i < ALERT_LEN; i++)
        o += snprintf(hex + o, sizeof(hex) - (size_t)o, "%02x", b[i]);

    Log("alert: %s ent=%llx +%02x.. = %s", tag, (unsigned long long)e,
        ALERT_FROM, hex);
}

/* ShFindEntities reads health for every candidate it keeps
 * (scripthook_entity.c:983), and a missed component lookup there is
 * a heap pass of tens of seconds.  So the on demand sample runs on
 * its own thread, never on the one that decides waves. */
static volatile LONG g_sampling = 0;

static DWORD WINAPI SampleThread(LPVOID p) {
    ShEntity found[4];
    char tag[16];
    int n, i;

    (void)p;
    if (!pFindEntities) {
        InterlockedExchange(&g_sampling, 0);
        return 0;
    }
    n = pFindEntities(SH_KIND_NPC, (float)g_scanR, 0, found, 4);
    if (n <= 0) {
        Log("alert: nothing within %d m came back (n=%d)", g_scanR, n);
        InterlockedExchange(&g_sampling, 0);
        return 0;
    }
    for (i = 0; i < n && i < 4; i++) {
        snprintf(tag, sizeof(tag), "near[%d]", i);
        AlertDump(tag, found[i].entity);
    }
    InterlockedExchange(&g_sampling, 0);
    return 0;
}

/* force skips the caps, for the menu's on demand sample. */
static void ProbeShooter(uint64_t e, int force) {
    ShComponent comps[PROBE_MAX_COMP];
    unsigned long long now;
    int n, i;

    if (!pGetComponents || !pReadU64 || g_idn <= 0) {
        if (force) SetStatus("probe needs components and reads");
        return;
    }

    now = GetTickCount64();
    if (!force) {
        LONG seen = g_probedN;

        if (g_probeLast && now - g_probeLast < PROBE_MIN_GAP_MS) return;
        if (seen >= PROBE_MAX_ENTS) return;
        /* Clamped rather than trusted: the count is written from two
         * threads, so it is a hint and not a promise. */
        for (i = 0; i < (int)seen && i < PROBE_MAX_ENTS; i++)
            if (g_probedEnt[i] == e) return;
    }

    g_probeLast = now;

    /* Claim a slot without a lock: the hit callback and the on demand
     * probe thread can both be in here at once, so count and store go
     * in one compare-exchange. */
    for (;;) {
        LONG n = InterlockedCompareExchange(&g_probedN, 0, 0);

        if (n >= PROBE_MAX_ENTS) break;
        if (InterlockedCompareExchange(&g_probedN, n + 1, n) == n) {
            g_probedEnt[n] = e;
            break;
        }
    }

    memset(comps, 0, sizeof(comps));
    n = pGetComponents(e, comps, PROBE_MAX_COMP);
    if (n > PROBE_MAX_COMP) n = PROBE_MAX_COMP;

    Log("probe: shooter %llx, %d components, %d ids known, window 0x%x",
        (unsigned long long)e, n, g_idn, PROBE_WINDOW);

    for (i = 0; i < n; i++) {
        Log("probe:   [%d] class %08x %s comp=%llx block=%llx",
            i, (unsigned)comps[i].classHash,
            comps[i].name[0] ? comps[i].name : "?",
            (unsigned long long)comps[i].component,
            (unsigned long long)comps[i].dataBlock);
        ProbeWindow("comp", comps[i].component, PROBE_WINDOW);
        ProbeWindow("block", comps[i].dataBlock, PROBE_WINDOW);
    }

    /* Whoever fired at the player is, by definition, in the state
     * the HUD calls "in combat", so this is a labelled sample. */
    AlertDump("shot-at-us", e);
}

/* The find itself, on its own thread for the same reason as
 * SampleThread: ShFindEntities reads health per candidate. */
static volatile LONG g_probing = 0;

static DWORD WINAPI ProbeThread(LPVOID p) {
    ShEntity found[8];
    int n;

    (void)p;
    if (!pFindEntities) {
        InterlockedExchange(&g_probing, 0);
        return 0;
    }
    n = pFindEntities(SH_KIND_NPC, (float)g_scanR, 0, found, 8);
    if (n <= 0) {
        Log("probe: nothing within %d m came back (n=%d)", g_scanR, n);
        InterlockedExchange(&g_probing, 0);
        return 0;
    }
    ProbeShooter(found[0].entity, 1);
    InterlockedExchange(&g_probing, 0);
    return 0;
}

/* ---- the events ------------------------------------------------- */

/* The one signal: the player was hit by something that is not the
 * player.  Returns the shooter, or 0 when this is not that.
 *
 * ShHit.kind is the kind of the thing that was HIT - the root -
 * and not of the shooter, so the handles are compared against the
 * player's own rather than trusting a kind.  That also covers
 * being hit while riding: the root re-parents to the vehicle
 * there, while the entity stays the soldier. */
static uint64_t PlayerHitBy(const ShHit *hit) {
    ShPlayer pl;

    if (hit->byPlayer || !hit->shooter) return 0;

    /* The kind already names the local player: teammates carry
     * SH_KIND_TEAMMATE, so it cannot be one of them.  Taken first
     * so that this trigger never depends on one reading alone. */
    if (hit->kind == SH_KIND_PLAYER) return hit->shooter;

    /* Otherwise compare the handles, which is what catches being
     * hit while riding: the root re-parents to the car there. */
    if (!pGetPlayer) return 0;

    memset(&pl, 0, sizeof(pl));
    if (!pGetPlayer(&pl) || !pl.entity) return 0;

    if (hit->entity == pl.entity) return hit->shooter;
    if (hit->root == pl.entity) return hit->shooter;
    if (pl.root && hit->root == pl.root) return hit->shooter;

    return 0;
}

static void OnHit(const ShHit *hit, void *user) {
    uint64_t shooter;
    unsigned long long now;
    int hits;

    (void)user;
    if (!hit) return;
    if (!InterlockedCompareExchange(&g_active, 0, 0)) return;

    shooter = PlayerHitBy(hit);
    if (!shooter) return;

    /* The flag is the trigger: set before anything else, so the
     * tick thread can only postpone a wave, never lose the event. */
    now = GetTickCount64();
    InterlockedExchange64(&g_lastHit, (LONG64)now);
    InterlockedExchange(&g_hitPending, 1);

    /* Nothing about the shooter is tracked: no entity handle is
     * kept and no health is touched.  This event's whole job is to
     * ask for the next wave.
     *
     * Rate limited by hand: a firefight lands ten hits a second
     * and the log has no throttle of its own. */
    hits = (int)InterlockedIncrement(&g_hits);
    if (now - g_lastHitLog >= 1000) {
        g_lastHitLog = now;
        Log("reinforce: hit by ent=%llx (%d hits so far)",
            (unsigned long long)shooter, hits);
    }

    if (InterlockedCompareExchange(&g_probe, 0, 0))
        ProbeShooter(shooter, 0);
}

/* The second way into the same state, and the closest reading the
 * API offers to the HUD's "in combat".  It is not a second trigger
 * in the old sense - it does not wave on any shot in the world, only
 * on one fired from inside the engage radius, which is what "the
 * enemies are fighting me right now" means in practice.  It sets the
 * same fight clock a hit does, so a firefight that is still going
 * keeps answering.
 *
 * Off by default ([Settings] fire_combat): "only the player being
 * hit starts a wave" is the rule this plugin was asked for, and this
 * is the opt in.
 *
 * The alert byte chase was dropped.  The window around the field an
 * earlier session found moved in only two bytes, and neither tells
 * the three states apart: the enemy that shot us at 22:06:37 read
 * 00/00 at that instant, and two of the four manual samples were
 * byte for byte identical.  A shot is evidence; that byte was
 * inference. */
static void OnFire(const ShShot *shot, void *user) {
    ShVec3 me;
    float dx, dy, dz, lim;
    unsigned long long now;

    (void)user;
    if (!shot || shot->byPlayer) return;
    if (!InterlockedCompareExchange(&g_active, 0, 0)) return;
    if (!InterlockedCompareExchange(&g_onFire, 0, 0)) return;

    /* On foot only.  This cannot place anything else, and guessing
     * would wave on an escort firing past us. */
    if (!shot->shooter || shot->kind != SH_KIND_NPC) return;
    if (!pGetPos || !pGetPos(&me)) return;

    dx = shot->origin.x - me.x;
    dy = shot->origin.y - me.y;
    dz = shot->origin.z - me.z;
    lim = (float)g_scanR;
    if (dx * dx + dy * dy + dz * dz > lim * lim) return;

    now = GetTickCount64();
    InterlockedExchange64(&g_lastHit, (LONG64)now);
    InterlockedExchange(&g_hitPending, 1);

    /* A firefight is many shots a second and the log has no
     * throttle of its own. */
    if (now - g_lastFireLog >= 2000) {
        g_lastFireLog = now;
        Log("reinforce: enemy fire inside %d m (ent=%llx, %.0f m)",
            g_scanR, (unsigned long long)shot->shooter,
            (double)sqrtf(dx * dx + dy * dy + dz * dz));
    }
}

/* ---- the type in use -------------------------------------------- */

/* <gamedir>\plugins\EnemyReinforce\EnemyReinforce.ini, from our own
 * module path.  Both read (IniInt) and written (IniSaveInt) with the
 * Win32 profile calls, so the UTF-8 translations in it come through
 * a save untouched. */
static HINSTANCE g_inst = NULL;
static char g_iniPath[MAX_PATH];

static void ResolveIniPath(void) {
    char mod[MAX_PATH];
    const char *base, *dot;
    size_t n;

    g_iniPath[0] = 0;
    if (!g_inst || !GetModuleFileNameA(g_inst, mod, sizeof(mod))) return;

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

/* Saving is the Win32 profile call, the same one firstperson.c uses
 * in its SaveIni.  It updates the value in place and leaves every
 * other byte alone, UTF-8 translations included - firstperson.ini,
 * Chinese comments and a full [zh_cn] table in one file, has been
 * the standing proof of that in the deployed tree. */
static void IniSaveInt(const char *key, int v) {
    char buf[24];

    if (!g_iniPath[0]) return;
    snprintf(buf, sizeof(buf), "%d", v);
    if (!WritePrivateProfileStringA("Settings", key, buf, g_iniPath))
        Log("reinforce: could not save %s", key);
}

static void UpdateNumberLabels(void) {
    int sel = g_sel[g_group];
    int count = g_groupCount > 0 ? g_groupCount : 1;
    int lo = sel - 1 < 1 ? count : sel - 1;
    int hi = sel + 1 > count ? 1 : sel + 1;

    snprintf(g_numLabel[0], sizeof(g_numLabel[0]), "%d", lo);
    snprintf(g_numLabel[1], sizeof(g_numLabel[1]), "%d", sel);
    snprintf(g_numLabel[2], sizeof(g_numLabel[2]), "%d", hi);
}

/* Count this faction, wrap the spinner, and hand back the
 * archetype the spinner points at.  The walk is the framework's.
 *
 * That walk goes through ShNpcCount, and the framework's EnsureList
 * WAITS UP TO THREE SECONDS while the archetype registry is not
 * built yet (scripthook_npc.c:244).  This runs from the tick loop,
 * so an unbuildable registry would turn a 500 ms loop into a 3.5
 * second one and stall every wave decision with it - the same
 * class of stall that made an earlier test run look like the
 * trigger was dead.  One attempt every GROUP_RETRY_MS is all it
 * takes to notice the registry has arrived, and once it has, the
 * framework caches it and never waits again. */
#define GROUP_RETRY_MS 5000

static int g_groupsOk = 0;                /* the registry answered */
static unsigned long long g_groupTryAt = 0;
static uint64_t g_typeLast = 0;           /* the last id it named  */

static int GroupScan(int group, int *outCount, uint64_t *outId) {
    ShNpcArchetype a;
    unsigned long long now;
    int c, idx;

    if (outCount) *outCount = 0;
    /* A failed walk hands back the last good id rather than nothing:
     * what the spinner points at did not stop existing just because
     * the registry is momentarily unreadable, and the status line
     * should not read "type 0" because of it. */
    if (outId) *outId = g_typeLast;
    if (!pGroupSize || !pAtInGroup) return -1;

    now = GetTickCount64();
    if (!g_groupsOk && g_groupTryAt && now - g_groupTryAt < GROUP_RETRY_MS)
        return -1;
    g_groupTryAt = now;

    c = pGroupSize(group);
    if (c <= 0) {
        g_groupsOk = 0;
        return -1;
    }
    g_groupsOk = 1;

    if (g_sel[group] < 1) g_sel[group] = c;
    else if (g_sel[group] > c) g_sel[group] = 1;

    memset(&a, 0, sizeof(a));
    idx = pAtInGroup(group, g_sel[group] - 1, &a);
    if (outCount) *outCount = c;
    if (outId) *outId = a.id;
    if (a.id) g_typeLast = a.id;
    return idx;
}

/* This group's remembered archetype wins over the spinner, so a
 * type found once keeps working even when the catalogue moves
 * around - while another group still answers with its own enemy,
 * which one global slot could not do. */
/* A per wave pick, for the menu's random option.  Xorshift over a
 * counter: no seeding, no library state, safe from any thread. */
static volatile LONG g_rnd = 1;

static unsigned NextRandom(void) {
    unsigned x = (unsigned)InterlockedIncrement(&g_rnd) * 2654435761u;

    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return x;
}

/* Types the formation spawner has refused this session.  A draw that
 * lands on one is skipped rather than waved away: the engine said no
 * in milliseconds, so asking again costs nothing.
 *
 * This exists because a draw can be a type the spawner will not
 * place.  On 22:22:27 the pick 9325bd7a52 came back "asked 5, got 0"
 * four milliseconds later, while a real spawn takes a quarter of a
 * second - and that wave simply never arrived, which is what made
 * the random option look like "one wave and then nothing". */
#define BAD_MAX 16

static uint64_t g_badId[BAD_MAX];
static int      g_badN = 0;

static int IsBadId(uint64_t id) {
    int i, hit = 0;

    EnterCriticalSection(&g_lock);
    for (i = 0; i < g_badN; i++)
        if (g_badId[i] == id) { hit = 1; break; }
    LeaveCriticalSection(&g_lock);
    return hit;
}

static void NoteBadId(uint64_t id) {
    int i, seen = 0;

    if (!id) return;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < g_badN; i++)
        if (g_badId[i] == id) { seen = 1; break; }
    if (!seen && g_badN < BAD_MAX) g_badId[g_badN++] = id;
    LeaveCriticalSection(&g_lock);
}

/* One archetype at random out of the group the menu has selected,
 * or 0 when the group cannot be walked.  Types already refused are
 * passed over, so a session stops drawing them; the index and the
 * group size go back to the caller so the draw is in the log. */
static uint64_t RandomTypeInGroup(int *outIdx, int *outN) {
    ShNpcArchetype a;
    uint64_t id = 0;
    int n, try;

    if (outIdx) *outIdx = -1;
    if (outN) *outN = 0;
    if (!pGroupSize || !pAtInGroup) return 0;

    n = pGroupSize(g_group);
    if (n <= 0) return 0;
    if (outN) *outN = n;

    for (try = 0; try < 6; try++) {
        int idx = (int)(NextRandom() % (unsigned)n);

        memset(&a, 0, sizeof(a));
        if (pAtInGroup(g_group, idx, &a) < 0) return id;

        id = a.id;
        if (outIdx) *outIdx = idx;
        if (id && !IsBadId(id)) return id;
    }
    return id;
}

static uint64_t ResolveType(void) {
    uint64_t id = 0;
    int count = 0;

    GroupScan(g_group, &count, &id);
    g_groupCount = count;
    UpdateNumberLabels();

    if (g_remembered[g_group]) return g_remembered[g_group];
    return id;
}

/* ---- waves ------------------------------------------------------ */

typedef struct {
    ShNpcSpawnRequest req;
    uint64_t fallback;   /* the spinner's type, kept for a refused draw */
} Wave;

static DWORD WINAPI WaveThread(LPVOID p) {
    Wave *w = (Wave *)p;
    ShNpcSpawnRequest req;
    uint64_t ent[SH_NPC_SPAWN_MAX];
    uint64_t fallback = 0;
    unsigned long long t0, dt;
    int got;

    if (!w) { InterlockedExchange(&g_spawning, 0); return 0; }
    req = w->req;
    fallback = w->fallback;
    free(w);

    t0 = GetTickCount64();
    got = pSpawnFormation(&req, ent, SH_NPC_SPAWN_MAX);
    dt = GetTickCount64() - t0;

    /* A drawn type can be one the spawner refuses outright: it
     * answers with nothing in milliseconds, where a real spawn takes
     * a quarter of a second.  Without this the wave is simply lost -
     * the log said "asked 5, got 0" and no enemy ever came.  The
     * spinner's own type is the fallback, so a bad draw costs the
     * randomness and not the wave.
     *
     * Only a FAST no is remembered as a bad type.  A slow zero is the
     * framework's own hand off to the game thread timing out, which
     * says nothing about the type - blacklisting on that would blame
     * a good archetype for a bad frame. */
    if (got == 0 && fallback && fallback != req.id) {
        if (dt < REFUSE_FAST_MS) NoteBadId(req.id);
        Log("reinforce: type %llx refused in %llu ms, asking for %llx instead",
            (unsigned long long)req.id, dt, (unsigned long long)fallback);
        req.id = fallback;
        got = pSpawnFormation(&req, ent, SH_NPC_SPAWN_MAX);
    }

    if (got > 0) RecordSpawns(got);
    Log("reinforce: wave type=%llx asked %d, got %d",
        (unsigned long long)req.id, req.count, got);

    InterlockedExchange64(&g_lastWave, (LONG64)GetTickCount64());
    InterlockedExchange(&g_spawning, 0);
    return 0;
}

static void StartWave(int count) {
    Wave *w;
    HANDLE h;
    uint64_t id, fallback;

    /* Every way out of here says why it happened.  A wave that
     * never arrives has to be distinguishable in the log from one
     * that never started - that is what made the last test run
     * undiagnosable. */
    if (InterlockedCompareExchange(&g_spawning, 1, 0)) {
        Log("reinforce: wave skipped, one is already in flight");
        return;
    }
    if (!pSpawnFormation) {
        Log("reinforce: wave failed, no spawn export");
        InterlockedExchange(&g_spawning, 0);
        return;
    }

    id = ResolveType();
    /* Held on to whatever the spinner says, because a random draw is
     * not guaranteed to be a type that can be placed. */
    fallback = id;

    if (InterlockedCompareExchange(&g_random, 0, 0)) {
        int idx = -1, n = 0;
        uint64_t r = RandomTypeInGroup(&idx, &n);

        if (r) {
            id = r;
            Log("reinforce: random type %llx, %d of %d in %s",
                (unsigned long long)id, idx + 1, n, g_groupOpts[g_group]);
        } else {
            Log("reinforce: random draw came back empty, using %llx",
                (unsigned long long)id);
        }
    }
    if (!id) {
        Log("reinforce: wave failed, no archetype for %s",
            g_groupOpts[g_group]);
        InterlockedExchange(&g_spawning, 0);
        return;
    }

    w = (Wave *)malloc(sizeof(*w));
    if (!w) {
        Log("reinforce: wave failed, out of memory");
        InterlockedExchange(&g_spawning, 0);
        return;
    }

    w->req.id = id;
    w->req.count = count;
    w->req.distance = g_distM[g_distIdx];
    w->req.formation = g_formation;
    w->req.facing = SH_NPC_FACING_PLAYER;
    w->fallback = fallback;

    /* Logged before the thread starts, so "wave" always precedes
     * the batch's own "asked ... got ..." line when both appear. */
    Log("reinforce: wave %llx x%d, %s", (unsigned long long)id, count,
        g_groupOpts[g_group]);

    h = CreateThread(NULL, 0, WaveThread, w, 0, NULL);
    if (!h) {
        Log("reinforce: wave failed, no thread");
        free(w);
        InterlockedExchange(&g_spawning, 0);
        return;
    }
    CloseHandle(h);
}

/* ---- the line under the menu ------------------------------------ */

static void RefreshStatus(void) {
    unsigned long long now, lastHit, type;
    int ours, left, random;

    if (!g_menu) return;

    if (!pGroupSize) { SetStatus("unavailable"); return; }

    now = GetTickCount64();
    lastHit = (unsigned long long)g_lastHit;
    ours = SpawnCount();
    /* Walked every tick both because the line shows the type and
     * because the walk is what keeps the spinner's labels current. */
    type = ResolveType();
    random = InterlockedCompareExchange(&g_random, 0, 0) ? 1 : 0;

    if (!InterlockedCompareExchange(&g_active, 0, 0)) {
        if (pMenuStatusF) {
            if (random)
                pMenuStatusF(g_menu, "off (%s, a random type each wave)",
                             g_groupOpts[g_group]);
            else
                pMenuStatusF(g_menu, "off (type %llx, %s)",
                             (unsigned long long)type,
                             g_groupOpts[g_group]);
        }
        return;
    }
    if (!lastHit || now - lastHit >= (unsigned long long)g_fightS * 1000u) {
        if (pMenuStatusF) {
            if (random)
                pMenuStatusF(g_menu, "on, waiting to be hit (%s, random)",
                             g_groupOpts[g_group]);
            else
                pMenuStatusF(g_menu, "on, waiting to be hit (type %llx, %s)",
                             (unsigned long long)type,
                             g_groupOpts[g_group]);
        }
        return;
    }

    left = g_interval - (int)((now - (unsigned long long)g_lastWave) / 1000);
    if (left < 0) left = 0;
    if (pMenuStatusF)
        pMenuStatusF(g_menu,
                     "in combat: %s answers, wave %d, spawned %d/%d, next %d s",
                     g_groupOpts[g_group],
                     (int)InterlockedCompareExchange(&g_waveN, 0, 0),
                     ours, g_cap, left);
}

/* ---- the main loop ---------------------------------------------- */

/* The wave is decided from the flag, never from a clock: a tick
 * that runs late postpones a wave, but can never lose the event.
 * That is the point of this shape - the old one read a timestamp
 * the callback thread had written, and the event went missing. */
static void CombatTick(void) {
    unsigned long long now;
    int live, want;

    now = GetTickCount64();

    /* Nothing new since the last wave. */
    if (!InterlockedCompareExchange(&g_hitPending, 0, 0)) return;

    if (!g_lastHit || now - (unsigned long long)g_lastHit >=
                      (unsigned long long)g_fightS * 1000u) {
        /* The fight is over - or the flag outlived it through a
         * load screen - so drop it rather than wave into a cold
         * scene, and open a fresh budget for whatever comes next. */
        int done = (int)InterlockedExchange(&g_waveN, 0);

        InterlockedExchange(&g_hitPending, 0);
        Log("reinforce: fight over, %d wave(s) sent, that hit is %llu ms old",
            done, now - (unsigned long long)g_lastHit);
        return;
    }

    /* The per fight budget, checked before the cap: with the menu's
     * default of 0 there is no budget at all. */
    if (g_waves > 0 &&
        (int)InterlockedCompareExchange(&g_waveN, 0, 0) >= g_waves) {
        InterlockedExchange(&g_hitPending, 0);
        if (now - g_lastSkipLog >= 5000) {
            g_lastSkipLog = now;
            Log("reinforce: wave held, this fight's %d wave(s) are sent",
                g_waves);
        }
        return;
    }

    live = SpawnCount();
    if (live >= g_cap) {
        InterlockedExchange(&g_hitPending, 0);
        if (now - g_lastSkipLog >= 5000) {
            g_lastSkipLog = now;
            Log("reinforce: wave held, at the cap %d/%d", live, g_cap);
        }
        return;
    }

    /* Too soon after the last one: keep the flag, so the next
     * tick tries again and the wave still arrives. */
    if (g_interval > 0 &&
        now - (unsigned long long)g_lastWave <
            (unsigned long long)g_interval * 1000u)
        return;

    want = g_batch;
    if (live + want > g_cap) want = g_cap - live;
    if (want <= 0) return;

    InterlockedIncrement(&g_waveN);
    InterlockedExchange(&g_hitPending, 0);
    StartWave(want);
}

/* ---- a new life ------------------------------------------------- */

/* The states a fight can happen in.  Anything else - a load screen,
 * the game over card, the shell menus - means the player is between
 * lives. */
static int LiveState(int st) {
    return st == SH_STATE_INGAME || st == SH_STATE_PAUSED ||
           st == SH_STATE_DRONE || st == SH_STATE_BINOCULAR ||
           st == SH_STATE_CINEMATIC;
}

/* A respawn or a redeploy is a new situation: the fight that was on
 * belonged to the last one.  So everything the next wave is decided
 * from is dropped - the pending flag, the hit it came from, the
 * fight's budget, and the spawn count from a place the player has
 * left.  Nothing crosses over, and the trigger re-arms itself with
 * the first hit in the new life. */
static void CheckNewLife(void) {
    static int last = -1;
    int st, had;

    if (!pGetGameState) return;

    st = pGetGameState();
    if (st == last) return;
    last = st;

    /* Unknown is left alone: it shows up at startup, and resetting
     * there would only hand out a second budget for nothing. */
    if (LiveState(st) || st == SH_STATE_UNKNOWN) return;

    had = (int)InterlockedExchange(&g_waveN, 0);
    InterlockedExchange(&g_hitPending, 0);
    InterlockedExchange64(&g_lastHit, 0);
    ClearSpawns();

    if (pGetGameStateName) {
        char nm[48] = "";
        pGetGameStateName(nm, sizeof(nm));
        Log("reinforce: %s - reinforcements stand down (%d wave(s) so far)",
            nm[0] ? nm : "between lives", had);
    } else {
        Log("reinforce: state %d - reinforcements stand down (%d wave(s))",
            st, had);
    }
}

static DWORD WINAPI TickThread(LPVOID p) {
    int tick = 0;
    (void)p;

    for (;;) {
        unsigned long long t0, dt;

        Sleep(TICK_MS);

        /* A respawn or a redeploy starts a new life, and nothing
         * from the last one may follow the player into it. */
        CheckNewLife();

        /* Nothing on this thread touches an entity any more: the
         * decision comes from a flag, and the cap from a count of
         * our own spawn records.  The timings stay so that if the
         * loop ever does lose seconds, the stage is named rather
         * than guessed at from a gap in the log. */
        if (InterlockedCompareExchange(&g_active, 0, 0) &&
            (!pIsInGame || pIsInGame())) {
            t0 = GetTickCount64();
            CombatTick();
            dt = GetTickCount64() - t0;
            if (dt >= 1000) Log("reinforce: slow: combat %llu ms", dt);
        }

        t0 = GetTickCount64();

        /* A count of our own recent spawns, every few seconds, so
         * the log has a heartbeat.  Deliberately NOT a sweep of the
         * field: nothing outside this plugin's own records is
         * looked at. */
        if (++tick % 10 == 0)
            Log("reinforce: %d spawned in the last %d s", SpawnCount(),
                SPAWN_WINDOW_MS / 1000);

        /* The catalogue reads as empty until the world is loaded,
         * which is what left the probe with nothing to match
         * against, so keep asking.  Each failed ask costs up to the
         * framework's three second wait, hence one every 20 s. */
        if (g_idn <= 0 && (tick % 40) == 0) {
            BuildCatalogue();
            if (g_idn > 0) {
                if (pNpcGroupOf) pNpcGroupOf(g_ids[0]);
                Log("reinforce: catalogue %d ids, probe %s", g_idn,
                    InterlockedCompareExchange(&g_probe, 0, 0) ? "on"
                                                               : "off");
            }
        }
        RefreshStatus();

        dt = GetTickCount64() - t0;
        if (dt >= 1000) Log("reinforce: slow: status %llu ms", dt);
    }
    return 0;
}

/* ---- menu callbacks --------------------------------------------- */

static void OnActive(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    InterlockedExchange(&g_active, value ? 1 : 0);
    InterlockedExchange(&g_hitPending, 0);
    InterlockedExchange(&g_waveN, 0);
    InterlockedExchange64(&g_lastHit, 0);
    /* Zero rather than now: the first hit after switching on is
     * answered at once, not one interval later. */
    if (value) InterlockedExchange64(&g_lastWave, 0);
    IniSaveInt("active", value ? 1 : 0);
    Log("reinforce: %s, %s answers", value ? "on" : "off",
        g_groupOpts[g_group]);
    RefreshStatus();
}

/* The opt in for treating "the enemies are shooting at me" as the
 * same thing as being hit. */
static void OnFireToggle(uint32_t menu, uint32_t item, int value,
                         void *user) {
    (void)menu; (void)item; (void)user;
    InterlockedExchange(&g_onFire, value ? 1 : 0);
    IniSaveInt("fire_combat", value ? 1 : 0);
    Log("reinforce: enemy fire within %d m %s", g_scanR,
        value ? "counts as combat" : "is ignored");
    RefreshStatus();
}

static void OnGroup(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    if (value < 0 || value >= SH_NPC_GROUP_MAX) return;
    g_group = value;
    /* Rescan: the count, the spinner and the remembered archetype
     * are all per group, so this is what makes the answering
     * faction follow the menu. */
    Log("reinforce: now answering as %s", g_groupOpts[g_group]);
    ResolveType();
    /* The file carries one group and one number, so both go down. */
    IniSaveInt("group", g_group);
    IniSaveInt("number", g_sel[g_group]);
    RefreshStatus();
}

/* The spinner: entry 0 means "one back", 2 means "one on". */
static void OnNumber(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    if (g_groupCount <= 0) return;
    if (value == 0) {
        if (--g_sel[g_group] < 1) g_sel[g_group] = g_groupCount;
    } else if (value == 2) {
        if (++g_sel[g_group] > g_groupCount) g_sel[g_group] = 1;
    }
    /* Spinning drops this group's remembered type, or the memory
     * would keep winning over the spinner being turned. */
    g_remembered[g_group] = 0;
    IniSaveInt("number", g_sel[g_group]);
    RefreshStatus();
    /* Snap the row back to its middle entry, so it keeps reading
     * one back / current / one on. */
    if (pMenuSetValue) pMenuSetValue(g_menu, "NPC Number", 1);
}

/* Off: the spinner above decides.  On: every wave draws a random
 * archetype out of the group the menu has selected, and the spinner
 * is only there to show what the group holds. */
static void OnRandom(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    InterlockedExchange(&g_random, value ? 1 : 0);
    IniSaveInt("random", value ? 1 : 0);
    /* Said out loud: this row used to be silent, so a test run could
     * not tell whether random had ever been switched on. */
    Log("reinforce: random type %s", value ? "on, a new draw each wave"
                                           : "off, the spinner decides");
    RefreshStatus();
}

static void OnInterval(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    g_interval = value;
    IniSaveInt("interval", g_interval);
    RefreshStatus();
}

static void OnBatch(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    g_batch = value;
    IniSaveInt("batch", g_batch);
    RefreshStatus();
}

static void OnCap(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    g_cap = value;
    IniSaveInt("cap", g_cap);
    RefreshStatus();
}

/* How many waves one fight gets.  0 means as many as the hits keep
 * asking for, which is what the plugin did before this row existed.
 * The budget opens again when the hits stop - see CombatTick. */
static void OnWaves(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    g_waves = value;
    IniSaveInt("waves", g_waves);
    RefreshStatus();
}

/* How long without being hit counts as the fight being over.  The
 * wave budget opens again there, and until it elapses the status
 * line keeps saying what is coming next. */
static void OnFight(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    g_fightS = value < 1 ? 1 : value;
    IniSaveInt("fight_window", g_fightS);
    RefreshStatus();
}

/* How far the two sample actions, and any state read later, look
 * for enemies.  The wave itself does not use it: that is placed
 * ahead of the player rather than found. */
static void OnScanR(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    g_scanR = value < SCAN_MIN ? SCAN_MIN
            : value > SCAN_MAX ? SCAN_MAX : value;
    IniSaveInt("scan_radius", g_scanR);
    RefreshStatus();
}

static void OnDist(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    if (value >= 0 && value < 6) g_distIdx = value;
    IniSaveInt("distance_idx", g_distIdx);
    RefreshStatus();
}

static void OnFormation(uint32_t menu, uint32_t item, int value,
                        void *user) {
    (void)menu; (void)item; (void)user;
    if (value >= 0 && value < 5) g_formation = value;
    IniSaveInt("formation", g_formation);
    RefreshStatus();
}

static void OnSpawnOne(uint32_t menu, uint32_t item, int value,
                       void *user) {
    (void)menu; (void)item; (void)value; (void)user;
    StartWave(1);
}

/* Sample the nearest NPC on demand, so the probe can be checked
 * standing next to an enemy of a known faction instead of waiting
 * to be shot at. */
/* The nearest NPC, sampled on a worker thread: the find itself
 * reads health for every candidate it keeps, and that is not
 * something to do on the thread the menu is drawn from. */
static void OnProbe(uint32_t menu, uint32_t item, int value,
                    void *user) {
    HANDLE h;

    (void)menu; (void)item; (void)value; (void)user;

    if (!pFindEntities || !pGetComponents || !pReadU64) {
        SetStatus("no find export");
        return;
    }
    if (InterlockedCompareExchange(&g_probing, 1, 0)) {
        SetStatus("a sample is already running");
        return;
    }
    h = CreateThread(NULL, 0, ProbeThread, NULL, 0, NULL);
    if (!h) {
        InterlockedExchange(&g_probing, 0);
        SetStatus("could not start the sample");
        return;
    }
    CloseHandle(h);
    SetStatus("probe written to the log");
}

/* One sample per press, on its own thread (see SampleThread).  Press
 * it once standing in each of the game's own three states - unseen,
 * alerted, in combat - and the log will carry all three to compare.
 * The other source is automatic: every shot that lands on the player
 * samples whoever fired it. */
static void OnSampleAlert(uint32_t menu, uint32_t item, int value,
                          void *user) {
    HANDLE h;

    (void)menu; (void)item; (void)value; (void)user;

    if (!pFindComponent || !pReadBytes) {
        SetStatus("no component exports");
        return;
    }
    if (InterlockedCompareExchange(&g_sampling, 1, 0)) {
        SetStatus("a sample is already running");
        return;
    }
    h = CreateThread(NULL, 0, SampleThread, NULL, 0, NULL);
    if (!h) {
        InterlockedExchange(&g_sampling, 0);
        SetStatus("could not start the sample");
        return;
    }
    CloseHandle(h);
    SetStatus("probe written to the log");
}

/* Written to scripthook.ini rather than to our own ini, because
 * that is where it has always been and it belongs to the framework's
 * config either way: the remembered type is per faction, not per
 * plugin setting.  Our own ini would take it just as well. */
static void OnRemember(uint32_t menu, uint32_t item, int value,
                       void *user) {
    uint64_t id = ResolveType();
    char buf[32], key[24];
    (void)menu; (void)item; (void)value; (void)user;

    if (!id) { SetStatus("nothing to remember"); return; }

    /* One key per group, so remembering Santa Blanca's type does
     * not overwrite what Unidad answers with.  Hex text, not a
     * number: an archetype id is wider than the 32 bit ints the
     * config setters take. */
    snprintf(key, sizeof(key), "type_id_%d", g_group);
    snprintf(buf, sizeof(buf), "%llx", (unsigned long long)id);

    if (pConfigSetStr && pConfigSetStr("EnemyReinforce", key, buf)) {
        g_remembered[g_group] = id;
        Log("reinforce: remembered %s type %llx as %s",
            g_groupOpts[g_group], (unsigned long long)id, key);
        SetStatus("type remembered");
    } else {
        SetStatus("could not write the config");
    }
}

/* ---- menu ------------------------------------------------------- */

static uint32_t BuildMenu(void) {
    uint32_t m;

    m = pMenuCreate("Enemy Reinforce");
    if (!m) return 0;

    UpdateNumberLabels();

    pMenuToggle(m, "Active", (int)InterlockedCompareExchange(&g_active, 0, 0),
                OnActive, NULL);
    pMenuToggle(m, "Nearby fire",
                (int)InterlockedCompareExchange(&g_onFire, 0, 0),
                OnFireToggle, NULL);
    pMenuList(m, "NPC Group", g_groupOpts, SH_NPC_GROUP_MAX, g_group,
              OnGroup, NULL);
    pMenuList(m, "NPC Number", g_numOpts, 3, 1, OnNumber, NULL);
    pMenuToggle(m, "Random number",
                (int)InterlockedCompareExchange(&g_random, 0, 0),
                OnRandom, NULL);
    pMenuAction(m, "Spawn one", OnSpawnOne, NULL);
    pMenuAction(m, "Remember type", OnRemember, NULL);
    pMenuAction(m, "Probe nearby enemy", OnProbe, NULL);
    pMenuAction(m, "Sample alert state", OnSampleAlert, NULL);
    pMenuNumber(m, "Interval s", (float)g_interval, 2.0f, 60.0f, 1.0f,
                OnInterval, NULL);
    pMenuNumber(m, "Batch size", (float)g_batch, 1.0f, 12.0f, 1.0f,
                OnBatch, NULL);
    pMenuNumber(m, "Cap", (float)g_cap, 1.0f, 32.0f, 1.0f, OnCap, NULL);
    pMenuNumber(m, "Wave limit", (float)g_waves, 0.0f, 50.0f, 1.0f,
                OnWaves, NULL);
    pMenuNumber(m, "Fight window s", (float)g_fightS, 5.0f, 120.0f, 5.0f,
                OnFight, NULL);
    pMenuNumber(m, "Engage radius m", (float)g_scanR, 20.0f, 300.0f, 10.0f,
                OnScanR, NULL);
    pMenuList(m, "Distance", g_distOpts, 6, g_distIdx, OnDist, NULL);
    pMenuList(m, "Formation", g_formationOpts, 5, g_formation,
              OnFormation, NULL);

    RefreshStatus();
    return m;
}

/* ---- startup ---------------------------------------------------- */

static void LoadSettings(void) {
    ResolveIniPath();
    g_group = IniInt("group", SH_NPC_GROUP_SANTA_BLANCA);
    if (g_group < 0 || g_group >= SH_NPC_GROUP_MAX)
        g_group = SH_NPC_GROUP_SANTA_BLANCA;
    g_sel[g_group] = IniInt("number", 1);
    g_interval = IniInt("interval", 8);
    g_batch = IniInt("batch", 3);
    g_cap = IniInt("cap", 8);
    g_waves = IniInt("waves", 0);
    InterlockedExchange(&g_random, IniInt("random", 0) ? 1 : 0);
    g_fightS = IniInt("fight_window", FIGHT_WINDOW_S);
    if (g_fightS < 1) g_fightS = FIGHT_WINDOW_S;
    g_scanR = IniInt("scan_radius", (int)NEAR_RADIUS);
    if (g_scanR < SCAN_MIN) g_scanR = SCAN_MIN;
    if (g_scanR > SCAN_MAX) g_scanR = SCAN_MAX;
    g_distIdx = IniInt("distance_idx", 3);
    g_formation = IniInt("formation", SH_NPC_FORMATION_RANDOM);
    InterlockedExchange(&g_active, IniInt("active", 0) ? 1 : 0);
    /* Defaults to on: the probe's dumps are what the next round of
     * this work reads, and it is capped either way. */
    InterlockedExchange(&g_probe, IniInt("probe", 1) ? 1 : 0);
    InterlockedExchange(&g_onFire, IniInt("fire_combat", 0) ? 1 : 0);

    if (g_distIdx < 0 || g_distIdx > 5) g_distIdx = 3;
    if (g_formation < 0 || g_formation > SH_NPC_FORMATION_RANDOM)
        g_formation = SH_NPC_FORMATION_RANDOM;
}

static DWORD WINAPI InitThread(LPVOID p) {
    HMODULE mod = NULL;
    uint32_t menu;
    (void)p;

    EnsureLocks();

    while (!mod) {
        mod = GetModuleHandleA("dinput8.dll");
        if (!mod) Sleep(500);
    }

    *(FARPROC *)&pGetVersion = GetProcAddress(mod, "ShGetVersion");
    *(FARPROC *)&pIsInGame = GetProcAddress(mod, "ShIsInGame");
    *(FARPROC *)&pGetGameState = GetProcAddress(mod, "ShGetGameState");
    *(FARPROC *)&pGetGameStateName =
        GetProcAddress(mod, "ShGetGameStateName");
    *(FARPROC *)&pHitHookInstall = GetProcAddress(mod, "ShHitHookInstall");
    *(FARPROC *)&pOnHit = GetProcAddress(mod, "ShOnHit");
    *(FARPROC *)&pOnFire = GetProcAddress(mod, "ShOnFire");
    *(FARPROC *)&pGetPlayer = GetProcAddress(mod, "ShGetPlayer");
    *(FARPROC *)&pGetPos = GetProcAddress(mod, "ShGetPlayerPosition");
    *(FARPROC *)&pFindEntities = GetProcAddress(mod, "ShFindEntities");
    *(FARPROC *)&pNpcCount = GetProcAddress(mod, "ShNpcCount");
    *(FARPROC *)&pNpcAt = GetProcAddress(mod, "ShNpcAt");
    *(FARPROC *)&pGroupSize = GetProcAddress(mod, "ShNpcGroupSize");
    *(FARPROC *)&pAtInGroup = GetProcAddress(mod, "ShNpcAtInGroup");
    *(FARPROC *)&pNpcGroupOf = GetProcAddress(mod, "ShNpcGroupOf");
    *(FARPROC *)&pNpcGroupName = GetProcAddress(mod, "ShNpcGroupName");
    *(FARPROC *)&pGetComponents = GetProcAddress(mod, "ShGetComponents");
    *(FARPROC *)&pReadU64 = GetProcAddress(mod, "ShReadU64");
    *(FARPROC *)&pFindComponent = GetProcAddress(mod, "ShFindComponent");
    *(FARPROC *)&pReadBytes = GetProcAddress(mod, "ShReadBytes");
    *(FARPROC *)&pSpawnFormation =
        GetProcAddress(mod, "ShNpcSpawnFormation");
    *(FARPROC *)&pMenuCreate = GetProcAddress(mod, "ShMenuCreate");
    *(FARPROC *)&pMenuList = GetProcAddress(mod, "ShMenuList");
    *(FARPROC *)&pMenuToggle = GetProcAddress(mod, "ShMenuToggle");
    *(FARPROC *)&pMenuNumber = GetProcAddress(mod, "ShMenuNumber");
    *(FARPROC *)&pMenuAction = GetProcAddress(mod, "ShMenuAction");
    *(FARPROC *)&pMenuSetValue = GetProcAddress(mod, "ShMenuSetValue");
    *(FARPROC *)&pMenuStatus = GetProcAddress(mod, "ShMenuStatus");
    *(FARPROC *)&pMenuStatusF = GetProcAddress(mod, "ShMenuStatusF");
    *(FARPROC *)&pConfigGetStr = GetProcAddress(mod, "ShConfigGetStr");
    *(FARPROC *)&pConfigSetStr = GetProcAddress(mod, "ShConfigSetStr");

    if (!pGetVersion || !pIsInGame ||
        !pGroupSize || !pAtInGroup || !pSpawnFormation ||
        !pMenuCreate || !pMenuList || !pMenuToggle || !pMenuNumber ||
        !pMenuAction || !pMenuStatus) {
        Log("reinforce: required export missing, giving up");
        return 1;
    }

    while (!pGetVersion()) Sleep(500);

    LoadSettings();

    /* Remembered types live in scripthook.ini as hex text, one key
     * per group.  The single key the first version wrote is read as
     * group 0, so an install from before keeps its type. */
    if (pConfigGetStr) {
        char buf[32] = "";
        int g;

        for (g = 0; g < SH_NPC_GROUP_MAX; g++) {
            char key[24];
            snprintf(key, sizeof(key), "type_id_%d", g);
            buf[0] = 0;
            if (pConfigGetStr("EnemyReinforce", key, "", buf,
                              sizeof(buf)) && buf[0])
                g_remembered[g] = _strtoui64(buf, NULL, 16);
        }

        buf[0] = 0;
        if (!g_remembered[SH_NPC_GROUP_SANTA_BLANCA] &&
            pConfigGetStr("EnemyReinforce", "type_id", "", buf,
                          sizeof(buf)) && buf[0])
            g_remembered[SH_NPC_GROUP_SANTA_BLANCA] =
                _strtoui64(buf, NULL, 16);

        for (g = 0; g < SH_NPC_GROUP_MAX; g++)
            if (g_remembered[g])
                Log("reinforce: remembered %s type %llx",
                    g_groupOpts[g], (unsigned long long)g_remembered[g]);
    }

    /* Walk the catalogue once here, so the tick loop never carries
     * the first call's wait - and the same for the probe, which has
     * to stay quick on the hit callback's thread. */
    ResolveType();
    BuildCatalogue();
    if (g_idn > 0 && pNpcGroupOf) pNpcGroupOf(g_ids[0]);
    Log("reinforce: catalogue %d ids, probe %s", g_idn,
        InterlockedCompareExchange(&g_probe, 0, 0) ? "on" : "off");

    if (pHitHookInstall && pOnHit) {
        if (!pHitHookInstall()) Log("reinforce: hit hook did not install");
        else {
            pOnHit(OnHit, NULL, 0);
            /* The same hook feeds the shot receiver, so this needs no
             * second install. */
            if (pOnFire) pOnFire(OnFire, NULL, 0);
            if (!pGetPlayer)
                Log("reinforce: no ShGetPlayer, the hit test falls back "
                    "to the entity kind");
        }
    } else {
        Log("reinforce: no hit hook, nothing can ever trigger a wave");
    }

    Log("reinforce: up, ini=%s active=%d probe=%d random=%d fire=%d "
        "group=%s interval=%d batch=%d cap=%d waves=%d fight=%d engage=%d",
        g_iniPath[0] ? g_iniPath : "(none)",
        (int)InterlockedCompareExchange(&g_active, 0, 0),
        (int)InterlockedCompareExchange(&g_probe, 0, 0),
        (int)InterlockedCompareExchange(&g_random, 0, 0),
        (int)InterlockedCompareExchange(&g_onFire, 0, 0),
        g_groupOpts[g_group],
        g_interval, g_batch, g_cap, g_waves, g_fightS, g_scanR);

    menu = BuildMenu();
    if (!menu) {
        Log("reinforce: menu create failed");
        return 1;
    }
    g_menu = menu;
    RefreshStatus();

    CreateThread(NULL, 0, TickThread, NULL, 0, NULL);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        g_inst = inst;
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
    }
    return TRUE;
}
