/* Ground queries through the engine's collision world.
 * Build pinned: GRW Definitive, base 0x140000000.
 *
 * Two pins, and both are checked before either is used: the hook site has to
 * open with the bytes below before it is patched, and the cast the pump calls
 * has to be code inside the image - and, once its own opening bytes are
 * pasted in, open with those. A build that moved either one is reported in
 * logs\scripthook_physics.log instead of being called through.
 */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"
#include "log.h"
#include <math.h>

/* Verified entry points, see GROUND_QUERY.md */
#define RAY_HOOK_SITE   SH_IMG(0x163F18D0)
#define CAST_RAY_FN     SH_IMG(0xFBB3580)

/* The engine's own 0x4000 mask rejects every hit in this
 * world, so query permissively and filter by distance.
 */
#define LAYER_MASK      0xFFFFFFFFFFFFFFFFULL

#define PROBE_UP        30.0f
#define PROBE_DOWN      80.0f

/* A cast answers with the nearest surface, and the nearest surface is not
 * always the ground: a canopy, a balcony or a roof over the point gets there
 * first, and a point laid on one of those is an NPC dropped out of the sky.
 * So a probe steps past what it found and asks again from just below it.
 *
 * The step has to be worth taking, though, and that is what PROBE_DROP is
 * for. A ray started a metre under a real surface comes back a metre or two
 * lower at most - that is the surface's own thickness - and taking that for a
 * finding walks the point underground a step at a time. A canopy is the other
 * way round: it sits well clear of whatever is beneath it. Measured on live
 * landing probes, ground answered 1.5 to 2.3 m lower and canopies 8 to 11 m
 * lower, so a threshold between the two tells them apart cleanly. */
#define PROBE_PASSES    2
#define PROBE_CLEAR     1.0f
#define PROBE_DROP      3.0f

/* How long to wait for the hook to see a live cast. */
#define CTX_WAIT_MS     3000

/* How long one ground probe waits for the game thread to service it. Two
 * frames is plenty: the callback runs inside the engine's own cast, so a
 * probe that has not been picked up in half a second was never going to be
 * (a caller on the game thread cannot be serviced at all, which is the case
 * this bounds). */
#define PROBE_WAIT_MS   500

/* Hintless sweep, proven working at 1400 down 900. */
#define SWEEP_TOP       1400.0f
#define SWEEP_ABOVE     300.0f
#define SWEEP_SPAN      2500.0f
#define REC_STRIDE      0x80
#define REC_COUNT       16

typedef uint8_t (__attribute__((ms_abi)) *CastRay_t)(void *, void *,
                                                     void *, char, char,
                                                     uint8_t, char);

static SH_ALIGNED(16) uint8_t g_hitArr[0x880];
static SH_ALIGNED(16) uint8_t g_recs[REC_COUNT * REC_STRIDE];
static SH_ALIGNED(16) uint8_t g_desc[0x80];
static SH_ALIGNED(16) float   g_org[4];
static SH_ALIGNED(16) float   g_dir[4];

static volatile LONG g_req = 0;
static volatile LONG g_done = 0;
static volatile int  g_busy = 0;
static volatile float g_hitZ = 0.0f;
static volatile int   g_hitOk = 0;

/* Signalled by the ray callback on the game thread, waited on by whoever
 * asked for a probe. The flags above stay the authority - this only decides
 * how soon the waiter learns they changed, which polling cannot do well:
 * Sleep(1) is a request the scheduler rounds up to its own tick, so the old
 * 3000-count loop could block its caller for the better part of a minute
 * rather than the three seconds it read as. */
static HANDLE g_probeEv;
static volatile LONG g_probeEvMade;

static HANDLE ProbeEvent(void) {
    if (!g_probeEv && InterlockedCompareExchange(&g_probeEvMade, 1, 0) == 0)
        g_probeEv = CreateEventA(NULL, FALSE, FALSE, NULL);
    return g_probeEv;
}

