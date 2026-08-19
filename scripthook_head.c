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

/* Model space, but a head sits within about 0.15m of the
 * centre line, so only its height is taken and the
 * character's rotation never matters. */
static int ReadHead(uint64_t skel, uint16_t bone, ShVec3 *out) {
    uint64_t pose, buf, rec;
    float org[3], b[3];

    if (bone >= 512) return 0;
    if (!ShReadableAddr(skel + SKEL_ORIGIN, 12)) return 0;
    memcpy(org, (void *)(uintptr_t)(skel + SKEL_ORIGIN), 12);

    pose = ShReadQ(skel + SKEL_POSE);
    if (!pose || !ShReadableAddr(pose + POSE_BUF, 8)) return 0;
    buf = ShReadQ(pose + POSE_BUF);
    if (!buf) return 0;

    rec = buf + (uint64_t)bone * BONE_STRIDE;
    if (!ShReadableAddr(rec, 12)) return 0;
    memcpy(b, (void *)(uintptr_t)rec, 12);

    if (b[2] < 0.1f || b[2] > 2.5f) return 0;
    out->x = org[0];
    out->y = org[1];
    out->z = org[2] + b[2];
    return 1;
}

/* Driven from the camera detour, on the game thread. */
void ShHeadPump(void) {
    ShPlayer p;
    uint64_t root;
    ShVec3 v;

    memset(&p, 0, sizeof(p));
    if (!ShGetPlayer(&p)) { g_valid = 0; return; }
    root = p.root ? p.root : p.entity;
    if (!root) { g_valid = 0; return; }

    if (root != g_skelEnt || !g_skel || ShReadQ(g_skel) != SKEL_VT) {
        g_skelEnt = root;
        g_skel = FindSkeleton(root);
        g_bone = BONE_NONE;
    }
    if (!g_skel) { g_valid = 0; return; }
    if (g_bone == BONE_NONE) g_bone = HeadBone(g_skel);

    if (ReadHead(g_skel, g_bone, &v)) {
        g_head = v;
        g_valid = 1;
    } else {
        g_valid = 0;
    }
}

int ShHeadCached(ShVec3 *out) {
    if (!g_valid) return 0;
    *out = g_head;
    return 1;
}

/** The player's head in world space, at eye height. */
SH_API int ShGetHeadPosition(ShVec3 *out) {
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
