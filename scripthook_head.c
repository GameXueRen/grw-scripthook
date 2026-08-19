/* The head, by bone name. ShGetPlayerPosition is NOT the
 * eye: measured 1.71m above the feet in one session and
 * 2.72m in another, so it moves against the body. */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

#define SKEL_VT      SH_IMG(0x3ACBBD8)
#define BONE_LOOKUP  SH_IMG(0xB435680)

/* Bones resolve from a name hash. The Head hash comes from
 * Firejumper93's rig code.
 */
#define HASH_HEAD    0x07C159A2u

#define SKEL_ORIGIN  0x120
#define SKEL_TABLE   0x220
#define SKEL_POSE    0x238
#define POSE_BUF     0x178
#define BONE_STRIDE  0x20
#define BONE_NONE    0xFFFF

#define OFF_ENT_COMPS  0x78
#define OFF_ENT_NCOMPS 0x82

extern int ShReadableAddr(uint64_t addr, size_t len);
extern uint64_t ShReadQ(uint64_t addr);
extern void ShSetError(int err);

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

/* Resolved addresses, so the per frame path reads them
 * directly and calls nothing.
 */
static uint64_t g_originAt = 0;
static uint64_t g_boneAt = 0;
static int      g_ready = 0;
static uint64_t g_lastTry = 0;

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
}

static uint64_t FindSkeleton(uint64_t entity) {
    uint64_t arr;
    uint16_t n = 0, i;

    if (!entity || !ShReadableAddr(entity, 0x90)) return 0;
    arr = ShReadQ(entity + OFF_ENT_COMPS);
    if (!arr || !ShReadableAddr(entity + OFF_ENT_NCOMPS, 2)) return 0;
    memcpy(&n, (void *)(uintptr_t)(entity + OFF_ENT_NCOMPS), 2);
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
    memset(&p, 0, sizeof(p));
    if (!ShGetPlayer(&p)) return 0;
    root = p.root ? p.root : p.entity;
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
    g_boneAt = buf + (uint64_t)g_bone * BONE_STRIDE;
    if (!ShReadableAddr(g_originAt, 12)) return 0;
    if (!ShReadableAddr(g_boneAt, 12)) return 0;

    g_ready = 1;
    return 1;
}

/* Driven from the camera detour, on the game thread. No
 * lookups here: the addresses were validated at resolve,
 * and a bad reading sends us back to resolve. */
void ShHeadPump(void) {
    float org[3], b[3];

    if (g_want <= 0) { g_valid = 0; return; }
    g_want--;

    if (!g_ready) {
        uint64_t now = GetTickCount64();
        if (now - g_lastTry < HEAD_RETRY_MS) return;
        g_lastTry = now;
        if (!Resolve()) { g_valid = 0; return; }
    }

    memcpy(org, (void *)(uintptr_t)g_originAt, 12);
    memcpy(b, (void *)(uintptr_t)g_boneAt, 12);

    /* A recycled buffer reads as nonsense, which is the
     * signal to resolve again rather than to poll.
     */
    if (!(b[2] > 0.1f && b[2] < 2.5f)) {
        g_ready = 0;
        g_valid = 0;
        return;
    }
    g_head.x = org[0];
    g_head.y = org[1];
    g_head.z = org[2] + b[2];
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
