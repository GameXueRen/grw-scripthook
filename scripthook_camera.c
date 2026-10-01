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
#include "log.h"

/* The projection selector. It takes the camera in RCX and
 * runs every frame, which is what makes it hookable.
 */
#define CAM_THUNK   SH_IMG(0x13796D0)
#define CAM_IMPL    SH_IMG(0xD67FFA0)

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
#define MGR_SITE    SH_IMG(0x81E0B7E)
#define MGR_LEN     5
#define MGR_NEXT    SH_IMG(0x10D8E20)

/* The instruction one step before that call: the engine storing
 * the position it has just built, movaps [rax+170],xmm2, seven
 * bytes, then the call this file hooks. It is the second way in
 * and it is the community table's - Wildlands First Person,
 * Last Rites RC1, decoded 2026-09-29. That table patches this
 * store, writes the position and needs nothing else of the
 * manager: no +170, no +190, no fov to read. So it is the way
 * back in for a build where those offsets drift, at the cost of
 * giving up the transform and the fov this file also writes.
 *
 * Checked every session, never patched - see MgrStoreCheck. It
 * is here so that the question "is the other way in still on
 * this build?" is answered by a log line rather than by a
 * session spent reading bytes, which is what the 2026-09-27
 * update cost.
 */
#define MGR_STORE     SH_IMG(0x81E0B77)
#define MGR_STORE_LEN 7
static const uint8_t MGR_STORE_SIG[MGR_STORE_LEN] = {
    0x0F, 0x29, 0x90, 0x70, 0x01, 0x00, 0x00
};

/* Verified live in gameplay: the mode at +0x6C reads 3, so
 * consumers take the position from +0x170 while the render
 * camera takes the rows at +0x190. Both are restated. */
#define MGR_MODE    0x6C
#define MGR_POS     0x170
#define MGR_FOV     0x180
#define MGR_XFORM   0x190

/* MEASURE 2026-09-30: MGR_ALT (+0x1E0) and ShCameraAltEye, the probe that
 * restated the eye into that third position the engine keeps in the same
 * manager, went with the rest of the measurement. What it established is in
 * docs/firstperson-aim-flash-and-offset.md: the copy sits about 0.39 m from
 * the eye, off along one axis, writing it changes nothing on screen, and it
 * is therefore not what the weapon or optic alignment reads.
 */

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
 * - and the first person path lets it go rather than
 * fighting. A consumer reads this to know whether the hidden
 * head is on camera or the player is in a view that should
 * show it.
 *
 * Rather than trusting our own writes, this is measured from
 * the camera the engine is actually rendering: when it sits
 * on the player's head the view is first person, when it is
 * pulled back it is the engine's own shoulder or cutscene
 * camera. That stays true even on frames the first person
 * path never writes (a stowed weapon, a parachute), which is
 * exactly when the head must come back. */
static volatile uint64_t g_headNearAt = 0; /* last frame cam was on the head */
#define HEAD_NEAR_DIST 1.0f   /* metres: first person eye to head bone */
#define FP_LIVE_MS     250u   /* how stale "near" may be before we say away */

/* Last frame the first person path really placed the eye. A
 * frame it declines writes nothing at all, so a stamp that
 * stops advancing means the engine owns the view - which is
 * exactly what "not first person" has to mean. */
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
static volatile float g_right = 0.0f;
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
extern void ShHeadPump(int light);
extern void ShHeadWant(void);
extern int ShFovSet(float radians);
extern void ShFovClear(void);
extern int ShFovInstall(void);
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
             p.x - m[4] * g_back + m[0] * g_right,
             p.y - m[5] * g_back + m[1] * g_right,
             p.z - m[6] * g_back + m[2] * g_right + g_up);
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
/* Kept as an interface: the head pump still reports the ride
 * state through it. The old placement read the hint to widen
 * its chase arm in vehicles; the engine path needs no such
 * thing, so nothing consumes it here any more. */
void ShCameraVehicleHint(int inVehicle) {
    (void)inVehicle;
}

/* Each field is written only if its bit is set, so the
 * engine keeps ownership of everything else.
 */
static void ApplyPose(float *m, float fov) {
    if (g_apply & SH_CAM_ROT) WriteRot(m);
    /* A frame that reaches here while first person owns the
     * position writes none: the eye is ShFp2PlaceEye's, and a
     * position write from here - SH_CAM_POS with an absolute
     * position nobody filled in, or the orbit arm - would
     * fight it. Rotation above still applies, because that is
     * what a free camera over a first person view needs. */
    if (g_apply & CAM_HEAD_BIT) return;
    if (g_apply & CAM_ORBIT_BIT) ApplyOrbit(m);
    else if (g_apply & SH_CAM_POS)
        WritePos(m, g_absPos.x, g_absPos.y, g_absPos.z);
    (void)fov;
}

/* MEASURE 2026-10-01: the first person near plane write lived here, and it
 * is gone. It held the render near plane at CAM_NEAR_FP 0.02 while first
 * person owned the eye, to keep the engine's third-person plane from slicing
 * the weapon and arms - and it cost what that always costs: with a far plane
 * of kilometres, the depth precision the distance needs. The two policies
 * tried here (a constant hold, then a hold keyed on the eye this file
 * actually wrote) each bought one symptom and paid for it with the other, and
 * the field reports name both: "opening the sights flashes for one frame" and
 * "opening the sights loses the buildings".
 *
 * Three things settled it, and they are why this is a removal rather than
 * another policy:
 *
 *   - it was ported in 6d32050 from the Wildlands Immersion Suite's camera
 *     module, where CAM_NEAR is DECLARED AND NEVER USED. Nothing in that
 *     fork ever wrote a near plane; the transplant activated a dormant
 *     constant and inherited its cost without its evidence.
 *   - nothing else in this ecosystem writes one. The community table (which
 *     patches 14858071 and calls it "close up blur"), GhostHook, the GRW-FP
 *     dxgi mod, Firejumper93's VR mod and the Immersion Suite itself all
 *     address the camera-inside-the-body problem with the blur byte and the
 *     head hide, and none of them touches a projection plane. Five
 *     implementations, one of which sits 2 m forward inside the head, and
 *     not one of them needed this.
 *   - it contradicts this framework's own defaults. The first person
 *     plugin ships engine_extras = 15, so EX_VIS (S_VIS) and EX_WALL
 *     (S_WALL) are ON: both exist to stop the engine hiding the body near
 *     the camera. Holding the near plane in as well is covering for a
 *     condition two of our own patches create.
 *
 * What it was covering, if anything, is the weapon and arms, which no patch
 * of ours hides. That is the one thing to watch when this ships - see the
 * note at ShCameraViewMode, which is where a view that has lost its near
 * plane would show up. It is NOT to be answered by restoring this constant:
 * a build that needs the weapon hidden wants a hide, the way S_VIS does it
 * for the body, not a projection parameter.
 *
 * CAM_NEAR/CAM_FAR/CAM_ASPECT below stay as the map of the camera object;
 * nothing writes them now, here or anywhere. */

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
    /* The near plane is not written here any more, and must not be brought
     * back without reading the note above. */
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
    /* The near plane is not restored here any more: nothing pulls it in, so
     * there is nothing to give back. See the note where FpNearHeld stood. */
    /* When we last owned the eye, for the hand over grace in
     * ShCameraViewMode. */
    if (g_apply & CAM_HEAD_BIT) g_headHeldAt = GetTickCount64();

    f = (const float *)(uintptr_t)(rcx + CAM_POSE);
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
        ShHeadPump(0);

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
    } else {
        /* Menu frames: the head pump must not take its full path here
         * - it would have to look the player up, which is what used to
         * crash menus - but with the rig already resolved the bones can
         * simply be read on. That keeps the head fresh behind the menu,
         * so coming back to the world puts the eye straight back
         * instead of waiting out a resolve. */
        if (g_apply & CAM_HEAD_BIT) {
            ShHeadWant();
            ShHeadPump(1);
        }
    }
    /* First person no longer reads the rig here - the eye is
     * the engine's own answer (ShFp2PlaceEye) - so the old
     * rig-rebuild self healing that served the bone reading
     * went with it. */
}

