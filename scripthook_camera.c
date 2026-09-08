/* Camera access. The engine rebuilds the transform every
 * frame, so an override is applied per call from inside
 * the engine's own call chain, never from a thread. */
/* Struct offsets, the write authority of +0x000 and the
 * yaw/pitch convention are Firejumper93's findings, MIT.
 */
/* https://github.com/Firejumper93/GhostReconWildlandsVR */
#include <windows.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

/* The projection selector. It takes the camera in RCX and
 * runs every frame, which is what makes it hookable.
 */
#define CAM_THUNK   SH_IMG(0x13781B0)
#define CAM_IMPL    SH_IMG(0xD7C0610)

/* Verified live: +0x2B0 is vertical fov in radians, planes
 * beside it. +0x2BC is an ASPECT multiplier, which the
 * selector scales by width over height. */
#define CAM_POSE    0x000
#define CAM_MODE    0x290
#define CAM_FOV     0x2B0
#define CAM_NEAR    0x2B4
#define CAM_FAR     0x2B8
#define CAM_ASPECT  0x2BC
#define CAM_SKEWX   0x2C4
#define CAM_SKEWY   0x2C8

/* Nine derived matrices, 0x40 apart, view and projection
 * and their inverses. Read only in practice.
 */
#define CAM_MATS    0x420
#define CAM_MAT_N   9

/* The camera manager, one frame ahead of the camera build.
 * The behaviour's transform lands here first, so an override
 * placed here reaches culling and the matrices together. */
#define MGR_SITE    SH_IMG(0x7E888FE)
#define MGR_LEN     5
#define MGR_NEXT    SH_IMG(0x10D8890)

/* Verified live in gameplay: the mode at +0x6C reads 3, so
 * consumers take the position from +0x170 while the render
 * camera takes the rows at +0x190. Both are restated. */
#define MGR_MODE    0x6C
#define MGR_POS     0x170
#define MGR_FOV     0x180
#define MGR_XFORM   0x190

/* Ownership is per field, so two plugins can hold different
 * parts of the camera at once. Orbit is a private bit: it
 * owns position, but derives it instead of storing it. */
#define CAM_ORBIT_BIT 0x100u
#define CAM_HEAD_BIT  0x200u
#define CAM_DERIVED   (CAM_ORBIT_BIT | CAM_HEAD_BIT)

static volatile uint64_t g_cam = 0;
static volatile uint64_t g_calls = 0;
static volatile uint64_t g_writes = 0;
/* The engine can take the camera away - a stowed weapon
 * widens the view, a parachute pulls back, a drone flies off
 * - and ApplyHead lets it go rather than fighting. A consumer
 * (the first person plugin) reads this to know whether the
 * hidden head is on camera or the player is in a view that
 * should show it.
 *
 * Rather than trusting our own writes, this is measured from
 * the camera the engine is actually rendering: when it sits
 * on the player's head the view is first person, when it is
 * pulled back it is the engine's own shoulder or cutscene
 * camera. That stays true even when the engine never routes
 * a frame through ApplyHead (a stowed weapon, a parachute),
 * which is exactly when the head must come back. */
static volatile uint64_t g_headNearAt = 0; /* last frame cam was on the head */
#define HEAD_NEAR_DIST 1.0f   /* metres: first person eye to head bone */
#define FP_LIVE_MS     250u   /* how stale "near" may be before we say away */

/* Last frame ApplyHead really placed the eye. The three places it
 * bows out (a scoped fov, a camera beyond a chase arm, no head to
 * read) write nothing at all, so a stamp that stops advancing means
 * the engine owns the view - which is exactly what "not first
 * person" has to mean. Unlike the distance below this says nothing
 * about where the eye sits, so a preset or a seat anchor that parks
 * the eye well off the head bone still counts as first person. */
static volatile uint64_t g_headWroteAt = 0;

/* How long after the last question the head has to stay live. The
 * view state is also read while we do NOT own the camera (to tell
 * "the engine took over" apart), and the head pump only runs while
 * somebody wants the head. A consumer polling it keeps it alive. */
static volatile uint64_t g_viewWantAt = 0;
#define VIEW_WANT_MS 2000u

/* Diagnostic only. Special views run mode 0 like the main
 * camera (measured: the drone), so mode gates nothing.
 */
static volatile uint64_t g_otherAt = 0;
static volatile int g_otherMode = 0;

/* The pause menu never leaves the Playing game state, but
 * its camera is a template: a bit exact identity basis,
 * which no steered camera ever holds. Measured live. */
static volatile uint64_t g_uiAt = 0;

static ShVec3 g_absPos;
static volatile float g_back = 0.0f;
static volatile float g_up = 0.0f;
static volatile float g_yaw = 0.0f;
static volatile float g_pitch = 0.0f;
static volatile float g_roll = 0.0f;
static volatile float g_fov = 0.0f;
static volatile float g_skewX = 0.0f;
static volatile float g_skewY = 0.0f;
static volatile int g_modeSet = 0;
static volatile uint32_t g_apply = 0;

static uint8_t *g_camStub = NULL;
static uint8_t  g_thunkOrig[5];

static uint8_t *g_mgrStub = NULL;
static int      g_mgrHooked = 0;

extern void ShSetError(int err);
extern void ShVisibilityPump(void);
extern void ShTransformPump(void);
extern void ShDominoPump(void);
extern void ShHeadPump(void);
extern void ShHeadWant(void);
extern int ShFovSet(float radians);
extern void ShFovClear(void);
extern int ShHeadCached(ShVec3 *out);
extern int ShGetGameState(void);
extern int ShReadableAddr(uint64_t addr, size_t len);
extern void *ShAllocNear(uint64_t target);

