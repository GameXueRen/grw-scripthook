/* phoenix::Scenes of our own, one per plugin layer. */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"
#include "log.h"

#define F_ALLOC_CTX     SH_IMG(0xe410100)
#define F_ALLOC         SH_IMG(0x674F1A0)
#define G_POOL          SH_IMG(0x4D78D80)
#define F_SCENE_CTOR    SH_IMG(0x32EE140)
#define F_SCENE_DTOR    SH_IMG(0x32EE1C0)
#define F_SCENE_TICK    SH_IMG(0x16b9bb20)
#define F_SCENE_FLIP    SH_IMG(0x16b9b440)
#define F_SCENE_RENDER  SH_IMG(0x16b99a30)
#define F_SCENE_RESIZE  SH_IMG(0x16b98d80)
#define F_SCENE_SETCTX  SH_IMG(0x16B9A730)
#define F_SCENE_SETRES  SH_IMG(0x16B99DC0)
#define F_ATTACH        SH_IMG(0x32F4230)
#define F_FREE          SH_IMG(0xE4F8110)
#define F_LOCK          SH_IMG(0x3621550)
#define F_UNLOCK        SH_IMG(0x3286FF0)
#define RENDER_THUNK    SH_IMG(0x32EE4A0)
#define G_UIMGR         SH_IMG(0x4D585E0)
#define VT_GAME_RESOLVER SH_IMG(0x3A05A00)

/* scene private */
#define SP_STATE     0x368
#define SP_ROOT      0x3E0
#define SP_IDX       0x3FC
#define STATE_LIVE   4

#define TICK_MS      16
#define JOB_WAIT_MS  3000
#define HASH_PLAYING 0x8816ABC6u
#define MAX_SCENES   16

extern uint32_t ShGetGameStateHash(void);
extern void ShSetError(int err);
extern int  ShIsInGame(void);
extern int  ShReadableAddr(uint64_t addr, size_t len);
extern void *ShAllocNear(uint64_t target);
extern int  ShQueueCall(uint64_t fn, uint64_t a0, uint64_t a1,
                        uint64_t a2, uint64_t a3);
extern int  ShQueueResult(uint64_t *outRet);

typedef uint64_t (__attribute__((ms_abi)) *Fn1)(uint64_t);
typedef uint64_t (__attribute__((ms_abi)) *Fn3)(uint64_t, uint64_t,
                                                uint64_t);
typedef int32_t *(__attribute__((ms_abi)) *Scene2)(uint64_t, int32_t *);
typedef int32_t *(__attribute__((ms_abi)) *Scene3)(uint64_t, int32_t *,
                                                   uint64_t);

/* our Localizer, nothing to translate */

static uint64_t __attribute__((ms_abi)) LocDtor(uint64_t self) {
    (void)self;
    return 0;
}

static int32_t *__attribute__((ms_abi)) LocLocalize(uint64_t self,
        int32_t *res, uint64_t id, uint64_t ctx, uint64_t out) {
    (void)self; (void)id; (void)ctx; (void)out;
    *res = 0;
    return res;
}

static uint64_t __attribute__((ms_abi)) LocOnText(uint64_t self,
                                                  uint64_t text) {
    (void)self; (void)text;
    return 0;
}

static void *g_locVt[4] = {
    (void *)LocDtor, (void *)LocLocalize, (void *)LocOnText,
    (void *)LocDtor
};
static struct { void **vt; } g_localizer = { g_locVt };
static struct { uint64_t vt; } g_resolver;

/* the scene table */

typedef struct {
    int      used;
    int      live;
    int      order;        /* negative: under the game */
    int      visible;
    uint64_t handle, priv, rootH, rootP;
    uint64_t vt;           /* the scene class vtable, to verify a teardown */
    LONG     flippedFrame; /* flip once per frame */
} SceneSlot;

static SceneSlot g_s[MAX_SCENES];
static CRITICAL_SECTION g_slock;
static volatile LONG g_slockInit = 0;
static uint8_t *g_stub;
static volatile LONG g_hookTried;
static int InstallHook(void);
static void EnsureHook(void);
static uint8_t g_thunkOrig[5];
static volatile uint32_t g_lastStamp;
static volatile int64_t g_lastRender;
static int64_t g_qpcFreq;
static volatile LONG g_frame;

static void SLock(void) {
    /* CAS-claimed init: two threads first-touching the lock at once
     * must not both InitializeCriticalSection (spawn.c pattern). */
    for (;;) {
        LONG s = InterlockedCompareExchange(&g_slockInit, 0, 0);
        if (s == 1) break;
        if (s == 2) { Sleep(0); continue; }
        if (InterlockedCompareExchange(&g_slockInit, 2, 0)) continue;
        InitializeCriticalSection(&g_slock);
        LogInit("scripthook_scene.log");
        InterlockedExchange(&g_slockInit, 1);
        break;
    }
    EnterCriticalSection(&g_slock);
}
static void SUnlock(void) { LeaveCriticalSection(&g_slock); }

static uint64_t RQ(uint64_t a) {
    uint64_t v = 0;
    if (ShReadableAddr(a, 8)) memcpy(&v, (void *)(uintptr_t)a, 8);
    return v;
}

static uint64_t EAlloc(size_t size) {
    uint64_t pool = RQ(G_POOL), ctx, mem;
    if (!pool) return 0;
    ctx = ((Fn3)F_ALLOC_CTX)(size, 8, pool);
    mem = ((Fn3)F_ALLOC)(size, 8, ctx);
    if (mem) memset((void *)(uintptr_t)mem, 0, size);
    return mem;
}

