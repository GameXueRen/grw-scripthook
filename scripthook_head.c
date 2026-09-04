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
#define OFF_ENT_MATRIX 0x20

extern int ShReadableAddr(uint64_t addr, size_t len);
extern int ShReadMem(uint64_t addr, void *out, size_t len);
extern uint64_t ShReadQ(uint64_t addr);
extern void ShSetError(int err);
extern void ShCameraVehicleHint(int inVehicle);

/* ShGetPlayer falls back to a heap scan, which stalls the
 * frame and races other threads. The pump runs inside the
 * camera callback, so it only gets the scan free lookup. */
extern int ShPeekPlayer(ShPlayer *out);
extern int ShInVehicleOf(uint64_t entity);

typedef uint16_t (__attribute__((ms_abi)) *BoneOf_t)(uint64_t,
                                                     uint32_t);

static uint64_t g_skel = 0;
static uint64_t g_skelEnt = 0;
static uint16_t g_bone = BONE_NONE;
static ShVec3   g_head;
static volatile int g_valid = 0;

/* Seat anchor while riding. The skeleton is not welded to
 * the chassis on the same clock: its origin moves on the
 * render clock while the vehicle matrix is the physics
 * body, so at speed the two drift apart by a frame's worth
 * of travel. The eye reads that as the cabin shaking and
 * even passing through the shell. The fix: after the mount
 * settles, measure where the skeleton eye sits in the
 * vehicle's own frame and lock that offset. From then on the
 * eye is rebuilt from the vehicle matrix alone - a turn, a
 * bump, a launch all move the matrix and the eye moves with
 * it, so nothing relative can shake, lag or pass through. */
#define VEH_ANCHOR     1
#define VEH_SETTLE_MS  2000u   /* wait for the mount animation to end */
#define VEH_LOCK_FRAMES 30     /* consecutive settled frames to lock */
#define VEH_LOCK_MAX_MS 5000u  /* force the lock after this much */
#define VEH_SETTLE_MAX  0.02f  /* slow/fast lag below this = settled
                                  per axis (stops mid-animation lock) */
#define VEH_MAT_NEAR   60.0f   /* sanity: vehicle within this */
/* Dismount detection: once the vehicle is slow and the eye
 * has left the seat offset, the player is climbing out. The
 * lock releases early so the exit animation is seen, instead
 * of parking on the seat until the engine reports out. */
#define VEH_EXIT_SPD    1.0f   /* m/s below which exit can begin */
#define VEH_EXIT_DIST   0.45f  /* eye left the seat by this much */
#define VEH_EXIT_FRAMES 5      /* sustained frames before unlock */
#define VEH_EXIT_BIG    0.55f  /* one big jump unlocks at once: the
                                  exit head travels progressively and
                                  the rig may vanish mid animation,
                                  so do not wait for a metre - half a
                                  metre off the seat is already out */
static uint64_t g_vehRoot = 0;    /* vehicle root entity */
static uint64_t g_vehMountAt = 0; /* tick the mount began */
static float   g_vehLocal[3];     /* eye offset in vehicle space */
static int     g_vehLocked = 0;   /* anchor locked */
/* Convergence: a slow and a fast average of the eye in the
 * vehicle frame. While the mount animation moves the head the
 * two lag different amounts; when it truly stops they meet.
 * Thirty settled frames then lock, so the eye cannot be
 * captured mid animation leaning on the door. */
static float   g_vehSlow[3];
static float   g_vehFast[3];
static int     g_vehLive = 0;   /* averages seeded */
static int     g_vehSettled = 0;/* consecutive settled frames */
/* Exit state: while true the anchor is held off so the exit
 * animation's moving head is never re-locked. Cleared only
 * when the engine reports the player out of the vehicle. */
static int     g_vehExiting = 0;
static float   g_vehLastO[3];          /* previous matrix origin */
static uint64_t g_vehLastT = 0;        /* when that origin was read */
static int     g_vehAway = 0;          /* frames eye away from seat */

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
    if (VEH_ANCHOR) {
        g_vehRoot = 0;
        g_vehLocked = 0;
        g_vehLive = 0;
        g_vehSettled = 0;
    }
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

/* Rebuild the eye from the vehicle matrix, when locked. The
 * matrix is read raw (64 bytes): rows 0..2 are the chassis
 * axes in world space, row 3 the origin. Once the lock is
 * taken the eye is rebuilt purely from that matrix, so a
 * turn, a bump or a launch moves matrix and eye together.
 * Before the lock the head is still the skeleton's, so the
 * mounting animation shows normally and then hands over.
 */
