/* Havok bodies and motions, so a thing can be pushed
 * rather than teleported. Offsets come from the engine's
 * own body and motion managers, see HAVOK.md. */
/* Entity to body runs backwards on purpose: a body names
 * its RigidBody at +0x90 and that names its entity. That
 * holds for entities with no RigidBodyComponent too. */
#include <windows.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

/* hknpBodyManager::allocateBody. Its rcx is the manager,
 * and the manager is hknpWorld + 0x18.
 */
#define HK_ALLOC_BODY   SH_IMG(0x16990140)
#define HK_ALLOC_LEN    5
#define STUB_SLOT       0x800

#define RB_VT_BASE      SH_IMG(0x3AD55D8)
#define RB_VT_VEHICLE   SH_IMG(0x3AD7020)
#define RB_VT_C         SH_IMG(0x39E7620)
#define RB_VT_D         SH_IMG(0x38C88B0)
#define RB_VT_E         SH_IMG(0x39979C8)

#define RB_ENTITY       0x10

#define WORLD_BODIES    0x28
#define WORLD_MOTIONS   0x120
#define MGR_HIGH_ID     0x9C

#define BODY_STRIDE     0xB0
#define BODY_MOTIONID   0x40
#define BODY_ID         0x70
#define BODY_OWNER      0x90

#define MOTION_STRIDE   0x80
#define MOTION_LINVEL   0x40
#define MOTION_ANGVEL   0x50

/* Spin per unit of lever arm times push, tuned so a shove
 * on a car's corner rolls it rather than spinning it like
 * a top. */
#define SHOVE_SPIN      0.05f

/* The world holds thousands of owned bodies, so a small
 * table silently drops entities and a lookup succeeds or
 * fails depending on rebuild order. */
#define MAP_MAX         65536
#define MAP_AGE_MS      2000

extern void ShSetError(int err);
extern int ShReadMem(uint64_t addr, void *out, size_t len);
extern uint64_t ShReadQ(uint64_t addr);
extern int ShReadableAddr(uint64_t addr, size_t len);
extern void *ShAllocNear(uint64_t target);

static uint8_t *g_stub = NULL;
static uint8_t  g_orig[HK_ALLOC_LEN];
static int      g_hooked = 0;

/* The stub stores into its own page: the DLL is far past
 * what a rip relative store can reach.
 */
static int BuildStub(void) {
    uint8_t *s = (uint8_t *)ShAllocNear(HK_ALLOC_BODY);
    int64_t back, disp;
    int o = 0;

    if (!s) return 0;
    memset(s, 0xCC, 0x1000);
    memset(s + STUB_SLOT, 0, 8);

    s[o++] = 0x48; s[o++] = 0x89; s[o++] = 0x0D;
    disp = (int64_t)(uintptr_t)(s + STUB_SLOT)
         - ((int64_t)(uintptr_t)(s + o) + 4);
    if (disp > 0x7FFFFFFFLL || disp < -0x7FFFFFFFLL) return 0;
    *(int32_t *)(s + o) = (int32_t)disp;
    o += 4;

    memcpy(s + o, g_orig, HK_ALLOC_LEN);
    o += HK_ALLOC_LEN;

    back = (int64_t)(HK_ALLOC_BODY + HK_ALLOC_LEN)
         - ((int64_t)(uintptr_t)(s + o) + 5);
    if (back > 0x7FFFFFFFLL || back < -0x7FFFFFFFLL) return 0;
    s[o++] = 0xE9;
    *(int32_t *)(s + o) = (int32_t)back;

    g_stub = s;
    return 1;
}

/* The entry is one five byte instruction, so the patch
 * never splits one.
 */
