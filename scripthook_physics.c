/* Ground queries through the engine's collision world.
 * Build pinned: GRW Definitive, base 0x140000000.
 */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"
#include <math.h>

/* Verified entry points, see GROUND_QUERY.md */
#define RAY_HOOK_SITE   SH_IMG(0x169B7630)
#define CAST_RAY_FN     SH_IMG(0xFBE88D0)

/* The engine's own 0x4000 mask rejects every hit in this
 * world, so query permissively and filter by distance.
 */
#define LAYER_MASK      0xFFFFFFFFFFFFFFFFULL

#define PROBE_UP        30.0f
#define PROBE_DOWN      80.0f

/* How long to wait for the hook to see a live cast. */
#define CTX_WAIT_MS     3000

/* Hintless sweep, proven working at 1400 down 900. */
#define SWEEP_TOP       1400.0f
#define SWEEP_ABOVE     300.0f
#define SWEEP_SPAN      2500.0f
#define REC_STRIDE      0x80
#define REC_COUNT       16

typedef uint8_t (__attribute__((ms_abi)) *CastRay_t)(void *, void *,
                                                     void *, char, char,
                                                     uint8_t, char);

static uint8_t  g_hitArr[0x880] __attribute__((aligned(16)));
static uint8_t  g_recs[REC_COUNT * REC_STRIDE] __attribute__((aligned(16)));
static uint8_t  g_desc[0x80] __attribute__((aligned(16)));
static float    g_org[4] __attribute__((aligned(16)));
static float    g_dir[4] __attribute__((aligned(16)));

static volatile LONG g_req = 0;
static volatile LONG g_done = 0;
static volatile int  g_busy = 0;
static volatile float g_hitZ = 0.0f;
static volatile int   g_hitOk = 0;

/* Written by the hook on the game thread and polled by
 * callers, so the compiler must reload them each spin.
 */
static volatile uint64_t g_ctx = 0;
static volatile uint64_t g_B = 0;
extern void ShSetError(int err);
extern int ShRequireInGame(void);
extern void *ShAllocNear(uint64_t target);

static int ShFailPhys(int e) {
    ShSetError(e);
    return 0;
}
static uint8_t *g_stub = NULL;
static uint64_t g_site = 0;
static uint8_t  g_orig[16];
static int      g_origLen = 0;

extern int ShReadableAddr(uint64_t addr, size_t len);
extern int ShReadMem(uint64_t addr, void *out, size_t len);
extern uint64_t ShReadQ(uint64_t addr);

static volatile uint64_t g_A = 0;

/* The world is rebuilt on level changes, so a cached
 * pointer goes stale. B and A must still agree.
 */
static int WorldValid(void) {
    if (!g_A || !g_B) return 0;
    if (ShReadQ(g_A) != g_B) return 0;
    return ShReadQ(g_B + 0xD08) == g_A;
}

/* Find A with [A+0x10]==ctx and [[A]+0xD08]==A, then B. */
static void ResolveWorld(uint64_t ctx) {
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = (uint8_t *)0x1000000;

    if (g_B || !ctx) return;
    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;
        if ((uint64_t)(uintptr_t)mbi.BaseAddress >= 0x800000000000ULL)
            break;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & PAGE_READWRITE) &&
            !(mbi.Protect & PAGE_GUARD))
        {
            uint8_t *b = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize, o, k, got;

            /* Chunked kernel reads: a page freed mid scan
             * skips instead of faulting. Chunks overlap so
             * no candidate spans a seam. */
            static uint8_t buf[0x10000];

            for (o = 0; o + 0x18 <= sz; o += sizeof(buf) - 0x18) {
                got = sz - o;
                if (got > sizeof(buf)) got = sizeof(buf);
                if (!ShReadMem((uint64_t)(uintptr_t)(b + o), buf, got))
                    continue;
                for (k = 0; k + 0x18 <= got; k += 8) {
                    uint64_t v, A, Bv;
                    memcpy(&v, buf + k, 8);
                    if (v != ctx) continue;
                    if (o + k < 0x10) continue;
                    A = (uint64_t)(uintptr_t)(b + o + k - 0x10);
                    Bv = ShReadQ(A);
                    if (!Bv) continue;
                    if (ShReadQ(Bv + 0xD08) != A) continue;
                    g_A = A;
                    g_B = Bv;
                    return;
                }
            }
        }
        scan = next;
    }
}

