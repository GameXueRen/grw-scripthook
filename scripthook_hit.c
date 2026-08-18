/* Bullet hits. The data lives on the PROJECTILE, not on
 * the physics collector, see FINDINGS.md.
 */
#include <windows.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

/* Inside FUN_154D38550, after its casts, where MOV RCX,RDI
 * has just put the projectile in RCX.
 */
#define HIT_SITE      SH_IMG(0x14D387E3)
#define HIT_ORIG_CALL SH_IMG(0x29B4970)

#define PROJ_LIST     0xA60
#define PROJ_COUNT    0xA6A
/* Verified live against the player entity. FUN_154D3B8D0
 * uses it to skip the shooter's own hits.
 */
#define PROJ_OWNER    0xB0
#define PROJ_PREV     0x190
#define PROJ_CUR      0x1A0
#define PROJ_FLOWN    0x164
#define PROJ_RANGE    0x3C
#define REC_STRIDE    0x80
#define REC_POS       0x00
#define REC_NORMAL    0x10
#define REC_HANDLE    0x38
#define REC_ID        0x40
#define REC_DIST      0x48
#define ENT_ID        0x138

#define MAX_RECS      64
#define HIT_RING      128
#define MAX_SINKS     8

extern int ShReadableAddr(uint64_t addr, size_t len);
extern uint64_t ShReadQ(uint64_t addr);
extern void ShSetError(int err);
extern void *ShAllocNear(uint64_t target);

static struct {
    ShHitFn fn;
    void   *user;
    int     flags;
} g_sinks[MAX_SINKS];

static ShHit g_ring[HIT_RING];
static volatile uint32_t g_ringHead = 0;
static uint8_t *g_hitStub = NULL;

/* One bullet is stepped every frame and its list carries
 * over, so remember what was already sent.
 */
static uint64_t g_lastProj = 0;
static uint64_t g_lastEnt = 0;

/* A bullet grazes the firer's own body on the way out, so
 * a self hit is one where the victim IS the shooter.
 */
static int WantsHit(const ShHit *hit, int flags) {
    int self = hit->shooter &&
               (hit->entity == hit->shooter ||
                hit->root == hit->shooter);

    if ((flags & SH_EVT_MINE_ONLY) && !hit->byPlayer) return 0;
    if ((flags & SH_EVT_NO_SELF) && self) return 0;
    return 1;
}

/* The projectile names its owner with a masked handle,
 * the same shape used everywhere else.
 */
/* Verified live: every Entity carries this vtable. Without
 * the check, junk handles pass as entities.
 */
#define VT_ENTITY SH_IMG(0x39C6FC8)

static int IsEntity(uint64_t p) {
    if (!p || (p & 7) || !ShReadableAddr(p, 0x140)) return 0;
    return ShReadQ(p) == VT_ENTITY;
}

static uint64_t ResolveOwner(uint64_t proj) {
    uint64_t h = ShReadQ(proj + PROJ_OWNER);
    uint64_t ent;
    int32_t flags = 0;

    if (!h || !ShReadableAddr(h, 0x10)) return 0;
    memcpy(&flags, (const void *)(uintptr_t)(h + 0xC), 4);
    if (flags >= 0) return 0;
    ent = ShReadQ(h);
    return IsEntity(ent) ? ent : 0;
}


SH_API int ShOnHit(ShHitFn fn, void *user, int flags) {
    int i;
    if (!fn) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    for (i = 0; i < MAX_SINKS; i++) {
        if (g_sinks[i].fn) continue;
        g_sinks[i].user = user;
        g_sinks[i].flags = flags;
        g_sinks[i].fn = fn;
        return 1;
    }
    ShSetError(SH_ERR_NO_CANDIDATE);
    return 0;
}

SH_API int ShOffHit(ShHitFn fn) {
    int i, n = 0;
    for (i = 0; i < MAX_SINKS; i++) {
        if (!g_sinks[i].fn) continue;
        if (fn && g_sinks[i].fn != fn) continue;
        g_sinks[i].fn = NULL;
        g_sinks[i].user = NULL;
        n++;
    }
    return n;
}

static struct {
    ShFireFn fn;
    void    *user;
    int      flags;
} g_fireSinks[MAX_SINKS];

static ShShot g_shots[HIT_RING];
static volatile uint32_t g_shotHead = 0;
static volatile uint32_t g_shotTail = 0;

SH_API int ShOnFire(ShFireFn fn, void *user, int flags) {
    int i;
    if (!fn) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    for (i = 0; i < MAX_SINKS; i++) {
        if (g_fireSinks[i].fn) continue;
        g_fireSinks[i].user = user;
        g_fireSinks[i].flags = flags;
        g_fireSinks[i].fn = fn;
        return 1;
    }
    ShSetError(SH_ERR_NO_CANDIDATE);
    return 0;
}