/* Row 3 is the translation, rows 0 to 2 the basis: row 0
 * right, row 1 forward, row 2 up. Engine owns the basis.
 */
/* The engine consumes this with no checks of its own, so a
 * NaN or a runaway value crashes it far from here. Refuse
 * the write and keep the engine's frame instead. */
/* Returns 1 when the position was really written. A refused
 * value (NaN, runaway) leaves the engine's own frame alone, and
 * the first person state machine has to be able to tell that no
 * eye was placed on this frame. */
static int WritePos(float *m, float x, float y, float z) {
    if (x != x || y != y || z != z) return 0;
    if (fabsf(x) > 1e6f || fabsf(y) > 1e6f || fabsf(z) > 1e6f)
        return 0;
    m[12] = x;
    m[13] = y;
    m[14] = z;
    m[15] = 1.0f;
    g_writes++;
    return 1;
}

/* Derived from the player and the engine's own basis, never
 * from the slot we write, so nothing can accumulate.
 */
static void ApplyOrbit(float *m) {
    ShVec3 p;

    if (!ShGetPlayerPosition(&p)) return;
    WritePos(m,
             p.x - m[4] * g_back,
             p.y - m[5] * g_back,
             p.z - m[6] * g_back + g_up);
}

/* Game basis: x right, y forward, z up. Rebuilt absolutely
 * from yaw and pitch, so roll is dropped.
 */
static void WriteRot(float *m) {
    float cy = cosf(g_yaw), sy = sinf(g_yaw);
    float cp = cosf(g_pitch), sp = sinf(g_pitch);
    float r[3], f[3], u[3];
    float cr, sr, i;

    r[0] = cy;       r[1] = -sy;      r[2] = 0.0f;
    f[0] = sy * cp;  f[1] = cy * cp;  f[2] = sp;
    u[0] = -sy * sp; u[1] = -cy * sp; u[2] = cp;

    /* Roll turns right and up about the forward axis, so
     * the view tilts without changing where it looks.
     */
    if (g_roll != 0.0f) {
        cr = cosf(g_roll);
        sr = sinf(g_roll);
        for (i = 0; i < 3; i += 1.0f) {
            int k = (int)i;
            float rr = r[k] * cr + u[k] * sr;
            float uu = -r[k] * sr + u[k] * cr;
            r[k] = rr;
            u[k] = uu;
        }
    }

    m[0] = r[0]; m[1] = r[1]; m[2] = r[2];
    m[4] = f[0]; m[5] = f[1]; m[6] = f[2];
    m[8] = u[0]; m[9] = u[1]; m[10] = u[2];
}

/* Iron sight ADS pulls the engine's hidden aim camera in
 * next to the head. Free roam keeps it metres back, which
 * is what separates the two cases below. */
#define CAM_ADS_NEAR 1.0f
#define CAM_ADS_FAR  1.7f

/* Iron sights run their ray through the eye. Over the
 * shoulder aim keeps it a fist or more to the right, and
 * that one should feel like hip fire, so it stays put. */
#define CAM_ADS_PMIN 0.12f
#define CAM_ADS_PMAX 0.22f

/* Firejumper93's gates. An engine camera beyond a chase
 * arm of the head is a drone or a remote view, and a
 * zoomed one is a scope drawing its own overlay. */
#define CAM_ARM_MAX  4.0f
#define CAM_ZOOM_FOV 0.30f
/* Past this the head reading cannot be real, see ApplyHead. */
#define CAM_SANE_MAX 100.0f

/* Kept for diagnostics: the last time the eye was bowed out to. */
static volatile uint64_t g_headBowAt = 0;

/* Where the eye actually went and how far the head was, for the
 * diagnostics in ShCameraEyeAt. */
static ShVec3 g_diagEye;
static float g_diagD = -1.0f;

/* Why the eye was not placed, for diagnostics: 0 it was, 1 there was
 * no head to read, 2 the camera is a scope, 3 it is beyond a chase
 * arm. Read with ShCameraHeadBow. */
static volatile int g_headBow = 0;

/* The eye placed on the last frame that had one, so a rig that keeps
 * flickering in and out of being readable holds the view where it was
 * instead of snapping it between the head and the body every other
 * frame - which reads as the view fighting for control.
 */
static ShVec3 g_lastEye;
static uint64_t g_lastEyeAt = 0;
/* When the real head bone was last read. Holding the eye has to be
 * timed from this and not from g_lastEyeAt: the eye we place
 * ourselves refreshes that one every frame, so a hold timed from it
 * never expires and the view stays nailed in place while the body
 * walks away from it. */
static uint64_t g_lastHeadAt = 0;
/* ... and where that head was. A head that has stopped refreshing is
 * still a head, and it is worth far more than the body position some
 * of these frames hand back, which is nowhere near the player at all
 * - a body swap puts it next to the origin, and an eye placed there
 * throws the view clean out of the world. */

#define EYE_HOLD_MS 500u

/* Metres the eye may travel in one frame before it is eased onto
 * rather than snapped to. Two sources feed this eye and they can
 * disagree for a frame or two - the rig becoming readable again, a
 * menu handing the world back - and a view that jumps metres between
 * two frames reads as the camera fighting for control. Past
 * EYE_JUMP_MAX it is a teleport, and easing that would fly the camera
 * across the map, so it is applied as it is.
 */