static void VehAnchor(float *h) {
    float m[16];
    float o[3], r[3], f[3], u[3];
    float len, dx, dy, dz;
    uint64_t now;
    int i;

    if (!g_vehRoot) return;
    /* A failed read means the vehicle is gone: the player
     * stepped out or a level swapped it. Release the lock so
     * the pump falls back to the skeleton next frame instead
     * of parking the eye on a freed matrix. */
    if (!ShReadMem(g_vehRoot + OFF_ENT_MATRIX, m, 64)) {
        g_vehRoot = 0;
        g_vehLocked = 0;
        g_vehLive = 0;
        g_vehSettled = 0;
        return;
    }

    o[0] = m[12]; o[1] = m[13]; o[2] = m[14];
    for (i = 0; i < 3; i++) {
        r[i] = m[i];
        f[i] = m[4 + i];
        u[i] = m[8 + i];
    }
    len = sqrtf(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
    if (!(len > 0.3f && len < 3.0f)) return;
    for (i = 0; i < 3; i++) r[i] /= len;
    len = sqrtf(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    if (!(len > 0.3f && len < 3.0f)) return;
    for (i = 0; i < 3; i++) f[i] /= len;
    len = sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    if (!(len > 0.3f && len < 3.0f)) return;
    for (i = 0; i < 3; i++) u[i] /= len;

    /* Distance gate: the mount must be the vehicle we are in,
     * not some wreck a block away after a cutscene. */
    dx = h[0] - o[0];
    dy = h[1] - o[1];
    dz = h[2] - o[2];
    if (dx*dx + dy*dy + dz*dz > VEH_MAT_NEAR * VEH_MAT_NEAR)
        return;

    now = GetTickCount64();

    if (g_vehLocked) {
        /* Locked: keep watching the skeleton. If the vehicle is
         * slow and the eye has left the measured seat offset, the
         * player is climbing out - release the lock and let the
         * skeleton drive the exit animation. Otherwise rebuild
         * the eye from the chassis alone. */
        float loc0, loc1, loc2, d0, d1, d2;
        float spd = 0.0f;

        if (g_vehLastT) {
            float dt = (float)(now - g_vehLastT);
            if (dt >= 5.0f && dt <= 150.0f) {
                d0 = o[0] - g_vehLastO[0];
                d1 = o[1] - g_vehLastO[1];
                d2 = o[2] - g_vehLastO[2];
                spd = sqrtf(d0*d0 + d1*d1 + d2*d2) /
                      (dt * 0.001f);
            } else {
                spd = VEH_EXIT_SPD + 1.0f; /* unknown: hold lock */
            }
        }
        g_vehLastO[0] = o[0];
        g_vehLastO[1] = o[1];
        g_vehLastO[2] = o[2];
        g_vehLastT = now;

        loc0 = dx*r[0] + dy*r[1] + dz*r[2];
        loc1 = dx*f[0] + dy*f[1] + dz*f[2];
        loc2 = dx*u[0] + dy*u[1] + dz*u[2];
        d0 = loc0 - g_vehLocal[0];
        d1 = loc1 - g_vehLocal[1];
        d2 = loc2 - g_vehLocal[2];
        if (spd < VEH_EXIT_SPD &&
            sqrtf(d0*d0 + d1*d1 + d2*d2) > VEH_EXIT_DIST)
            g_vehAway++;
        else
            g_vehAway = 0;

        if (g_vehAway >= VEH_EXIT_FRAMES ||
            (spd < VEH_EXIT_SPD &&
             sqrtf(d0*d0 + d1*d1 + d2*d2) > VEH_EXIT_BIG)) {
            /* Leaving the seat: hand the eye back to the
             * skeleton so the exit animation plays, and hold
             * the anchor off until the engine reports out. The
             * one-frame big jump covers the case where the rig
             * disappears mid animation and the counter starves. */
            g_vehLocked = 0;
            g_vehLive = 0;
            g_vehSettled = 0;
            g_vehAway = 0;
            g_vehExiting = 1;
            return;
        }

        h[0] = o[0] + g_vehLocal[0]*r[0]
                    + g_vehLocal[1]*f[0] + g_vehLocal[2]*u[0];
        h[1] = o[1] + g_vehLocal[0]*r[1]
                    + g_vehLocal[1]*f[1] + g_vehLocal[2]*u[1];
        h[2] = o[2] + g_vehLocal[0]*r[2]
                    + g_vehLocal[1]*f[2] + g_vehLocal[2]*u[2];
        return;
    }

    /* Not locked. A dismount in progress keeps the skeleton eye
     * and never tries to re-measure the seat mid exit. */
    if (g_vehExiting) return;

    /* Unlocked: converge on the seat. */
    {
        float loc[3];
        loc[0] = dx*r[0] + dy*r[1] + dz*r[2];
        loc[1] = dx*f[0] + dy*f[1] + dz*f[2];
        loc[2] = dx*u[0] + dy*u[1] + dz*u[2];
        /* phase 0: mount animation, keep skeleton eye. */
        if (now - g_vehMountAt < VEH_SETTLE_MS) {
            g_vehLive = 0;
            return;
        }
        /* The eye must sit above the seat floor, not be
         * standing or wedged under the dash. */
        if (loc[2] < 0.2f || loc[2] > 3.0f) {
            g_vehLive = 0;
            return;
        }
        if (!g_vehLive) {
            g_vehSlow[0] = g_vehFast[0] = loc[0];
            g_vehSlow[1] = g_vehFast[1] = loc[1];
            g_vehSlow[2] = g_vehFast[2] = loc[2];
            g_vehLive = 1;
            g_vehSettled = 0;
            return;
        }
        for (i = 0; i < 3; i++) {
            g_vehSlow[i] += 0.10f * (loc[i] - g_vehSlow[i]);
            g_vehFast[i] += 0.60f * (loc[i] - g_vehFast[i]);
        }
        if (fabsf(g_vehSlow[0] - g_vehFast[0]) < VEH_SETTLE_MAX &&
            fabsf(g_vehSlow[1] - g_vehFast[1]) < VEH_SETTLE_MAX &&
            fabsf(g_vehSlow[2] - g_vehFast[2]) < VEH_SETTLE_MAX)
            g_vehSettled++;
        else
            g_vehSettled = 0;
        if (g_vehSettled >= VEH_LOCK_FRAMES ||
            now - g_vehMountAt > VEH_LOCK_MAX_MS) {
            /* Lock the slow average: it is the converged eye
             * once settled, and the least noisy fallback on
             * the timeout. */
            g_vehLocal[0] = g_vehSlow[0];
            g_vehLocal[1] = g_vehSlow[1];
            g_vehLocal[2] = g_vehSlow[2];
            g_vehLocked = 1;
        }
    }
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
    if (!ShPeekPlayer(&p)) return 0;

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

/* While the seat lock is held a failed skeleton read must not
 * drop the eye: keep the chassis-derived eye and stay valid.
 * Returns 1 when the frame is handled as a locked fallback. */
static int VehReadFallback(void) {
    if (!(VEH_ANCHOR && g_vehRoot && g_vehLocked)) return 0;
    VehAnchor(&g_head.x);
    if (g_head.x == g_head.x && g_head.y == g_head.y &&
        g_head.z == g_head.z) {
        g_valid = 1;
        return 1;
    }
    g_valid = 0;
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

    /* Menus hide the player from the static lookup, and
     * the rig is still there behind the menu. Polling
     * here only found nothing, slowly. Hold and wait. */
    if (ShInPauseMenu()) {
        if (!g_ready) g_valid = 0;
        g_mismatch = 0;
        g_lastCheck = now;
        g_lastTry = now;
        if (!g_ready) return;
    }

    /* A death or a body swap frees the rig while the old
     * memory still reads as plausible, so the identity is
     * rechecked on a timer rather than trusted. The seat
     * anchor needs the same peek, so the two share a beat. */
    if (now - g_lastCheck >= HEAD_RETRY_MS) {
        ShPlayer p;
        int inVeh;
        uint64_t ent;

        g_lastCheck = now;
        memset(&p, 0, sizeof(p));
        if (!ShPeekPlayer(&p)) {
            if (g_ready && ++g_mismatch >= 2) {
                g_mismatch = 0;
                g_ready = 0;
            }
            inVeh = 0;
        } else {
            inVeh = ShInVehicleOf(p.entity);
            if (g_ready) {
                ent = p.entity ? p.entity : p.root;
                if (ent != g_skelEnt) {
                    if (++g_mismatch >= 2) {
                        g_mismatch = 0;
                        g_ready = 0;
                    }
                } else {
                    g_mismatch = 0;
                }
            }
        }
        ShCameraVehicleHint(inVeh);

        /* Refresh the ride state for the seat anchor. A
         * failed peek or stepping out drops the anchor and
         * the eye falls back to the skeleton next frame.
         * The convergence state is cleared on every transition
         * so a fresh mount always re-measures the seat. */
        if (VEH_ANCHOR) {
            if (inVeh && p.root && p.root != p.entity) {
                if (p.root != g_vehRoot) {
                    g_vehRoot = p.root;
                    g_vehLocked = 0;
                    g_vehLive = 0;
                    g_vehSettled = 0;
                    g_vehExiting = 0;
                    g_vehAway = 0;
                    g_vehLastT = 0;
                    g_vehMountAt = now;
                }
            } else {
                g_vehRoot = 0;
                g_vehLocked = 0;
                g_vehLive = 0;
                g_vehSettled = 0;
                g_vehExiting = 0;
                g_vehAway = 0;
                g_vehLastT = 0;
            }
        }
    }

    /* Locked, but the rig is unavailable (a vehicle that hides
     * the soldier's skeleton): rebuild the eye from the chassis
     * alone so the camera never stalls, and keep retrying the
     * resolve on the slow beat so the live skeleton head comes
     * back when it does - the exit detection below needs it. */
    if (VEH_ANCHOR && g_vehRoot && g_vehLocked && !g_ready) {
        if (now - g_lastTry >= HEAD_RETRY_MS) {
            g_lastTry = now;
            if (Resolve()) {
                /* rig back: fall through to the normal read */
            } else {
                float h[3];
                h[0] = g_head.x; h[1] = g_head.y; h[2] = g_head.z;
                VehAnchor(h);
                if (h[0] == h[0] && h[1] == h[1] && h[2] == h[2]) {
                    g_head.x = h[0];
                    g_head.y = h[1];
                    g_head.z = h[2];
                    g_valid = 1;
                    return;
                }
                g_valid = 0;
                return;
            }
        } else {
            float h[3];
            h[0] = g_head.x; h[1] = g_head.y; h[2] = g_head.z;
            VehAnchor(h);
            if (h[0] == h[0] && h[1] == h[1] && h[2] == h[2]) {
                g_head.x = h[0];
                g_head.y = h[1];
                g_head.z = h[2];
                g_valid = 1;
                return;
            }
            g_valid = 0;
            return;
        }
    }

    if (!g_ready) {
        if (now - g_lastTry < HEAD_RETRY_MS) { g_valid = 0; return; }
        g_lastTry = now;
        if (!Resolve()) { g_valid = 0; return; }
    }

    /* Kernel reads: the rig can be freed on another thread
     * between any check and use, and these must not fault.
     * A locked seat anchor falls back to the chassis eye
     * instead of dropping the frame.
     */
    if (!ShReadMem(g_boneAt, b, 12) ||
        !ShReadMem(g_poseAt + POSE_FLAGS, &flags, 4)) {
        if (VehReadFallback()) return;
        g_ready = 0;
        g_valid = 0;
        return;
    }

    if (flags & POSE_WORLD) {
        if (b[0] != b[0] || b[1] != b[1] || b[2] != b[2]) {
            if (VehReadFallback()) return;
            g_ready = 0;
            g_valid = 0;
            return;
        }
        g_head.x = b[0];
        g_head.y = b[1];
        g_head.z = b[2];
        /* During the seat settle the eye stays the skeleton's
         * so the mount animation shows; once locked, VehAnchor
         * replaces it with the chassis-derived eye. */
        VehAnchor(&g_head.x);
        g_valid = 1;
        return;
    }

    /* A recycled buffer reads as nonsense, which is the
     * signal to resolve again rather than to poll.
     */
    if (!(b[2] > 0.1f && b[2] < 2.5f)) {
        if (VehReadFallback()) return;
        g_ready = 0;
        g_valid = 0;
        return;
    }

    if (!ShReadMem(g_originAt, org, 12) ||
        !ShReadMem(g_poseAt + POSE_ROOT_Q, rq, 16)) {
        if (VehReadFallback()) return;
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
        if (VehReadFallback()) return;
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
        if (VehReadFallback()) return;
        g_ready = 0;
        g_valid = 0;
        return;
    }
    VehAnchor(&g_head.x);
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
