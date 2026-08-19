/* The head, by bone name. ShGetPlayerPosition is NOT the
 * eye: measured 1.71m above the feet in one session and
 * 2.72m in another, so it moves against the body. */
#include <windows.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

#define SKEL_VT      SH_IMG(0x3ACBBD8)
#define BONE_LOOKUP  SH_IMG(0xB435680)

/* Bones resolve from a name hash. The Head hash and the
 * pose layout come from Firejumper93's rig code.
 */
#define HASH_HEAD    0x07C159A2u

#define SKEL_ORIGIN  0x120
#define SKEL_TABLE   0x220
#define SKEL_POSE    0x238
#define POSE_BUF     0x178
#define BONE_STRIDE  0x20
#define BONE_NONE    0xFFFF

/* The pose root quat at +0x10 maps model space to world.
 * The base stays the skeleton origin: it moves on the
 * render clock, so the eye is smooth while running. */
/* Bit 26 of +0x8C marks an already world space buffer. */
#define POSE_ROOT_Q  0x010
#define POSE_FLAGS   0x08C
#define POSE_WORLD   0x04000000u

#define OFF_ENT_COMPS  0x78
#define OFF_ENT_NCOMPS 0x82

extern int ShReadableAddr(uint64_t addr, size_t len);
extern int ShReadMem(uint64_t addr, void *out, size_t len);
extern uint64_t ShReadQ(uint64_t addr);
extern void ShSetError(int err);
extern void ShCameraVehicleHint(int inVehicle);

typedef uint16_t (__attribute__((ms_abi)) *BoneOf_t)(uint64_t,
                                                     uint32_t);

static uint64_t g_skel = 0;
static uint64_t g_skelEnt = 0;
static uint16_t g_bone = BONE_NONE;
static ShVec3   g_head;
static volatile int g_valid = 0;

/* Resolving the skeleton walks the component list, which is
 * a VirtualQuery each. Idle frames must not pay that, so
 * the pump runs only while something wants the head. */
#define HEAD_WANT_FRAMES 600
#define HEAD_RETRY_MS    500u
static volatile int g_want = 0;

/* The bone offset moves on the anim tick and can be read
 * mid write, which jitters. Blend it, the origin is on the
 * render clock and stays raw. */
#define HEAD_SMOOTH 0.25f
static float g_offs[3];
static int   g_offsLive = 0;

/* Resolved addresses, so the per frame path reads them
 * directly and calls nothing.
 */
static uint64_t g_originAt = 0;
static uint64_t g_poseAt = 0;
static uint64_t g_boneAt = 0;
static int      g_ready = 0;
static uint64_t g_lastTry = 0;
static uint64_t g_lastCheck = 0;
static int      g_mismatch = 0;

void ShHeadWant(void) {
    g_want = HEAD_WANT_FRAMES;
}

/* A new session invalidates everything cached. */
void ShHeadOnEnterPlaying(void) {
    g_ready = 0;
    g_valid = 0;
    g_skel = 0;
    g_skelEnt = 0;
    g_bone = BONE_NONE;
    g_lastTry = 0;
    g_offsLive = 0;
}

static uint64_t FindSkeleton(uint64_t entity) {
    uint64_t arr;
    uint16_t n = 0, i;

    if (!entity || !ShReadableAddr(entity, 0x90)) return 0;
    arr = ShReadQ(entity + OFF_ENT_COMPS);
    if (!arr) return 0;
    if (!ShReadMem(entity + OFF_ENT_NCOMPS, &n, 2)) return 0;
    if (!n || n > 512) return 0;

    for (i = 0; i < n; i++) {
        uint64_t c = ShReadQ(arr + (uint64_t)i * 8);
        if (c && ShReadableAddr(c, 8) && ShReadQ(c) == SKEL_VT)
            return c;
    }
    return 0;
}

/* The lookup is an engine call, so this only runs from the
 * pump, which the camera detour drives on the game thread.
 */
static uint16_t HeadBone(uint64_t skel) {
    uint64_t table;
    BoneOf_t boneOf;
    uint16_t idx;

    if (!ShReadableAddr(skel + SKEL_TABLE, 8)) return BONE_NONE;
    table = ShReadQ(skel + SKEL_TABLE);
    if (!table || !ShReadableAddr(table, 8)) return BONE_NONE;

    boneOf = (BoneOf_t)(uintptr_t)BONE_LOOKUP;
    idx = boneOf(table, HASH_HEAD);
    return (idx < 512) ? idx : BONE_NONE;
}

/* Everything expensive happens here, once. After this the
 * per frame cost is two reads of known addresses.
 */
static int Resolve(void) {
    ShPlayer p;
    uint64_t root, pose, buf;

    g_ready = 0;
    g_offsLive = 0;
    memset(&p, 0, sizeof(p));
    if (!ShGetPlayer(&p)) return 0;

    /* The soldier keeps the skeleton. The root re-parents
     * to the vehicle, so resolving from it would lose the
     * head on every mount. */
    root = p.entity ? p.entity : p.root;
    if (!root) return 0;

    g_skelEnt = root;
    g_skel = FindSkeleton(root);
    if (!g_skel) return 0;

    g_bone = HeadBone(g_skel);
    if (g_bone >= 512) return 0;

    pose = ShReadQ(g_skel + SKEL_POSE);
    if (!pose || !ShReadableAddr(pose + POSE_BUF, 8)) return 0;
    buf = ShReadQ(pose + POSE_BUF);
    if (!buf) return 0;

    g_originAt = g_skel + SKEL_ORIGIN;
    g_poseAt = pose;
    g_boneAt = buf + (uint64_t)g_bone * BONE_STRIDE;
    if (!ShReadableAddr(g_originAt, 12)) return 0;
    if (!ShReadableAddr(g_poseAt, 0x20)) return 0;
    if (!ShReadableAddr(g_poseAt + POSE_FLAGS, 4)) return 0;
    if (!ShReadableAddr(g_boneAt, 12)) return 0;

    g_ready = 1;
    return 1;
}