#define EYE_SLEW_MAX 0.35f
#define EYE_JUMP_MAX 8.0f

/* Largest move asked for in the last second, for diagnostics. */
static float g_eyeJump = 0.0f;
static uint64_t g_eyeJumpAt = 0;

/* Two ways the view can end up not being ours even while we place an
 * eye every frame, which is what a flicker between first and third
 * person looks like from the outside. Both are counted for
 * ShCameraEyeDiag:
 *   over  - the pose left on a camera is not the pose that camera
 *           carries the next time it comes round, so something wrote
 *           over it after we did;
 *   swaps - the camera object itself changed, so two of them are
 *           taking turns and only one of them ever gets our eye.
 */
static uint64_t g_eyeCam = 0;
static float g_eyeSet[3];
static int g_eyeSetOk = 0;
static int g_eyeOver = 0;
static int g_camSwaps = 0;

/* Last frame we still owned the eye. Handing it back for iron sights
 * is not a change of view, so the state stays first person for a
 * moment after it - see ShCameraViewMode. */
static volatile uint64_t g_headHeldAt = 0;
/* Only long enough to cover the hand over itself. Held any longer it
 * keeps the head hidden while the engine draws an over the shoulder
 * aim, which is a view with the body in it. */
#define FP_HANDOVER_MS 250u

/* Vehicles run longer chase arms, so the head pump feeds
 * this hint on its own slow cadence. The frame path must
 * stay call free: player lookups here crashed the menu. */
static volatile int g_vehHint = 0;

void ShCameraVehicleHint(int inVehicle) {
    g_vehHint = inVehicle;
}

/* First person. The eye is the head bone, nudged forward
 * along the engine's own view axis to clear the face.
 */
