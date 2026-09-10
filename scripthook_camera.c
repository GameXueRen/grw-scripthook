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
 * translation. RAX holds the manager at the patch site. */
static void __attribute__((ms_abi)) MgrCallback(uint64_t cm) {
    float *m, *p;

    /* The head's visibility has a claim every frame whether
     * first person runs or not: the show window that follows a
     * handover has to restate itself here, where the engine
     * cannot out-talk it. */
    ShFp2HeadFrame();
    if (!cm || !g_apply) return;

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
        if (ShFp2PlaceEye(cm, m, p)) {
            g_headWroteAt = GetTickCount64();
            g_writes++;
        }
        return;
    }

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
    s[o++] = 0x48; s[o++] = 0x89; s[o++] = 0xEC;    /* mov rsp,rbp */
    s[o++] = 0x5D;                                  /* pop rbp   */

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
    /* The first person sites are optional, and installing them
     * is a separate question: a build without them keeps the
     * placement it has always had. */
    ShFp2Install();
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