SH_API int ShOffFire(ShFireFn fn) {
    int i, n = 0;
    for (i = 0; i < MAX_SINKS; i++) {
        if (!g_fireSinks[i].fn) continue;
        if (fn && g_fireSinks[i].fn != fn) continue;
        g_fireSinks[i].fn = NULL;
        g_fireSinks[i].user = NULL;
        n++;
    }
    return n;
}

SH_API uint32_t ShShotCount(void) { return g_shotHead; }

SH_API int ShGetShots(ShShot *out, int max) {
    uint32_t head = g_shotHead;
    int have, i;

    if (!out || max <= 0) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    have = (int)(head < (uint32_t)HIT_RING ? head : (uint32_t)HIT_RING);
    if (have > max) have = max;
    for (i = 0; i < have; i++)
        out[i] = g_shots[(head - 1 - (uint32_t)i) % HIT_RING];
    return have;
}

SH_API uint32_t ShHitCount(void) { return g_ringHead; }

SH_API int ShGetHits(ShHit *out, int max) {
    uint32_t head = g_ringHead;
    int have, i;

    if (!out || max <= 0) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    have = (int)(head < (uint32_t)HIT_RING ? head : (uint32_t)HIT_RING);
    if (have > max) have = max;
    for (i = 0; i < have; i++)
        out[i] = g_ring[(head - 1 - (uint32_t)i) % HIT_RING];
    return have;
}

/* The engine resolves the handle then checks the id, so
 * do exactly the same before trusting it.
 */
static uint64_t ResolveHit(uint64_t rec, uint32_t *outId) {
    uint64_t h, ent;
    int32_t flags = 0;
    uint32_t want = 0, got = 0;

    h = ShReadQ(rec + REC_HANDLE);
    if (!h || !ShReadableAddr(h, 0x10)) return 0;
    memcpy(&flags, (const void *)(uintptr_t)(h + 0xC), 4);
    if (flags >= 0) return 0;

    ent = ShReadQ(h);
    if (!IsEntity(ent)) return 0;
    memcpy(&want, (const void *)(uintptr_t)(rec + REC_ID), 4);
    memcpy(&got, (const void *)(uintptr_t)(ent + ENT_ID), 4);
    if (want != got) return 0;
    if (outId) *outId = got;
    return ent;
}

/* The game thread only enqueues. Receivers are called from
 * the worker below, so they may use the whole API.
 */
static volatile uint32_t g_ringTail = 0;
static HANDLE g_pump = NULL;

static void Publish(const ShHit *hit) {
    g_ring[g_ringHead % HIT_RING] = *hit;
    g_ringHead++;
}

/* A bullet is stepped every frame and grazes things on the
 * way, so the impact is its FURTHEST hit, once it stops.
 */
#define PENDING_SLOTS 16
#define SETTLE_MS     120

static struct {
    uint64_t proj;
    ShHit    best;
    DWORD    seen;
    int      live;
} g_pending[PENDING_SLOTS];

static void Accumulate(const ShHit *hit) {
    int i, free = -1;

    for (i = 0; i < PENDING_SLOTS; i++) {
        if (!g_pending[i].live) { if (free < 0) free = i; continue; }
        if (g_pending[i].proj != hit->projectile) continue;
        if (hit->distance > g_pending[i].best.distance)
            g_pending[i].best = *hit;
        g_pending[i].seen = GetTickCount();
        return;
    }
    if (free < 0) {
        /* Full, so retire the stalest rather than drop. */
        DWORD oldest = 0;
        free = 0;
        for (i = 0; i < PENDING_SLOTS; i++) {
            DWORD age = GetTickCount() - g_pending[i].seen;
            if (age < oldest) continue;
            oldest = age;
            free = i;
        }
        Publish(&g_pending[free].best);
    }
    g_pending[free].proj = hit->projectile;
    g_pending[free].best = *hit;
    g_pending[free].seen = GetTickCount();
    g_pending[free].live = 1;
}

/* Nothing has stepped this bullet lately, so it landed. */
static void FlushSettled(void) {
    DWORD now = GetTickCount();
    int i;

    for (i = 0; i < PENDING_SLOTS; i++) {
        if (!g_pending[i].live) continue;
        if ((long)(now - g_pending[i].seen) < SETTLE_MS) continue;
        g_pending[i].live = 0;
        Publish(&g_pending[i].best);
    }
}