static int IsOurs(uint64_t scene) {
    int i;
    for (i = 0; i < MAX_SCENES; i++)
        if (g_s[i].live && g_s[i].handle == scene) return 1;
    return 0;
}

/* ---- diagnostics, SH_UI_DIAG builds only ---- */

#ifdef SH_UI_DIAG
typedef struct {
    uint32_t frame, seq;
    uint64_t scene, renderer;
    uint8_t  ours;
} CallRec;
#define CRING 4096
static volatile CallRec g_callRing[CRING];
static volatile LONG g_callSeq;

static void RecordCall(uint64_t scene, uint64_t renderer, int ours) {
    LONG n = InterlockedIncrement(&g_callSeq);
    volatile CallRec *c = &g_callRing[n % CRING];
    c->frame = (uint32_t)g_frame; c->seq = (uint32_t)n;
    c->scene = scene; c->renderer = renderer; c->ours = (uint8_t)ours;
}

SH_API int ShSceneCallRec(int i, uint32_t *frame, uint32_t *seq,
                          uint64_t *scene, uint64_t *renderer, int *ours) {
    volatile CallRec *c;
    if (i < 0 || i >= CRING) return 0;
    c = &g_callRing[i];
    *frame = c->frame; *seq = c->seq; *scene = c->scene;
    *renderer = c->renderer; *ours = c->ours;
    return 1;
}
#else
#define RecordCall(scene, renderer, ours) ((void)0)
#endif

/* ---- render hook ---- */

static int NewFrame(void) {
    uint64_t mgr = RQ(G_UIMGR);
    uint32_t stamp = 0;
    LARGE_INTEGER now;
    int fresh;

    if (mgr && ShReadableAddr(mgr + 0xDC, 4))
        memcpy(&stamp, (void *)(uintptr_t)(mgr + 0xDC), 4);
    QueryPerformanceCounter(&now);
    fresh = (stamp != g_lastStamp) ||
            (now.QuadPart - g_lastRender) * 1000 > 5 * g_qpcFreq;
    if (fresh) {
        g_lastStamp = stamp;
        g_lastRender = now.QuadPart;
    }
    return fresh;
}

/* Which scenes this sign draws, lowest order first - kept between passes.
 *
 * The game draws its UI in several passes per frame and this runs before and
 * after each of them, so one answer was being sorted out again and again: a
 * selection sort over MAX_SCENES, up to two dozen times for a frame. The
 * list only changes when a scene is created, shown, hidden, reordered or the
 * world is invalidated, and each of those bumps the generation below. */
static volatile LONG g_ordGen;
static LONG          g_ordBuilt = -1;   /* the generation the lists hold */
static int           g_ordNeg[MAX_SCENES], g_nOrdNeg;
static int           g_ordPos[MAX_SCENES], g_nOrdPos;

/* Anything that can change which scenes are drawn, or in what order. */
static void BumpOrder(void) {
    InterlockedIncrement(&g_ordGen);
}

/* Fills out[] with the slots to draw for one sign, lowest order first. */
static void OrderOf(int negatives, int *out, int *n) {
    int done[MAX_SCENES] = {0};
    int i, pass, k = 0;

    for (pass = 0; pass < MAX_SCENES; pass++) {
        int best = -1;
        for (i = 0; i < MAX_SCENES; i++) {
            if (done[i] || !g_s[i].live || !g_s[i].visible) continue;
            if ((g_s[i].order < 0) != (negatives != 0)) continue;
            if (best < 0 || g_s[i].order < g_s[best].order) best = i;
        }
        if (best < 0) break;
        done[best] = 1;
        out[k++] = best;
    }
    *n = k;
}

static void RenderOurs(uint64_t renderer, int negatives) {
    const int *list;
    LONG gen = (LONG)InterlockedCompareExchange(&g_ordGen, 0, 0);
    int n, i;

    if (gen != g_ordBuilt) {
        OrderOf(1, g_ordNeg, &g_nOrdNeg);
        OrderOf(0, g_ordPos, &g_nOrdPos);
        g_ordBuilt = gen;
    }
    list = negatives ? g_ordNeg : g_ordPos;
    n = negatives ? g_nOrdNeg : g_nOrdPos;
    for (i = 0; i < n; i++) {
        SceneSlot *s = &g_s[list[i]];
        int32_t r2 = 0, r3 = 0;

        if (s->flippedFrame != g_frame) {
            s->flippedFrame = g_frame;
            ((Scene2)F_SCENE_FLIP)(s->handle, &r2);
        }
        RecordCall(s->handle, renderer, 1);
        ((Scene3)F_SCENE_RENDER)(s->handle, &r3, renderer);
    }
}

/* one tick per frame, render thread, before the flip */
static int64_t g_lastTick;

static void TickAll(void) {
    LARGE_INTEGER now;
    int64_t dt;
    int i;

    /* The render hook can now be installed by the HUD scene API alone
     * (EnsureHook), i.e. without any scene ever being allocated - in that
     * case Init() has never run and g_qpcFreq is still 0. Dividing by it
     * raised C0000094 (integer divide by zero) on the first frame. */
    if (!g_qpcFreq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcFreq = f.QuadPart ? f.QuadPart : 1;
    }

    QueryPerformanceCounter(&now);
    dt = g_lastTick ? (now.QuadPart - g_lastTick) * 1000 / g_qpcFreq : TICK_MS;
    g_lastTick = now.QuadPart;
    if (dt < 1) dt = 1;
    if (dt > 100) dt = 100;
    for (i = 0; i < MAX_SCENES; i++) {
        int32_t res = 0;
        if (!g_s[i].live || !g_s[i].visible) continue;
        ((Scene3)F_SCENE_TICK)(g_s[i].handle, &res, (uint64_t)dt);
    }
}