/* ---- the cast, made from the frame callback --------------------------
 *
 * The ray callback is a hook on the engine's own cast, so it runs *inside*
 * one. A cast asked for from there is one thread taking the physics lock a
 * second time, and that is the freeze: the game thread sat at stage 6 and
 * never came back, five times on 2026-09-24, every one of them a ground
 * probe. The frame callback runs on the game thread as well, but between
 * frames instead of inside a cast, so the same engine call is safe there.
 *
 * The request is a slot, like the spawn pump's: the prober leaves an origin
 * and a direction and waits, the frame callback sees it, casts, and fills
 * in the answer.
 *
 * A second, separate set of buffers on purpose - a physics callback may now
 * arrive on a worker thread while this is mid-cast, and sharing g_hitArr
 * with it would be a race.
 */
static SH_ALIGNED(16) uint8_t g_pgHit[0x880];
static SH_ALIGNED(16) uint8_t g_pgRecs[REC_COUNT * REC_STRIDE];
static SH_ALIGNED(16) uint8_t g_pgDesc[0x80];
static SH_ALIGNED(16) float   g_pgOrg[4];
static SH_ALIGNED(16) float   g_pgDir[4];

static volatile LONG  g_pgWanted = 0;   /* a cast is waiting to be made */
static volatile LONG  g_pgDone = 0;
static volatile LONG  g_pgArmed = 0;    /* the frame hook is up */
static volatile int   g_pgOk = 0;
static volatile float g_pgZ = 0.0f;

extern int ShRegisterFrameCallback(void (*fn)(void *), void *user);

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

/* Built into a caller supplied descriptor rather than always into the ray
 * hook's own: the frame callback's probe uses a separate one, because a
 * physics callback may arrive on a worker thread while the frame callback
 * is in the middle of a cast and the two must not share the buffer. */
static void BuildDescriptorAt(const float *org, const float *dir,
                              uint8_t *desc) {
    uint16_t all = 0xFFFF;
    uint32_t two = 2, mask = 0, i;
    const uint32_t BIG = 0x7F7FFFEEu;
    float inv[4];

    memset(desc, 0, 0x80);
    memcpy(desc + 0x10, &all, 2);
    memcpy(desc + 0x20, &two, 4);
    memcpy(desc + 0x30, org, 16);
    memcpy(desc + 0x40, dir, 16);
    for (i = 0; i < 4; i++) {
        float d = dir[i];
        if (d == 0.0f) memcpy(&inv[i], &BIG, 4);
        else inv[i] = 1.0f / d;
        if (d >= 0.0f) mask |= (1u << i);
    }
    memcpy(desc + 0x50, inv, 16);
    mask = (mask & 7u) | 0x3F000000u;
    memcpy(desc + 0x5C, &mask, 4);
}

static void BuildDescriptor(void) {
    BuildDescriptorAt(g_org, g_dir, g_desc);
}

/* The probe's cast, made from the frame hook: on the game thread, but
 * between frames rather than inside a cast. See the note on the buffers
 * above for why it cannot be made from RayHookCallback. */