static void PumpShots(void) {
    uint32_t head = g_shotHead;

    if (head - g_shotTail > (uint32_t)HIT_RING)
        g_shotTail = head - HIT_RING;
    while (g_shotTail != head) {
        ShShot shot = g_shots[g_shotTail % HIT_RING];
        int i;
        g_shotTail++;
        for (i = 0; i < MAX_SINKS; i++) {
            if (!g_fireSinks[i].fn) continue;
            if ((g_fireSinks[i].flags & SH_EVT_MINE_ONLY) &&
                !shot.byPlayer)
                continue;
            g_fireSinks[i].fn(&shot, g_fireSinks[i].user);
        }
    }
}

static DWORD WINAPI HitPump(LPVOID p) {
    (void)p;
    for (;;) {
        uint32_t head = g_ringHead;

        PumpShots();
        FlushSettled();
        head = g_ringHead;
        if (g_ringTail == head) {
            Sleep(4);
            continue;
        }
        /* A slow receiver can be lapped, so never replay
         * records the ring has already overwritten.
         */
        if (head - g_ringTail > (uint32_t)HIT_RING)
            g_ringTail = head - HIT_RING;

        while (g_ringTail != head) {
            ShHit hit = g_ring[g_ringTail % HIT_RING];
            int i;
            g_ringTail++;
            for (i = 0; i < MAX_SINKS; i++)
                if (g_sinks[i].fn &&
                    WantsHit(&hit, g_sinks[i].flags))
                    g_sinks[i].fn(&hit, g_sinks[i].user);
        }
    }
    return 0;
}

/* A projectile is stepped every frame, so a shot is its
 * FIRST step. Objects are pooled, hence the flown check.
 */
#define SEEN_SLOTS 32
static struct {
    uint64_t proj;
    float    flown;
} g_seen[SEEN_SLOTS];
static int g_seenNext = 0;

static int IsFirstStep(uint64_t proj, float flown) {
    int i;

    for (i = 0; i < SEEN_SLOTS; i++) {
        if (g_seen[i].proj != proj) continue;
        if (flown >= g_seen[i].flown) {
            g_seen[i].flown = flown;
            return 0;
        }
        /* Distance went backwards, so the slot was reused
         * by a new bullet in the same memory.
         */
        g_seen[i].flown = flown;
        return 1;
    }
    g_seen[g_seenNext].proj = proj;
    g_seen[g_seenNext].flown = flown;
    g_seenNext = (g_seenNext + 1) % SEEN_SLOTS;
    return 1;
}

static void ReportShot(uint64_t proj) {
    ShShot shot;
    float prev[4], cur[4], flown = 0.0f, len;
    uint64_t h;

    if (!ShReadableAddr(proj + PROJ_CUR + 16, 4)) return;
    memcpy(&flown, (const void *)(uintptr_t)(proj + PROJ_FLOWN), 4);
    if (!IsFirstStep(proj, flown)) return;

    memcpy(prev, (const void *)(uintptr_t)(proj + PROJ_PREV), 16);
    memcpy(cur, (const void *)(uintptr_t)(proj + PROJ_CUR), 16);

    memset(&shot, 0, sizeof(shot));
    shot.projectile = proj;
    shot.origin.x = prev[0];
    shot.origin.y = prev[1];
    shot.origin.z = prev[2];
    shot.dir.x = cur[0] - prev[0];
    shot.dir.y = cur[1] - prev[1];
    shot.dir.z = cur[2] - prev[2];

    len = (float)sqrt((double)(shot.dir.x * shot.dir.x +
                               shot.dir.y * shot.dir.y +
                               shot.dir.z * shot.dir.z));
    if (len <= 1e-4f) return;
    shot.dir.x /= len;
    shot.dir.y /= len;
    shot.dir.z /= len;

    shot.yaw = (float)(atan2((double)shot.dir.y,
                             (double)shot.dir.x) * 57.2957795);
    shot.pitch = (float)(asin((double)shot.dir.z) * 57.2957795);
    if (ShReadableAddr(proj + PROJ_RANGE, 4))
        memcpy(&shot.range,
               (const void *)(uintptr_t)(proj + PROJ_RANGE), 4);

    shot.shooter = ResolveOwner(proj);
    if (shot.shooter) {
        ShPlayer me;
        shot.kind = ShGetEntityKind(shot.shooter);
        if (ShGetPlayer(&me))
            shot.byPlayer = (shot.shooter == me.entity ||
                             shot.shooter == me.root);
    }
    (void)h;

    g_shots[g_shotHead % HIT_RING] = shot;
    g_shotHead++;
}