/* The game draws its UI in several passes per frame. */
/* A repeated scene starts a new pass; the scene before */
/* it ended the last one. Ours follows every pass end. */
#define MAX_PASS 8
#define MAX_SEEN 64
static uint64_t g_ends[MAX_PASS], g_endsNext[MAX_PASS];
static int g_nEnds, g_nEndsNext;
static uint64_t g_seen[MAX_SEEN];
static int g_nSeen;
static volatile uint64_t g_prevCall;
static volatile int g_doneThisFrame;

static int InList(const uint64_t *l, int n, uint64_t v) {
    int i;
    for (i = 0; i < n; i++) if (l[i] == v) return 1;
    return 0;
}

/* the game scenes rendered last frame: the UI state
 *
 * Two buffers and a sequence number, because the writer (the render hook,
 * once per frame) and the readers (any thread: the chat thread's 15ms
 * poll, a plugin's own worker, the menu's callback thread) do not share a
 * lock. Plain double buffering is not enough: the publish of the count and
 * the copy of the array are two separate stores, so a reader could see the
 * new count over a half-copied array - `ShGameSceneAt(i)` then answers with
 * a scene handle belonging to the *previous* frame, which is dead, and every
 * caller treats a dead handle as "nothing there".
 *
 * The sequence makes the copy and the publish one thing:
 *   writer  seq++ (odd)  copy  publish count  seq++ (even)
 *   reader  seq  n+element  seq   retry if either seq was odd or they differ
 * The cost is two interlocked increments a frame for the writer and two
 * interlocked reads per element for the reader; the reader's retry path is
 * bounded and, when it gives up, it answers "no scene" rather than a handle
 * from a frame nobody can reason about. */
#define MAX_ACTIVE 64
static uint64_t g_activeCur[MAX_ACTIVE], g_activePrev[MAX_ACTIVE];
static int g_nActiveCur;
static volatile LONG g_nActivePrev;   /* published by the seqlock below */
static volatile LONG g_sceneSeq;

/* Writer side, called once per frame by the render hook. */
static void PublishActive(void) {
    InterlockedIncrement(&g_sceneSeq);            /* odd: copy in progress */
    memcpy(g_activePrev, g_activeCur, sizeof(g_activePrev));
    InterlockedExchange(&g_nActivePrev, (LONG)g_nActiveCur);
    InterlockedIncrement(&g_sceneSeq);            /* even: stable again    */
}

static void NoteActive(uint64_t scene) {
    if (g_nActiveCur < MAX_ACTIVE && !InList(g_activeCur, g_nActiveCur, scene))
        g_activeCur[g_nActiveCur++] = scene;
}

/* name: root widget's template instance, string at +0x10 */
#define NAME_CACHE 128
static struct { uint64_t scene; char name[48]; } g_names[NAME_CACHE];
static volatile LONG g_nNames;

static const char *SceneName(uint64_t scene) {
    uint64_t priv, root, rootP, inst, blk;
    uint32_t len = 0;
    int i, n;

    /* A name already known is answered without the lock. This is called from
     * the render hook for every scene of every pass, and the cache fills once:
     * the lock was being taken to walk a table that is already there. Safe
     * because a slot is written once and the count is published after it
     * (InterlockedIncrement is a full barrier), so a reader that sees the count
     * sees the name; and because entries are never moved or rewritten, the
     * pointer handed back stays good for the session. */
    n = (int)InterlockedCompareExchange(&g_nNames, 0, 0);
    for (i = 0; i < n; i++)
        if (g_names[i].scene == scene) return g_names[i].name;

    /* A miss still takes the lock, and looks again under it: two threads can
     * miss the same scene at once, and without the lock two first-lookups raced
     * for one slot and a wrong name stuck there for the session (UI state
     * misread). */
    SLock();
    for (i = 0; i < g_nNames; i++)
        if (g_names[i].scene == scene) {
            const char *hit = g_names[i].name;
            SUnlock();
            return hit;
        }
    if (g_nNames >= NAME_CACHE) {
        SUnlock();
        return "";
    }
    priv = RQ(scene + 8);
    root = priv ? RQ(priv + SP_ROOT) : 0;
    rootP = root ? RQ(root + 0x20) : 0;
    inst = rootP ? RQ(rootP + 0x150) : 0;
    blk = inst ? RQ(inst + 0x10) : 0;
    g_names[g_nNames].scene = scene;
    g_names[g_nNames].name[0] = 0;
    if (blk && ShReadableAddr(blk, 12)) {
        memcpy(&len, (void *)(uintptr_t)blk, 4);
        if (len > 0 && len < sizeof(g_names[0].name) &&
            ShReadableAddr(blk + 12, len)) {
            memcpy(g_names[g_nNames].name, (void *)(uintptr_t)(blk + 12), len);
            g_names[g_nNames].name[len] = 0;
        }
    }
    {
        const char *stored = g_names[g_nNames].name;

        InterlockedIncrement(&g_nNames);  /* publish after the name is written */
        SUnlock();
        return stored;
    }
}

SH_API int ShGameSceneActive(const char *name) {
    int i, n;
    if (!name) return 0;
    n = ShGameSceneCount();
    for (i = 0; i < n; i++) {
        uint64_t sc = ShGameSceneAt(i);

        if (sc && strcmp(SceneName(sc), name) == 0) return 1;
    }
    return 0;
}