/* Driven from the camera detour, on the game thread. No
 * lookups here: the addresses were validated at resolve,
 * and a bad reading sends us back to resolve. */
void ShHeadPump(void) {
    float b[3], org[3], rq[4];
    float n, cx, cy, cz, dx, dy, dz;
    uint32_t flags;
    uint64_t now;

    if (g_want <= 0) { g_valid = 0; return; }
    g_want--;

    now = GetTickCount64();

    /* A death or a body swap frees the rig while the old
     * memory still reads as plausible, so the identity is
     * rechecked on a timer rather than trusted. */
    if (g_ready && now - g_lastCheck >= HEAD_RETRY_MS) {
        ShPlayer p;

        g_lastCheck = now;
        memset(&p, 0, sizeof(p));

        /* One bad answer can be a transient, and dropping
         * on it flashes the engine camera. Two in a row is
         * a real body change. */
        if (!ShGetPlayer(&p) ||
            (p.entity ? p.entity : p.root) != g_skelEnt) {
            if (++g_mismatch >= 2) {
                g_mismatch = 0;
                g_ready = 0;
            }
        } else {
            g_mismatch = 0;

            /* The camera's chase arm gate reads a cached
             * hint, so it never looks the player up on
             * the frame path. Refreshed here instead. */
            ShCameraVehicleHint(ShIsInVehicle());
        }
    }

    if (!g_ready) {
        if (now - g_lastTry < HEAD_RETRY_MS) { g_valid = 0; return; }
        g_lastTry = now;
        if (!Resolve()) { g_valid = 0; return; }
    }

    /* Kernel reads: the rig can be freed on another thread
     * between any check and use, and these must not fault.
     */
    if (!ShReadMem(g_boneAt, b, 12) ||
        !ShReadMem(g_poseAt + POSE_FLAGS, &flags, 4)) {
        g_ready = 0;
        g_valid = 0;
        return;
    }

    if (flags & POSE_WORLD) {
        if (b[0] != b[0] || b[1] != b[1] || b[2] != b[2]) {
            g_ready = 0;
            g_valid = 0;
            return;
        }
        g_head.x = b[0];
        g_head.y = b[1];
        g_head.z = b[2];
        g_valid = 1;
        return;
    }

    /* A recycled buffer reads as nonsense, which is the
     * signal to resolve again rather than to poll.
     */
    if (!(b[2] > 0.1f && b[2] < 2.5f)) {
        g_ready = 0;
        g_valid = 0;
        return;
    }

    if (!ShReadMem(g_originAt, org, 12) ||
        !ShReadMem(g_poseAt + POSE_ROOT_Q, rq, 16)) {
        g_ready = 0;
        g_valid = 0;
        return;
    }

    /* The root quat's magnitude carries a uniform scale,
     * so normalise before rotating.
     */
    n = sqrtf(rq[0] * rq[0] + rq[1] * rq[1] +
              rq[2] * rq[2] + rq[3] * rq[3]);
    if (!(n > 1e-6f)) {
        g_ready = 0;
        g_valid = 0;
        return;
    }
    rq[0] /= n; rq[1] /= n; rq[2] /= n; rq[3] /= n;

    /* v' = v + 2*qw*(q x v) + 2*(q x (q x v)) */
    cx = rq[1] * b[2] - rq[2] * b[1];
    cy = rq[2] * b[0] - rq[0] * b[2];
    cz = rq[0] * b[1] - rq[1] * b[0];
    dx = rq[1] * cz - rq[2] * cy;
    dy = rq[2] * cx - rq[0] * cz;
    dz = rq[0] * cy - rq[1] * cx;
    b[0] += 2.0f * (rq[3] * cx + dx);
    b[1] += 2.0f * (rq[3] * cy + dy);
    b[2] += 2.0f * (rq[3] * cz + dz);

    if (!g_offsLive) {
        g_offs[0] = b[0];
        g_offs[1] = b[1];
        g_offs[2] = b[2];
        g_offsLive = 1;
    } else {
        g_offs[0] += (b[0] - g_offs[0]) * HEAD_SMOOTH;
        g_offs[1] += (b[1] - g_offs[1]) * HEAD_SMOOTH;
        g_offs[2] += (b[2] - g_offs[2]) * HEAD_SMOOTH;
    }
    g_head.x = org[0] + g_offs[0];
    g_head.y = org[1] + g_offs[1];
    g_head.z = org[2] + g_offs[2];
    if (g_head.x != g_head.x || g_head.y != g_head.y ||
        g_head.z != g_head.z ||
        fabsf(g_head.x) > 1e5f || fabsf(g_head.y) > 1e5f ||
        fabsf(g_head.z) > 1e5f) {
        g_ready = 0;
        g_valid = 0;
        return;
    }
    g_valid = 1;
}

int ShHeadCached(ShVec3 *out) {
    if (!g_valid) return 0;
    *out = g_head;
    return 1;
}

/** The player's head in world space, at eye height. */
SH_API int ShGetHeadPosition(ShVec3 *out) {
    ShHeadWant();
    if (!out) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (!g_valid) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    *out = g_head;
    ShSetError(SH_OK);
    return 1;
}

/** Which bone the Head hash resolved to, or -1. */
SH_API int ShHeadBone(void) {
    return (g_bone == BONE_NONE) ? -1 : (int)g_bone;
}