static int Install(void) {
    uint8_t *at = (uint8_t *)(uintptr_t)HK_ALLOC_BODY;
    uint8_t patch[HK_ALLOC_LEN];
    int64_t rel;
    DWORD old;

    if (g_hooked) return 1;
    if (!ShReadableAddr(HK_ALLOC_BODY, HK_ALLOC_LEN)) return 0;
    memcpy(g_orig, at, HK_ALLOC_LEN);
    if (g_orig[0] != 0x48 || g_orig[1] != 0x89 || g_orig[2] != 0x5C)
        return 0;
    if (!BuildStub()) return 0;

    rel = (int64_t)(uintptr_t)g_stub - ((int64_t)HK_ALLOC_BODY + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) return 0;
    patch[0] = 0xE9;
    memcpy(patch + 1, &rel, 4);

    if (!VirtualProtect(at, HK_ALLOC_LEN, PAGE_EXECUTE_READWRITE,
                        &old))
        return 0;
    memcpy(at, patch, HK_ALLOC_LEN);
    VirtualProtect(at, HK_ALLOC_LEN, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, HK_ALLOC_LEN);
    g_hooked = 1;
    return 1;
}

extern uint64_t ShHavokWorldPtr(void);

/* The physics module already resolves this, so the world
 * is there as soon as physics is, with nothing to wait
 * for. The trampoline is only a fallback. */
SH_API uint64_t ShHavokWorld(void) {
    uint64_t w = ShHavokWorldPtr(), m;

    if (w && ShReadQ(w + WORLD_BODIES)) return w;
    if (!Install() || !g_stub) return 0;
    m = *(volatile uint64_t *)(g_stub + STUB_SLOT);
    return m ? m - 0x18 : 0;
}

/* Cars and characters share this vtable, so it is the
 * one test that holds across every RigidBody class.
 */
#define SH_VT_ENTITY  SH_IMG(0x39C6FC8)

static int IsEntity(uint64_t e) {
    if (e < 0x10000ULL || (e & 7)) return 0;
    return ShReadQ(e) == SH_VT_ENTITY;
}

typedef struct { uint64_t ent; uint32_t id; } ShPair;

static ShPair   g_map[MAP_MAX];
static int      g_mapN = 0;
static uint64_t g_mapAt = 0;
static int      g_lastBodies = 0, g_lastRb = 0;

extern int ShWalkToRoot(uint64_t entity, uint64_t *out);

/* An entity owns several bodies: a car has a collision
 * proxy whose motion is never stepped and a chassis whose
 * motion is. Both are kept and both get written. */
static void MapAdd(uint64_t ent, uint32_t id) {
    int i;

    if (!ent || g_mapN >= MAP_MAX) return;
    for (i = 0; i < g_mapN; i++)
        if (g_map[i].ent == ent && g_map[i].id == id) return;
    g_map[g_mapN].ent = ent;
    g_map[g_mapN].id = id;
    g_mapN++;
}

static int RebuildMap(void) {
    uint64_t world = ShHavokWorld(), bodies;
    uint32_t high = 0, id;

    g_mapN = 0;
    g_lastBodies = 0;
    g_lastRb = 0;
    if (!world) return 0;
    bodies = ShReadQ(world + WORLD_BODIES);
    if (!bodies) return 0;
    if (!ShReadMem(world + 0x18 + MGR_HIGH_ID, &high, 4)) return 0;
    if (!high || high > 4000000u) return 0;

    for (id = 0; id <= high && g_mapN < MAP_MAX; id++) {
        uint64_t body = bodies + (uint64_t)id * BODY_STRIDE;
        uint64_t rb, ent;
        uint32_t self = 0;

        if (!ShReadMem(body + BODY_ID, &self, 4)) continue;
        if ((self & 0xFFFFFF) != id) continue;
        g_lastBodies++;

        rb = ShReadQ(body + BODY_OWNER);
        if (!rb || (rb & 7)) continue;

        /* The wrapper sits at 0x10C0 on one class only,
         * so the owner is judged by whether it names a
         * real entity instead. */
        ent = ShReadQ(rb + RB_ENTITY);
        if (!IsEntity(ent)) continue;
        g_lastRb++;
        MapAdd(ent, id);
    }

    /* Roots fill gaps in a second pass. Walking from a
     * seated player lands on the car, so mixing this in
     * would shadow the car's own body. */
    for (id = 0; id <= high && g_mapN < MAP_MAX; id++) {
        uint64_t body = bodies + (uint64_t)id * BODY_STRIDE;
        uint64_t rb, ent, root = 0;
        uint32_t self = 0;

        if (!ShReadMem(body + BODY_ID, &self, 4)) continue;
        if ((self & 0xFFFFFF) != id) continue;
        rb = ShReadQ(body + BODY_OWNER);
        if (!rb || (rb & 7)) continue;
        ent = ShReadQ(rb + RB_ENTITY);
        if (!IsEntity(ent)) continue;
        if (!ShWalkToRoot(ent, &root) || !root || root == ent)
            continue;
        /* A passenger walks to the car it rides, so only
         * a body of the same kind may claim the root.
         */
        if (ShGetEntityKind(ent) != ShGetEntityKind(root))
            continue;
        MapAdd(root, id);
    }
    g_mapAt = GetTickCount64();
    return g_mapN;
}