static void GroundProbeFrame(void *user) {
    uint16_t hits;

    (void)user;

    if (!InterlockedExchange(&g_pgWanted, 0)) return;
    if (!WorldValid()) { InterlockedExchange(&g_pgDone, 1); return; }

    g_pgOk = 0;
    memset(g_pgHit, 0, sizeof(g_pgHit));
    memset(g_pgRecs, 0, sizeof(g_pgRecs));
    *(void **)(g_pgHit + 0x10) = g_pgRecs;
    *(uint32_t *)(g_pgHit + 0x18) = 0x00008010u;
    *(uint64_t *)(g_pgHit + 0x860) = LAYER_MASK;

    BuildDescriptorAt(g_pgOrg, g_pgDir, g_pgDesc);
    ((CastRay_t)CAST_RAY_FN)(g_pgHit, (void *)g_B, g_pgDesc, 0, 0, 0, 0);

    hits = *(uint16_t *)(g_pgHit + 0x1a);
    if (hits) {
        float p[3];
        memcpy(p, g_pgRecs, 12);
        g_pgZ = p[2];
        g_pgOk = 1;
        /* A ray that only ever goes down cannot hit above where it started.
         * A value up there is not a surface, and handing it up as one is
         * how an NPC ends up dropped from the sky. */
        if (g_pgZ > g_pgOrg[2]) g_pgOk = 0;
    }

    InterlockedExchange(&g_pgDone, 1);
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
static volatile LONG g_qPending = 0;
static volatile int g_qDone = 0;
static volatile int g_qFloat = 0;
static volatile int g_qSix = 0;
static volatile float g_qF[3];

/* Multi-producer safety: the slot is claimed by CAS, every job gets
 * a sequence number, and ShQueueResult only reports the sequence the
 * CALLING THREAD submitted (tracked per thread id).  Completed
 * results rest in a small ring keyed by sequence, so the next
 * submitter can no longer steal or invalidate an earlier waiter's
 * answer.  The per-thread table has 8 slots - more than 8 threads
 * queuing simultaneously would recycle an entry, far beyond real
 * usage. */
#define QDONE_MAX 8
static volatile LONG g_qClaim = 0;
static volatile LONG g_qSeqIssued = 0;
static volatile LONG g_qSeqCur = 0;
static volatile LONG g_qDoneSeq[QDONE_MAX];
static uint64_t g_qDoneRet[QDONE_MAX];
static struct { DWORD tid; LONG seq; } g_qTls[8];

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

/* Claim the slot, remember this thread's sequence, fill everything
 * and publish the pending flag LAST so the pump never sees a
 * half-filled job. */
static LONG QueueBegin(void) {
    DWORD tid = GetCurrentThreadId();
    LONG seq;
    int i;

    if (InterlockedCompareExchange(&g_qClaim, 1, 0)) return 0;
    seq = InterlockedIncrement(&g_qSeqIssued);
    g_qSeqCur = seq;
    for (i = 0; i < 8; i++) {
        if (!g_qTls[i].tid || g_qTls[i].tid == tid) {
            g_qTls[i].tid = tid;
            g_qTls[i].seq = seq;
            break;
        }
    }
    return seq;
}

/* Six integer arguments; the fifth and sixth go on the
 * stack, which the four argument path cannot reach. */
SH_API int ShQueueCall6(uint64_t fn, const uint64_t *args) {
    int i;
    if (!fn || !args) return 0;
    if (!QueueBegin()) return 0;
    for (i = 0; i < 6; i++) g_qArg[i] = args[i];
    g_qFloat = 0;
    g_qSix = 1;
    g_qFn = fn;
    InterlockedExchange(&g_qPending, 1);
    return 1;
}

SH_API int ShQueueCall(uint64_t fn, uint64_t a0, uint64_t a1,
                       uint64_t a2, uint64_t a3) {
    if (!fn) return 0;
    if (!QueueBegin()) return 0;
    g_qArg[0] = a0; g_qArg[1] = a1;
    g_qArg[2] = a2; g_qArg[3] = a3;
    g_qFloat = 0;
    g_qSix = 0;
    g_qFn = fn;
    InterlockedExchange(&g_qPending, 1);
    return 1;
}

/* Args three to five are floats, which the ABI puts in
 * xmm2, xmm3 and the stack. The integer path cannot reach
 * those, so the call goes through its own signature. */
SH_API int ShQueueCallF(uint64_t fn, uint64_t a0, uint64_t a1,
                        float f2, float f3, float f4) {
    if (!fn) return 0;
    if (!QueueBegin()) return 0;
    g_qArg[0] = a0; g_qArg[1] = a1;
    g_qF[0] = f2; g_qF[1] = f3; g_qF[2] = f4;
    g_qFloat = 1;
    g_qSix = 0;
    g_qFn = fn;
    InterlockedExchange(&g_qPending, 1);
    return 1;
}

SH_API int ShQueueResult(uint64_t *outRet) {
    DWORD tid = GetCurrentThreadId();
    LONG seq = 0;
    int i;

    for (i = 0; i < 8; i++)
        if (g_qTls[i].tid == tid) { seq = g_qTls[i].seq; break; }
    if (!seq) return 0;
    for (i = 0; i < QDONE_MAX; i++) {
        if (g_qDoneSeq[i] == seq) {
            if (outRet) *outRet = g_qDoneRet[i];
            return 1;
        }
    }
    return 0;
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

/* Which stage of the ray callback is running right now.
 *
 * When the game freezes, every log simply stops: the frame rate goes to
 * zero, waves time out three seconds later, and nothing says where the
 * game thread is sitting. This is three words that answer it. The callback
 * is on the game thread, so a watcher on another thread can still read
 * them after the game thread has stopped - and the stage number it reads
 * is the call the game thread never came back from. The call counter tells
 * the two cases apart: a stage that has not moved with g_cbIn above zero
 * means the game thread is inside the callback, and one that has not moved
 * with g_cbIn at zero means the callback is not being called at all, so
 * the game thread stopped somewhere else entirely.
 */
static volatile LONG g_cbStage = 0;
static volatile LONG g_cbIn = 0;
static volatile LONG g_cbSeq = 0;
static volatile LONG g_stallWatched = 0;

static DWORD WINAPI StallWatchThread(LPVOID p) {
    LONG lastStage = -1, lastIn = -1, lastSeq = -1;
    int still = 0;

    (void)p;
    if (!g_logFile) LogInit("scripthook_physics.log");
    for (;;) {
        LONG stage, in, seq;

        Sleep(2000);
        stage = g_cbStage;
        in = g_cbIn;
        seq = g_cbSeq;

        /* The counter moving means the game thread is alive, whatever the
         * stage happened to be caught at. */
        if (seq == lastSeq && in == lastIn && stage == lastStage) {
            still++;
            if (still == 2) {           /* four seconds without a call */
                if (in > 0)
                    Log("physics: the game thread has been inside the ray "
                        "callback for %d s - stage %ld is where it stopped",
                        still * 2, (long)stage);
                else
                    Log("physics: no ray callback for %d s and none in "
                        "flight - the game thread stopped elsewhere",
                        still * 2);
            }
        } else {
            still = 0;
        }
        lastStage = stage;
        lastIn = in;
        lastSeq = seq;
    }
    return 0;
}

/* Runs on the game thread inside a live physics call. */
static void __attribute__((ms_abi))
RayHookCallback(uint64_t rcx, uint64_t rdx, uint64_t r8) {
    InterlockedIncrement(&g_cbIn);
    InterlockedIncrement(&g_cbSeq);
    g_ctx = rcx;
    if (g_probeEv) SetEvent(g_probeEv);
    g_cbStage = 1;
    FinishPrevious();
    RecordRay(rdx, r8);

    g_cbStage = 2;
    ShSpawnPump();
    g_cbStage = 3;
    ShNpcPump();
    g_cbStage = 4;
    ShSceneTick();

    g_cbStage = 5;                    /* the queued engine call */
    if (g_qPending && g_qFn) {
        uint64_t f = g_qFn;
        uint64_t a0 = g_qArg[0], a1 = g_qArg[1];
        uint64_t a2 = g_qArg[2], a3 = g_qArg[3];
        uint64_t a4 = g_qArg[4], a5 = g_qArg[5];
        int isF = g_qFloat, isSix = g_qSix;
        float f2 = g_qF[0], f3 = g_qF[1], f4 = g_qF[2];
        LONG seq = g_qSeqCur;

        /* Snapshot taken; release the claim so the next submitter
         * can fill the slot while this job runs. */
        InterlockedExchange(&g_qPending, 0);
        InterlockedExchange(&g_qClaim, 0);
        if (isF)        g_qRet = ((ShQFnF_t)f)(a0, a1, f2, f3, f4);
        else if (isSix) g_qRet = ((ShQFn6_t)f)(a0, a1, a2, a3, a4, a5);
        else            g_qRet = ((ShQFn_t)f)(a0, a1, a2, a3);
        {
            int slot = (int)(seq % QDONE_MAX);
            g_qDoneRet[slot] = g_qRet;
            InterlockedExchange(&g_qDoneSeq[slot], seq);
        }
        g_qDone = 1;
    }
    /* No cast is made here any more, and that is the fix. This callback is
     * a hook on the engine's own cast, so it runs inside one - and asking
     * for another cast from here was one thread taking the physics lock a
     * second time, which is exactly where the game froze: five times on
     * 2026-09-24, the game thread stopped at stage 6 and never came back,
     * once per "spawn wave" pressed from the overlay menu.
     *
     * The probe's cast now lives in GroundProbeFrame, which the frame hook
     * calls between frames rather than inside a cast. Nothing on this path
     * calls an engine function any more: the pump and the queued calls
     * above are the only work left, and they are the reason the hook is
     * here at all. */
    g_cbStage = 0;
    InterlockedDecrement(&g_cbIn);
}

/* CAST_RAY_FN is called straight through, so it is the one pin in this
 * module with nothing under it: a build that moved it would be a call into
 * whatever now lives at that address. These six bytes are its prologue, read
 * off the running build on 2026-09-21 - the line below prints them on every
 * install, so the next pin is read straight out of
 * logs\scripthook_physics.log rather than searched for.
 *
 * Six bytes is the number asked for: long enough not to repeat by accident,
 * short enough to survive a build that moved nothing but a relative call.
 */
static const uint8_t kCastSig[8] = {
    0x40, 0x55, 0x57, 0x41, 0x54, 0x41
};
static const int     kCastSigLen = 6;

static int CastUsable(void) {
    uint64_t fn = CAST_RAY_FN;
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t got[sizeof kCastSig];

    if (!ShInImage(fn)) {
        Log("cast fn: %llX is outside the game image - the cast is not called",
            (unsigned long long)fn);
        return 0;
    }
    if (!VirtualQuery((const void *)(uintptr_t)fn, &mbi, sizeof mbi) ||
        mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) ||
        !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
        Log("cast fn: %llX is not executable code - the cast is not called",
            (unsigned long long)fn);
        return 0;
    }
    if (!ShReadMem(fn, got, sizeof got)) {
        Log("cast fn: %llX is not readable - the cast is not called",
            (unsigned long long)fn);
        return 0;
    }

    Log("cast fn: %llX opens %02X %02X %02X %02X %02X %02X",
        (unsigned long long)fn, got[0], got[1], got[2], got[3], got[4],
        got[5]);

    if (kCastSigLen && memcmp(got, kCastSig, (size_t)kCastSigLen) != 0) {
        Log("cast fn: %llX does not open like the pinned cast - not called "
            "(every ground query and teleport-to-ground would be wrong)",
            (unsigned long long)fn);
        return 0;
    }
    return 1;
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

    /* The opening of the function, with no operand in it. Without this the
     * site is patched blindly, and a stale one does not fail: it rewrites
     * the first five bytes of whatever lives at the old address. That is a
     * hook on the wrong function at best - the pump then runs when that
     * function runs, if ever - and a live corruption at worst. */
    static const uint8_t sig[20] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
        0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20
    };
    static int logInited;

    if (!logInited) { logInited = 1; LogInit("scripthook_physics.log"); }
    if (g_stub) return 1;
    if (!ShReadableAddr(fn, (size_t)sizeof sig)) {
        Log("ray hook: %llX is not readable", (unsigned long long)fn);
        return 0;
    }
    if (memcmp((const void *)(uintptr_t)fn, sig, sizeof sig) != 0) {
        Log("ray hook: %llX does not open like the pinned site - not patched "
            "(the pump and every ray would never run)", (unsigned long long)fn);
        return 0;
    }
    /* The site holds; the function it calls has to hold too, or the pump
     * would run and then call something else at a fixed address. Checked
     * once here rather than per ray: this is the only place it can be
     * decided for the session. */
    if (!CastUsable()) return 0;

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
    Log("ray hook: %llX hooked - the pump, the ground queries and every "
        "queued engine call run from this callback",
        (unsigned long long)fn);
    return 1;
}