static void BuildDescriptor(void) {
    uint16_t all = 0xFFFF;
    uint32_t two = 2, mask = 0, i;
    const uint32_t BIG = 0x7F7FFFEEu;
    float inv[4];

    memset(g_desc, 0, sizeof(g_desc));
    memcpy(g_desc + 0x10, &all, 2);
    memcpy(g_desc + 0x20, &two, 4);
    memcpy(g_desc + 0x30, g_org, 16);
    memcpy(g_desc + 0x40, g_dir, 16);
    for (i = 0; i < 4; i++) {
        float d = g_dir[i];
        if (d == 0.0f) memcpy(&inv[i], &BIG, 4);
        else inv[i] = 1.0f / d;
        if (d >= 0.0f) mask |= (1u << i);
    }
    memcpy(g_desc + 0x50, inv, 16);
    mask = (mask & 7u) | 0x3F000000u;
    memcpy(g_desc + 0x5C, &mask, 4);
}

/* Game thread dispatcher. Lock taking engine calls
 * deadlock from any other thread, so they queue here.
 */
typedef uint64_t (__attribute__((ms_abi)) *ShQFn_t)(uint64_t,
                                                    uint64_t,
                                                    uint64_t,
                                                    uint64_t);
extern void ShSpawnPump(void);
extern void ShNpcPump(void);
extern void ShSceneTick(void);

static volatile uint64_t g_qFn = 0;
static uint64_t g_qArg[6];
static volatile uint64_t g_qRet = 0;
static volatile int g_qPending = 0;
static volatile int g_qDone = 0;
static volatile int g_qFloat = 0;
static volatile int g_qSix = 0;
static volatile float g_qF[3];

typedef uint64_t (__attribute__((ms_abi)) *ShQFnF_t)(uint64_t,
                                                     uint64_t,
                                                     float, float,
                                                     float);
typedef uint64_t (__attribute__((ms_abi)) *ShQFn6_t)(uint64_t,
                                                     uint64_t,
                                                     uint64_t,
                                                     uint64_t,
                                                     uint64_t,
                                                     uint64_t);

/* Six integer arguments; the fifth and sixth go on the
 * stack, which the four argument path cannot reach. */
SH_API int ShQueueCall6(uint64_t fn, const uint64_t *args) {
    int i;
    if (!fn || !args || g_qPending) return 0;
    for (i = 0; i < 6; i++) g_qArg[i] = args[i];
    g_qFloat = 0;
    g_qSix = 1;
    g_qRet = 0;
    g_qDone = 0;
    g_qFn = fn;
    g_qPending = 1;
    return 1;
}

SH_API int ShQueueCall(uint64_t fn, uint64_t a0, uint64_t a1,
                       uint64_t a2, uint64_t a3) {
    if (!fn || g_qPending) return 0;
    g_qArg[0] = a0; g_qArg[1] = a1;
    g_qArg[2] = a2; g_qArg[3] = a3;
    g_qFloat = 0;
    g_qSix = 0;
    g_qRet = 0;
    g_qDone = 0;
    g_qFn = fn;
    g_qPending = 1;
    return 1;
}

/* Args three to five are floats, which the ABI puts in
 * xmm2, xmm3 and the stack. The integer path cannot reach
 * those, so the call goes through its own signature. */
SH_API int ShQueueCallF(uint64_t fn, uint64_t a0, uint64_t a1,
                        float f2, float f3, float f4) {
    if (!fn || g_qPending) return 0;
    g_qArg[0] = a0; g_qArg[1] = a1;
    g_qF[0] = f2; g_qF[1] = f3; g_qF[2] = f4;
    g_qFloat = 1;
    g_qSix = 0;
    g_qRet = 0;
    g_qDone = 0;
    g_qFn = fn;
    g_qPending = 1;
    return 1;
}

SH_API int ShQueueResult(uint64_t *outRet) {
    if (!g_qDone) return 0;
    if (outRet) *outRet = g_qRet;
    return 1;
}

/* Every ray the engine casts passes through here, bullet
 * traces included, so record them for plugins to read.
 */
#define RAY_LOG 256
static ShRay g_rayLog[RAY_LOG];
static volatile uint32_t g_rayHead = 0;
static volatile int g_rayLogOn = 0;