/* Runs on the game thread, inside the projectile step. */
static void __attribute__((ms_abi)) HitDispatch(uint64_t proj) {
    uint64_t list, owner;
    uint16_t n = 0;
    ShPlayer me;
    int i, havePlayer, byPlayer;

    if (!proj || !ShReadableAddr(proj + PROJ_COUNT, 2)) return;
    ReportShot(proj);
    memcpy(&n, (const void *)(uintptr_t)(proj + PROJ_COUNT), 2);
    if (!n || n > MAX_RECS) return;

    list = ShReadQ(proj + PROJ_LIST);
    if (!list) return;

    havePlayer = ShGetPlayer(&me);
    owner = ResolveOwner(proj);
    byPlayer = havePlayer && owner &&
               (owner == me.entity || owner == me.root);

    for (i = 0; i < (int)n; i++) {
        uint64_t rec = list + (uint64_t)i * REC_STRIDE;
        uint32_t id = 0;
        uint64_t ent;
        ShHit hit;

        if (!ShReadableAddr(rec + REC_DIST + 4, 4)) break;
        ent = ResolveHit(rec, &id);
        if (!ent) continue;

        (void)havePlayer;

        memset(&hit, 0, sizeof(hit));
        hit.entity = ent;
        hit.root = ent;
        if (!ShWalkToRoot(ent, &hit.root) || !hit.root)
            hit.root = ent;
        hit.kind = ShGetEntityKind(hit.root);
        hit.shooter = owner;
        hit.byPlayer = byPlayer;
        hit.id = id;
        hit.projectile = proj;
        hit.index = i;
        memcpy(&hit.pos, (const void *)(uintptr_t)(rec + REC_POS), 12);
        memcpy(&hit.normal,
               (const void *)(uintptr_t)(rec + REC_NORMAL), 12);
        memcpy(&hit.distance,
               (const void *)(uintptr_t)(rec + REC_DIST), 4);

        g_lastProj = proj;
        g_lastEnt = ent;
        Accumulate(&hit);
    }
}

/* The site is a CALL rel32, so the stolen instruction
 * cannot be copied: it is reissued absolutely instead.
 */
SH_API int ShHitHookInstall(void) {
    uint64_t fn = HIT_SITE;
    uint8_t *s;
    int o = 0, n = 5;
    int64_t rel;
    DWORD old;
    uint8_t patch[8];
    static const uint8_t PU[] = {
        0x50, 0x51, 0x52, 0x41,0x50, 0x41,0x51, 0x41,0x52, 0x41,0x53
    };
    static const uint8_t PO[] = {
        0x41,0x5B, 0x41,0x5A, 0x41,0x59, 0x41,0x58, 0x5A, 0x59, 0x58
    };

    if (g_hitStub) return 1;
    if (!ShReadableAddr(fn, n)) {
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    s = (uint8_t *)ShAllocNear(fn);
    if (!s) { ShSetError(SH_ERR_HOOK_FAILED); return 0; }
    memset(s, 0xCC, 0x1000);

    memcpy(s + o, PU, sizeof(PU)); o += sizeof(PU);
    /* 0x28 aligns the callee from a CALL site, where the
     * ray hook needs 0x20 from a function entry.
     */
    s[o++]=0x48; s[o++]=0x83; s[o++]=0xEC; s[o++]=0x28;
    s[o++]=0x48; s[o++]=0xB8;
    *(uint64_t *)(s+o) = (uint64_t)(uintptr_t)HitDispatch; o += 8;
    s[o++]=0xFF; s[o++]=0xD0;
    s[o++]=0x48; s[o++]=0x83; s[o++]=0xC4; s[o++]=0x28;
    memcpy(s + o, PO, sizeof(PO)); o += sizeof(PO);

    /* Reissue the call the patch displaced, then rejoin. */
    s[o++]=0x48; s[o++]=0xB8;
    *(uint64_t *)(s+o) = HIT_ORIG_CALL; o += 8;
    s[o++]=0xFF; s[o++]=0xD0;
    s[o++]=0xFF; s[o++]=0x25;
    *(int32_t *)(s+o) = 0; o += 4;
    *(uint64_t *)(s+o) = fn + n;

    rel = (int64_t)(uintptr_t)s - (int64_t)(fn + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) {
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    if (!VirtualProtect((void *)(uintptr_t)fn, n,
                        PAGE_EXECUTE_READWRITE, &old)) {
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    patch[0] = 0xE9;
    *(int32_t *)(patch + 1) = (int32_t)rel;
    memcpy((void *)(uintptr_t)fn, patch, n);
    VirtualProtect((void *)(uintptr_t)fn, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(),
                          (void *)(uintptr_t)fn, n);
    g_hitStub = s;
    if (!g_pump)
        g_pump = CreateThread(NULL, 0, HitPump, NULL, 0, NULL);
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShHitHookReady(void) { return g_hitStub != NULL; }