/** The entity's Havok body id, 0 if it has none. */
SH_API uint32_t ShGetBodyId(uint64_t entity) {
    uint64_t now = GetTickCount64();
    int i;

    if (!entity) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (!g_mapN || now - g_mapAt > MAP_AGE_MS) RebuildMap();
    for (i = 0; i < g_mapN; i++)
        if (g_map[i].ent == entity) {
            ShSetError(SH_OK);
            return g_map[i].id;
        }
    /* A miss may just mean the table was stale, so it is
     * rebuilt once before reporting failure.
     */
    RebuildMap();
    for (i = 0; i < g_mapN; i++)
        if (g_map[i].ent == entity) {
            ShSetError(SH_OK);
            return g_map[i].id;
        }
    ShSetError(SH_ERR_NO_CANDIDATE);
    return 0;
}

/** Rebuilds now and reports bodies seen, RigidBodies
 *  found, and entities mapped.
 */
SH_API int ShHavokScan(int *bodies, int *rigidbodies, int *mapped) {
    int n = RebuildMap();

    if (bodies) *bodies = g_lastBodies;
    if (rigidbodies) *rigidbodies = g_lastRb;
    if (mapped) *mapped = n;
    ShSetError(n ? SH_OK : SH_ERR_NO_CANDIDATE);
    return n > 0;
}

/* The player rides a character controller, which owns its
 * body's position. Writing velocity there does nothing and
 * strands the collision body away from the player. */
static int IsController(uint64_t entity) {
    ShPlayer p;
    uint64_t root = 0;

    memset(&p, 0, sizeof(p));
    if (!ShGetPlayer(&p)) return 0;
    if (entity == p.entity) return 1;
    if (ShWalkToRoot(p.entity, &root) && root == entity && !ShIsInVehicle())
        return 1;
    return 0;
}

static uint64_t MotionOf(uint32_t id) {
    uint64_t world = ShHavokWorld(), bodies, motions, body;
    uint32_t mid = 0;

    if (!world) return 0;
    bodies = ShReadQ(world + WORLD_BODIES);
    motions = ShReadQ(world + WORLD_MOTIONS);
    if (!bodies || !motions) return 0;
    body = bodies + (uint64_t)id * BODY_STRIDE;
    if (!ShReadMem(body + BODY_MOTIONID, &mid, 4)) return 0;
    if (mid == 0xFFFFFFFFu || mid > 4000000u) return 0;
    return motions + (uint64_t)mid * MOTION_STRIDE;
}

static int SaneVec(const ShVec3 *v) {
    if (v->x != v->x || v->y != v->y || v->z != v->z) return 0;
    return fabsf(v->x) < 1e5f && fabsf(v->y) < 1e5f &&
           fabsf(v->z) < 1e5f;
}

static int WriteAt(uint64_t motion, int off, const ShVec3 *v) {
    SIZE_T put = 0;

    if (!SaneVec(v)) return 0;
    if (!WriteProcessMemory(GetCurrentProcess(),
                            (void *)(uintptr_t)(motion + off),
                            v, 12, &put))
        return 0;
    return put == 12;
}

/* An entity owns several bodies and only some are stepped,
 * so all of them are written and the caller never has to
 * know which is which. */