/* The manager's own transform, before any consumer reads
 * it. Rows match CAM_POSE: 0 right, 1 forward, 2 up, 3 the
 * translation. RAX holds the manager at the patch site.
 *
 * MEASURE 2026-09-29: the return value is the eye this frame placed, or
 * NULL. The stub in BuildMgrStub loads it into xmm2 before the tail
 * jump, so the engine's own downstream - the weapon and optic alignment
 * among it - reads the eye rather than the camera the engine had in
 * xmm2. Bounded by g_altEye, off in a stock build, and live: the next
 * frame is already on the new answer. NULL means "no eye to offer, keep
 * the engine's own xmm2", which is what every path that declines the
 * frame returns.
 */
/* MEASURE 2026-09-29: the eye-to-rig vector, learned on the frames the
 * engine takes and applied on the frames we keep. In the eye's own axes,
 * the same three numbers the player's offset uses. Zero and "not yet
 * learned" until an aim has been handed over once; see the learn branch
 * below and the apply in the tail. */
static volatile float g_aimRig[3];
static volatile int   g_aimRigHave;

/* MEASURE 2026-10-01: the switch, OFF by default.
 *
 * The walk onto the aim rig is the only thing in this module that moves the
 * eye somewhere other than the eye, and it is also the only thing that can be
 * felt: with it on, the camera sits 14-22 cm toward the engine's weapon seat
 * for the whole aim, which is what centres the sights - and with it off the
 * camera stays on the eye and the sights carry a small FIXED offset instead.
 *
 * The field's own report decided the default. The 2026-10-01 session ran the
 * walk while its slot was doubling (see the note at the assignment below), and
 * what the player saw was a silencer sliding back into place and a scope
 * flashing on the right - a moving offset, which reads far worse than a still
 * one. The session before it ran with the walk disabled by accident and was
 * reported as good. A fixed offset is the better failure mode, so the walk
 * ships off and the switch is how it gets judged rather than assumed.
 *
 * Learning still runs with the switch off, and the log still prints what the
 * walk WOULD apply - so the numbers a decision needs stay available without
 * the eye being moved to get them. */
static volatile int g_aimRigOn = 0;

/** 1 when the eye is walked onto the engine's aim seat. Off by default. */
SH_API int ShCameraAimRigOn(void) { return g_aimRigOn ? 1 : 0; }

/** Turn the walk onto the aim seat on (1) or off (0). Read once a frame by
 *  the manager callback, so a change is in force on the next frame. Nothing
 *  is cleared either way: the learned slots survive a toggle, so an A/B in
 *  one session compares the same numbers. */
SH_API int ShCameraAimRigSet(int on) {
    g_aimRigOn = on ? 1 : 0;
    Log("aim rig: walk %s by request (slots kept)", g_aimRigOn ? "ON" : "OFF");
    return 1;
}

