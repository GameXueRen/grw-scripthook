/* Calls recovered from the Domino operator nodes. See
 * scripthook/spawnmaps/map-domino-*.md for the derivations.
 */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

/* World and weather, from map-domino-world.md. */
#define RVA_WORLD_GET     0x4969BF90
#define RVA_ENV_GET       0x40D43530
#define RVA_WEATHER_OF    0x4C744C10
#define RVA_WETNESS_SET   0x4C901280
#define RVA_LIGHTNING     0x54780750
#define RVA_PLAYER_MODE   0x44B878C8
#define RVA_EXPL_MGR      0x44BA0178
#define RVA_EXPL_SET      0x4A501680
#define RVA_EXPL_CLEAR    0x4A502AA0

/* Entities, from map-domino-world.md and the spawn map. */
#define RVA_VIS_ONE       0x4C6BEE90
#define RVA_BODY_BEGIN    0x4C58BB70
#define RVA_BODY_END      0x4C5ACA50
#define RVA_BODY_ENABLE   0x40F859D0
#define RVA_ATTACH        0x493A5130
#define RVA_DETACH        0x493A6290


/* The RVAs above are written as the map does, absolute
 * minus 0x140000000, so they grep against the maps. */
#define DOM(rva) SH_IMG((rva) - 0x40000000)

extern uint64_t ShReadQ(uint64_t addr);
extern int ShReadableAddr(uint64_t addr, size_t len);
extern int ShReadMem(uint64_t addr, void *out, size_t len);
extern int ShRequireInGame(void);
extern void ShSetError(int err);

/* Same test the API uses: an entity is its vtable. */
#define SH_VT_ENTITY SH_IMG(0x39C6FC8)

static int IsEntity(uint64_t obj) {
    return obj && ShReadQ(obj) == SH_VT_ENTITY;
}

typedef uint64_t (__attribute__((ms_abi)) *Get0_t)(void);
typedef uint64_t (__attribute__((ms_abi)) *Get1_t)(uint64_t);
typedef void (__attribute__((ms_abi)) *Void1_t)(uint64_t);
typedef void (__attribute__((ms_abi)) *Void2_t)(uint64_t, uint32_t);
typedef void (__attribute__((ms_abi)) *Vis_t)(uint64_t, uint64_t,
                                              int, int);
typedef void (__attribute__((ms_abi)) *Body_t)(uint64_t, uint8_t);
typedef uint64_t (__attribute__((ms_abi)) *Iter_t)(uint64_t,
                                                   uint64_t);
typedef int (__attribute__((ms_abi)) *Attach_t)(uint64_t, uint64_t,
                                                const void *);
typedef void (__attribute__((ms_abi)) *Expl_t)(uint64_t,
                                               const ShVec3 *,
                                               float);

/* ---- weather: plain writes and non physics calls ---- */

static uint64_t WeatherObj(void) {
    uint64_t world = ((Get0_t)DOM(RVA_WORLD_GET))();
    if (!world) return 0;
    return ((Get1_t)DOM(RVA_WEATHER_OF))(world);
}

SH_API int ShTriggerLightning(void) {
    uint64_t wr;
    if (!ShRequireInGame()) return 0;
    wr = WeatherObj();
    if (!wr) { ShSetError(SH_ERR_NO_GLOBAL); return 0; }
    ((Void1_t)DOM(RVA_LIGHTNING))(wr);
    return 1;
}

/* Setting wetness went nowhere: FUN_14C901280 writes
 * *(env+0xE70)+0x164 and that slot reads back 0 at once, so
 * the request is ignored. Read only until that is solved. */
SH_API float ShGetWetness(void) {
    uint64_t world, sub;
    float v = 0.0f;

    world = ((Get0_t)DOM(RVA_WORLD_GET))();
    if (!world) return 0.0f;
    sub = ShReadQ(world + 0xD0);
    if (!sub) return 0.0f;
    ShReadMem(sub + 0x1260, &v, 4);
    return v;
}

SH_API int ShSetLightningFrequency(int enable, float value) {
    uint64_t wr;
    if (!ShRequireInGame()) return 0;
    wr = WeatherObj();
    if (!wr || !ShReadableAddr(wr + 0x150, 8)) {
        ShSetError(SH_ERR_NO_GLOBAL);
        return 0;
    }
    *(volatile float *)(uintptr_t)(wr + 0x154) = value;
    *(volatile uint8_t *)(uintptr_t)(wr + 0x150) = enable ? 1 : 0;
    return 1;
}

/* ---- player mode bytes ---- */

static int ModeByte(int off, int on) {
    uint64_t g = ShReadQ(DOM(RVA_PLAYER_MODE));
    if (!g || !ShReadableAddr(g + 0x28, 4)) {
        ShSetError(SH_ERR_NO_GLOBAL);
        return 0;
    }
    *(volatile uint8_t *)(uintptr_t)(g + off) = on ? 1 : 0;
    return 1;
}

SH_API int ShSetGodMode(int on)   { return ModeByte(0x29, on); }
SH_API int ShSetGhostMode(int on) { return ModeByte(0x2A, on); }

SH_API int ShGetGodMode(void) {
    uint64_t g = ShReadQ(DOM(RVA_PLAYER_MODE));
    uint8_t v = 0;
    if (g) ShReadMem(g + 0x29, &v, 1);
    return v;
}