/* Record time filter. Query time filtering is useless here,
 * the ring wraps in milliseconds without this.
 */
static ShVec3 g_filtFrom;
static float g_filtRadius = 0.0f;
static float g_filtMinLen = 0.0f;
static volatile int g_filtPlayer = 0;

SH_API void ShRayFilter(const ShVec3 *from, float radius,
                        float minLength) {
    g_filtPlayer = 0;
    if (from) g_filtFrom = *from;
    g_filtRadius = radius;
    g_filtMinLen = minLength;
}

SH_API void ShRayFilterPlayer(float radius, float minLength) {
    g_filtPlayer = radius > 0.0f;
    g_filtRadius = radius;
    g_filtMinLen = minLength;
}

SH_API void ShRayLog(int mode) {
    if (mode && !g_rayLogOn) g_rayHead = 0;
    g_rayLogOn = mode;
}

SH_API uint32_t ShRayCount(void) { return g_rayHead; }

SH_API int ShGetRays(ShRay *out, int max) {
    return ShQueryRays(NULL, out, max);
}

static float Len3(const ShVec3 *v) {
    return (float)sqrt((double)(v->x * v->x + v->y * v->y +
                                v->z * v->z));
}

static float Dist3(const ShVec3 *a, const ShVec3 *b) {
    ShVec3 d;
    d.x = a->x - b->x;
    d.y = a->y - b->y;
    d.z = a->z - b->z;
    return Len3(&d);
}

/* Closest approach of the segment origin..origin+dir to p,
 * so a trace can be found by what it passes through.
 */
static float DistToRay(const ShRay *r, const ShVec3 *p) {
    float len2 = r->dir.x * r->dir.x + r->dir.y * r->dir.y +
                 r->dir.z * r->dir.z;
    float t;
    ShVec3 c;

    if (len2 <= 0.0f) return Dist3(&r->origin, p);
    t = ((p->x - r->origin.x) * r->dir.x +
         (p->y - r->origin.y) * r->dir.y +
         (p->z - r->origin.z) * r->dir.z) / len2;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    c.x = r->origin.x + r->dir.x * t;
    c.y = r->origin.y + r->dir.y * t;
    c.z = r->origin.z + r->dir.z * t;
    return Dist3(&c, p);
}

static int Matches(const ShRay *r, const ShRayQuery *q) {
    float len;

    if (!q) return 1;
    if (q->hitsOnly && !r->hits) return 0;
    len = Len3(&r->dir);
    if (q->minLength > 0.0f && len < q->minLength) return 0;
    if (q->maxLength > 0.0f && len > q->maxLength) return 0;
    if (q->fromRadius > 0.0f &&
        Dist3(&r->origin, &q->from) > q->fromRadius) return 0;
    if (q->throughRadius > 0.0f &&
        DistToRay(r, &q->through) > q->throughRadius) return 0;
    return 1;
}

SH_API int ShQueryRays(const ShRayQuery *q, ShRay *out, int max) {
    uint32_t head = g_rayHead;
    int have, i, n = 0;

    if (!out || max <= 0) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    have = (int)(head < (uint32_t)RAY_LOG ? head : (uint32_t)RAY_LOG);
    for (i = 0; i < have && n < max; i++) {
        uint32_t idx = (head - 1 - (uint32_t)i) % RAY_LOG;
        if (!Matches(&g_rayLog[idx], q)) continue;
        out[n++] = g_rayLog[idx];
    }
    return n;
}