static void ApplyHead(float *m, float fov) {
    ShVec3 h;
    float fx = m[4], fy = m[5], fz = m[6];
    float ex = m[12], ey = m[13], ez = m[14];
    float gx = fx, gy = fy, gl, len;
    float px, py, pz, d;
    int fromBody = 0;
    int holdEye = 0;

    if (!ShHeadCached(&h)) {
        /* Nothing to read: a respawn, a body swap, a rig kept hidden
         * inside a vehicle. Writing nothing at all leaves the frame
         * on the engine's own camera, and that is third person - for
         * as long as the rig is missing, which is seconds. The body
         * stands in for the head instead: ShGetPlayerPosition is
         * already about head height, and an eye a little off is far
         * better than a view that drops out of first person. */
        if (g_lastEyeAt && g_lastHeadAt &&
            GetTickCount64() - g_lastHeadAt < EYE_HOLD_MS) {
            /* Hold the last eye for the moment the rig is missing - a
             * respawn, a body swap, a rig kept hidden inside a
             * vehicle. Going anywhere else for those few frames is far
             * more visible than an eye that is a little stale. */
            h = g_lastEye;
            fromBody = 1;
            holdEye = 1;
        } else {
            /* Nothing sound to go on, and guessing is exactly what
             * threw the view around: a body position handed back in
             * the middle of a body swap can be kilometres from the
             * player, and a head that stopped refreshing may be a head
             * from somewhere else entirely. Writing nothing hands the
             * frame back to the engine for a moment, which is at least
             * honest, and the eye returns the instant the head can be
             * read again. */
            g_headBow = 1;
            return;
        }
    } else {
        g_lastHeadAt = GetTickCount64();
    }

    /* Only while the world is being played. A pause menu, the map and
     * the loadout keep rendering world frames behind them, and those
     * frames have a camera of their own: placing the eye on them sets
     * the two against each other and the view drifts about instead of
     * showing either one. Hand them back and take the eye again the
     * moment play resumes. */
    {
        int s = ShGetGameState();
        if (s != SH_STATE_INGAME && s != SH_STATE_UNKNOWN) {
            g_headBow = 6;
            return;
        }
    }

    /* A zoomed camera is a scope. The mask and reticle
     * anchor to the engine's own view, so moving the eye
     * displaces them. Skip and let it render. */
    if (fov < CAM_ZOOM_FOV) { g_headBow = 2; return; }

    d = sqrtf((ex - h.x) * (ex - h.x) + (ey - h.y) * (ey - h.y)
              + (ez - h.z) * (ez - h.z));

    /* A head hundreds of metres from the camera is not a head. It is
     * a rig that was freed and recycled, or a body standing in for a
     * player that is not there any more, and placing the eye on it
     * throws the view into the sky. Leave the frame to the engine
     * rather than chase a reading that makes no sense. */
    if (d > CAM_SANE_MAX) { g_headBow = 5; return; }

    /* On foot, an engine camera beyond a chase arm is not
     * looking through the soldier: a drone, a cutscene, a
     * tacmap. Vehicles keep longer arms, so they pass.
     *
     * Bowing out is only meant to cover the moment the engine
     * takes the view. Kept up for longer it locks itself in: a
     * menu hands the world back with the camera still out there,
     * we bow out, so it stays out there, the eye is never placed,
     * and the view sits in third person until something else
     * happens to move it. Past a short grace it is written anyway.
     */
    if (d > CAM_ARM_MAX && !g_vehHint) {
        int s = ShGetGameState();
        /* A camera out there only means the engine took the view if
         * the state says so. On its own the distance proves nothing:
         * the engine recomputes its own third person pose every single
         * frame whatever we write, so it is always out there, and
         * bowing out for it means never placing the eye again - which
         * is a view that flickers between first and third person
         * instead of settling. A drone, a cinematic and the binoculars
         * do own the view, and so does a camera that is not the
         * player's own; those still get it. */
        if (s == SH_STATE_DRONE || s == SH_STATE_BINOCULAR ||
            s == SH_STATE_CINEMATIC) {
            g_headBow = 3;
            return;
        }
    }
    g_headBowAt = 0;
    g_headBow = fromBody ? 4 : 0;

    /* Flattened: nudging along a downward view would drop
     * the eye to the chest.
     */
    gl = sqrtf(gx * gx + gy * gy);
    if (gl > 0.01f) { gx /= gl; gy /= gl; }
    else { gx = 0.0f; gy = 1.0f; }
    if (holdEye) {
        /* What is being held is the eye itself, not a place to
         * measure from. Applying the offsets again would add them
         * again every frame, walking the view forward and upward for
         * as long as the head stays unreadable. */
        px = h.x;
        py = h.y;
        pz = h.z;
    } else {
        px = h.x + gx * g_back;
        py = h.y + gy * g_back;
        pz = h.z + g_up;
    }

    /* ADS only: ease the eye the few cm onto the engine's
     * aim ray, at head depth, so the sights and the
     * bullets pass through screen center. */
    len = sqrtf(fx * fx + fy * fy + fz * fz);
    if (!holdEye && d < CAM_ADS_FAR && len > 0.01f) {
        float t, w, perp;

        fx /= len; fy /= len; fz /= len;
        t = (h.x - ex) * fx + (h.y - ey) * fy + (h.z - ez) * fz;

        /* How far the aim ray misses the head. Small is a
         * sight line, large is the shoulder camera.
         */
        perp = d * d - t * t;
        perp = (perp > 0.0f) ? sqrtf(perp) : 0.0f;

        if (perp < CAM_ADS_PMAX) {
            w = (CAM_ADS_FAR - d) / (CAM_ADS_FAR - CAM_ADS_NEAR);
            if (w > 1.0f) w = 1.0f;
            if (perp > CAM_ADS_PMIN)
                w *= (CAM_ADS_PMAX - perp)
                   / (CAM_ADS_PMAX - CAM_ADS_PMIN);
            t += g_back;
            px += (ex + fx * t - px) * w;
            py += (ey + fy * t - py) * w;
            pz += (ez + fz * t - pz) * w;
        }
    }
    /* Only a frame that really placed the eye counts. A refused
     * value (or one of the bows out above) leaves the engine's own
     * camera on screen, and the state machine has to see that. */
    if (g_lastEyeAt) {
        float dx = px - g_lastEye.x;
        float dy = py - g_lastEye.y;
        float dz = pz - g_lastEye.z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        uint64_t now = GetTickCount64();

        if (dist > EYE_SLEW_MAX && dist < EYE_JUMP_MAX) {
            float k = EYE_SLEW_MAX / dist;
            px = g_lastEye.x + dx * k;
            py = g_lastEye.y + dy * k;
            pz = g_lastEye.z + dz * k;
        }
        if (now - g_eyeJumpAt > 1000) {
            g_eyeJump = 0.0f;
            g_eyeJumpAt = now;
        }
        if (dist > g_eyeJump) g_eyeJump = dist;
    }
    if (WritePos(m, px, py, pz)) {
        g_headWroteAt = GetTickCount64();
        g_diagEye.x = px;
        g_diagEye.y = py;
        g_diagEye.z = pz;
        g_diagD = d;
        g_lastEye.x = px;
        g_lastEye.y = py;
        g_lastEye.z = pz;
        g_lastEyeAt = g_headWroteAt;
    }
}

/* Each field is written only if its bit is set, so the
 * engine keeps ownership of everything else.
 */
static void ApplyPose(float *m, float fov) {
    if (g_apply & SH_CAM_ROT) WriteRot(m);
    if (g_apply & CAM_HEAD_BIT) ApplyHead(m, fov);
    else if (g_apply & CAM_ORBIT_BIT) ApplyOrbit(m);
    else if (g_apply & SH_CAM_POS)
        WritePos(m, g_absPos.x, g_absPos.y, g_absPos.z);
}

/* Skew and mode belong to the render camera, so they stay
 * on the camera build. Position, rotation and fov are all
 * taken further up, at their own source. */
static void ApplyFields(uint64_t cam) {
    if (g_apply & SH_CAM_SKEW) {
        *(float *)(uintptr_t)(cam + CAM_SKEWX) = g_skewX;
        *(float *)(uintptr_t)(cam + CAM_SKEWY) = g_skewY;
    }
    if (g_apply & SH_CAM_MODE)
        *(int *)(uintptr_t)(cam + CAM_MODE) = g_modeSet;
}

/* Runs on the engine's own thread, immediately before the
 * transform is consumed, so there is no race to lose.
 */