static float *__attribute__((ms_abi)) MgrCallback(uint64_t cm) {
    float *m, *p;

    /* MEASURE 2026-10-01: the engine's own position for this frame, taken HERE
     * - the first thing this callback does, before any branch and before the
     * early return below, and before ShFp2PlaceEye can write the eye over it.
     * The manager's transform and position vector are the same memory the eye
     * goes into, so every later reader of p[] in this frame is reading our own
     * write; this is the frame's baseline, and the rig learning measures
     * against it instead of against itself.
     *
     * It has to be unconditional and it has to be first. An earlier attempt
     * put this call inside the head branch, where it ran on the frames the eye
     * is PLACED and was read on the frames the placement DECLINES - which is
     * exactly the frame it is needed on - and every one of the session's 495
     * probes logged raw=0 with the residual starting from the origin. One
     * capture per frame, here, and nothing downstream re-captures. See
     * ShFp2EngineRawCapture. */
    if (cm) ShFp2EngineRawCapture((const float *)(uintptr_t)(cm + MGR_POS));

    /* The head's visibility has a claim every frame whether
     * first person runs or not: the show window that follows a
     * handover has to restate itself here, where the engine
     * cannot out-talk it. */
    ShFp2HeadFrame();
    if (!cm || !g_apply) return 0;

    m = (float *)(uintptr_t)(cm + MGR_XFORM);
    p = (float *)(uintptr_t)(cm + MGR_POS);

    /* The engine's own head position has the first say. It is
     * not a reading of ours pushed along the camera basis, it
     * is the answer the engine computes for the head. And when
     * the module is up it owns the frame outright: placed, the
     * eye goes in; declined - an aim, a menu, the drone - the
     * engine's own camera is the answer and nothing else
     * writes here. The old placement below used to take the
     * declined frames and fight the aim camera for them, one
     * slewed step a frame, which read as a view that kept
     * pulling after the sights had settled. It answers only
     * for builds whose sites were never found. */
    if ((g_apply & CAM_HEAD_BIT) && ShFp2Ready()) {
        static float lastEye[3];
        static float lastBasis[9];
        /* The engine's own position for this frame, captured above before the
         * placement could overwrite it. Read into locals here, before the
         * branch, because the walk inside the placed branch moves p[] a second
         * time and the rig learning in the declined branch measures against
         * this baseline. MEASURE 2026-10-01. */
        float eg0, eg1, eg2;
        float dx, dy, dz;
        float rx, ry, rz;
        float fw, up;
        /* 0 until this frame's capture is read: the rig probe logs it, and a
         * frame that reaches the log without one must print "no capture"
         * rather than an uninitialised byte that reads as a yes. */
        int   engOk = 0;

        /* MEASURE 2026-10-01: the engine's own position for this frame, read
         * from the capture taken at the top of this callback - NOT p[], which
         * holds our eye by now. Read once, before the branch, so the placed and
         * the declined frames measure the same baseline. */
        {
            float raw[3];
            engOk = ShFp2EngineRaw(raw);
            if (engOk) { eg0 = raw[0]; eg1 = raw[1]; eg2 = raw[2]; }
            else       { eg0 = p[0];   eg1 = p[1];   eg2 = p[2];   }
        }

        if (ShFp2PlaceEye(cm, m, p)) {
            static float rigTop = 9.9f, rigLow = 9.9f, rigProg;
            float fv = *(const float *)(uintptr_t)(cm + MGR_FOV);

            g_headWroteAt = GetTickCount64();
            g_writes++;
            lastEye[0] = m[12]; lastEye[1] = m[13]; lastEye[2] = m[14];
            lastBasis[0] = m[0]; lastBasis[1] = m[1]; lastBasis[2] = m[2];
            lastBasis[3] = m[4]; lastBasis[4] = m[5]; lastBasis[5] = m[6];
            lastBasis[6] = m[8]; lastBasis[7] = m[9]; lastBasis[8] = m[10];

            /* MEASURE 2026-09-30: the walk onto the aim rig, in the only
             * branch that runs. It used to sit in the tail below, which this
             * branch returns before ever reaching - so the rig was never
             * applied, the learned residual stayed the same number frame
             * after frame, and the accumulator ran away to 244 m in one
             * session. What caught it was the log line, not the code: a probe
             * on a path nobody verified is worth exactly nothing.
             *
             * The fov is the engine's own transition clock, self-calibrating:
             * high at the hip, low when settled, and every weapon's settled
             * value differs, so the top and the running low are read here
             * rather than fixed. One eye, moved by a vector that grows from
             * nothing - no second source, nothing to switch - is what keeps
             * this off the list of turns that flashed.
             */
            if (g_aimRigHave && fv > 0.0f) {
                if (fv > rigTop) rigTop = fv;
                if (fv < rigLow) rigLow = fv;
                if (rigTop - rigLow > 0.02f) {
                    rigProg = (rigTop - fv) / (rigTop - rigLow);
                    if (rigProg < 0.0f) rigProg = 0.0f;
                    if (rigProg > 1.0f) rigProg = 1.0f;
                } else {
                    rigProg = 0.0f;
                }
                /* Two guards, both at the only place the eye moves.
                 *
                 * The clock's top IS the hip fov, so prog reads 1 at the hip
                 * - the offset must never apply there at all, or the eye is
                 * displaced for the whole session and the view after an aim
                 * ends up somewhere else. Not settled means not shifted.
                 *
                 * And a runaway vector must never move the eye: the figure is
                 * 0.19 m, so anything past 0.30 m is a broken accumulation
                 * (244 m was reached in the 2026-09-30 session) and is thrown
                 * away rather than applied, along with the clock that fed it.
                 */
                /* PLAN A 2026-09-30: the walk lives only inside the aim's own
                 * fall.
                 *
                 * Down here the view is on its way down - that is the aim's
                 * transition, and the only stretch of time the eye should be
                 * moved. The moment the view turns back up the aim is over and
                 * the walk is off, with a 0.02 margin so ordinary jitter in
                 * the hip view cannot keep it half-engaged: the version that
                 * ended on "back to within 0.004 of the highest value seen"
                 * left the eye pushed up to 0.19 m whenever the running high
                 * had been raised by a run, a menu or a vehicle - the "view
                 * feels odd after lowering the sights" report of 2026-09-30.
                 *
                 * 0.004 and not 0.02 on the hip side (see the rifle/pistol
                 * log): a pistol's whole aim moves the fov far less than a
                 * magnified rifle's, and a two-percent margin cancelled the
                 * entire pistol aim - its residual sat at (50, 186, -10) mm
                 * frame after frame while the rifle's fell to 1 mm. */
                {
                    static float rigDown = 9.9f;
                    static int   rigFall;

                    if (fv < rigDown - 0.0005f) rigFall = 1;   /* still coming down */
                    if (fv > rigLow + 0.02f)    rigFall = 0;   /* it turned back up */
                    rigDown = fv;
                    if (!rigFall) rigProg = 0.0f;
                }
                if (fv >= rigTop - 0.004f) rigProg = 0.0f;
                if (g_aimRig[0] * g_aimRig[0] + g_aimRig[1] * g_aimRig[1] +
                    g_aimRig[2] * g_aimRig[2] > 0.09f) {
                    Log("aim rig: %0.1f mm is out of range - dropped",
                        (double)(1000.0f * sqrtf(
                            g_aimRig[0] * g_aimRig[0] + g_aimRig[1] * g_aimRig[1] +
                            g_aimRig[2] * g_aimRig[2])));
                    g_aimRig[0] = g_aimRig[1] = g_aimRig[2] = 0.0f;
                    g_aimRigHave = 0;
                    rigTop = rigLow = 9.9f;
                    rigProg = 0.0f;
                }
                /* MEASURE 2026-10-01: the switch gates the APPLY only. The
                 * learning above runs either way, so the numbers a decision
                 * needs stay in the log - and one line per half second says
                 * what the walk WOULD have moved, which is what makes the two
                 * settings comparable inside a single session. See g_aimRigOn. */
                if (!g_aimRigOn) {
                    static uint64_t offAt;

                    if (rigProg > 0.0f && GetTickCount64() - offAt >= 500) {
                        offAt = GetTickCount64();
                        Log("aim rig: walk is OFF - would move %.1f,%.1f,%.1f mm "
                            "at prog=%.2f",
                            (double)(g_aimRig[0] * rigProg * 1000.0f),
                            (double)(g_aimRig[1] * rigProg * 1000.0f),
                            (double)(g_aimRig[2] * rigProg * 1000.0f),
                            (double)rigProg);
                    }
                    rigProg = 0.0f;
                }
                if (rigProg > 0.0f) {
                    rx = g_aimRig[0] * rigProg;
                    ry = g_aimRig[1] * rigProg;
                    rz = g_aimRig[2] * rigProg;
                    m[12] += m[0] * rx + m[4] * ry + m[8] * rz;
                    m[13] += m[1] * rx + m[5] * ry + m[9] * rz;
                    m[14] += m[2] * rx + m[6] * ry + m[10] * rz;
                    p[0] += m[0] * rx + m[4] * ry + m[8] * rz;
                    p[1] += m[1] * rx + m[5] * ry + m[9] * rz;
                    p[2] += m[2] * rx + m[6] * ry + m[10] * rz;
                }
            } else if (fv <= 0.0f) {
                rigTop = rigLow = 9.9f;
                rigProg = 0.0f;
            }

            /* The eye AS PLACED - recorded here, after the walk, not before
             * it. The residual learned on the hand-over frame is measured
             * against this, so it has to be the eye the picture actually
             * shows: recording it before the offset made every residual the
             * same number frame after frame (2026-09-30 log: (84.0, -87.3,
             * -15.3) repeated, and (55.6, 236.6, -6.0) for the pistol), so
             * the accumulator had nothing to converge to and grew without
             * bound instead. The two stores above are the pre-offset eye and
             * stay only so a stale basis is never used; these are the ones
             * the learning reads. */
            lastEye[0] = m[12]; lastEye[1] = m[13]; lastEye[2] = m[14];
            lastBasis[0] = m[0]; lastBasis[1] = m[1]; lastBasis[2] = m[2];
            lastBasis[3] = m[4]; lastBasis[4] = m[5]; lastBasis[5] = m[6];
            lastBasis[6] = m[8]; lastBasis[7] = m[9]; lastBasis[8] = m[10];
        } else {
            /* MEASURE 2026-09-29: the place was declined, so this frame
             * belongs to the engine - and that makes p[0..2] the engine's own
             * aim rig, the shoulder-and-weapon seat the weapon is placed
             * from and the one the sights are exact against. Learn the vector
             * from the eye we last placed to that seat, in the eye's own axes
             * (the last basis we wrote, so the numbers mean the same thing the
             * player's offset means), and hand it to first person. It then
             * walks the eye onto the seat over the aim's own transition, so
             * the frame can be given up with both cameras already in the same
             * place - no cut to see, and the sights exact either way.
             *
             * One frame of staleness in lastEye costs almost nothing: the
             * seat and the eye do not move between two frames of a settled
             * aim. A build whose sites were never found never reaches this
             * branch, and never learns anything. */
            /* MEASURE 2026-09-30: the one line that decides this whole
             * question, kept short on purpose. engineRig is the seat the
             * weapon is placed from, lastEye is the eye we last placed - the
             * eye the picture showed, offset included - and aimRig is what we
             * believe the difference is. Read the samples across an entry
             * transition, where lastEye moves by the whole vector:
             *
             *   engineRig sliding along with lastEye  -> the seat is derived
             *       from the camera we present, so no offset we can apply
             *       will ever close the gap, and this whole line of work ends
             *       here, honestly.
             *   engineRig holding still while lastEye moves -> the walk is
             *       real work and the residual is the error in it.
             *
             * Also prints whether the walk is running at all: eye is the raw
             * eye, lastEye is after the offset, so their difference IS the
             * applied vector. Ten a second while the engine owns the frame.
             */
            {
                static uint64_t rigAt;

                if (GetTickCount64() - rigAt >= 100) {
                    rigAt = GetTickCount64();
                    Log("rig probe: engineRig=(%.3f,%.3f,%.3f) lastEye=(%.3f,%.3f,%.3f) "
                        "aimRig=(%.1f,%.1f,%.1f)mm fov=%.4f raw=%d polluted=(%.3f,%.3f,%.3f)",
                        (double)eg0, (double)eg1, (double)eg2,
                        (double)lastEye[0], (double)lastEye[1], (double)lastEye[2],
                        (double)(g_aimRig[0] * 1000.0f),
                        (double)(g_aimRig[1] * 1000.0f),
                        (double)(g_aimRig[2] * 1000.0f),
                        (double)*(const float *)(uintptr_t)(cm + MGR_FOV),
                        engOk,
                        (double)p[0], (double)p[1], (double)p[2]);
                }
            }

            /* The engine's own position for this frame, from the capture taken
             * before the placement - NOT p[], which holds our eye by now. See
             * eg0 above. */
            dx = eg0 - lastEye[0];
            dy = eg1 - lastEye[1];
            dz = eg2 - lastEye[2];
            rx = dx * lastBasis[0] + dy * lastBasis[1] + dz * lastBasis[2];
            fw = dx * lastBasis[3] + dy * lastBasis[4] + dz * lastBasis[5];
            up = dx * lastBasis[6] + dy * lastBasis[7] + dz * lastBasis[8];

            if (rx == rx && fw == fw && up == up) {
                if (rx * rx + fw * fw + up * up < 1.0f) {
                    /* One rig PER WEAPON, and each residual added once.
                     *
                     * The weapon's own signature here is the fov the aim
                     * settles on - the frame already carries it and nothing
                     * else per-weapon is available without a new API. Measured
                     * 2026-09-30: pistol iron sights settle at 0.6882 and a
                     * magnified rifle at 0.4916, and their rigs are nothing
                     * alike ((51, 245, -6) mm against (84, -88, -16) mm), which
                     * is why one slot had the pistol wearing the rifle's
                     * offset. Four slots, matched within 2 percent, oldest
                     * reused.
                     *
                     * And each residual is added ONCE. The hand-over lasts
                     * many frames while lastEye is frozen, so the same residual
                     * arrived on every one of them and was added every one of
                     * them: 0.23 -> 0.46 -> 0.92 -> ... -> the doubling in the
                     * log (7175, 11393, 25230 mm, each thrown away by the range
                     * guard) and the reason a session of aims never improved
                     * anything.
                     *
                     * Whatever the slot now holds is published to g_aimRig,
                     * which is what the walk in the placed branch reads - so
                     * the next aim of this weapon walks onto this weapon's
                     * seat. */
                    static uint64_t logAt, lastLearn;
                    static float key[4], acc[4][3], last[4][3];
                    static int   next;
                    static float learnLow = 9.9f;
                    uint64_t now = GetTickCount64();
                    float fvs = *(const float *)(uintptr_t)(cm + MGR_FOV);
                    int i, use = -1, settled;

                    /* MEASURE 2026-09-30, a rifle's first aim after a pistol:
                     * a transition passes through many fov values, and the slot
                     * was matched on the fov of the moment - so one weapon's
                     * residual landed in two or three slots, and g_aimRig was
                     * published from whichever slot matched THIS frame. The walk
                     * then chased a different vector every frame: (33, -271, -5)
                     * then (83, -88, -65) then one of them negated, three swings
                     * in the 10:06 log, each the engine's optic snapping to a new
                     * seat. That is the ghost the field reported - a scope in the
                     * face, on the right, on a weapon's first aim.
                     *
                     * So only a settled aim teaches: the fov has to be at the
                     * lowest it has been since this aim started, which is when
                     * the engine's transition is over and its seat is the seat.
                     * A slot's key is only written then too, so a slot stands for
                     * a weapon rather than for a frame of its raise. The first
                     * aim of a weapon is spent on nothing; from the second there
                     * is one slot, one vector, and nothing to swing between. */
                    if (now - lastLearn > 2000) learnLow = fvs;
                    if (fvs < learnLow) learnLow = fvs;
                    settled = (fvs <= learnLow + 0.010f);

                    for (i = 0; i < 4; i++) {
                        if (key[i] != 0.0f && fabsf(key[i] - fvs) < 0.02f) {
                            use = i;
                            break;
                        }
                    }
                    if (use < 0) {
                        for (i = 0; i < 4; i++) {
                            if (key[i] == 0.0f) { use = i; break; }
                        }
                    }
                    if (use < 0) { use = next; next = (next + 1) & 3; }
                    if (key[use] == 0.0f && settled) key[use] = fvs;

                    /* ONCE PER HAND-OVER, and a slot that has left the range
                     * starts over.
                     *
                     * The per-value dedupe was not enough: the residual comes
                     * back slightly different every frame (48.9 then 48.3 then
                     * 43.6 ...), so every frame of a hand-over passed it and
                     * every frame added another 0.2 m - 415 m inside a minute,
                     * the whole of it thrown away by the range guard, so
                     * nothing was ever learned. A hand-over is one event and
                     * gets one sample: 300 ms between samples is inside every
                     * aim measured (359 ms to 2.4 s) and outside the frame
                     * time by two orders of magnitude.
                     *
                     * And a slot that the guard has already refused must not be
                     * published again and again - it starts from zero instead,
                     * which is what lets this converge from a bad state rather
                     * than sitting at 415 m for the rest of the session. */
                    if (!settled) {
                        ;   /* the raise is still moving: not a seat yet */
                    } else if (now - lastLearn < 1200) {
                        ;   /* same hand-over: nothing to learn twice */
                    } else if (rx * rx + fw * fw + up * up > 0.16f) {
                        /* A sample over 250 mm is not a seat either - it is a
                         * frame of a walk that was still in flight, or the
                         * hip's own 1.9 m. It is dropped, and the slot is only
                         * cleared when the slot ITSELF is out of range, so a
                         * bad sample can no longer zero a good slot. */
                        lastLearn = now;
                        if (acc[use][0] * acc[use][0] +
                            acc[use][1] * acc[use][1] +
                            acc[use][2] * acc[use][2] > 0.0625f) {
                            acc[use][0] = acc[use][1] = acc[use][2] = 0.0f;
                            last[use][0] = last[use][1] = last[use][2] = 0.0f;
                        }
                    } else if (fabsf(rx - last[use][0]) > 0.002f ||
                               fabsf(fw - last[use][1]) > 0.002f ||
                               fabsf(up - last[use][2]) > 0.002f) {
                        /* MEASURE 2026-10-01: ASSIGN, never accumulate.
                         *
                         * rx/fw/up is the COMPLETE vector from the eye we last
                         * placed to the engine's seat - not a small correction
                         * to add on top of one. The seat is fixed; our eye is
                         * what moves, and the walk above moves it onto the
                         * slot. So this vector is already "how far is left",
                         * and it shrinks on its own as the eye arrives. Adding
                         * it to what the slot holds instead makes every aim
                         * double the offset: the 2026-10-01 session walked the
                         * pistol from (49, 210, -59) mm to (241, 1013, -298)
                         * and the rifle to (369, -666, -254), which is the
                         * scope ghost on the right and the silencer trail the
                         * field reported that day.
                         *
                         * The earlier "+=" carried a note saying each residual
                         * was added once. It was not: the guard below is a
                         * 1.2 s window and two hand-overs a second apart both
                         * pass it, and the residual comes back the same size
                         * every time because the eye starts each aim from the
                         * same place - so the slot grew by one full offset per
                         * aim. Assignment has no such failure mode: the same
                         * sample twice is the same value twice. */
                        lastLearn = now;
                        last[use][0] = rx;
                        last[use][1] = fw;
                        last[use][2] = up;
                        acc[use][0] = rx;
                        acc[use][1] = fw;
                        acc[use][2] = up;
                    } else {
                        lastLearn = now;
                    }
                    /* MEASURE 2026-09-30, the weapon switch: only a slot whose
                     * key belongs to the fov on screen may be published. A slot
                     * was published for every fov the transition passed through,
                     * so for the first frames of a rifle's first raise the eye
                     * was still being moved by the PISTOL's 229 mm - one frame
                     * of another weapon's seat, read as a scope ghost in the
                     * face and gone before a 110 ms log line could see it. It is
                     * why only the first raise after a switch showed it, and why
                     * the second did not (by then the published value was this
                     * weapon's own). No match: publish nothing, and the walk
                     * applies nothing. */
                    if (key[use] != 0.0f && fabsf(key[use] - fvs) < 0.02f) {
                        g_aimRig[0] = acc[use][0];
                        g_aimRig[1] = acc[use][1];
                        g_aimRig[2] = acc[use][2];
                        g_aimRigHave = 1;
                    } else {
                        g_aimRigHave = 0;
                    }
                    /* One line per half second - this branch runs on every
                     * frame the engine owns, which was 1672 lines in one
                     * session before. */
                    if (now - logAt >= 500) {
                        logAt = now;
                        Log("aim rig: corrected by (%.1f, %.1f, %.1f) mm -> "
                            "now (%.1f, %.1f, %.1f) mm  fov=%.4f",
                            (double)(rx * 1000.0f), (double)(fw * 1000.0f),
                            (double)(up * 1000.0f),
                            (double)(g_aimRig[0] * 1000.0f),
                            (double)(g_aimRig[1] * 1000.0f),
                            (double)(g_aimRig[2] * 1000.0f),
                            (double)*(const float *)(uintptr_t)(cm + MGR_FOV));
                    }
                }
            }
        }
        /* The head position owned this frame, so there is no eye of ours
         * to offer - the engine's own xmm2 stays, as it was. */
        return 0;
    }

    ApplyPose(m, *(const float *)(uintptr_t)(cm + MGR_FOV));

    /* The engine fills the position vector from a second
     * call, so the row above is restated here rather than
     * left to disagree with it. */
    /* MEASURE 2026-09-29: walk the eye onto the aim rig it learned, in step
     * with the engine's own ADS transition.
     *
     * The fov is the transition's own clock (a hip view sits high, a settled
     * aim low, and every weapon's settled value differs), so the progress is
     * self-calibrating: the fov seen at the top of the aim is the start, the
     * running minimum is the end. Nothing here is a threshold on a distance,
     * and nothing switches between two sources - one eye, moved by a vector
     * that grows from nothing - which is what keeps it off the list of turns
     * that flashed (see the note at g_aimArm in scripthook_fpx.c). Moving the
     * eye 191 mm over an aim's few hundred milliseconds reads as the sights
     * coming up, not as a cut.
     */
    {
        static float rigGot, fovTop = 9.9f, fovLow = 9.9f, prog, shown[3];
        static float prevFv = 9.9f, steady;
        float fv = *(const float *)(uintptr_t)(cm + MGR_FOV);
        float rx, ry, rz;
        int   k;

        if (g_aimRigHave) {
            rigGot = 1.0f;
            /* MEASURE 2026-09-30: the fov is a clock only while it is MOVING.
             * A raise passes through other weapons' settled fovs, and a slot
             * matched on the way up published that weapon's seat - so a rifle's
             * first raise was moved by the PISTOL's 229 mm for a few frames,
             * which is the scope ghost the field reported (right side, at the
             * face, once, then gone). Nothing is applied until the transition
             * has stopped: three frames inside 0.5 mrad. */
            steady = (fabsf(fv - prevFv) < 0.0005f) ? steady + 1.0f : 0.0f;
            if (steady > 60.0f) steady = 60.0f;
            prevFv = fv;
            if (fv > 0.0f) {
                if (fv > fovTop) fovTop = fv;              /* the hip value   */
                if (fv < fovLow) fovLow = fv;              /* the settled one */
                if (fovTop - fovLow > 0.02f) {
                    prog = (fovTop - fv) / (fovTop - fovLow);
                    if (prog < 0.0f) prog = 0.0f;
                    if (prog > 1.0f) prog = 1.0f;
                } else {
                    prog = 0.0f;
                }
                if ((g_apply & CAM_HEAD_BIT) && prog > 0.0f) {
                    /* MEASURE 2026-09-30, a rifle's first aim: the seat it has to
                     * reach is 274 mm away, and how far along the raise the fov
                     * clock is says nothing about how far along THIS eye is - so
                     * the rig arrived whole in one frame. That is both halves of
                     * what the field reported: learned, it is a snap; refused by
                     * the range gate, the eye stays 274 mm ahead of the seat and
                     * the engine's optic model (drawn at the seat) sits behind
                     * the camera and crosses it on the raise - the ghost in the
                     * face, on the right, on a weapon's first aim.
                     *
                     * So the eye does not jump to the rig, it travels to it: at
                     * most 40 mm a frame, which is 2.4 m/s at 60 fps and puts
                     * 274 mm behind 7 frames, around 0.12 s. Fast enough to be
                     * over before the raise is, slow enough to read as the sight
                     * coming up rather than as a cut. */
                    for (k = 0; k < 3; k++) {
                        float want = g_aimRig[k] * prog;
                        float step = want - shown[k];

                        if (step > 0.040f) step = 0.040f;
                        if (step < -0.040f) step = -0.040f;
                        shown[k] += step;
                    }
                    rx = shown[0];
                    ry = shown[1];
                    rz = shown[2];
                    m[12] += m[0] * rx + m[4] * ry + m[8] * rz;
                    m[13] += m[1] * rx + m[5] * ry + m[9] * rz;
                    m[14] += m[2] * rx + m[6] * ry + m[10] * rz;
                }
            }
        } else if (rigGot > 0.0f) {
            /* The rig was known and the build changed under us (a new
             * session's module, a plugin reload): forget the clock too. */
            fovTop = fovLow = 9.9f;
            prog = 0.0f;
        }
    }

    p[0] = m[12];
    p[1] = m[13];
    p[2] = m[14];
    p[3] = 0.0f;
    g_writes++;

    /* MEASURE 2026-09-29: the engine's own answer, taken before this
     * frame's write replaces it. Everything the four failed candidates
     * taught, in one place: the aim camera is computed in the aim path
     * from the character's pose and is read there - no copy of it in the
     * manager (+0x170, +0x190, +0x1E0) and no register at the camera
     * build (xmm2) is what the weapon and optic alignment consults. What
     * the field does have is a number: with the frame handed over, the
     * aim camera sits 191 mm from the eye on a settled aim, and 1873 mm
     * at the hip, and the sights are exact in the first case and 191 mm
     * out in the second.
     *
     * So the frame is handed over for exactly the part of the aim where
     * it costs nothing. The engine's own ADS transition walks its camera
     * from 1873 mm behind the head to 191 mm off it, and that walk passes
     * through the eye - so the frame is given up at the frame where the
     * two positions meet (a third of a metre, well inside the metre the
     * trace calls a jump) and taken back when the aim ends. At the switch
     * the two cameras hold the same position, so there is no cut to see:
     * this is not the threshold of 2026-09-26, which switched between two
     * cameras metres apart and flashed on ordinary play. The distance is
     * only ever read to decide "are we at the crossing yet", never to
     * move the camera onto a second source.
     */
    return 0;
}