static void RecordRay(uint64_t desc, uint64_t coll) {
    ShRay *r;
    float dir[3];
    uint16_t hits = 0;

    if (!g_rayLogOn) return;
    if (!ShReadableAddr(desc, 0x60)) return;
    memcpy(dir, (const void *)(uintptr_t)(desc + 0x40), 12);

    /* Ground probes are straight down and drown out
     * everything else, so DIRECTED mode skips them.
     */
    if (g_rayLogOn == SH_RAY_DIRECTED &&
        dir[0] == 0.0f && dir[1] == 0.0f) return;

    if (g_filtMinLen > 0.0f) {
        float l2 = dir[0] * dir[0] + dir[1] * dir[1] +
                   dir[2] * dir[2];
        if (l2 < g_filtMinLen * g_filtMinLen) return;
    }
    if (g_filtRadius > 0.0f) {
        ShVec3 o, ref = g_filtFrom;
        float dx, dy, dz;
        memcpy(&o, (const void *)(uintptr_t)(desc + 0x30), 12);
        if (g_filtPlayer && !ShGetPlayerPosition(&ref)) return;
        dx = o.x - ref.x; dy = o.y - ref.y; dz = o.z - ref.z;
        if (dx * dx + dy * dy + dz * dz >
            g_filtRadius * g_filtRadius) return;
    }

    r = &g_rayLog[g_rayHead % RAY_LOG];
    memset(r, 0, sizeof(*r));
    memcpy(&r->origin, (const void *)(uintptr_t)(desc + 0x30), 12);
    memcpy(&r->dir, dir, 12);
    r->descriptor = desc;
    r->collector = coll;

    /* At this hook RCX is the physics world, not a
     * projectile, so hits are captured at FUN_154D38550.
     */
    g_rayHead++;
    (void)hits;
}

/* The collector is filled by the call we hooked, so read
 * the previous ray's results now that its call is done.
 */
static void FinishPrevious(void) {
    ShRay *r;
    uint64_t coll, recs;
    uint16_t hits = 0;

    if (g_rayHead == 0) return;
    r = &g_rayLog[(g_rayHead - 1) % RAY_LOG];
    coll = r->collector;
    if (!coll || r->hits) return;
    if (!ShReadableAddr(coll, 0x40)) return;

    memcpy(r->raw, (const void *)(uintptr_t)coll, 32);
    memcpy(&hits, (const void *)(uintptr_t)(coll + 0x1A), 2);
    r->hits = hits;
    recs = ShReadQ(coll + 0x10);
    if (hits && hits < 4096 && ShReadableAddr(recs, 12))
        memcpy(&r->hitPos, (const void *)(uintptr_t)recs, 12);
}

/* Runs on the game thread inside a live physics call. */
static void __attribute__((ms_abi))
RayHookCallback(uint64_t rcx, uint64_t rdx, uint64_t r8) {
    g_ctx = rcx;
    FinishPrevious();
    RecordRay(rdx, r8);

    ShSpawnPump();
    ShNpcPump();
    ShSceneTick();

    if (g_qPending && g_qFn) {
        uint64_t f = g_qFn;
        uint64_t a0 = g_qArg[0], a1 = g_qArg[1];
        uint64_t a2 = g_qArg[2], a3 = g_qArg[3];
        uint64_t a4 = g_qArg[4], a5 = g_qArg[5];
        int isF = g_qFloat, isSix = g_qSix;
        float f2 = g_qF[0], f3 = g_qF[1], f4 = g_qF[2];

        g_qPending = 0;
        if (isF)        g_qRet = ((ShQFnF_t)f)(a0, a1, f2, f3, f4);
        else if (isSix) g_qRet = ((ShQFn6_t)f)(a0, a1, a2, a3, a4, a5);
        else            g_qRet = ((ShQFn_t)f)(a0, a1, a2, a3);
        g_qDone = 1;
    }
    if (!g_req || g_busy || !g_B) return;

    g_busy = 1;
    g_req = 0;
    memset(g_hitArr, 0, sizeof(g_hitArr));
    memset(g_recs, 0, sizeof(g_recs));
    *(void **)(g_hitArr + 0x10) = g_recs;
    *(uint32_t *)(g_hitArr + 0x18) = 0x00008010u;
    *(uint64_t *)(g_hitArr + 0x860) = LAYER_MASK;

    BuildDescriptor();
    ((CastRay_t)CAST_RAY_FN)(g_hitArr, (void *)g_B, g_desc, 0, 0, 0, 0);

    if (*(uint16_t *)(g_hitArr + 0x1a)) {
        float p[3];
        memcpy(p, g_recs, 12);
        g_hitZ = p[2];
        g_hitOk = 1;
    } else {
        g_hitOk = 0;
    }
    g_done = 1;
    g_busy = 0;
}

