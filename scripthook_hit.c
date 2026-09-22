/* Bullet hits. The data lives on the PROJECTILE, not on
 * the physics collector, see FINDINGS.md.
 */
#include <windows.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "scripthook_tick.h"
#include "image.h"
#include "log.h"

/* Inside FUN_154D38550, after its casts, where MOV RCX,RDI
 * has just put the projectile in RCX.
 */
#define HIT_SITE      SH_IMG(0x14703F83)
#define HIT_ORIG_CALL SH_IMG(0x29B4E00)

/* Common damage-projectile trajectory update. FIRST/STEP are the two
 * verified sites inside the 2D shot-line generator, kept as the
 * documented alternate to the trail patch; the shipped feature patches
 * only TRAIL. These three are addresses of the 2026-09-15 title update
 * (build 25120584), ported from the Wildlands Immersion Suite. The hit
 * site above is of the same build, re-pinned with it. */
#define VELOCITY_SITE_FIRST SH_IMG(0x147094E9)
#define VELOCITY_SITE_STEP  SH_IMG(0x147095F3)
#define VELOCITY_SITE_TRAIL SH_IMG(0x14756F1E)
/* The trajectory accumulator store inside the same shot-line walk.
 * The owner gate and the field offsets are decoded from the community
 * "Ballistic Drop" plugin (its current-build site), whose stub this
 * port follows: [rbx+0xB0] is the shot's owner object, [owner] the
 * entity it belongs to, and the store's y lane is the drop. */
#define DROP_SITE        SH_IMG(0x14708F3C)
#define DROP_ORIG_LEN    14

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
/* The non-blocking lookup. The detour below runs on the game thread inside
 * the projectile step, where ShGetPlayer's fallback heap walk would take
 * the player lock for seconds and stall the frame. */
extern int ShPeekPlayer(ShPlayer *out);
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

/* Bullet physics, see the Sh*Projectile* exports below. */
volatile LONG g_velocityMilli = 1000;   /* extern: the accuracy module */
static volatile LONG g_dropMilli = 1000;
static volatile LONG g_trajectoryCalls = 0;
static volatile LONG g_trailCalls = 0;
static int g_trajectoryReady = 0;
static volatile float g_velocityScale = 1.0f;
static volatile float g_dropScale = 1.0f;
static volatile uint64_t g_dropOwner1 = 0, g_dropOwner2 = 0;
static volatile uint64_t g_dropOwnerAt = 0;
static int g_dropReady = 0;

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
 * the check, junk handles pass as entities. The value itself is asked
 * for through ShEntityVtable(), which learns it live - a second pinned
 * copy here only meant a second place to forget on an update.
 */