/* Shared by both stubs. Volatile registers only: everything
 * else has to come back exactly as it was, because the
 * engine's frame carries straight on from here. The spills are
 * aligned stores, so rsp has to be 16 byte aligned on the way
 * in - at a call site it is 8 off, which the push in front of
 * this block puts right.
 */
static const uint8_t STUB_SAVE[] = {
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
static const uint8_t STUB_REST[] = {
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

/* The site keeps its call opcode, so the stub is entered
 * with the return address already pushed and tail jumps to
 * the original target, which returns past the site. */
static int BuildMgrStub(void) {
    uint8_t *s = (uint8_t *)ShAllocNear(MGR_SITE);
    int64_t rel;
    int o = 0;

    if (!s) return 0;
    memset(s, 0xCC, 0x1000);

    /* The spills below are aligned stores, so the frame has to
     * be exactly 16 off when they run: align first rather than
     * assume what the site left behind, then take the 8 back so
     * the shared block sees the frame it expects. Everything is
     * undone through rbp, so the engine's own rsp is untouched.
     */
    s[o++] = 0x55;                                  /* push rbp  */
    s[o++] = 0x48; s[o++] = 0x89; s[o++] = 0xE5;    /* mov rbp,rsp */
    s[o++] = 0x48; s[o++] = 0x83; s[o++] = 0xE4;    /* and rsp,-16 */
    s[o++] = 0xF0;
    s[o++] = 0x48; s[o++] = 0x83; s[o++] = 0xEC;    /* sub rsp,8  */
    s[o++] = 0x08;
    memcpy(s + o, STUB_SAVE, sizeof(STUB_SAVE));
    o += (int)sizeof(STUB_SAVE);
    s[o++] = 0x48; s[o++] = 0x89; s[o++] = 0xC1;    /* mov rcx,rax */
    s[o++] = 0x48; s[o++] = 0xB8;
    *(uint64_t *)(s + o) = (uint64_t)(uintptr_t)MgrCallback;
    o += 8;
    s[o++] = 0xFF; s[o++] = 0xD0;                   /* call rax  */
    memcpy(s + o, STUB_REST, sizeof(STUB_REST));
    o += (int)sizeof(STUB_REST);
    /* MEASURE 2026-09-29: the callback's answer is the eye it placed, or
     * NULL when the frame was not ours to place. Hand it on in xmm2 - the
     * register the store at MGR_STORE put the engine's own camera into,
     * and the one its downstream reads. Deliberately after STUB_REST: that
     * block has just put the engine's xmm2 back from the spill, and this
     * is the overwrite of it. Nothing here touches flags, and rax is dead
     * between the call above and the tail jump below, so the test costs
     * the caller nothing. NULL leaves the engine's own value alone, which
     * is the whole reason this is a conditional and not a plain store. */
    s[o++] = 0x48; s[o++] = 0x85; s[o++] = 0xC0;    /* test rax,rax     */
    s[o++] = 0x74; s[o++] = 0x04;                   /* je +4            */
    s[o++] = 0x0F; s[o++] = 0x28; s[o++] = 0x10;    /* movaps xmm2,[rax] */
    s[o++] = 0x48; s[o++] = 0x89; s[o++] = 0xEC;    /* mov rsp,rbp */
    s[o++] = 0x5D;                                  /* pop rbp   */

    rel = (int64_t)MGR_NEXT - ((int64_t)(uintptr_t)(s + o) + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) return 0;
    s[o++] = 0xE9;
    *(int32_t *)(s + o) = (int32_t)rel;

    g_mgrStub = s;
    return 1;
}

/* Read only: is the store above still the store? Nothing here
 * patches anything. The answer is one log line, and it is the
 * whole reason the fallback site is in this file - on a build
 * whose call site moved, this line says whether the other way
 * in is there before anyone goes looking for it by hand.
 */
static void MgrStoreCheck(void) {
    const uint8_t *at = (const uint8_t *)(uintptr_t)MGR_STORE;
    int i;

    if (!ShReadableAddr(MGR_STORE, MGR_STORE_LEN)) {
        Log("manager store: %llX is not readable - the second way in is "
            "gone on this build", (unsigned long long)MGR_STORE);
        return;
    }
    for (i = 0; i < MGR_STORE_LEN; i++) {
        if (at[i] != MGR_STORE_SIG[i]) {
            Log("manager store: %llX holds %02X %02X %02X %02X %02X %02X %02X, "
                "not the store - it moved",
                (unsigned long long)MGR_STORE,
                (unsigned)at[0], (unsigned)at[1], (unsigned)at[2],
                (unsigned)at[3], (unsigned)at[4], (unsigned)at[5],
                (unsigned)at[6]);
            return;
        }
    }
    Log("manager store: %llX is the store, left alone (the fallback)",
        (unsigned long long)MGR_STORE);
}

/* Refuse anything but the call we decoded, so a build we do
 * not know keeps its own camera. */
static int MgrInstall(void) {
    uint8_t *at = (uint8_t *)(uintptr_t)MGR_SITE;
    int64_t rel;
    DWORD old;

    if (g_mgrHooked) return 1;
    MgrStoreCheck();
    /* The failure that says nothing is the dangerous one: a manager site
     * left over from an older build refuses the hook, the camera call
     * then reports a success it does not have, and first person reads as
     * a feature that simply does not work. Put the two addresses in the
     * log instead - together they say which half moved. */
    if (!ShReadableAddr(MGR_SITE, MGR_LEN)) {
        Log("manager site: %llX is not readable - the eye has no frame",
            (unsigned long long)MGR_SITE);
        return 0;
    }
    if (at[0] != 0xE8) {
        Log("manager site: %llX starts with %02X, not E8 - it moved",
            (unsigned long long)MGR_SITE, (unsigned)at[0]);
        return 0;
    }
    if ((uint64_t)((int64_t)MGR_SITE + MGR_LEN
                   + *(int32_t *)(at + 1)) != MGR_NEXT)
    {
        Log("manager site: %llX calls %llX, not %llX - one of them moved",
            (unsigned long long)MGR_SITE,
            (unsigned long long)((int64_t)MGR_SITE + MGR_LEN
                                 + *(int32_t *)(at + 1)),
            (unsigned long long)MGR_NEXT);
        return 0;
    }
    if (!BuildMgrStub()) return 0;

    rel = (int64_t)(uintptr_t)g_mgrStub - ((int64_t)MGR_SITE + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) return 0;
    if (!VirtualProtect(at, MGR_LEN, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    *(int32_t *)(at + 1) = (int32_t)rel;
    VirtualProtect(at, MGR_LEN, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, MGR_LEN);
    g_mgrHooked = 1;
    Log("manager site: %llX hooked, calls on to %llX",
        (unsigned long long)MGR_SITE, (unsigned long long)MGR_NEXT);
    return 1;
}

static int BuildStub(void) {
    uint8_t *s = (uint8_t *)ShAllocNear(CAM_THUNK);
    int o = 0;

    if (!s) return 0;
    memset(s, 0xCC, 0x1000);

    memcpy(s + o, STUB_SAVE, sizeof(STUB_SAVE));
    o += (int)sizeof(STUB_SAVE);
    s[o++] = 0x48; s[o++] = 0xB8;
    *(uint64_t *)(s + o) = (uint64_t)(uintptr_t)CamCallback; o += 8;
    s[o++] = 0xFF; s[o++] = 0xD0;
    memcpy(s + o, STUB_REST, sizeof(STUB_REST));
    o += (int)sizeof(STUB_REST);
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
    static int logInited;

    if (!logInited) { logInited = 1; LogInit("scripthook_camera.log"); }

    if (g_camStub) {
        /* The thunk is ours; the manager site is the half a build we do
         * not know can refuse, and once g_camStub is set every later call
         * used to answer 1 regardless - a camera reported as working that
         * never reaches the engine. Retry the site instead. */
        if (!g_mgrHooked && !MgrInstall()) {
            ShSetError(SH_ERR_HOOK_FAILED);
            return 0;
        }
        /* The fov stub goes up with the camera rather than on the first
         * override: it is what makes the engine's own value readable, and
         * with nothing overriding it just passes that value through. */
        ShFovInstall();
        return 1;
    }
    if (!ShReadableAddr(CAM_THUNK, 5)) {
        Log("camera thunk: %llX is not readable",
            (unsigned long long)CAM_THUNK);
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    if (t[0] != 0xE9) {
        Log("camera thunk: %llX starts with %02X, not E9 - it moved",
            (unsigned long long)CAM_THUNK, (unsigned)t[0]);
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    cur = (int64_t)CAM_THUNK + 5 + *(int32_t *)(t + 1);
    if ((uint64_t)cur != CAM_IMPL) {
        Log("camera thunk: %llX jmps to %llX, not %llX - one moved",
            (unsigned long long)CAM_THUNK, (unsigned long long)cur,
            (unsigned long long)CAM_IMPL);
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    if (!BuildStub()) {
        Log("camera thunk: no stub near %llX",
            (unsigned long long)CAM_THUNK);
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    if (!PatchThunk()) {
        Log("camera thunk: the patch at %llX did not take",
            (unsigned long long)CAM_THUNK);
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    if (!MgrInstall()) {
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    /* The first person sites are optional, and installing them
     * is a separate question: a build without them keeps the
     * placement it has always had. */
    ShFp2Install();
    ShFovInstall();
    Log("camera: thunk %llX hooked (calls on to %llX), manager site %llX hooked",
        (unsigned long long)CAM_THUNK, (unsigned long long)CAM_IMPL,
        (unsigned long long)MGR_SITE);
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
 *     the first person path did write on a recent frame. Where that
 *     eye sits is irrelevant - an offset moving it off the head bone
 *     is still the first person view, and calling those third person
 *     is what flashed the head on in vehicles.
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

/* Drop the hand over grace. Turning first person off is not handing
 * the camera to the engine for an aim, so the view is third person
 * from that moment on. */
SH_API void ShCameraHandoverClear(void) {
    g_headHeldAt = 0;
}

SH_API uint64_t ShCameraCalls(void) { return g_calls; }
SH_API uint64_t ShCameraWrites(void) { return g_writes; }

/* MEASURE 2026-09-30: ShCameraAltEye lived here - the knob that restated
 * the eye into the engine's second camera copy at +0x1E0. It was added for
 * one measurement, several sessions of asking it produced no visible change
 * (the copy is not what the weapon or optic alignment reads), and no line
 * of the framework or of a plugin ever called it. The measurement and what
 * it ruled out are written down in
 * docs/firstperson-aim-flash-and-offset.md, so the export is gone with it.
 */

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
    (void)InterlockedAnd((volatile LONG *)&g_apply,
                         (LONG)~CAM_DERIVED);
    (void)InterlockedOr((volatile LONG *)&g_apply,
                        (LONG)SH_CAM_POS);
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
    (void)InterlockedAnd((volatile LONG *)&g_apply,
                         (LONG)~CAM_HEAD_BIT);
    (void)InterlockedOr((volatile LONG *)&g_apply,
                        (LONG)(SH_CAM_POS | CAM_ORBIT_BIT));
    ShSetError(SH_OK);
    return 1;
}

/* The same orbit with a third axis: metres sideways along the camera's
 * right vector, which is what an over-the-shoulder offset is. Ported
 * from the Wildlands Immersion Suite, whose ApplyOrbit carried the
 * right term from the start. */
SH_API int ShCameraOrbitAdvanced(float back, float right, float up) {
    if (!ShCameraHookInstall()) return 0;
    g_back = back;
    g_right = right;
    g_up = up;
    (void)InterlockedAnd((volatile LONG *)&g_apply,
                         (LONG)~CAM_HEAD_BIT);
    (void)InterlockedOr((volatile LONG *)&g_apply,
                        (LONG)(SH_CAM_POS | CAM_ORBIT_BIT));
    ShSetError(SH_OK);
    return 1;
}

/* First person: the eye is the engine's own head position,
 * placed by ShFp2PlaceEye from the manager's frame. The
 * arguments are the table's own (forward, up) and are kept for
 * the call's shape only - the eye offset is ShFp2SetOffset's
 * business now, one setting rather than one per consumer.
 *
 * Note what is NOT set: SH_CAM_POS. It used to be, as a way of
 * saying "the position is ours", but position writes on this
 * path come from PlaceEye, and a frame that reaches ApplyPose
 * with SH_CAM_POS set writes g_absPos - which nobody filled in
 * for first person - and parks the camera at the origin. */
SH_API int ShCameraFirstPerson(float forward, float up) {
    if (!ShCameraHookInstall()) return 0;
    g_back = forward;
    g_up = up;
    (void)InterlockedAnd((volatile LONG *)&g_apply,
                         (LONG)~CAM_ORBIT_BIT);
    (void)InterlockedOr((volatile LONG *)&g_apply,
                        (LONG)CAM_HEAD_BIT);
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
    (void)InterlockedAnd((volatile LONG *)&g_apply,
                         (LONG)~CAM_DERIVED);
    (void)InterlockedOr((volatile LONG *)&g_apply,
                        (LONG)(SH_CAM_POS | SH_CAM_ROT));
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
     * position and rotation alone. Interlocked because two
     * plugins may be talking at once and a plain read-modify
     * -write loses whichever bit the other just set.
     */
    if (o->apply & SH_CAM_POS)
        (void)InterlockedAnd((volatile LONG *)&g_apply,
                             (LONG)~CAM_DERIVED);
    (void)InterlockedOr((volatile LONG *)&g_apply,
                        (LONG)o->apply);
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
    (void)InterlockedAnd((volatile LONG *)&g_apply, 0L);
    ShFovClear();
}

/* Give back only what you took, so releasing a free camera
 * leaves another plugin's fov override running. Releasing the
 * position releases the derived claims with it - the orbit
 * arm and the first person eye are both ways of putting the
 * position somewhere, and leaving either bit set after the
 * caller let go is a camera nobody is driving.
 */
SH_API void ShCameraReleaseFields(uint32_t fields) {
    if (fields & SH_CAM_POS) fields |= CAM_DERIVED;
    if (fields & SH_CAM_FOV) ShFovClear();
    (void)InterlockedAnd((volatile LONG *)&g_apply, (LONG)~fields);
}

/** Which fields are currently overridden. */
SH_API uint32_t ShCameraOwned(void) {
    return g_apply & 0xFFu;
}