/* The scenes the game drew last frame, as engine handles.
 * Everything under one is reachable with the ShWidget
 * calls, so a plugin can read the game's own UI. */
/* Frames the render hook has counted. Internal: the state cache keys on it,
 * so one frame is one read of the state machine however many threads ask.
 * Not exported on purpose - a plugin has no business reasoning about frames,
 * and adding an export costs an API version. */
uint32_t ShSceneFrame(void) {
    return (uint32_t)InterlockedCompareExchange(&g_frame, 0, 0);
}

/* The name of a scene, as the pointer the name cache already holds - no
 * copy, no buffer. Internal (not in proxy.def): the UI-state table hashes
 * this, and asking "is HUD_Drone up" fourteen times used to walk the same
 * scene list fourteen times, each time comparing strings. */
const char *ShSceneNameRef(uint64_t scene) {
    return SceneName(scene);
}

SH_API int ShGameSceneCount(void) {
    int n = (int)InterlockedCompareExchange(&g_nActivePrev, 0, 0);

    /* The count and the array are published together; a reader that lands
     * mid-copy sees an odd sequence and is answered 0 by ShGameSceneAt, so
     * the count on its own is never a promise about the elements. */
    return n;
}

SH_API uint64_t ShGameSceneAt(int i) {
    uint64_t v;
    int tries, seq0, seq1, n;

    if (i < 0 || i >= MAX_ACTIVE) return 0;
    for (tries = 0; tries < 4; tries++) {
        seq0 = (int)InterlockedCompareExchange(&g_sceneSeq, 0, 0);
        if (seq0 & 1) { YieldProcessor(); continue; }   /* copy in flight */
        n = (int)InterlockedCompareExchange(&g_nActivePrev, 0, 0);
        if (i >= n) return 0;
        v = g_activePrev[i];
        seq1 = (int)InterlockedCompareExchange(&g_sceneSeq, 0, 0);
        if (seq0 == seq1) return v;
        YieldProcessor();
    }
    /* Four passes and the writer is still copying: a handle read out of a
     * frame nobody can reason about is worse than "no scene here". */
    return 0;
}

SH_API int ShGameSceneName(uint64_t scene, char *buf, int n) {
    const char *nm;
    if (!buf || n < 1) return 0;
    nm = SceneName(scene);
    strncpy(buf, nm, (size_t)n - 1);
    buf[n - 1] = 0;
    return buf[0] != 0;
}

/* The root widget of a scene, ours or the game's. */
SH_API uint64_t ShSceneRoot(uint64_t scene) {
    uint64_t priv = RQ(scene + 8);
    return priv ? RQ(priv + SP_ROOT) : 0;
}

/* Is this handle the root of a scene that is live right now?
 *
 * A scene root has no parent - that is what "root" means - so the framework's
 * write-liveness test (parent != 0) refuses it, and one plugin writes the
 * minimap's opacity exactly there. Answering "yes, and it is a root" is what
 * lets that write through while still refusing a widget whose tree detached.
 * Internal: not exported, so the API version does not move.
 *
 * Two tables, because there are two kinds of scene and the first version of
 * this only looked at one of them:
 *
 *  - g_s[] holds the scenes the framework itself created through
 *    ShSceneCreate (the CreateJob path sets live and rootH). That is where
 *    the overlay scenes live.
 *  - The scenes the GAME creates - every HUD_* layer among them - never enter
 *    g_s[]; they only exist in the active list the render hook fills. Asking
 *    the wrong one made ShSceneIsRoot answer 0 for HUD_Minimap's root every
 *    time, which is why the opacity write stayed refused on 2026-10-27 even
 *    after this check was added.
 *
 * The second loop resolves the root exactly the way a plugin does it
 * (ShSceneRoot is RQ(RQ(scene + 8) + SP_ROOT)), so the handle a plugin got
 * from ShSceneRoot is the handle compared here - two different chains would
 * be the same mistake in a new place. */
/* A scene's root, resolved without the readability query.
 *
 * ShSceneRoot (the exported one) guards both hops with ShReadableAddr, which
 * costs a kernel transition - measured 2026-10-27, about 8us here. This
 * check runs on every write to a root, about fifty times a second, and it
 * resolves fourteen scenes per ask: 28 transitions, 10us on top of a 3us
 * write. Two reads inside objects the caller has already validated do not
 * need the query, and __try answers 0 on the one frame a world is torn down. */