static int ApplyMotion(uint64_t entity, const ShVec3 *v, int off,
                       int add) {
    int i, hit = 0;

    if (!entity || !v) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (IsController(entity)) {
        ShSetError(SH_ERR_CONTROLLER);
        return 0;
    }
    ShGetBodyId(entity);
    for (i = 0; i < g_mapN; i++) {
        uint64_t m;
        ShVec3 w = *v;

        if (g_map[i].ent != entity) continue;
        m = MotionOf(g_map[i].id);
        if (!m) continue;
        if (add) {
            ShVec3 cur;

            if (!ShReadMem(m + off, &cur, 12)) continue;
            w.x += cur.x;
            w.y += cur.y;
            w.z += cur.z;
        }
        if (WriteAt(m, off, &w)) hit++;
    }
    ShSetError(hit ? SH_OK : SH_ERR_NO_CANDIDATE);
    return hit ? 1 : 0;
}

static int ReadMotion(uint64_t entity, ShVec3 *out, int off) {
    int i;

    if (!entity || !out) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    ShGetBodyId(entity);
    for (i = 0; i < g_mapN; i++) {
        uint64_t m;

        if (g_map[i].ent != entity) continue;
        m = MotionOf(g_map[i].id);
        if (m && ShReadMem(m + off, out, 12)) {
            ShSetError(SH_OK);
            return 1;
        }
    }
    ShSetError(SH_ERR_NO_CANDIDATE);
    return 0;
}

SH_API int ShGetVelocity(uint64_t entity, ShVec3 *out) {
    return ReadMotion(entity, out, MOTION_LINVEL);
}

SH_API int ShSetVelocity(uint64_t entity, const ShVec3 *v) {
    return ApplyMotion(entity, v, MOTION_LINVEL, 0);
}

SH_API int ShAddVelocity(uint64_t entity, const ShVec3 *v) {
    return ApplyMotion(entity, v, MOTION_LINVEL, 1);
}

SH_API int ShGetAngularVelocity(uint64_t entity, ShVec3 *out) {
    return ReadMotion(entity, out, MOTION_ANGVEL);
}

SH_API int ShSetAngularVelocity(uint64_t entity, const ShVec3 *v) {
    return ApplyMotion(entity, v, MOTION_ANGVEL, 0);
}

SH_API int ShAddAngularVelocity(uint64_t entity, const ShVec3 *v) {
    return ApplyMotion(entity, v, MOTION_ANGVEL, 1);
}

/** Whether velocity moves this entity at all. */
SH_API int ShCanMove(uint64_t entity) {
    ShVec3 v;

    if (!entity) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (IsController(entity)) {
        ShSetError(SH_ERR_CONTROLLER);
        return 0;
    }
    return ReadMotion(entity, &v, MOTION_LINVEL);
}

/* A push off centre spins as well as shoves, which is the
 * cross product of the lever arm and the push.
 */
SH_API int ShShove(uint64_t entity, const ShVec3 *dir, float strength,
                   const ShVec3 *atWorldPos) {
    ShVec3 push, spin, centre, r;
    float len;

    if (!entity || !dir) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    len = sqrtf(dir->x * dir->x + dir->y * dir->y +
                dir->z * dir->z);
    if (len < 1e-6f) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    push.x = dir->x / len * strength;
    push.y = dir->y / len * strength;
    push.z = dir->z / len * strength;

    if (!atWorldPos ||
        !ShGetEntityTransform(entity, &centre, NULL, NULL, NULL))
        return ShAddVelocity(entity, &push);

    r.x = atWorldPos->x - centre.x;
    r.y = atWorldPos->y - centre.y;
    r.z = atWorldPos->z - centre.z;
    spin.x = (r.y * push.z - r.z * push.y) * SHOVE_SPIN;
    spin.y = (r.z * push.x - r.x * push.z) * SHOVE_SPIN;
    spin.z = (r.x * push.y - r.y * push.x) * SHOVE_SPIN;

    if (!ShAddVelocity(entity, &push)) return 0;
    ShAddAngularVelocity(entity, &spin);
    ShSetError(SH_OK);
    return 1;
}