/* Installed once, on the first call that needs it, and
 * shared by every plugin in the process.
 */
static int EnsurePhysics(void) {
    HANDLE ev;
    ULONGLONG deadline;

    if (g_stub && WorldValid()) return 1;
    /* Stale after a level change, so resolve again. */
    g_A = 0;
    g_B = 0;
    if (!InstallHook()) return ShFailPhys(SH_ERR_HOOK_FAILED);

    /* Wait for the hook to see one live cast, rather than
     * handing the first caller a mystery failure. Through the event the
     * callback signals, so it returns the moment a cast lands instead of
     * paying a scheduler tick for it - and the deadline is what the old
     * spin count only looked like. */
    ev = ProbeEvent();
    deadline = GetTickCount64() + CTX_WAIT_MS;
    while (!g_ctx && GetTickCount64() < deadline) {
        if (ev) WaitForSingleObject(ev, 1);
        else Sleep(1);
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
 *
 * The cold resolve is a whole-address-space scan and can take
 * tens of seconds on a first spawn, and TrackState runs this on
 * whoever polled the state first - in practice the chat poll
 * thread at 15ms, which then missed every T press for that whole
 * time. The wait/scan loop therefore runs on a worker, like the
 * spawn module's warm thread; until it lands, WorldValid() is
 * simply false and callers see "no world yet".
 */
static volatile LONG g_warmRunning = 0;

static DWORD WINAPI WorldWarmThread(LPVOID p) {
    int spins;

    (void)p;
    for (spins = 0; spins < 40 && !WorldValid(); spins++) {
        if (g_ctx) ResolveWorld(g_ctx);
        if (WorldValid()) break;
        Sleep(250);
    }
    InterlockedExchange(&g_warmRunning, 0);
    return 0;
}

void ShPhysicsOnEnterPlaying(void) {
    HANDLE h;

    g_A = 0;
    g_B = 0;
    if (!InstallHook()) return;

    /* The stall watcher, once. It lives outside the game thread on purpose:
     * it has to keep running after the game thread stops, because that is
     * the case it exists to report. */
    if (InterlockedCompareExchange(&g_stallWatched, 1, 0) == 0) {
        HANDLE w = CreateThread(NULL, 0, StallWatchThread, NULL, 0, NULL);

        if (w) CloseHandle(w);
        else InterlockedExchange(&g_stallWatched, 0);
    }

    /* The ground probe's cast runs from the frame hook rather than from the
     * ray callback - see GroundProbeFrame for why. Once, and a failure is
     * worth a line: without it every probe would time out. */
    if (InterlockedCompareExchange(&g_pgArmed, 1, 0) == 0) {
        if (!ShRegisterFrameCallback(GroundProbeFrame, NULL))
            Log("physics: no frame hook - ground probes cannot be serviced");
    }

    if (InterlockedCompareExchange(&g_warmRunning, 1, 0)) return;
    h = CreateThread(NULL, 0, WorldWarmThread, NULL, 0, NULL);
    if (!h) InterlockedExchange(&g_warmRunning, 0);
    else CloseHandle(h);
}


/* Live play only.
 *
 * The ray callback answers a query by running the engine's own cast, and
 * that cast does not come back in the pause menu: the world is loaded
 * enough for every read to keep working - which is exactly why
 * ShIsInGame() says yes there - but the physics side is not ticking, so
 * the engine sits inside the cast forever. The session of 2026-09-24
 * froze this way five times, every one of them when the player pressed
 * "spawn wave" from the menu: four seconds later the watcher reported the
 * game thread stopped at stage 6, the cast.
 *
 * Drone, binoculars and cinematics are live play and are allowed; menu,
 * load screens and the game over card are not, and a caller told "no
 * ground" there can fall back or give up with the game still alive. */
extern int ShGetGameState(void);
extern uint32_t ShGetUiState(void);
extern int ShMenuIsOpen(void);

static int LivePlay(void) {
    int s = ShGetGameState();

    return s == SH_STATE_INGAME || s == SH_STATE_DRONE ||
           s == SH_STATE_BINOCULAR || s == SH_STATE_CINEMATIC;
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

/* Cast down from startZ for `span` metres.
 *
 * The cast itself belongs to GroundProbeFrame - on the game thread, but
 * outside any cast. This leaves the request and waits for the answer; see
 * the note above that function for why it is not made here. */
static int ProbeDown(float x, float y, float startZ, float span,
                     float *outZ) {
    ULONGLONG deadline;

    g_pgOrg[0] = x; g_pgOrg[1] = y; g_pgOrg[2] = startZ; g_pgOrg[3] = 0.0f;
    g_pgDir[0] = 0.0f; g_pgDir[1] = 0.0f;
    g_pgDir[2] = -span; g_pgDir[3] = 1.0f;

    g_pgOk = 0;
    InterlockedExchange(&g_pgDone, 0);
    InterlockedExchange(&g_pgWanted, 1);

    deadline = GetTickCount64() + PROBE_WAIT_MS;
    while (!InterlockedCompareExchange(&g_pgDone, 0, 0) &&
           GetTickCount64() < deadline)
        Sleep(1);

    if (!InterlockedCompareExchange(&g_pgDone, 0, 0) || !g_pgOk) {
        /* Say why it did not come back, once per call site: a probe that
         * never answered is otherwise indistinguishable from one that hit
         * nothing, and the state at that moment is what a report needs. */
        InterlockedExchange(&g_pgWanted, 0);
        LogFirst("scripthook_physics.log",
                 "probe: no answer in %d ms - state %d, ui 0x%X, menu open "
                 "%d, done %d ok %d, frame hook %d",
                 PROBE_WAIT_MS, ShGetGameState(), (unsigned)ShGetUiState(),
                 ShMenuIsOpen(),
                 (int)InterlockedCompareExchange(&g_pgDone, 0, 0),
                 (int)g_pgOk,
                 (int)InterlockedCompareExchange(&g_pgArmed, 0, 0));
        return 0;
    }
    *outZ = g_pgZ;
    return 1;
}

/* Cast down from startZ for `span` metres, and go on past the surface it
 * finds until the ground is the answer - see PROBE_PASSES.
 *
 * A surface that really is the ground answers the next ray with itself, or
 * with nothing at all because that ray starts inside it, and the point
 * stands. A canopy does neither: the ray starts clear underneath it and
 * travels on. */
static int ProbeSurface(float x, float y, float startZ, float span,
                        float *outZ) {
    float z, below;
    int i;

    if (!ProbeDown(x, y, startZ, span, &z)) return 0;

    for (i = 0; i < PROBE_PASSES; i++) {
        if (!ProbeDown(x, y, z - PROBE_CLEAR, span, &below)) break;
        if (z - below < PROBE_DROP) break;
        z = below;
    }

    *outZ = z;
    return 1;
}

SH_API int ShGroundHeightFrom(float x, float y, float nearZ,
                              float *outZ) {
    if (!outZ) return ShFailPhys(SH_ERR_BAD_ARG);
    if (!ShRequireInGame()) return 0;
    if (!LivePlay()) return ShFailPhys(SH_ERR_NOT_IN_GAME);
    if (!EnsurePhysics()) return 0;
    if (!InStreamRange(x, y)) return ShFailPhys(SH_ERR_NOT_STREAMED);
    if (!ProbeSurface(x, y, nearZ + PROBE_UP, PROBE_UP + PROBE_DOWN,
                      outZ))
        return ShFailPhys(SH_ERR_NO_GROUND);
    ShSetError(SH_OK);
    return 1;
}

/* The full record of the first collision, not just its height (ported
 * from the Wildlands Immersion Suite). */
SH_API int ShProbeSurface(float x, float y, float nearZ,
                          ShSurfaceProbe *out) {
    float z;
    uint16_t hits;
    if (!out) return ShFailPhys(SH_ERR_BAD_ARG);
    memset(out, 0, sizeof(*out));
    if (!ShRequireInGame()) return 0;
    if (!EnsurePhysics()) return 0;
    if (!InStreamRange(x, y)) return ShFailPhys(SH_ERR_NOT_STREAMED);
    /* Character/render and physics space differ by roughly ten metres in
     * Wildlands. A local 24 m window covers that offset without selecting
     * unrelated collision high above the player. */
    if (!ProbeDown(x, y, nearZ + 16.0f, 24.0f, &z))
        return ShFailPhys(SH_ERR_NO_GROUND);
    hits = *(uint16_t *)(g_hitArr + 0x1a);
    out->hitPos.x = x; out->hitPos.y = y; out->hitPos.z = z;
    out->hits = hits;
    memcpy(out->record, g_recs, sizeof(out->record));
    ShSetError(SH_OK);
    return 1;
}

/* No hint, so sweep down from high altitude. */
SH_API int ShGroundHeight(float x, float y, float *outZ) {
    ShVec3 here;
    float start = SWEEP_TOP;

    if (!outZ) return ShFailPhys(SH_ERR_BAD_ARG);
    if (!ShRequireInGame()) return 0;
    if (!LivePlay()) return ShFailPhys(SH_ERR_NOT_IN_GAME);
    if (!EnsurePhysics()) return 0;
    if (!InStreamRange(x, y)) return ShFailPhys(SH_ERR_NOT_STREAMED);
    if (ShGetPlayerPosition(&here) && here.z + SWEEP_ABOVE > start)
        start = here.z + SWEEP_ABOVE;

    if (!ProbeSurface(x, y, start, SWEEP_SPAN, outZ))
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