static int InstallHook(void) {
    uint64_t fn = RAY_HOOK_SITE;
    uint8_t *s;
    int o = 0, n = 5;
    int64_t rel;
    DWORD old;
    uint8_t patch[16];
    static const uint8_t PU[] = {
        0x50, 0x51, 0x52, 0x41,0x50, 0x41,0x51, 0x41,0x52, 0x41,0x53
    };
    static const uint8_t PO[] = {
        0x41,0x5B, 0x41,0x5A, 0x41,0x59, 0x41,0x58, 0x5A, 0x59, 0x58
    };

    if (g_stub) return 1;
    if (!ShReadableAddr(fn, n)) return 0;

    s = (uint8_t *)ShAllocNear(fn);
    if (!s) return 0;
    memset(s, 0xCC, 0x1000);

    memcpy(s + o, PU, sizeof(PU)); o += sizeof(PU);
    s[o++]=0x48; s[o++]=0x83; s[o++]=0xEC; s[o++]=0x20;
    s[o++]=0x48; s[o++]=0xB8;
    *(uint64_t *)(s+o) = (uint64_t)(uintptr_t)RayHookCallback; o += 8;
    s[o++]=0xFF; s[o++]=0xD0;
    s[o++]=0x48; s[o++]=0x83; s[o++]=0xC4; s[o++]=0x20;
    memcpy(s + o, PO, sizeof(PO)); o += sizeof(PO);
    memcpy(s + o, (void *)(uintptr_t)fn, n); o += n;
    s[o++]=0xFF; s[o++]=0x25;
    *(int32_t *)(s+o) = 0; o += 4;
    *(uint64_t *)(s+o) = fn + n;

    rel = (int64_t)(uintptr_t)s - (int64_t)(fn + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) return 0;
    if (!VirtualProtect((void *)(uintptr_t)fn, n,
                        PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(g_orig, (void *)(uintptr_t)fn, n);
    g_origLen = n;
    memset(patch, 0x90, n);
    patch[0] = 0xE9;
    *(int32_t *)(patch + 1) = (int32_t)rel;
    memcpy((void *)(uintptr_t)fn, patch, n);
    VirtualProtect((void *)(uintptr_t)fn, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)fn, n);

    g_stub = s;
    g_site = fn;
    return 1;
}

/* Installed once, on the first call that needs it, and
 * shared by every plugin in the process.
 */
static int EnsurePhysics(void) {
    int spins = 0;

    if (g_stub && WorldValid()) return 1;
    /* Stale after a level change, so resolve again. */
    g_A = 0;
    g_B = 0;
    if (!InstallHook()) return ShFailPhys(SH_ERR_HOOK_FAILED);

    /* Wait for the hook to see one live cast, rather than
     * handing the first caller a mystery failure.
     */
    while (!g_ctx && spins < CTX_WAIT_MS) {
        Sleep(1);
        spins++;
    }
    if (!g_ctx) return ShFailPhys(SH_ERR_NO_PHYSICS);

    if (!g_B) ResolveWorld(g_ctx);
    if (!g_B) return ShFailPhys(SH_ERR_NO_PHYSICS);
    ShSetError(SH_OK);
    return 1;
}

/* Pure predicate, no side effects. */
/* The ray callback's rcx is the hknpWorld itself, and
 * ResolveWorld finds the game's PhysicsWorld by requiring
 * its +0x10 to equal that. See HAVOK.md. */
SH_API uint64_t ShPhysicsWorldObject(void) { return g_A; }

SH_API uint64_t ShHavokWorldPtr(void) {
    if (g_ctx) return g_ctx;
    return g_A ? ShReadQ(g_A + 0x10) : 0;
}

SH_API int ShPhysicsReady(void) {
    return (g_stub != NULL && g_B != 0);
}

/* Called on the transition into Playing, so the world is
 * ready before any plugin asks for it.
 */
void ShPhysicsOnEnterPlaying(void) {
    int spins;

    g_A = 0;
    g_B = 0;
    if (!InstallHook()) return;
    for (spins = 0; spins < 40 && !WorldValid(); spins++) {
        if (g_ctx) ResolveWorld(g_ctx);
        if (WorldValid()) break;
        Sleep(250);
    }
}


/* Collision streams in around the player, so a query
 * outside that radius can never hit anything.
 */
static int InStreamRange(float x, float y) {
    ShVec3 p;
    float dx, dy;

    if (!ShGetPlayerPosition(&p)) return 1;
    dx = x - p.x;
    dy = y - p.y;
    return (dx * dx + dy * dy)
         <= (SH_STREAM_RADIUS * SH_STREAM_RADIUS);
}

/* Cast down from startZ for `span` metres. */
static int ProbeDown(float x, float y, float startZ, float span,
                     float *outZ) {
    int spins = 0;

    g_org[0] = x; g_org[1] = y; g_org[2] = startZ; g_org[3] = 0.0f;
    g_dir[0] = 0.0f; g_dir[1] = 0.0f;
    g_dir[2] = -span; g_dir[3] = 1.0f;

    g_hitOk = 0;
    g_done = 0;
    g_req = 1;
    while (!g_done && spins < 3000) {
        Sleep(1);
        spins++;
    }
    if (!g_done || !g_hitOk) return 0;
    *outZ = g_hitZ;
    return 1;
}

SH_API int ShGroundHeightFrom(float x, float y, float nearZ,
                              float *outZ) {
    if (!outZ) return ShFailPhys(SH_ERR_BAD_ARG);
    if (!ShRequireInGame()) return 0;
    if (!EnsurePhysics()) return 0;
    if (!InStreamRange(x, y)) return ShFailPhys(SH_ERR_NOT_STREAMED);
    if (!ProbeDown(x, y, nearZ + PROBE_UP, PROBE_UP + PROBE_DOWN,
                   outZ))
        return ShFailPhys(SH_ERR_NO_GROUND);
    ShSetError(SH_OK);
    return 1;
}

/* No hint, so sweep down from high altitude. */
SH_API int ShGroundHeight(float x, float y, float *outZ) {
    ShVec3 here;
    float start = SWEEP_TOP;

    if (!outZ) return ShFailPhys(SH_ERR_BAD_ARG);
    if (!ShRequireInGame()) return 0;
    if (!EnsurePhysics()) return 0;
    if (!InStreamRange(x, y)) return ShFailPhys(SH_ERR_NOT_STREAMED);
    if (ShGetPlayerPosition(&here) && here.z + SWEEP_ABOVE > start)
        start = here.z + SWEEP_ABOVE;

    if (!ProbeDown(x, y, start, SWEEP_SPAN, outZ))
        return ShFailPhys(SH_ERR_NO_GROUND);
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShTeleportPlayerToGround(float x, float y,
                                    float clearance) {
    ShVec3 dest;
    float z = 0.0f;

    /* Within the streamed radius only. Beyond it there is
     * no collision to query, so the caller picks a Z.
     */
    if (!ShGroundHeight(x, y, &z)) return 0;
    dest.x = x;
    dest.y = y;
    dest.z = z + clearance;
    return ShTeleportPlayer(&dest, NULL);
}

#if 0
/* Two stage far teleport. Went to altitude to stream the
 * destination, then dropped. It left the player falling.
 */
static int FarToGround(float x, float y, float clearance) {
    ShVec3 dest, here;
    float z = 0.0f;
    int spins;

    dest.x = x;
    dest.y = y;

    /* Too far to query, since nothing is streamed there.
     * Go first at altitude, which streams it, then drop.
     */
    dest.z = 1600.0f;
    if (ShGetPlayerPosition(&here) && here.z + 400.0f > dest.z)
        dest.z = here.z + 400.0f;
    if (!ShTeleportPlayer(&dest, NULL)) return 0;

    /* A region still streaming in answers with a bogus
     * high hit, so take it only once it stops moving.
     */
    {
        float prev = 0.0f;
        int agree = 0;

        /* Wait until the player is there, a free read.
         * Casting into a region still building kills it.
         */
        for (spins = 0; spins < 200; spins++) {
            if (ShGetPlayerPosition(&here)
                && fabsf(here.x - x) < 8.0f
                && fabsf(here.y - y) < 8.0f)
                break;
            Sleep(10);
        }
        if (spins >= 200) return ShFailPhys(SH_ERR_NO_GROUND);

        for (spins = 0; spins < 120; spins++) {
            Sleep(50);
            if (!ShGroundHeight(x, y, &z)) { agree = 0; continue; }
            if (agree && fabsf(z - prev) < 0.5f) {
                dest.z = z + clearance;
                return ShTeleportPlayer(&dest, NULL);
            }
            prev = z;
            agree = 1;
        }
    }
    return ShFailPhys(SH_ERR_NO_GROUND);
}
#endif