static void __attribute__((ms_abi)) CamCallback(uint64_t rcx) {
    const float *f;
    int mode, ui;

    if (!rcx || !ShReadableAddr(rcx, CAM_FOV + 4)) return;
    mode = *(const int *)(uintptr_t)(rcx + CAM_MODE);
    if (mode != 0) {
        g_otherMode = mode;
        g_otherAt = g_calls;
        return;
    }

    g_cam = rcx;
    g_calls++;
    /* When we last owned the eye, for the hand over grace in
     * ShCameraViewMode. */
    if (g_apply & CAM_HEAD_BIT) g_headHeldAt = GetTickCount64();

    f = (const float *)(uintptr_t)(rcx + CAM_POSE);
    /* Did the eye we left on this camera survive to this frame? */
    if (g_eyeSetOk && g_eyeCam == rcx &&
        (fabsf(f[12] - g_eyeSet[0]) > 0.01f ||
         fabsf(f[13] - g_eyeSet[1]) > 0.01f ||
         fabsf(f[14] - g_eyeSet[2]) > 0.01f))
        g_eyeOver++;
    if (g_cam && rcx != g_cam) g_camSwaps++;
    ui = f[0] == 1.0f && f[1] == 0.0f && f[2] == 0.0f &&
         f[4] == 0.0f && f[5] == 1.0f && f[6] == 0.0f &&
         f[8] == 0.0f && f[9] == 0.0f && f[10] == 1.0f;
    if (ui) g_uiAt = g_calls;

    /* The engine stamps visibility back here, so a held
     * override is reapplied in the same window.
     */
    ShVisibilityPump();
    ShTransformPump();
    ShDominoPump();

    /* The menu camera never takes the head, so its frames
     * do no head work at all. The pump keeps its state and
     * resumes on the first world frame. */
    if (!ui) {
        /* Keep the head live while we place the eye AND while
         * somebody is asking for the view state: telling "we own
         * the camera" from "the engine took it" needs the engine's
         * own camera measured against the head on exactly the
         * frames we are not writing. The pump is a few direct reads
         * once resolved and a failed resolve is rate limited, so an
         * idle view state costs nothing. */
        if ((g_apply & CAM_HEAD_BIT) ||
            GetTickCount64() < g_viewWantAt)
            ShHeadWant();
        ShHeadPump();

        /* Whether the first person eye is really on camera,
         * measured from the frame's own camera rather than
         * from our write: a view the engine took (stowed
         * weapon, parachute, drone) sits away from the head
         * even on the frames our write path never ran.
         * Measured unconditionally - gated on our own bit this
         * could never refresh after we let go, which is the only
         * moment case 2 of ShCameraViewMode can be decided. */
        {
            ShVec3 h;
            float dx, dy, dz;
            if (ShHeadCached(&h)) {
                dx = f[12] - h.x;
                dy = f[13] - h.y;
                dz = f[14] - h.z;
                if (dx * dx + dy * dy + dz * dz <=
                    HEAD_NEAR_DIST * HEAD_NEAR_DIST)
                    g_headNearAt = GetTickCount64();
            }
        }
        if (g_apply) ApplyFields(rcx);
    }
    /* Remember the eye placed here so the next visit can tell whether
     * it survived. */
    if ((g_apply & CAM_HEAD_BIT) && g_headBow != 1 && g_headBow != 2 &&
        g_headBow != 3) {
        g_eyeCam = rcx;
        g_eyeSet[0] = f[12];
        g_eyeSet[1] = f[13];
        g_eyeSet[2] = f[14];
        g_eyeSetOk = 1;
    }
}

/* The manager's own transform, before any consumer reads
 * it. Rows match CAM_POSE: 0 right, 1 forward, 2 up, 3 the
 * translation. RAX holds the manager at the patch site. */
static void __attribute__((ms_abi)) MgrCallback(uint64_t cm) {
    float *m, *p;

    if (!cm || !g_apply) return;
    if (!ShReadableAddr(cm + MGR_XFORM, 0x40)) return;
    /* Same identity basis test ShInPauseMenu reports on. */
    if (g_uiAt != 0 && g_calls - g_uiAt <= 4) return;

    m = (float *)(uintptr_t)(cm + MGR_XFORM);
    p = (float *)(uintptr_t)(cm + MGR_POS);

    ApplyPose(m, *(const float *)(uintptr_t)(cm + MGR_FOV));

    /* The engine fills the position vector from a second
     * call, so the row above is restated here rather than
     * left to disagree with it. */
    p[0] = m[12];
    p[1] = m[13];
    p[2] = m[14];
    p[3] = 0.0f;
    g_writes++;
}

/* The site keeps its call opcode, so the stub is entered
 * with the return address already pushed and tail jumps to
 * the original target, which returns past the site. */
/* The site's frame sits 8 off a 16 byte boundary, which
 * faults the callback's movaps spills. Align through rbp
 * rather than assume, then restore the exact rsp. */
static int BuildMgrStub(void) {
    uint8_t *s = (uint8_t *)ShAllocNear(MGR_SITE);
    int64_t rel;
    int o = 0;

    if (!s) return 0;
    memset(s, 0xCC, 0x1000);

    s[o++] = 0x55;
    s[o++] = 0x48; s[o++] = 0x89; s[o++] = 0xE5;
    s[o++] = 0x48; s[o++] = 0x83; s[o++] = 0xE4; s[o++] = 0xF0;
    s[o++] = 0x48; s[o++] = 0x83; s[o++] = 0xEC; s[o++] = 0x20;
    s[o++] = 0x48; s[o++] = 0x89; s[o++] = 0xC1;
    s[o++] = 0x48; s[o++] = 0xB8;
    *(uint64_t *)(s + o) = (uint64_t)(uintptr_t)MgrCallback;
    o += 8;
    s[o++] = 0xFF; s[o++] = 0xD0;
    s[o++] = 0x48; s[o++] = 0x89; s[o++] = 0xEC;
    s[o++] = 0x5D;

    rel = (int64_t)MGR_NEXT - ((int64_t)(uintptr_t)(s + o) + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) return 0;
    s[o++] = 0xE9;
    *(int32_t *)(s + o) = (int32_t)rel;

    g_mgrStub = s;
    return 1;
}