/* ---- explosion exclusion sphere ---- */

SH_API int ShExplosionShield(const ShVec3 *at, float radius) {
    uint64_t mgr = ShReadQ(DOM(RVA_EXPL_MGR));

    if (!mgr) { ShSetError(SH_ERR_NO_GLOBAL); return 0; }
    if (!at) {
        ((Void1_t)DOM(RVA_EXPL_CLEAR))(mgr);
        return 1;
    }
    ((Expl_t)DOM(RVA_EXPL_SET))(mgr, at, radius);
    return 1;
}

/* ---- frame path work: transforms and physics ----
 * The ray hook holds the physics lock, so these queue and
 * drain from the camera hook beside ShTransformPump. */
#define DQ_MAX 32
enum { DQ_VIS = 1, DQ_PHYS, DQ_ATTACH, DQ_DETACH };

typedef struct {
    int      kind;
    uint64_t a, b;
    int      i0, i1;
    float    m[16];
    volatile int ready;
} DomJob;

static DomJob g_dq[DQ_MAX];

static int ShFail_NoSlot(void) {
    ShSetError(SH_ERR_NO_CANDIDATE);
    return 0;
}

static int ShFail_NotEntity(void) {
    ShSetError(SH_ERR_NOT_ENTITY);
    return 0;
}

static int Queue(const DomJob *job) {
    int i;
    for (i = 0; i < DQ_MAX; i++) {
        if (g_dq[i].ready) continue;
        g_dq[i].kind = job->kind;
        g_dq[i].a = job->a;
        g_dq[i].b = job->b;
        g_dq[i].i0 = job->i0;
        g_dq[i].i1 = job->i1;
        memcpy(g_dq[i].m, job->m, sizeof(job->m));
        g_dq[i].ready = 1;
        return 1;
    }
    return ShFail_NoSlot();
}

/* The node's array helper sizes its buffer from a STALE
 * cache, so its count and its contents disagree and the walk
 * runs into uninitialised memory. Use the iterators. */
static void DoPhysics(uint64_t entity, int on) {
    uint64_t begin, end, p, base;
    int n = 0;

    begin = ((Iter_t)DOM(RVA_BODY_BEGIN))(entity + 0x154, entity);
    end = ((Iter_t)DOM(RVA_BODY_END))(entity + 0x154, entity);
    if (!begin || end <= begin || end - begin > 0x800) return;

    base = SH_IMG(0);
    for (p = begin; p < end && n < 256; p += 8, n++) {
        uint64_t body = ShReadQ(p);
        uint64_t vt = body ? ShReadQ(body) : 0;
        /* A real body's vtable lives in the image, so this
         * refuses to call through anything else. */
        if (vt < base || vt > base + 0x20000000) continue;
        ((Body_t)DOM(RVA_BODY_ENABLE))(body, on ? 1 : 0);
    }
}

/* Drained on the camera hook, the window that works. */
void ShDominoPump(void) {
    int i;

    for (i = 0; i < DQ_MAX; i++) {
        DomJob *j = &g_dq[i];
        if (!j->ready) continue;
        switch (j->kind) {
        case DQ_VIS:
            /* The node resolves the handle first and passes
             * the entity, with the flag as the second arg. */
            ((Body_t)DOM(RVA_VIS_ONE))(j->a, j->i0);
            break;
        case DQ_PHYS:
            DoPhysics(j->a, j->i0);
            break;
        case DQ_ATTACH:
            ((Attach_t)DOM(RVA_ATTACH))(j->a, j->b, j->m);
            break;
        case DQ_DETACH:
            ((Void1_t)DOM(RVA_DETACH))(j->a);
            break;
        default:
            break;
        }
        j->ready = 0;
    }
}

/* Visibility via FUN_14C6BEE90 is dropped: the bit at
 * entity+0x88 toggles and nothing vanishes, and ShSetVisible
 * already does the job. */

SH_API int ShSetEntityPhysics(uint64_t entity, int on) {
    DomJob j;
    if (!IsEntity(entity)) return ShFail_NotEntity();
    memset(&j, 0, sizeof(j));
    j.kind = DQ_PHYS;
    j.a = entity;
    j.i0 = on ? 1 : 0;
    return Queue(&j);
}

SH_API int ShAttachEntity(uint64_t child, uint64_t parent,
                          const ShVec3 *offset) {
    DomJob j;
    if (!IsEntity(child) || !IsEntity(parent))
        return ShFail_NotEntity();
    memset(&j, 0, sizeof(j));
    j.kind = DQ_ATTACH;
    j.a = child;
    j.b = parent;
    j.m[0] = 1.0f; j.m[5] = 1.0f; j.m[10] = 1.0f; j.m[15] = 1.0f;
    if (offset) {
        j.m[12] = offset->x;
        j.m[13] = offset->y;
        j.m[14] = offset->z;
    }
    return Queue(&j);
}

SH_API int ShDetachEntity(uint64_t child) {
    DomJob j;
    if (!IsEntity(child)) return ShFail_NotEntity();
    memset(&j, 0, sizeof(j));
    j.kind = DQ_DETACH;
    j.a = child;
    return Queue(&j);
}