static int IsEntity(uint64_t p) {
    if (!p || (p & 7) || !ShReadableAddr(p, 0x140)) return 0;
    return ShReadQ(p) == ShEntityVtable();
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

        ShTickPing(SH_TICK_HITPUMP);
        /* The drop patch gates on the local player's entity and root,
         * which change across sessions: refresh them here, off the
         * frame path, the way the community drop plugin polls. */
        if (g_dropReady &&
            GetTickCount64() - g_dropOwnerAt >= 500) {
            ShPlayer p;
            g_dropOwnerAt = GetTickCount64();
            if (ShPeekPlayer(&p)) {
                g_dropOwner1 = p.entity;
                g_dropOwner2 = p.root;
            }
        }
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

/* ---- bullet physics -------------------------------------------------
 * Ported from the Wildlands Immersion Suite. The multipliers are read
 * by the trail patch below; nothing here walks the engine.
 */

SH_API int ShSetProjectileVelocityMultiplier(float multiplier) {
    LONG value;
    if (!isfinite(multiplier)) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (multiplier < 0.10f) multiplier = 0.10f;
    if (multiplier > 10.0f) multiplier = 10.0f;
    value = (LONG)(multiplier * 1000.0f + 0.5f);
    InterlockedExchange(&g_velocityMilli, value);
    g_velocityScale = multiplier;
    return 1;
}

SH_API int ShSetProjectileDropMultiplier(float multiplier) {
    LONG value;
    if (!isfinite(multiplier)) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (multiplier < 0.0f) multiplier = 0.0f;
    if (multiplier > 10.0f) multiplier = 10.0f;
    value = (LONG)(multiplier * 1000.0f + 0.5f);
    InterlockedExchange(&g_dropMilli, value);
    g_dropScale = multiplier;
    return 1;
}

SH_API float ShGetProjectileVelocityMultiplier(void) {
    return (float)InterlockedCompareExchange(&g_velocityMilli,0,0) / 1000.0f;
}

SH_API uint32_t ShGetProjectileVelocityHookCount(void) {
    return (uint32_t)InterlockedCompareExchange(&g_trajectoryCalls,0,0);
}

SH_API uint32_t ShGetProjectileTrailHookCount(void) {
    return (uint32_t)InterlockedCompareExchange(&g_trailCalls,0,0);
}

SH_API float ShGetProjectileDropMultiplier(void) {
    return (float)InterlockedCompareExchange(&g_dropMilli,0,0) / 1000.0f;
}

/* At TrailFX creation xmm0 is the weapon's final muzzle velocity, including
 * attachments. RDI is the new TrailFX; +70 already contains its resolved cap.
 * Scale once at spawn, retaining vanilla assistance and lifecycle handling. */
static int InstallTrailVelocity(void){
    static const uint8_t expected[]={0xF3,0x0F,0x11,0x47,0x28};
    uint64_t site=VELOCITY_SITE_TRAIL;uint8_t *stub,*p,*skip1,*skip2,*vanilla,patch[5];int64_t rel;DWORD old;
    if(!ShReadableAddr(site,5)||memcmp((void*)(uintptr_t)site,expected,5))return 0;
    stub=ShAllocNear(site);if(!stub)return 0;p=stub;
    *p++=0x9C;*p++=0x50;*p++=0x52; /* preserve flags, RAX and RDX */
    /* The drawn trail is a second mover with its own speed ceiling.  Scaling
     * only m_fMuzzleVelocity makes the authoritative round arrive first while
     * the visible tracer is still clamped to the vanilla (usually 1000 m/s)
     * m_fMaxTrailSpeed.  RDI is a freshly initialized TrailFX, so scale its
     * object-local ceiling once here without touching the shared ammo data. */
    *p++=0x48;*p++=0x83;*p++=0xEC;*p++=0x10;             /* sub rsp,10h */
    *p++=0xF3;*p++=0x0F;*p++=0x7F;*p++=0x0C;*p++=0x24; /* movdqu [rsp],xmm1 */
    *p++=0x48;*p++=0xBA;memcpy(p,(uint64_t[]){(uint64_t)(uintptr_t)&g_velocityScale},8);p+=8;
    *p++=0xF3;*p++=0x0F;*p++=0x10;*p++=0x4F;*p++=0x70; /* movss xmm1,[rdi+70h] */
    *p++=0xF3;*p++=0x0F;*p++=0x59;*p++=0x0A;           /* mulss xmm1,[rdx] */
    *p++=0xF3;*p++=0x0F;*p++=0x11;*p++=0x4F;*p++=0x70; /* movss [rdi+70h],xmm1 */
    *p++=0xF3;*p++=0x0F;*p++=0x6F;*p++=0x0C;*p++=0x24; /* movdqu xmm1,[rsp] */
    *p++=0x48;*p++=0x83;*p++=0xC4;*p++=0x10;             /* add rsp,10h */
    *p++=0x66;*p++=0x0F;*p++=0x7E;*p++=0xC0; /* movd eax,xmm0 */
    *p++=0x41;*p++=0x39;*p++=0x87;*p++=0xB4;*p++=0x01;*p++=0x00;*p++=0x00;
    *p++=0x75;skip1=p++;
    *p++=0x41;*p++=0x39;*p++=0x87;*p++=0xB8;*p++=0x01;*p++=0x00;*p++=0x00;
    *p++=0x75;skip2=p++;
    *p++=0x48;*p++=0xBA;memcpy(p,(uint64_t[]){(uint64_t)(uintptr_t)&g_velocityScale},8);p+=8;
    *p++=0xF3;*p++=0x0F;*p++=0x59;*p++=0x02;
    *p++=0x66;*p++=0x0F;*p++=0x7E;*p++=0xC2;
    *p++=0xF0;*p++=0x41;*p++=0x0F;*p++=0xB1;*p++=0x97;*p++=0xB4;*p++=0x01;*p++=0x00;*p++=0x00;
    vanilla=p;*skip1=(uint8_t)(vanilla-(skip1+1));*skip2=(uint8_t)(vanilla-(skip2+1));
    memcpy(p,expected,sizeof(expected));p+=sizeof(expected);
    *p++=0x48;*p++=0xB8;memcpy(p,(uint64_t[]){(uint64_t)(uintptr_t)&g_trailCalls},8);p+=8;
    *p++=0xF0;*p++=0xFF;*p++=0x00;
    *p++=0x5A;*p++=0x58;*p++=0x9D;
    *p++=0xFF;*p++=0x25;memset(p,0,4);p+=4;memcpy(p,(uint64_t[]){site+5},8);
    rel=(int64_t)(uintptr_t)stub-(int64_t)(site+5);if(rel>INT32_MAX||rel<INT32_MIN)return 0;
    patch[0]=0xE9;*(int32_t*)(patch+1)=(int32_t)rel;
    FlushInstructionCache(GetCurrentProcess(),stub,128);
    if(!VirtualProtect((void*)(uintptr_t)site,5,PAGE_EXECUTE_READWRITE,&old))return 0;
    memcpy((void*)(uintptr_t)site,patch,5);VirtualProtect((void*)(uintptr_t)site,5,old,&old);
    FlushInstructionCache(GetCurrentProcess(),(void*)(uintptr_t)site,5);return 1;
}

/* The drop patch: scale the y lane of the per-step trajectory
 * accumulator the engine stores at [rbx+0x1A0], for the local
 * player's shots only. Decoded from the community "Ballistic Drop"
 * plugin (current-build site and field offsets): [rbx+0xB0] is the
 * shot's owner object and [owner] the entity it belongs to, so the
 * gate is the same player-only rule the accuracy hooks use. The stub
 * reproduces the engine's own flat-shot value (the per-axis
 * projection of this step's increment) and lerps the stored lane
 * toward it by the live scale: 1.0 vanilla, 0.0 flat, past 1.0
 * extrapolated drop. */
static int InstallDropScale(void){
    static const uint8_t expected[DROP_ORIG_LEN]={
        0x0F,0x29,0x83,0xA0,0x01,0x00,0x00,      /* movaps [rbx+1A0],xmm0 */
        0x0F,0x5C,0x8B,0x90,0x01,0x00,0x00       /* subps  xmm1,[rbx+190] */
    };
    uint64_t site=DROP_SITE;uint8_t *stub,*p,*q,patch[DROP_ORIG_LEN];DWORD old;
    int oJe1=0,oJge=0,oJe2=0,oScale=0,oJne=0,restore=0;
    if(g_dropReady)return 1;
    if(!ShReadableAddr(site,DROP_ORIG_LEN)||
       memcmp((void*)(uintptr_t)site,expected,DROP_ORIG_LEN))return 0;
    stub=ShAllocNear(site);if(!stub)return 0;p=stub;
    *p++=0x9C;*p++=0x50;*p++=0x41;*p++=0x52;              /* pushfq; push rax; push r10 */
    *p++=0x48;*p++=0x83;*p++=0xEC;*p++=0xA0;              /* sub rsp,0A0h */
    *p++=0x0F;*p++=0x11;*p++=0x14;*p++=0x24;              /* movups [rsp],xmm2 */
    *p++=0x0F;*p++=0x11;*p++=0x5C;*p++=0x24;*p++=0x10;    /* movups [rsp+10h],xmm3 */
    *p++=0x0F;*p++=0x11;*p++=0x64;*p++=0x24;*p++=0x20;    /* movups [rsp+20h],xmm4 */
    *p++=0x0F;*p++=0x11;*p++=0x6C;*p++=0x24;*p++=0x30;    /* movups [rsp+30h],xmm5 */
    *p++=0x48;*p++=0x8B;*p++=0x83;*(uint32_t*)p=0x000000B0u;p+=4; /* mov rax,[rbx+B0h] */
    *p++=0x48;*p++=0x85;*p++=0xC0;                        /* test rax,rax */
    *p++=0x0F;*p++=0x84;oJe1=(int)(p-stub);*(uint32_t*)p=0;p+=4;   /* je restore */
    *p++=0x83;*p++=0x78;*p++=0x0C;*p++=0x00;              /* cmp dword [rax+Ch],0 */
    *p++=0x0F;*p++=0x8D;oJge=(int)(p-stub);*(uint32_t*)p=0;p+=4;   /* jge restore */
    *p++=0x48;*p++=0x8B;*p++=0x00;                        /* mov rax,[rax] */
    *p++=0x48;*p++=0x85;*p++=0xC0;                        /* test rax,rax */
    *p++=0x0F;*p++=0x84;oJe2=(int)(p-stub);*(uint32_t*)p=0;p+=4;   /* je restore */
    /* the owner gate: only the local player's shots are scaled */
    *p++=0x49;*p++=0xBA;*(uint64_t*)p=(uint64_t)(uintptr_t)&g_dropOwner1;p+=8;
    *p++=0x49;*p++=0x3B;*p++=0x02;                        /* cmp rax,[r10] */
    *p++=0x0F;*p++=0x84;oScale=(int)(p-stub);*(uint32_t*)p=0;p+=4; /* je scale-path */
    *p++=0x49;*p++=0xBA;*(uint64_t*)p=(uint64_t)(uintptr_t)&g_dropOwner2;p+=8;
    *p++=0x49;*p++=0x3B;*p++=0x02;                        /* cmp rax,[r10] */
    *p++=0x0F;*p++=0x85;oJne=(int)(p-stub);*(uint32_t*)p=0;p+=4;   /* jne restore */
    /* scale path */
    *p++=0x0F;*p++=0x11;*p++=0x44;*p++=0x24;*p++=0x40;    /* movups [rsp+40h],xmm0 */
    *p++=0x0F;*p++=0x11;*p++=0x4C;*p++=0x24;*p++=0x50;    /* movups [rsp+50h],xmm1 */
    *p++=0x0F;*p++=0x28;*p++=0xD0;                        /* movaps xmm2,xmm0 */
    *p++=0x0F;*p++=0x5C;*p++=0x93;*(uint32_t*)p=0x00000190u;p+=4; /* subps xmm2,[rbx+190h] */
    *p++=0x0F;*p++=0x11;*p++=0x54;*p++=0x24;*p++=0x60;    /* movups [rsp+60h],xmm2 */
    *p++=0xF3;*p++=0x0F;*p++=0x10;*p++=0x5C;*p++=0x24;*p++=0x60;  /* movss xmm3,[rsp+60h] */
    *p++=0xF3;*p++=0x0F;*p++=0x59;*p++=0x9B;*(uint32_t*)p=0x00000130u;p+=4; /* mulss xmm3,[rbx+130h] */
    *p++=0xF3;*p++=0x0F;*p++=0x10;*p++=0x64;*p++=0x24;*p++=0x64;  /* movss xmm4,[rsp+64h] */
    *p++=0xF3;*p++=0x0F;*p++=0x59;*p++=0xA3;*(uint32_t*)p=0x00000134u;p+=4; /* mulss xmm4,[rbx+134h] */
    *p++=0xF3;*p++=0x0F;*p++=0x58;*p++=0xDC;              /* addss xmm3,xmm4 */
    *p++=0xF3;*p++=0x0F;*p++=0x10;*p++=0x64;*p++=0x24;*p++=0x68;  /* movss xmm4,[rsp+68h] */
    *p++=0xF3;*p++=0x0F;*p++=0x59;*p++=0xA3;*(uint32_t*)p=0x00000138u;p+=4; /* mulss xmm4,[rbx+138h] */
    *p++=0xF3;*p++=0x0F;*p++=0x58;*p++=0xDC;              /* addss xmm3,xmm4 */
    *p++=0xF3;*p++=0x0F;*p++=0x10;*p++=0xA3;*(uint32_t*)p=0x00000138u;p+=4; /* movss xmm4,[rbx+138h] */
    *p++=0xF3;*p++=0x0F;*p++=0x59;*p++=0xE3;              /* mulss xmm4,xmm3 */
    *p++=0xF3;*p++=0x0F;*p++=0x58;*p++=0xA3;*(uint32_t*)p=0x00000198u;p+=4; /* addss xmm4,[rbx+198h] */
    *p++=0xF3;*p++=0x0F;*p++=0x10;*p++=0x5C;*p++=0x24;*p++=0x48;  /* movss xmm3,[rsp+48h] */
    *p++=0xF3;*p++=0x0F;*p++=0x5C;*p++=0xDC;              /* subss xmm3,xmm4 */
    *p++=0x49;*p++=0xBA;*(uint64_t*)p=(uint64_t)(uintptr_t)&g_dropScale;p+=8;
    *p++=0xF3;*p++=0x41;*p++=0x0F;*p++=0x10;*p++=0x12;    /* movss xmm2,[r10] */
    *p++=0xF3;*p++=0x0F;*p++=0x59;*p++=0xD3;              /* mulss xmm3,xmm2 */
    *p++=0xF3;*p++=0x0F;*p++=0x58;*p++=0xE3;              /* addss xmm4,xmm3 */
    *p++=0xF3;*p++=0x0F;*p++=0x11;*p++=0x64;*p++=0x24;*p++=0x48;  /* movss [rsp+48h],xmm4 */
    *p++=0xF3;*p++=0x0F;*p++=0x11;*p++=0x64;*p++=0x24;*p++=0x58;  /* movss [rsp+58h],xmm4 */
    *p++=0x0F;*p++=0x10;*p++=0x44;*p++=0x24;*p++=0x40;    /* movups xmm0,[rsp+40h] */
    *p++=0x0F;*p++=0x10;*p++=0x4C;*p++=0x24;*p++=0x50;    /* movups xmm1,[rsp+50h] */
    /* restore path */
    restore=(int)(p-stub);
    *p++=0x0F;*p++=0x10;*p++=0x14;*p++=0x24;              /* movups xmm2,[rsp] */
    *p++=0x0F;*p++=0x10;*p++=0x5C;*p++=0x24;*p++=0x10;    /* movups xmm3,[rsp+10h] */
    *p++=0x0F;*p++=0x10;*p++=0x64;*p++=0x24;*p++=0x20;    /* movups xmm4,[rsp+20h] */
    *p++=0x0F;*p++=0x10;*p++=0x6C;*p++=0x24;*p++=0x30;    /* movups xmm5,[rsp+30h] */
    *p++=0x48;*p++=0x83;*p++=0xC4;*p++=0xA0;              /* add rsp,0A0h */
    *p++=0x41;*p++=0x5A;*p++=0x58;*p++=0x9D;              /* pop r10; pop rax; popfq */
    memcpy(p,expected,DROP_ORIG_LEN);p+=DROP_ORIG_LEN;   /* the displaced instructions */
    *p++=0xFF;*p++=0x25;*(uint32_t*)p=0;p+=4;*(uint64_t*)p=site+DROP_ORIG_LEN;p+=8;
    /* land all five jumps on the restore path */
    *(int32_t*)(stub+oJe1)=restore-(oJe1+4);
    *(int32_t*)(stub+oJge)=restore-(oJge+4);
    *(int32_t*)(stub+oJe2)=restore-(oJe2+4);
    *(int32_t*)(stub+oScale)=restore-(oScale+4);   /* corrected below to the scale path */
    *(int32_t*)(stub+oJne)=restore-(oJne+4);
    /* the je after the first owner compare lands on the SCALE path, not restore */
    q=stub;
    while(q+5<p && !(q[0]==0x0F&&q[1]==0x11&&q[2]==0x44&&q[3]==0x24&&q[4]==0x40)) q++;
    *(int32_t*)(stub+oScale)=(int)((q-stub))-(oScale+4);
    FlushInstructionCache(GetCurrentProcess(),stub,(SIZE_T)(p-stub));
    /* the site becomes a pure absolute jump into the stub */
    patch[0]=0xFF;patch[1]=0x25;*(uint32_t*)(patch+2)=0;
    *(uint64_t*)(patch+6)=(uint64_t)(uintptr_t)stub;
    if(!VirtualProtect((void*)(uintptr_t)site,DROP_ORIG_LEN,PAGE_EXECUTE_READWRITE,&old))return 0;
    memcpy((void*)(uintptr_t)site,patch,DROP_ORIG_LEN);
    VirtualProtect((void*)(uintptr_t)site,DROP_ORIG_LEN,old,&old);
    FlushInstructionCache(GetCurrentProcess(),(void*)(uintptr_t)site,DROP_ORIG_LEN);
    g_dropReady=1;
    return 1;
}

static int InstallTrajectoryHook(void){
    if(g_trajectoryReady)return 1;
    if(!ShReadableAddr(VELOCITY_SITE_TRAIL,5)||memcmp((void*)(uintptr_t)VELOCITY_SITE_TRAIL,"\xF3\x0F\x11\x47\x28",5)){
        if(ShReadableAddr(VELOCITY_SITE_TRAIL,5)){
            const uint8_t *b=(const uint8_t*)(uintptr_t)VELOCITY_SITE_TRAIL;
            Log("hit: trail velocity guard mismatch: %02x%02x%02x%02x%02x",
                b[0],b[1],b[2],b[3],b[4]);
        }else{
            Log("hit: trail velocity site unreadable at %llX",
                (unsigned long long)VELOCITY_SITE_TRAIL);
        }
        return 0;
    }
    if(!InstallTrailVelocity()){ShSetError(SH_ERR_HOOK_FAILED);return 0;}
    g_trajectoryReady=1;
    Log("hit: trail velocity hook installed at %llX",
        (unsigned long long)VELOCITY_SITE_TRAIL);
    /* The drop patch stands on its own two feet: a build that disagrees
     * with it keeps the velocity scaling, and says so. */
    if(!InstallDropScale())
        Log("hit: drop scale patch not taken at %llX",
            (unsigned long long)DROP_SITE);
    else
        Log("hit: drop scale patch installed at %llX",
            (unsigned long long)DROP_SITE);
    return 1;
}

SH_API int ShBallisticsHookInstall(void){
    int ok=InstallTrajectoryHook();
    ShSetError(ok?SH_OK:SH_ERR_HOOK_FAILED);
    return ok;
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
        if (ShPeekPlayer(&me))
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

    havePlayer = ShPeekPlayer(&me);
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
        LogFirst("scripthook_hit.log", "hit site %llX is not readable",
                 (unsigned long long)fn);
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    /* This site is a call the patch reissues by hand and there is no byte
     * check to catch a move, so the line records what is there this session:
     * compared against the previous run's, it is what says the constant
     * still points at the same call. */
    {
        uint8_t b[4];
        memcpy(b, (const void *)(uintptr_t)fn, sizeof(b));
        LogFirst("scripthook_hit.log",
                 "hit site %llX holds %02X %02X %02X %02X",
                 (unsigned long long)fn, b[0], b[1], b[2], b[3]);
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
    if (!g_pump) {
        g_pump = CreateThread(NULL, 0, HitPump, NULL, 0, NULL);
        /* The value is only ever read as "the pump exists": nothing waits
         * on it, so the thread object is released now. */
        if (g_pump) CloseHandle(g_pump);
    }
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShHitHookReady(void) { return g_hitStub != NULL; }