static uint64_t RootOf(uint64_t scene) {
    uint64_t priv = 0, root = 0;

    if (!scene) return 0;
    __try {
        memcpy(&priv, (void *)(uintptr_t)(scene + 8), 8);
        if (priv) memcpy(&root, (void *)(uintptr_t)(priv + SP_ROOT), 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        root = 0;
    }
    return root;
}

int ShSceneIsRoot(uint64_t handle) {
    int i, n;

    if (!handle) return 0;
    for (i = 0; i < MAX_SCENES; i++)
        if (g_s[i].live && g_s[i].rootH == handle) return 1;
    n = ShGameSceneCount();
    for (i = 0; i < n; i++) {
        uint64_t sc = ShGameSceneAt(i);

        if (sc && RootOf(sc) == handle) return 1;
    }
    return 0;
}

SH_API int ShGameScenes(char *buf, int n) {
    int i, used = 0, count = 0;
    if (!buf || n < 1) return 0;
    buf[0] = 0;
    EnsureHook();
    for (i = 0; i < ShGameSceneCount(); i++) {
        uint64_t sc = ShGameSceneAt(i);
        const char *nm;
        int l;

        if (!sc) continue;
        nm = SceneName(sc);
        l = (int)strlen(nm);
        if (!l) continue;
        if (used + l + 2 > n) break;
        if (used) buf[used++] = ',';
        memcpy(buf + used, nm, l);
        used += l;
        buf[used] = 0;
        count++;
    }
    return count;
}

/* Show or suppress the game-owned HUD_* scenes. Framework and menu
 * scenes remain visible either way (ported from the Wildlands
 * Immersion Suite). */
static volatile LONG g_gameHudVisible = 1;

SH_API int ShGameHudShow(int visible) {
    InterlockedExchange(&g_gameHudVisible, visible ? 1 : 0);
    return 1;
}

/* ---- one HUD scene at a time -------------------------------------------
 *
 * ShGameHudShow is all or nothing: every HUD_* layer goes at once. What a
 * settings screen wants is a switch per element - minimap here, crosshair
 * there - so the table below overrides the global answer for a named scene.
 *
 * A name that is not in the table follows ShGameHudShow, which is what
 * keeps this backwards compatible: with an empty table the render hook
 * behaves exactly as before.
 *
 * The lookup is a handful of strcmp against a table that is almost always
 * empty, once per HUD scene per frame; SceneName already caches the string,
 * so nothing here allocates or touches the engine. */

/* 192, not 64: a settings page that lists every HUD layer the game has (the
 * plugin's roster is 85 rows) must be able to hold the player's whole
 * selection at once. A hidden layer costs one entry and a shown one costs
 * none - the override is removed instead of being written as a 1 - so this is
 * the ceiling on "how many layers can be hidden together", and 64 was low
 * enough that a full sweep silently stopped applying after the table filled.
 * 192 x 52 bytes is 10 KB of .bss, which is not worth a second thought. */
#define HUD_OVR_MAX  192
#define HUD_OVR_NAME 48

typedef struct {
    char name[HUD_OVR_NAME];
    int  visible;
} HudOvr;

static HudOvr g_hudOvr[HUD_OVR_MAX];
static volatile LONG g_nHudOvr;

/* -1: no entry for this name, 0: hidden, 1: shown.
 * The render hook asks this for every scene it sees, so the common case
 * (nothing has been overridden yet) must return without taking the lock. */
static int HudSceneOverride(const char *name) {
    int i, n;
    if (g_nHudOvr <= 0) return -1;
    SLock();
    n = g_nHudOvr;
    for (i = 0; i < n; i++)
        if (strcmp(g_hudOvr[i].name, name) == 0) {
            int v = g_hudOvr[i].visible;
            SUnlock();
            return v;
        }
    SUnlock();
    return -1;
}

static int HudSceneVisible(const char *name) {
    int ovr = HudSceneOverride(name);
    return ovr < 0 ? g_gameHudVisible : ovr;
}

/* The render hook used to be installed only when a plugin built a scene of
 * its own (CreateJob), which meant that on a plain install nothing was
 * watching the game's scenes at all: ShGameScenes answered with an empty
 * list and ShGameHudShow was a no-op. Anything that wants to read, or
 * suppress, a game scene has to ask for it. Idempotent and cheap. */
static void EnsureHook(void) {
    if (g_stub) return;
    if (InterlockedCompareExchange(&g_hookTried, 1, 0) != 0) return;
    if (InstallHook())
        Log("scene: render hook installed (game scenes observable)");
    else
        Log("scene: render hook install failed - HUD control will not work");
}

SH_API int ShGameHudSceneShow(const char *name, int visible) {
    int i, n;
    if (!name || !name[0] || strlen(name) >= HUD_OVR_NAME) return 0;
    EnsureHook();
    SLock();
    n = g_nHudOvr;
    for (i = 0; i < n; i++)
        if (strcmp(g_hudOvr[i].name, name) == 0) {
            if (visible == SH_HUD_INHERIT) {
                memmove(&g_hudOvr[i], &g_hudOvr[i + 1],
                        (size_t)(n - i - 1) * sizeof(HudOvr));
                g_nHudOvr = n - 1;
            } else {
                g_hudOvr[i].visible = visible ? 1 : 0;
            }
            SUnlock();
            return 1;
        }
    if (visible != SH_HUD_INHERIT && n < HUD_OVR_MAX) {
        strcpy(g_hudOvr[n].name, name);
        g_hudOvr[n].visible = visible ? 1 : 0;
        g_nHudOvr = n + 1;
        SUnlock();
        return 1;
    }
    SUnlock();
    return 0;
}

SH_API int ShGameHudSceneVisible(const char *name) {
    if (!name || !name[0]) return 0;
    return HudSceneVisible(name);
}

/* Every HUD_* scene seen since the world came up, in the order it first drew.
 *
 * ShGameScenes only answers for the last frame, and a HUD layer is not always
 * drawn (the drone layer exists while you walk), so a settings page built from
 * that list would be missing rows. This one accumulates. The render thread is
 * the only writer; the count is published after the name is copied, so a
 * reader that sees n never reads a half written entry. */
#define HUD_SEEN_MAX 128
static char g_hudSeen[HUD_SEEN_MAX][HUD_OVR_NAME];
static volatile LONG g_nHudSeen;

/* Not every HUD layer carries the HUD_ prefix - the crosshair scene is just
 * "CrossHair" - so a prefix test alone silently loses rows. MENU_* belongs to
 * the front end and is deliberately left alone. */
static int IsHudLayer(const char *name) {
    if (!name || !name[0]) return 0;
    if (strncmp(name, "HUD_", 4) == 0) return 1;
    if (strcmp(name, "CrossHair") == 0) return 1;
    return 0;
}

static void NoteHudScene(const char *name) {
    int i, n;
    if (!IsHudLayer(name)) return;

    /* This runs on the render thread but the game renders from more than one
     * of them. Publishing the count before the name had been copied let a
     * second thread see a half written (empty) slot, decide the layer was new
     * and append it again - the table filled up with duplicates within a few
     * minutes and later layers (HUD_Minimap) never got in. */
    SLock();
    n = g_nHudSeen;
    for (i = 0; i < n; i++)
        if (strcmp(g_hudSeen[i], name) == 0) { SUnlock(); return; }
    if (n < HUD_SEEN_MAX) {
        strncpy(g_hudSeen[n], name, HUD_OVR_NAME - 1);
        g_hudSeen[n][HUD_OVR_NAME - 1] = 0;
        g_nHudSeen = n + 1;
    }
    SUnlock();
}

/* The HUD_* scenes seen this session, comma separated; returns the count.
 * This is the list a settings page is built from. */
SH_API int ShGameHudScenes(char *buf, int n) {
    int i, used = 0, count = 0;
    if (!buf || n < 1) return 0;
    buf[0] = 0;
    EnsureHook();
    for (i = 0; i < g_nHudSeen; i++) {
        const char *nm = g_hudSeen[i];
        int l, j, dup = 0;
        if (!nm[0]) continue;
        /* The table is meant to be unique already; the check keeps a list
         * built by an older build from showing the same row many times. */
        for (j = 0; j < i; j++)
            if (g_hudSeen[j][0] && strcmp(g_hudSeen[j], nm) == 0) { dup = 1; break; }
        if (dup) continue;
        l = (int)strlen(nm);
        if (!l || used + l + 2 > n) break;
        if (used) buf[used++] = ',';
        memcpy(buf + used, nm, (size_t)l);
        used += l;
        buf[used] = 0;
        count++;
    }
    return count;
}

static void PassEnded(uint64_t last) {
    if (last && g_nEndsNext < MAX_PASS && !InList(g_endsNext, g_nEndsNext, last))
        g_endsNext[g_nEndsNext++] = last;
    g_nSeen = 0;
}

static int32_t *__attribute__((ms_abi)) RenderHook(uint64_t scene,
        int32_t *res, uint64_t renderer) {
    int32_t *r;
    int fresh, passStart = 0;

    if (IsOurs(scene)) {
        RecordCall(scene, renderer, 1);
        return ((Scene3)F_SCENE_RENDER)(scene, res, renderer);
    }
    RecordCall(scene, renderer, 0);
    fresh = NewFrame();
    if (fresh) {
        PassEnded(g_prevCall);
        memcpy(g_ends, g_endsNext, sizeof(g_ends));
        g_nEnds = g_nEndsNext;
        g_nEndsNext = 0;
        PublishActive();
        g_nActiveCur = 0;
        g_doneThisFrame = 0;
        InterlockedIncrement(&g_frame);
        TickAll();
        passStart = 1;
    } else if (InList(g_seen, g_nSeen, scene)) {
        PassEnded(g_prevCall);
        passStart = 1;
    }
    if (g_nSeen < MAX_SEEN) g_seen[g_nSeen++] = scene;
    NoteActive(scene);
    /* under the game: before each pass's first scene */
    if (passStart && renderer) RenderOurs(renderer, 1);
    /* Preserve scene tracking, but suppress the game's HUD layers on
     * request; framework and menu scenes still draw (ShGameHudShow, and
     * per-scene overrides through ShGameHudSceneShow). */
    {
        const char *nm = SceneName(scene);
        int ovr = nm ? HudSceneOverride(nm) : -1;

        /* An explicit override wins for any name, prefix or not; without one
         * only the game's HUD layers are touched (ShGameHudShow). */
        if (nm && (ovr >= 0 || IsHudLayer(nm))) {
            NoteHudScene(nm);
            if (ovr == 0 || (ovr < 0 && !g_gameHudVisible)) {
                if (res) *res = 0;
                r = res;
            } else
                r = ((Scene3)F_SCENE_RENDER)(scene, res, renderer);
        } else
            r = ((Scene3)F_SCENE_RENDER)(scene, res, renderer);
    }
    g_prevCall = scene;
    /* over the game: after each pass's last scene */
    if (renderer) {
        int hit = g_nEnds ? InList(g_ends, g_nEnds, scene) : !g_doneThisFrame;
        if (hit) {
            g_doneThisFrame = 1;
            RenderOurs(renderer, 0);
        }
    }
    return r;
}

/* rel32 of the 5 byte jmp thunk, pointed at a near stub */
static int InstallHook(void) {
    uint8_t *t = (uint8_t *)(uintptr_t)RENDER_THUNK;
    int64_t cur, rel;
    DWORD old;
    int o = 0;

    if (g_stub) return 1;
    if (!ShReadableAddr(RENDER_THUNK, 5) || t[0] != 0xE9) {
        Log("scene: render thunk %llx is not a jmp - pins are stale",
            (unsigned long long)RENDER_THUNK);
        return 0;
    }
    cur = (int64_t)RENDER_THUNK + 5 + *(int32_t *)(t + 1);
    if ((uint64_t)cur != F_SCENE_RENDER) {
        Log("scene: render thunk jumps to %llx, expected %llx - pins are stale",
            (unsigned long long)cur, (unsigned long long)F_SCENE_RENDER);
        return 0;
    }

    g_stub = (uint8_t *)ShAllocNear(RENDER_THUNK);
    if (!g_stub) return 0;
    memset(g_stub, 0xCC, 0x1000);
    g_stub[o++] = 0x48; g_stub[o++] = 0xB8;
    *(uint64_t *)(g_stub + o) = (uint64_t)(uintptr_t)RenderHook; o += 8;
    g_stub[o++] = 0xFF; g_stub[o++] = 0xE0;

    rel = (int64_t)(uintptr_t)g_stub - ((int64_t)RENDER_THUNK + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) return 0;
    if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(g_thunkOrig, t, 5);
    *(int32_t *)(t + 1) = (int32_t)rel;
    VirtualProtect(t, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 5);
    /* Registered in the shared layer's book: this address is spoken
     * for by a hand-written patch, so a plugin asking for the same
     * site is told who owns it instead of writing a second jump over
     * this one. The patch itself is the lines above - the claim does
     * not touch a byte of it. */
    ShHookClaim("scene:render", (void *)t, "render thunk");
    return 1;
}

/* ---- jobs on the game thread ---- */

static void DestroyEngineScene(uint64_t scene) {
    ((Fn3)F_SCENE_DTOR)(scene, 0, 0);
    ((Fn1)F_FREE)(scene);
    Log("scene: destroyed %llx", (unsigned long long)scene);
}

static uint64_t __attribute__((ms_abi)) CreateJob(uint64_t sid, uint64_t b,
                                                  uint64_t c, uint64_t d) {
    SceneSlot *s = &g_s[sid - 1];
    uint64_t scene, priv, root, rootP;
    int32_t res = 0;
    uint8_t idx;
    (void)b; (void)c; (void)d;

    /* No teardown of a previous scene is owed here any more: a world reload
     * hands its handles back to the engine and clears the slot (see
     * ShSceneInvalidate), which is where the deferred destroy used to be queued
     * and where it took the game down on 2026-09-27. A scene this slot still
     * owns is one it made in this world, and there is none before the first
     * create. */

    scene = EAlloc(0x10);
    if (!scene) return 0;
    ((Fn1)F_SCENE_CTOR)(scene);
    s->vt = RQ(scene);
    priv = RQ(scene + 8);
    if (!priv) goto fail;
    ((Scene3)F_SCENE_SETCTX)(scene, &res, (uint64_t)(uintptr_t)&g_localizer);
    /* the game's name resolver passes texture names through */
    g_resolver.vt = VT_GAME_RESOLVER;
    ((Scene3)F_SCENE_SETRES)(scene, &res, (uint64_t)(uintptr_t)&g_resolver);
    ((Scene2)F_SCENE_RESIZE)(scene, &res);
    root = RQ(priv + SP_ROOT);
    rootP = RQ(root + 0x20);
    if (!root || !rootP) goto fail;
    if (RQ(rootP + 0x130) != scene)
        ((Fn3)F_ATTACH)(rootP, scene, 0);
    idx = *(uint8_t *)(uintptr_t)(priv + SP_IDX);
    *(uint8_t *)(uintptr_t)(priv + SP_STATE + idx) = STATE_LIVE;

    if (!InstallHook()) {
        Log("scene %llu: hook install failed", (unsigned long long)sid);
        goto fail;
    }
    s->handle = scene; s->priv = priv;
    s->rootH = root; s->rootP = rootP;
    s->flippedFrame = -1;
    s->live = 1;
    BumpOrder();
    Log("scene %llu: created %llx priv %llx root %llx/%llx order %d",
        (unsigned long long)sid, (unsigned long long)scene,
        (unsigned long long)priv, (unsigned long long)root,
        (unsigned long long)rootP, s->order);
    return 1;

fail:
    /* A half-built scene must not stay in the engine pool: undo the
     * ctor exactly the way a teardown would. */
    Log("scene %llu: create failed, freeing %llx",
        (unsigned long long)sid, (unsigned long long)scene);
    DestroyEngineScene(scene);
    s->vt = 0;
    return 0;
}

/* A teardown is only issued when the object still looks like the one this slot
 * made: the vtable is cached at creation and compared here. That is a heuristic
 * rather than a proof - a block the engine reused for a scene of the same class
 * would pass it - which is exactly why a world reload queues nothing at all (see
 * ShSceneInvalidate). This guard is what is left for the teardowns a caller asks
 * for by name, where the slot still believes it owns a live scene. */
static void DestroyIfOurs(uint64_t scene, uint64_t vt) {
    if (!scene) return;
    if (vt && RQ(scene) != vt) {
        Log("scene %llx: not this slot's any more (class %llx), left alone",
            (unsigned long long)scene, (unsigned long long)RQ(scene));
        return;
    }
    DestroyEngineScene(scene);
}

static uint64_t __attribute__((ms_abi)) DestroyJob(uint64_t sid, uint64_t b,
                                                   uint64_t c, uint64_t d) {
    SceneSlot *s = &g_s[sid - 1];
    (void)b; (void)c; (void)d;
    if (s->live) {
        s->live = 0;
        DestroyIfOurs(s->handle, s->vt);
    }
    memset(s, 0, sizeof(*s));
    BumpOrder();
    return 1;
}

static int RunJob(uint64_t fn, uint64_t sid) {
    uint64_t ret = 0;
    int waited = 0;

    while (!ShQueueCall(fn, sid, 0, 0, 0)) {
        if (++waited > JOB_WAIT_MS) return 0;
        Sleep(1);
    }
    while (!ShQueueResult(&ret)) {
        if (++waited > JOB_WAIT_MS) return 0;
        Sleep(1);
    }
    return ret != 0;
}

/* physics step entry: only until the first render */
void ShSceneTick(void) {
    int i;

    if (g_frame != 0) return;
    for (i = 0; i < MAX_SCENES; i++) {
        int32_t res = 0;
        if (!g_s[i].live || !g_s[i].visible) continue;
        ((Scene3)F_SCENE_TICK)(g_s[i].handle, &res, TICK_MS);
    }
}

/* ---- table API for scripthook_ui.c ---- */

static int Playing(void) {
    return ShIsInGame() && ShGetGameStateHash() == HASH_PLAYING;
}

static void Init(void) {
    LARGE_INTEGER f;
    if (!g_qpcFreq) {
        QueryPerformanceFrequency(&f);
        g_qpcFreq = f.QuadPart ? f.QuadPart : 1;   /* never leave it 0: TickAll divides */
    }
}

int ShSceneAlloc(int order) {
    int i;
    Init();
    SLock();
    for (i = 0; i < MAX_SCENES; i++) {
        if (g_s[i].used) continue;
        memset(&g_s[i], 0, sizeof(g_s[i]));
        g_s[i].used = 1;
        g_s[i].order = order;
        g_s[i].visible = 1;
        SUnlock();
        return i + 1;
    }
    SUnlock();
    ShSetError(SH_ERR_NO_CANDIDATE);
    return 0;
}

static SceneSlot *Slot(int sid) {
    if (sid < 1 || sid > MAX_SCENES || !g_s[sid - 1].used) {
        ShSetError(SH_ERR_BAD_ARG);
        return NULL;
    }
    return &g_s[sid - 1];
}

int ShSceneEnsure(int sid, uint64_t *scene, uint64_t *rootH,
                  uint64_t *rootP) {
    SceneSlot *s = Slot(sid);
    Init();
    if (!s) return 0;
    if (!s->live) {
        if (!Playing()) { ShSetError(SH_ERR_UI_NOT_READY); return 0; }
        if (!RunJob((uint64_t)(uintptr_t)CreateJob, (uint64_t)sid)) {
            ShSetError(SH_ERR_HOOK_FAILED);
            return 0;
        }
    }
    *scene = s->handle; *rootH = s->rootH; *rootP = s->rootP;
    return 1;
}

int ShSceneLive(int sid) {
    return sid >= 1 && sid <= MAX_SCENES && g_s[sid - 1].live;
}

int ShSceneRelease(int sid) {
    if (!Slot(sid)) return 0;
    if (!RunJob((uint64_t)(uintptr_t)DestroyJob, (uint64_t)sid)) {
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    return 1;
}

int ShSceneSetOrder(int sid, int order) {
    SceneSlot *s = Slot(sid);
    if (!s) return 0;
    s->order = order;
    BumpOrder();
    return 1;
}

int ShSceneShow(int sid, int visible) {
    SceneSlot *s = Slot(sid);
    if (!s) return 0;
    s->visible = visible ? 1 : 0;
    BumpOrder();
    return 1;
}

uint64_t ShScenePriv(int sid) {
    return ShSceneLive(sid) ? g_s[sid - 1].priv : 0;
}

/* A world reload. The engine tears its world down and its scenes with it, so the
 * handles in these slots are dangling from this moment on, and nothing here is
 * ours to destroy any more: the engine owns its scenes again and frees them
 * itself.
 *
 * This used to queue each handle for a teardown on the next ensure, with a vtable
 * comparison as the guard against a handle the engine had already freed. On
 * 2026-09-27 a handle passed that comparison - a recycled block of the same class
 * looks exactly like ours - and the engine called a null pointer inside its own
 * scene teardown, which ended the session. So the slot forgets the handle
 * instead. At worst one scene per reload stays with the engine, which is the leak
 * the leak probe exists to show; a session that ends is worse than a scene that
 * outlives its world. */
void ShSceneInvalidate(void) {
    int i;
    SLock();
    for (i = 0; i < MAX_SCENES; i++) {
        if (!g_s[i].live) continue;
        Log("scene %d: invalidated %llx (the engine owns it from here)",
            i + 1, (unsigned long long)g_s[i].handle);
        g_s[i].handle = 0;
        g_s[i].priv = 0;
        g_s[i].rootH = 0;
        g_s[i].rootP = 0;
        g_s[i].vt = 0;
        g_s[i].flippedFrame = -1;
        g_s[i].live = 0;
        BumpOrder();
    }
    SUnlock();
}

/* the engine's pool free */
void ShSceneFree(uint64_t p) {
    if (p) ((Fn1)F_FREE)(p);
}

SH_API uint64_t ShSceneHandle(void) {
    return ShSceneLive(1) ? g_s[0].handle : 0;
}

/* the scene's own critical section at priv+0x370 */
void ShSceneLock(uint64_t priv) {
    if (priv) ((Fn1)F_LOCK)(priv + 0x370);
}

void ShSceneUnlock(uint64_t priv) {
    if (priv) ((Fn1)F_UNLOCK)(priv + 0x370);
}

/* Leak probe: counts for the 5s overlay heartbeat (see ovl).  A live slot holds
 * an engine scene. There is no third kind of slot any more - a world reload hands
 * its scenes back to the engine instead of queueing them for a teardown (see
 * ShSceneInvalidate) - so the third count is reported as 0 and is kept only
 * because the overlay still prints it. Watch for live never returning to zero
 * after leaving the world. */
int ShSceneLeakProbe(int *used, int *live, int *dead) {
    int i, u = 0, l = 0;
    SLock();
    for (i = 0; i < MAX_SCENES; i++) {
        if (g_s[i].used) u++;
        if (g_s[i].live) l++;
    }
    SUnlock();
    if (used) *used = u;
    if (live) *live = l;
    if (dead) *dead = 0;
    return 0;
}