/* Refuse anything but the call we decoded, so a build we do
 * not know keeps its own camera. */
static int MgrInstall(void) {
    uint8_t *at = (uint8_t *)(uintptr_t)MGR_SITE;
    int64_t rel;
    DWORD old;

    if (g_mgrHooked) return 1;
    if (!ShReadableAddr(MGR_SITE, MGR_LEN)) return 0;
    if (at[0] != 0xE8) return 0;
    if ((uint64_t)((int64_t)MGR_SITE + MGR_LEN
                   + *(int32_t *)(at + 1)) != MGR_NEXT)
        return 0;
    if (!BuildMgrStub()) return 0;

    rel = (int64_t)(uintptr_t)g_mgrStub - ((int64_t)MGR_SITE + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) return 0;
    if (!VirtualProtect(at, MGR_LEN, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    *(int32_t *)(at + 1) = (int32_t)rel;
    VirtualProtect(at, MGR_LEN, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, MGR_LEN);
    g_mgrHooked = 1;
    return 1;
}

static int BuildStub(void) {
    static const uint8_t SAVE[] = {
        0x48,0x81,0xEC,0xC8,0x00,0x00,0x00,
        0x48,0x89,0x4C,0x24,0x20,
        0x48,0x89,0x54,0x24,0x28,
        0x4C,0x89,0x44,0x24,0x30,
        0x4C,0x89,0x4C,0x24,0x38,
        0x4C,0x89,0x54,0x24,0x40,
        0x4C,0x89,0x5C,0x24,0x48,
        0x48,0x89,0x44,0x24,0x50,
        0x0F,0x11,0x44,0x24,0x60,
        0x0F,0x11,0x4C,0x24,0x70,
        0x0F,0x11,0x94,0x24,0x80,0x00,0x00,0x00,
        0x0F,0x11,0x9C,0x24,0x90,0x00,0x00,0x00,
        0x0F,0x11,0xA4,0x24,0xA0,0x00,0x00,0x00,
        0x0F,0x11,0xAC,0x24,0xB0,0x00,0x00,0x00,
        0x48,0x8B,0x4C,0x24,0x20
    };
    static const uint8_t REST[] = {
        0x0F,0x10,0xAC,0x24,0xB0,0x00,0x00,0x00,
        0x0F,0x10,0xA4,0x24,0xA0,0x00,0x00,0x00,
        0x0F,0x10,0x9C,0x24,0x90,0x00,0x00,0x00,
        0x0F,0x10,0x94,0x24,0x80,0x00,0x00,0x00,
        0x0F,0x10,0x4C,0x24,0x70,
        0x0F,0x10,0x44,0x24,0x60,
        0x48,0x8B,0x44,0x24,0x50,
        0x4C,0x8B,0x5C,0x24,0x48,
        0x4C,0x8B,0x54,0x24,0x40,
        0x4C,0x8B,0x4C,0x24,0x38,
        0x4C,0x8B,0x44,0x24,0x30,
        0x48,0x8B,0x54,0x24,0x28,
        0x48,0x8B,0x4C,0x24,0x20,
        0x48,0x81,0xC4,0xC8,0x00,0x00,0x00
    };
    uint8_t *s = (uint8_t *)ShAllocNear(CAM_THUNK);
    int o = 0;

    if (!s) return 0;
    memset(s, 0xCC, 0x1000);

    memcpy(s + o, SAVE, sizeof(SAVE)); o += (int)sizeof(SAVE);
    s[o++] = 0x48; s[o++] = 0xB8;
    *(uint64_t *)(s + o) = (uint64_t)(uintptr_t)CamCallback; o += 8;
    s[o++] = 0xFF; s[o++] = 0xD0;
    memcpy(s + o, REST, sizeof(REST)); o += (int)sizeof(REST);
    s[o++] = 0x48; s[o++] = 0xB8;
    *(uint64_t *)(s + o) = CAM_IMPL; o += 8;
    s[o++] = 0xFF; s[o++] = 0xE0;

    g_camStub = s;
    return 1;
}

/* A 5 byte jmp in a 16 byte slot, so the hook is a rel32
 * rewrite with nothing displaced or relocated.
 */
static int PatchThunk(void) {
    uint8_t *t = (uint8_t *)(uintptr_t)CAM_THUNK;
    int64_t rel;
    DWORD old;

    if (!g_camStub) return 0;
    rel = (int64_t)(uintptr_t)g_camStub - ((int64_t)CAM_THUNK + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) return 0;
    if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(g_thunkOrig, t, 5);
    *(int32_t *)(t + 1) = (int32_t)rel;
    VirtualProtect(t, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 5);
    return 1;
}

SH_API int ShCameraHookInstall(void) {
    uint8_t *t = (uint8_t *)(uintptr_t)CAM_THUNK;
    int64_t cur, rel;
    DWORD old;

    if (g_camStub) return 1;
    if (!ShReadableAddr(CAM_THUNK, 5)) {
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    if (t[0] != 0xE9) {
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    cur = (int64_t)CAM_THUNK + 5 + *(int32_t *)(t + 1);
    if ((uint64_t)cur != CAM_IMPL) {
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    if (!BuildStub()) {
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }

    if (!PatchThunk()) {
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    if (!MgrInstall()) {
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    (void)rel;
    (void)old;
    ShSetError(SH_OK);
    return 1;
}

/* The patch is code, so it survives a level change. Verify
 * rather than assume, and re-arm if anything restored it.
 */
static int ThunkPointsAtStub(void) {
    const uint8_t *t = (const uint8_t *)(uintptr_t)CAM_THUNK;
    int64_t tgt;

    if (!g_camStub) return 0;
    if (!ShReadableAddr(CAM_THUNK, 5) || t[0] != 0xE9) return 0;
    tgt = (int64_t)CAM_THUNK + 5 + *(const int32_t *)(t + 1);
    return (uint64_t)tgt == (uint64_t)(uintptr_t)g_camStub;
}

/* Called on the transition into Playing, so the camera is
 * available to plugins without anyone asking for it.
 */
void ShCameraOnEnterPlaying(void) {
    g_cam = 0;
    if (!g_camStub) {
        ShCameraHookInstall();
        return;
    }
    if (!ThunkPointsAtStub()) PatchThunk();
}

SH_API int ShCameraReady(void) {
    return g_camStub != NULL && g_cam != 0;
}

/* Two ways the view is first person, and the second one only
 * exists because handing the camera back for iron sights must not
 * flash the head back on:
 *
 *  1. we are placing the eye every frame: CAM_HEAD_BIT is ours and
 *     ApplyHead did write on a recent frame. Where that eye sits is
 *     irrelevant - a preset or a seat anchor offsetting it from the
 *     head bone is still the first person view, and calling those
 *     third person is what flashed the head on in vehicles.
 *
 *  2. we have let go and the engine's own camera is the one on
 *     screen, sitting on the head bone: the aim camera of an iron
 *     sight. Third person, drones, cutscenes and every pulled back
 *     view are metres away, so they fail this.
 *
 * Everything else is third person. Reading this keeps the head
 * measurement alive for the next VIEW_WANT_MS, which is what makes
 * case 2 measurable at all.
 */
SH_API int ShCameraViewMode(void) {
    uint64_t now = GetTickCount64();

    g_viewWantAt = now + VIEW_WANT_MS;
    if ((g_apply & CAM_HEAD_BIT) && g_headWroteAt &&
        now - g_headWroteAt < FP_LIVE_MS)
        return SH_VIEW_FIRST_PERSON;
    if (g_headNearAt && now - g_headNearAt < FP_LIVE_MS)
        return SH_VIEW_FIRST_PERSON;
    /* Handed back on purpose - iron sights - and only for the world:
     * the engine's aim camera is drawing but the view is still the
     * first person one, and flashing the head back on for the length
     * of an aim reads as a bug. This is only for a camera we let go
     * of: while we still hold the bit a bowed out frame (a stowed
     * weapon, a parachute) really is another view. A drone or a
     * cinematic is a change of view too, not a hand over, so those
     * are excluded by the state. */
    if (!(g_apply & CAM_HEAD_BIT) && g_headHeldAt &&
        now - g_headHeldAt < FP_HANDOVER_MS &&
        ShGetGameState() == SH_STATE_INGAME)
        return SH_VIEW_FIRST_PERSON;
    if (!g_headWroteAt && !g_headNearAt) return SH_VIEW_UNKNOWN;
    return SH_VIEW_THIRD_PERSON;
}

SH_API int ShCameraFirstPersonActive(void) {
    return ShCameraViewMode() == SH_VIEW_FIRST_PERSON;
}

/* Diagnostics: why the eye was not placed. 0 it was, 1 no head,
 * 2 scope, 3 beyond a chase arm.
 */
SH_API int ShCameraHeadBow(void) {
    return g_headBow;
}

/** Diagnostics: the largest distance the eye was asked to move in one
 *  frame over the last second, in metres. A view that flickers shows
 *  up here as a jump every frame.
 */
SH_API float ShCameraEyeJump(void) {
    return g_eyeJump;
}

/* Where the last eye ended up and how far that head was from the
 * camera, so a view thrown into the sky can be traced to a reading. */
SH_API void ShCameraEyeAt(float pos[3], float *dist) {
    if (pos) { pos[0] = g_diagEye.x; pos[1] = g_diagEye.y; pos[2] = g_diagEye.z; }
    if (dist) *dist = g_diagD;
}

/* Counts since the last call, see the note on g_eyeOver. */
SH_API void ShCameraEyeDiag(int *overwritten, int *swaps) {
    if (overwritten) *overwritten = g_eyeOver;
    if (swaps) *swaps = g_camSwaps;
    g_eyeOver = 0;
    g_camSwaps = 0;
}

/* Drop the hand over grace. Turning first person off is not handing
 * the camera to the engine for an aim, so the view is third person
 * from that moment on. */
SH_API void ShCameraHandoverClear(void) {
    g_headHeldAt = 0;
}

SH_API uint64_t ShCameraCalls(void) { return g_calls; }
SH_API uint64_t ShCameraWrites(void) { return g_writes; }

/* The last nonzero camera mode seen, and how many mode 0
 * frames ago, so a REPL probe can name the special views.
 */
SH_API int ShCameraOtherMode(uint64_t *framesAgo) {
    if (framesAgo) *framesAgo = g_calls - g_otherAt;
    return g_otherMode;
}

/** True while the pause menu's camera is rendering. */
SH_API int ShInPauseMenu(void) {
    return g_uiAt != 0 && g_calls - g_uiAt <= 4;
}

SH_API int ShGetCamera(ShCamera *out) {
    uint64_t cam = g_cam;
    const float *m;

    if (!out) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (!cam || !ShReadableAddr(cam, CAM_FOV + 4)) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    m = (const float *)(uintptr_t)(cam + CAM_POSE);
    memset(out, 0, sizeof(*out));
    out->camera = cam;
    out->right.x = m[0];   out->right.y = m[1];   out->right.z = m[2];
    out->forward.x = m[4]; out->forward.y = m[5]; out->forward.z = m[6];
    out->up.x = m[8];      out->up.y = m[9];      out->up.z = m[10];
    out->pos.x = m[12];    out->pos.y = m[13];    out->pos.z = m[14];
    out->fov = *(const float *)(uintptr_t)(cam + CAM_FOV);
    out->mode = *(const int *)(uintptr_t)(cam + CAM_MODE);
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShSetCamera(const ShVec3 *pos) {
    if (!pos) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (!ShCameraHookInstall()) return 0;
    g_absPos = *pos;
    g_apply = (g_apply & ~CAM_DERIVED) | SH_CAM_POS;
    ShSetError(SH_OK);
    return 1;
}

/* Metres behind the player along the camera's forward axis,
 * and height above. It follows the player because the
 * engine still owns the basis and the position. */
SH_API int ShCameraOrbit(float back, float up) {
    if (!ShCameraHookInstall()) return 0;
    g_back = back;
    g_up = up;
    g_apply = (g_apply & ~CAM_HEAD_BIT) | SH_CAM_POS | CAM_ORBIT_BIT;
    ShSetError(SH_OK);
    return 1;
}

/* First person: the eye tracks the head bone every frame
 * and eases onto the aim ray during ADS, so sights stay
 * centered. forward clears the face. */
SH_API int ShCameraFirstPerson(float forward, float up) {
    if (!ShCameraHookInstall()) return 0;
    g_back = forward;
    g_up = up;
    g_apply = (g_apply & ~CAM_ORBIT_BIT) | SH_CAM_POS | CAM_HEAD_BIT;
    ShSetError(SH_OK);
    return 1;
}

/* Position and aim both ours. Angles are radians: yaw 0
 * faces +y, pitch is positive looking up.
 */
SH_API int ShCameraFree(const ShVec3 *pos, float yaw, float pitch) {
    if (!pos) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (!ShCameraHookInstall()) return 0;
    if (pitch > 1.55f) pitch = 1.55f;
    if (pitch < -1.55f) pitch = -1.55f;
    g_absPos = *pos;
    g_yaw = yaw;
    g_pitch = pitch;
    g_apply = (g_apply & ~CAM_DERIVED) | SH_CAM_POS | SH_CAM_ROT;
    ShSetError(SH_OK);
    return 1;
}

/* Where the engine's camera is aiming right now, so a free
 * camera can start from the current view.
 */
SH_API int ShCameraAngles(float *yaw, float *pitch) {
    ShCamera c;

    if (!ShGetCamera(&c)) return 0;
    if (yaw) *yaw = atan2f(c.forward.x, c.forward.y);
    if (pitch) {
        float s = c.forward.z;
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        *pitch = asinf(s);
    }
    return 1;
}

SH_API int ShCameraApply(const ShCameraOverride *o) {
    if (!o || !o->apply) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (!ShCameraHookInstall()) return 0;

    if (o->apply & SH_CAM_POS) g_absPos = o->pos;
    if (o->apply & SH_CAM_ROT) {
        float p = o->pitch;
        if (p > 1.55f) p = 1.55f;
        if (p < -1.55f) p = -1.55f;
        g_yaw = o->yaw;
        g_pitch = p;
        g_roll = o->roll;
    }
    if (o->apply & SH_CAM_FOV) {
        g_fov = o->fov;
        if (!ShFovSet(o->fov)) {
            ShSetError(SH_ERR_NO_CANDIDATE);
            return 0;
        }
    }
    if (o->apply & SH_CAM_SKEW) {
        g_skewX = o->skewX;
        g_skewY = o->skewY;
    }
    if (o->apply & SH_CAM_MODE) g_modeSet = o->mode;

    /* Merged, so applying fov leaves another plugin's
     * position and rotation alone.
     */
    if (o->apply & SH_CAM_POS) g_apply &= ~CAM_DERIVED;
    g_apply |= o->apply;
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShCameraMatrix(int index, float *out16) {
    uint64_t cam = g_cam;
    uint64_t at;

    if (!out16 || index < 0 || index >= CAM_MAT_N) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    if (!cam) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    at = cam + CAM_MATS + (uint64_t)index * 0x40;
    if (!ShReadableAddr(at, 0x40)) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    memcpy(out16, (const void *)(uintptr_t)at, 0x40);
    ShSetError(SH_OK);
    return 1;
}

SH_API void ShCameraRelease(void) {
    g_apply = 0;
    ShFovClear();
}

/* Give back only what you took, so releasing a free camera
 * leaves another plugin's fov override running.
 */
SH_API void ShCameraReleaseFields(uint32_t fields) {
    if (fields & SH_CAM_POS) fields |= CAM_DERIVED;
    if (fields & SH_CAM_FOV) ShFovClear();
    g_apply &= ~fields;
}

/** Which fields are currently overridden. */
SH_API uint32_t ShCameraOwned(void) {
    return g_apply & 0xFFu;
}
