/* phoenix::Scenes of our own, one per plugin layer. */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"
#include "log.h"

#define F_ALLOC_CTX     SH_IMG(0xE064390)
#define F_ALLOC         SH_IMG(0x60ACBF0)
#define G_POOL          SH_IMG(0x4D78D00)
#define F_SCENE_CTOR    SH_IMG(0x32EEC50)
#define F_SCENE_DTOR    SH_IMG(0x32EECD0)
#define F_SCENE_TICK    SH_IMG(0x173FA610)
#define F_SCENE_FLIP    SH_IMG(0x173FA280)
#define F_SCENE_RENDER  SH_IMG(0x173F8C60)
#define F_SCENE_RESIZE  SH_IMG(0x173F8400)
#define F_SCENE_SETCTX  SH_IMG(0x173F9930)
#define F_SCENE_SETRES  SH_IMG(0x173F9160)
#define F_ATTACH        SH_IMG(0x32F4D00)
#define F_FREE          SH_IMG(0xF93CA90)
#define F_LOCK          SH_IMG(0x36206D0)
#define F_UNLOCK        SH_IMG(0x3287730)
#define RENDER_THUNK    SH_IMG(0x32EEFB0)
#define G_UIMGR         SH_IMG(0x4D58560)
#define VT_GAME_RESOLVER SH_IMG(0x3A05AA0)

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
    uint64_t dead;         /* last session's scene */
    LONG     flippedFrame; /* flip once per frame */
} SceneSlot;

static SceneSlot g_s[MAX_SCENES];
static CRITICAL_SECTION g_slock;
static int g_slockInit;
static uint8_t *g_stub;
static uint8_t g_thunkOrig[5];
static volatile uint32_t g_lastStamp;
static volatile int64_t g_lastRender;
static int64_t g_qpcFreq;
static volatile LONG g_frame;

static void SLock(void) {
    if (!g_slockInit) {
        InitializeCriticalSection(&g_slock);
        LogInit("scripthook_scene.log");
        g_slockInit = 1;
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

/* visible scenes of one sign, lowest order first */
static void RenderOurs(uint64_t renderer, int negatives) {
    int done[MAX_SCENES] = {0};
    int i, pass;

    for (pass = 0; pass < MAX_SCENES; pass++) {
        int best = -1;
        for (i = 0; i < MAX_SCENES; i++) {
            if (done[i] || !g_s[i].live || !g_s[i].visible) continue;
            if ((g_s[i].order < 0) != (negatives != 0)) continue;
            if (best < 0 || g_s[i].order < g_s[best].order) best = i;
        }
        if (best < 0) return;
        done[best] = 1;
        {
            int32_t r2 = 0, r3 = 0;
            if (g_s[best].flippedFrame != g_frame) {
                g_s[best].flippedFrame = g_frame;
                ((Scene2)F_SCENE_FLIP)(g_s[best].handle, &r2);
            }
            RecordCall(g_s[best].handle, renderer, 1);
            ((Scene3)F_SCENE_RENDER)(g_s[best].handle, &r3, renderer);
        }
    }
}

/* one tick per frame, render thread, before the flip */
static int64_t g_lastTick;

static void TickAll(void) {
    LARGE_INTEGER now;
    int64_t dt;
    int i;

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
        g_doneThisFrame = 0;
        InterlockedIncrement(&g_frame);
        TickAll();
        passStart = 1;
    } else if (InList(g_seen, g_nSeen, scene)) {
        PassEnded(g_prevCall);
        passStart = 1;
    }
    if (g_nSeen < MAX_SEEN) g_seen[g_nSeen++] = scene;
    /* under the game: before each pass's first scene */
    if (passStart && renderer) RenderOurs(renderer, 1);
    r = ((Scene3)F_SCENE_RENDER)(scene, res, renderer);
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
    if (!ShReadableAddr(RENDER_THUNK, 5) || t[0] != 0xE9) return 0;
    cur = (int64_t)RENDER_THUNK + 5 + *(int32_t *)(t + 1);
    if ((uint64_t)cur != F_SCENE_RENDER) return 0;

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

    if (s->dead) { DestroyEngineScene(s->dead); s->dead = 0; }

    scene = EAlloc(0x10);
    if (!scene) return 0;
    ((Fn1)F_SCENE_CTOR)(scene);
    priv = RQ(scene + 8);
    if (!priv) return 0;
    ((Scene3)F_SCENE_SETCTX)(scene, &res, (uint64_t)(uintptr_t)&g_localizer);
    /* the game's name resolver passes texture names through */
    g_resolver.vt = VT_GAME_RESOLVER;
    ((Scene3)F_SCENE_SETRES)(scene, &res, (uint64_t)(uintptr_t)&g_resolver);
    ((Scene2)F_SCENE_RESIZE)(scene, &res);
    root = RQ(priv + SP_ROOT);
    rootP = RQ(root + 0x20);
    if (!root || !rootP) return 0;
    if (RQ(rootP + 0x130) != scene)
        ((Fn3)F_ATTACH)(rootP, scene, 0);
    idx = *(uint8_t *)(uintptr_t)(priv + SP_IDX);
    *(uint8_t *)(uintptr_t)(priv + SP_STATE + idx) = STATE_LIVE;

    if (!InstallHook()) { Log("scene: hook install failed"); return 0; }
    s->handle = scene; s->priv = priv;
    s->rootH = root; s->rootP = rootP;
    s->flippedFrame = -1;
    s->live = 1;
    Log("scene %llu: created %llx priv %llx root %llx/%llx order %d",
        (unsigned long long)sid, (unsigned long long)scene,
        (unsigned long long)priv, (unsigned long long)root,
        (unsigned long long)rootP, s->order);
    return 1;
}

static uint64_t __attribute__((ms_abi)) DestroyJob(uint64_t sid, uint64_t b,
                                                   uint64_t c, uint64_t d) {
    SceneSlot *s = &g_s[sid - 1];
    (void)b; (void)c; (void)d;
    if (s->dead) { DestroyEngineScene(s->dead); s->dead = 0; }
    if (s->live) {
        s->live = 0;
        DestroyEngineScene(s->handle);
    }
    memset(s, 0, sizeof(*s));
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
        g_qpcFreq = f.QuadPart;
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
    return 1;
}

int ShSceneShow(int sid, int visible) {
    SceneSlot *s = Slot(sid);
    if (!s) return 0;
    s->visible = visible ? 1 : 0;
    return 1;
}

uint64_t ShScenePriv(int sid) {
    return ShSceneLive(sid) ? g_s[sid - 1].priv : 0;
}

uint64_t ShSceneDeadPriv(int sid) {
    if (sid < 1 || sid > MAX_SCENES || !g_s[sid - 1].dead) return 0;
    return RQ(g_s[sid - 1].dead + 8);
}

/* world reload: scenes stop, destroyed on next ensure */
void ShSceneInvalidate(void) {
    int i;
    SLock();
    for (i = 0; i < MAX_SCENES; i++) {
        if (!g_s[i].live) continue;
        Log("scene %d: invalidated %llx", i + 1,
            (unsigned long long)g_s[i].handle);
        if (g_s[i].dead) DestroyEngineScene(g_s[i].dead);
        g_s[i].dead = g_s[i].handle;
        g_s[i].live = 0;
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
