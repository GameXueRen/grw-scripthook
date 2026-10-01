/* First person the way the Cheat Engine table does it.
 *
 * The engine already computes where the head is: one call,
 * GRW.exe+188CE00, is handed an argument and writes a world
 * position. The table does not reinvent that, it captures the
 * argument once - from a call site of the very same function -
 * then asks the function again every frame and writes the
 * answer where the camera position goes. What the table adds
 * is an offset and the gate: menus, the drone and iron sights
 * each have a byte the engine already maintains, and while any
 * of them is up the camera is left alone.
 *
 * That is the whole trick, and it is why it feels right: no
 * bone lookup of our own, no forward and up rebuilt from the
 * camera basis, no easing - the engine's own answer, verbatim.
 *
 * Everything here runs on the frame path. The placement is
 * bounded: a handful of guarded dereferences, one engine call,
 * one store. Nothing scans, nothing allocates, nothing logs.
 *
 * The camera POSITION is the eye below plus the user's offset, on an
 * aim's frame as on any other: the frame does not change hands, so
 * there is no cut at either end of an aim and the flash the field
 * reports for years cannot be produced. The engine's own ADS byte is
 * still the signal that says an aim is up, but it is read for the log
 * alone - see the branch in ShFp2PlaceEye and the note above it, which
 * carries the price of the choice and the one knob that reverses it
 * live for an A/B. The note at g_aimArm carries the wrong turns that
 * must not come back.
 */
#include <windows.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"
#include "log.h"

/* ---- sites -------------------------------------------------
 * RVAs of the bytes the table rewrites, with the instruction
 * each one carries in a known build. Every install checks for
 * its own bytes and refuses anything else, so a build we have
 * not seen keeps behaving exactly as it does today.
 *
 * 2026-09-29: the whole list was put beside the community table
 * this file came from - Wildlands First Person, Last Rites RC1
 * - whose [DISABLE] section carries the instruction each site
 * held before that table patched it. Twelve of the thirteen
 * agree byte for byte, the four call sites among them, and
 * those four are what names the engine functions below. The
 * odd one out is S_SHOULDER: the table prints 0F 84 8F (je
 * +0x8F) where this build has 0F 85 87 (jne +0x87) - the
 * target that table's own comment names. install miss=000 with
 * extras mask=f is the live proof that the bytes here are the
 * bytes on the build, so the table's line is the stale one.
 *
 * Three GRW.exe numbers that used to sit in this file's
 * comments were the table's older revision and are corrected
 * to what the code has always used: 188CE00 (the head call,
 * not 188BA20), 2A257C0 (the visibility call the aim sites
 * carry, not 2A25600) and 2A185A0 (the body position call,
 * not 2A183E0).
 */

/* call GRW.exe+188CE00: the capture site. This is where the
 * argument we reuse comes from. */
#define S_ARGS      SH_IMG(0xA074190)
#define S_ARGS_FN   SH_IMG(0x188CE00)

/* The menu counter at +0x56C, pushed from three places. The
 * table tracks it because a menu renders the body too, and a
 * hidden head in a menu is a headless loadout screen. */
#define S_MENU1     SH_IMG(0x120ADE51)   /* inc [rdi+56C] */
#define S_MENU2     SH_IMG(0x1209EA35)   /* dec [rbx+56C] */
#define S_MENU3     SH_IMG(0x1209C0CC)   /* dec [rbx+56C] */

/* mov [rdi+181A],bl: 1 while the tactical drone flies. */
#define S_DRONE     SH_IMG(0x113A0875)

/* call GRW.exe+2A257C0 on entering and leaving aim down
 * sight. The dl the engine passes is the ADS state, and the
 * call is the same visibility one as FN_VIS below - which is
 * why an aim can hide the head through it instead of needing a
 * second engine call. */
#define S_ADS_OUT   SH_IMG(0x147FF653)
#define S_ADS_IN    SH_IMG(0x147FF669)

/* call GRW.exe+2A185A0: carries the body position, which the
 * table keeps to vet the head reading against. */
#define S_BODY      SH_IMG(0x14897FBA)
#define S_BODY_FN   SH_IMG(0x2A185A0)

/* movzx eax,[rax+58] / add rsp,20: body visibility, taken so
 * the body can be told to stay visible. */
#define S_VIS       SH_IMG(0x1489A365)

/* jne: refuses a shoulder swap while aiming. Nopped so the
 * swap always answers. */
#define S_SHOULDER  SH_IMG(0x13A1255B)

/* mov byte [r13+58],01: hides the body when it is pushed
 * against a wall. Nopped so the body stays. */
#define S_WALL      SH_IMG(0x149C3E7A)

/* The two engine calls the whole thing rests on. */
#define FN_HEAD     SH_IMG(0x188CE00)
#define FN_VIS      SH_IMG(0x2A257C0)

/* Head of the chain that names the head for the visibility
 * call. Read once per attempt, not once per frame. */
#define HEAD_ROOT   SH_IMG(0x4B90638)

/* How far the head reading may sit from the body before it is
 * treated as a stale transform. The table compares against
 * zero - an exact match, which is really "these two readings
 * came from the same place". A little tolerance keeps a
 * rounding difference from vetoing every frame. */
#define SANE_TOL    0.001f

/* Kept for the asking: how long since the head was last told
 * anything. Nothing throttles on it any more. */

/* ---- which installs failed -------------------------------
 * Reported as a mask so a partial install is visible from the
 * log instead of looking like a total failure. Only the
 * capture site is required: without it there is no argument to
 * reuse and the old placement takes over.
 */
#define M_ARGS      0x001u
#define M_MENU1     0x002u
#define M_MENU2     0x004u
#define M_MENU3     0x008u
#define M_DRONE     0x010u
#define M_ADS       0x020u
#define M_BODY      0x040u
#define M_VIS       0x080u
#define M_SHOULDER  0x100u
#define M_WALL      0x200u
#define M_REQUIRED  M_ARGS

/* The state the stubs write and the placement reads. Mirrors
 * the table's own data block; volatile because the stubs write
 * it from inside the engine's frame, not from a thread of
 * ours, and the compiler must not cache any of it.
 */
typedef struct {
    float    off[4];        /* X, Y, Z in metres; [3] stays 0 */
    float    bodyPos[4];    /* the body reading, from S_BODY  */
    uint64_t headArg;       /* captured argument for FN_HEAD  */
    uint64_t headArg8;      /* the engine's own third and     */
    uint64_t headArg9;      /*   fourth arguments with it     */
    uint64_t headArgPrev;   /* last one that vetted clean     */
    uint64_t headPtr;       /* for FN_VIS                     */
    uint64_t placedAt;      /* last successful placement      */
    uint64_t visAt;         /* last visibility call           */
    uint8_t  skip[4];       /* 0 menu 1 drone 2 fresh 3 ads   */
    uint8_t  bodyVis;       /* flicker counter, see S_VIS     */
    uint8_t  want;          /* 1 while first person is asked  */
    uint8_t  hidden;        /* last thing we told the head    */
    uint8_t  bodyOk;        /* body reading has landed once   */
    /* MEASURE 2026-09-29: which aim site wrote the gate, and what
     * it wrote. Two engine call sites write skip[3], and a burst
     * can only be read for what it is if the two are told apart.
     * Taken out with the measurement block below. */
    uint8_t  adsIn;         /* S_ADS_IN  last value           */
    uint8_t  adsOut;        /* S_ADS_OUT last value           */
} FpState;

static volatile FpState g_fp;
static volatile int     g_ready = 0;
static volatile int     g_tried = 0;
static volatile uint32_t g_miss = 0;
/* The body and shoulder patches are a separate install, so a
 * build that disagrees with them can leave them out. */
static volatile int     g_exTried = 0;
static volatile uint32_t g_exMiss = 0;

/* Why the last frame placed nothing. Read from the plugin so a
 * view that stays third person can say why. */
static volatile int     g_bow = 0;

/* The camera position has TWO states, and only the community table's: on an
 * ordinary frame it is our own eye plus the user's offset, and on an aim's
 * frame the engine's own aim camera stands. The switch between them is the
 * engine's own ADS byte, read as it is, on the frame itself - see the branch
 * in ShFp2PlaceEye.
 *
 * That is the table's camera hook, reproduced line for line, and it is what
 * gives the first person view its feel while aiming.
 *
 * The wrong turns are worth naming, because each one is a thing someone will
 * want to try again:
 *
 *   - until 2026-09-26 an aim handed the whole frame over with a latch. The
 *     latch held past the byte and showed the exit animation, third person.
 *
 *   - from 2026-09-26 to 2026-09-29 the frame was kept and the engine's aim
 *     POSITION was borrowed, switched by distance thresholds. It flashed: a
 *     threshold on a per-frame distance is crossed by ordinary play, and a
 *     one-frame move between two cameras is a flash. 600 mm was visible as a
 *     flash, 250 mm was not, but "not seen" is not "cannot happen" - the field
 *     report of 2026-09-29 is a burst of hand-offs inside one aim.
 *
 *   - a fov hysteresis (FOV_AIM_IN / FOV_AIM_OUT) was in the decision as well,
 *     and it flashed at each edge.
 *
 * So: no grace, no threshold that can drift, no borrowing the engine's aim
 * position as a middle form. The byte the engine writes is the one signal, and
 * it is read raw. See docs/beta-audit-first-public-beta.md 13.66.
 *
 * 2026-09-29, later: and the byte is no longer the signal for the camera at
 * all. The floor is that the frame never changes hands - a hand-off is one
 * frame of another picture, and that is the flash, whether it arrives once or
 * in the bursts above. The byte is still read, by the diagnostics and by the
 * A/B knob (g_aimHold), and the aim's two edges still reach the log. What the
 * floor costs is at the handover branch in ShFp2PlaceEye, not here.
 */
static volatile int  g_aimArm;        /* OBSERVED ONLY: the fov says an aim is up */
/* Aiming is armed by the engine's fov: a gameplay view holds 0.78 to 0.83 and
 * an aim takes it below 0.75 - a pistol's iron sights land around 0.70, a 4x
 * optic at 0.49 (measured live, 2026-09-29). g_aimArm is DIAGNOSTIC ONLY: it is
 * read by ShFp2AimProbe(10) and by the Meas heartbeat, and it decides NOTHING
 * about the camera. No camera line may read it. Kept because the aim's edges
 * are what the field reports are read against. */
#define FOV_AIM_IN   0.78f
#define FOV_AIM_OUT  0.80f
/* The distance, in millimetres, between the engine's own camera position and the
 * eye this session computes, measured once per frame just before the eye is
 * written over it. READ ONLY, DIAGNOSTIC ONLY: it is never consulted and must
 * never be, or it is a second position source and the flash comes back with
 * whatever threshold reads it (see the note at g_aimArm). Written on every
 * frame, an aim's included and a hip shot's included, so the number stands on
 * its own as a measurement - a hip shot reads the engine camera a metre or two
 * behind the shoulder, which is itself worth having. Cheap and bounded by
 * construction: only the sum of squares is formed, no square root, so there is
 * nothing here that can be slow or overflow. Read out through
 * ShFp2AimProbe(8). */
static volatile int  g_lastGapMm;
/* The first few placements are written to the log, step by
 * step. After that it goes quiet: this runs every frame. */
static uint32_t         g_trace = 0;

enum {
    BOW_NONE = 0,   /* placed                                  */
    BOW_OFF,        /* first person not asked for              */
    BOW_MENU, BOW_DRONE, BOW_ADS, BOW_STALE,
    BOW_ARG,        /* no captured argument at all             */
    BOW_BAD         /* the engine's answer was not a position  */
};

extern int ShReadableAddr(uint64_t addr, size_t len);
extern int ShReadMem(uint64_t addr, void *out, size_t len);
extern void *ShAllocNear(uint64_t target);
extern void ShSetError(int err);
/* Read only, no state tracking: in the world with no screen up. See
 * scripthook_state.c. */
extern int ShInLivePlay(void);

/* ---- byte level helpers ---------------------------------- */

/* Reads. The heap path goes through the kernel read, which
 * fails clean on a page the engine decommits between the
 * check and the copy - the plain memcpy after VirtualQuery
 * that was here before had exactly that race, and repeated
 * menu and map transitions were where it lost. The image is
 * exempt: it lives as long as the process, so it reads
 * directly. */
static uint64_t RdQ(uint64_t addr) {
    uint64_t v = 0;

    if (!addr) return 0;
    if (ShReadMem(addr, &v, 8)) return v;
    if (ShReadableAddr(addr, 8)) {
        memcpy(&v, (const void *)(uintptr_t)addr, 8);
        return v;
    }
    return 0;
}

static int RdF(uint64_t addr, float *out, int n) {
    if (!addr || n < 1 || n > 4) return 0;
    if (ShReadMem(addr, out, (size_t)n * 4)) return 1;
    if (!ShReadableAddr(addr, (size_t)n * 4)) return 0;
    memcpy(out, (const void *)(uintptr_t)addr, (size_t)n * 4);
    return 1;
}

/* Where a call or a jmp at this site goes. */
static uint64_t SiteTarget(uint64_t site) {
    const uint8_t *at = (const uint8_t *)(uintptr_t)site;
    return (uint64_t)((int64_t)site + 5 + *(const int32_t *)(at + 1));
}

/* Refuse anything but the bytes we know: a changed build has
 * to keep its own behaviour, not take a patch meant for
 * another one. */
static int Check(uint64_t site, int len, const uint8_t *sig) {
    uint8_t at[16];

    if (len < 0 || len > 16) return 0;
    if (!ShReadableAddr(site, (size_t)len)) return 0;
    memcpy(at, (const void *)(uintptr_t)site, (size_t)len);
    if (sig && memcmp(at, sig, (size_t)len) != 0) return 0;
    return 1;
}

/* Tail of every stub: a jump that lands past the site. */
static int EmitJmp(uint8_t *s, int o, uint64_t from, uint64_t to) {
    int64_t rel = (int64_t)to - ((int64_t)from + 5);

    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) return -1;
    s[o++] = 0xE9;
    *(int32_t *)(s + o) = (int32_t)rel;
    return o + 4;
}

static void EmitMov64(uint8_t *s, int *o, uint8_t opcode, uint64_t addr) {
    int i = *o;
    s[i++] = 0x48;
    s[i++] = opcode;                 /* B8 rax, B9 rcx */
    *(uint64_t *)(s + i) = addr;
    i += 8;
    *o = i;
}

/* A call site keeps its opcode and only has its rel32
 * redirected, so the stub is entered with the return address
 * already on the stack and leaves by tail jumping to the
 * original target, which returns past the site. */
static int PatchCall(uint64_t site, uint64_t want, uint8_t *stub) {
    uint8_t *at = (uint8_t *)(uintptr_t)site;
    int64_t rel = (int64_t)(uintptr_t)stub - ((int64_t)site + 5);
    DWORD old;

    if (!ShReadableAddr(site, 5)) return 0;
    if (at[0] != 0xE8) return 0;
    if (SiteTarget(site) != want) return 0;
    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) return 0;
    if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *(int32_t *)(at + 1) = (int32_t)rel;
    VirtualProtect(at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    return 1;
}

/* Any other site is overwritten with a jump and padded, so the
 * instruction boundary after it does not move. */
static int PatchJmp(uint64_t site, int len, uint8_t *stub) {
    uint8_t *at = (uint8_t *)(uintptr_t)site;
    uint8_t patch[16];
    int64_t rel = (int64_t)(uintptr_t)stub - ((int64_t)site + 5);
    DWORD old;

    if (len < 5 || len > 16) return 0;
    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) return 0;
    memset(patch, 0x90, (size_t)len);
    patch[0] = 0xE9;
    memcpy(patch + 1, &rel, 4);
    if (!VirtualProtect(at, (size_t)len, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(at, patch, (size_t)len);
    VirtualProtect(at, (size_t)len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, (size_t)len);
    return 1;
}

/* An unconditional branch the engine should never take. */
static int PatchNop(uint64_t site, int len) {
    uint8_t *at = (uint8_t *)(uintptr_t)site;
    uint8_t patch[16];
    DWORD old;

    if (len < 1 || len > 16) return 0;
    if (!ShReadableAddr(site, (size_t)len)) return 0;
    memset(patch, 0x90, (size_t)len);
    if (!VirtualProtect(at, (size_t)len, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(at, patch, (size_t)len);
    VirtualProtect(at, (size_t)len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, (size_t)len);
    return 1;
}

static uint8_t *NewStub(uint64_t nearSite) {
    uint8_t *s = (uint8_t *)ShAllocNear(nearSite);
    if (s) memset(s, 0xCC, 0x1000);
    return s;
}

/* ---- the capture site ------------------------------------
 * The engine runs the head call once per character, not once
 * per frame. A stub that keeps only the last set of arguments
 * therefore keeps whoever was processed last - fine alone, and
 * in a squad it is why the eye wandered onto a teammate and
 * back. So the stub keeps the last eight, rcx with its r8 and
 * r9, and the placement picks the one that sits where the
 * local player is.
 *
 * Layout, fixed so the stub can address it without help - the two
 * strides follow RING_SLOTS, so the sizes here and the offsets the
 * stub emits cannot drift apart:
 *   +0x0000  uint64 rcx of the calls that hashed here
 *   +0x0800  uint64 r8 of the same calls   (RING_SLOTS * 8)
 *   +0x1000  uint64 r9 of the same calls   (RING_SLOTS * 16)
 *   +0x1800  uint64 the frame it was captured in (RING_SLOTS * 24)
 *
 * No counter, no flag: the stub would have to read, bump and
 * write a shared index on every call, and all three crashes
 * landed on exactly that instruction. The slot comes from the
 * argument itself instead - mixed bits of the heap pointer,
 * so two calls that hash alike simply overwrite each other.
 * The placement walks every slot and picks the one nearest
 * the player, which is why a squad of four gets sixty four
 * slots and a stir of the pointer's upper bits: eight slots
 * let two soldiers of the same squad collide every other
 * frame, and the eye went wandering onto whoever wrote last.
 *
 * Sixty four was still not enough in co-op: with four soldiers the
 * local capture was missing often enough, frame by frame, for the
 * placement below to fall through to a squadmate - the flicker the
 * field reported on 2026-09-18 (it stopped as soon as the squad was
 * far away). The ring is 256 slots now, which is a wider spread of
 * the same hash and no extra work in the stub: still one store per
 * call, still no read and no bump.
 */
#define RING_SLOTS   256
static volatile struct {
    uint64_t a0[RING_SLOTS];              /* +0x0000 */
    uint64_t a2[RING_SLOTS];              /* +0x0800 */
    uint64_t a3[RING_SLOTS];              /* +0x1000 */
    uint64_t t [RING_SLOTS];              /* +0x1800  the stamp it was written */
} g_ring;

/* The stamp the ring entries carry: the generation in the top bits and
 * GetTickCount64() below it. The placement writes this one word every
 * frame and the stub copies it into the slot along with the capture -
 * the emitted code below loads it and stores it, so nothing about the
 * engine's path changes with its meaning.
 *
 * Milliseconds rather than a frame count (2026-09-19). A frame count is
 * only meaningful while the placement runs, and the ring is filled by
 * the engine's own per-character calls, not by us: an entry the engine
 * had written two seconds earlier was thrown away as too old, and in
 * the field log of 2026-09-18 that is what "the eye is never placed
 * again" looked like - one successful pick at 19:15, then nothing for
 * four hours. What the stamp has to catch is a world that has been
 * replaced, and that is the generation, not an entry a few seconds old.
 */
#define RING_GEN_SHIFT 40
#define RING_GEN_MASK  (((uint64_t)1 << RING_GEN_SHIFT) - 1)
static volatile uint64_t g_ringStamp;
static volatile uint32_t g_ringGen = 1;

/* Written from the placement, once a frame, and from Fp2Forget when the
 * generation it carries is bumped. */
static void RingStampNow(void) {
    g_ringStamp = ((uint64_t)g_ringGen << RING_GEN_SHIFT)
                | (GetTickCount64() & RING_GEN_MASK);
}

/* 1 while the head belongs to the engine's own state, 0 while
 * first person holds it down. ShFp2Enable turns it off - taking
 * the camera means taking the head - and the plugin hands it
 * back through ShFp2HeadShow. */
static volatile int g_headShow = 1;

/* While non zero, a show window: the camera frame restates
 * "head visible" until it runs out. Handing the head back is
 * not a moment but a span - the engine's own idea of the head
 * lags the switch, and a single call loses to it. */
static volatile uint64_t g_showUntil;

static void HeadVis(int hide);

/* The eye offset's publish stamp, see ShFp2SetOffset. */
static volatile uint32_t g_offSeq;

static int InstallArgs(void) {
    static const uint8_t sig[1] = { 0xE8 };
    uint8_t *s;
    int o = 0, i;

    if (!Check(S_ARGS, 1, sig)) return 0;
    s = NewStub(S_ARGS);
    if (!s) return 0;

    /* Only rdx is borrowed and put back, and r10 - which the
     * call convention lets anything clobber anyway - holds the
     * ring's address. Nothing is read from memory and nothing
     * is read-modify-write: three plain aligned stores, which
     * is all a capture needs to be.
     */
    s[o++] = 0x52;                                  /* push rdx            */
    s[o++] = 0x49; s[o++] = 0xBA;                   /* mov r10, imm64      */
    *(uint64_t *)(s + o) = (uint64_t)(uintptr_t)&g_ring; o += 8;
    s[o++] = 0x89; s[o++] = 0xCA;                   /* mov edx,ecx         */
    s[o++] = 0xC1; s[o++] = 0xEA; s[o++] = 0x04;    /* shr edx,4           */
    s[o++] = 0x31; s[o++] = 0xCA;                   /* xor edx,ecx         */
    s[o++] = 0xC1; s[o++] = 0xEA; s[o++] = 0x04;    /* shr edx,4           */
    /* movzx edx,dl: the slot, 0..RING_SLOTS-1. Taking the low byte is
     * deliberate - `and edx,imm8` sign-extends, so 0xFF would mask
     * nothing and index straight past the ring, and `and edx,imm32`
     * is two bytes longer. Three bytes, the same as the 64-slot
     * version used. */
    s[o++] = 0x0F; s[o++] = 0xB6; s[o++] = 0xD2;    /* movzx edx,dl  slot  */
    s[o++] = 0x49; s[o++] = 0x89; s[o++] = 0x0C;    /* mov [r10+rdx*8],    */
    s[o++] = 0xD2;                                  /*   rcx               */
    /* REX is 4D here, not 49: with the register number of r8/r9 the R
     * bit is part of it (0100 WRXB), and 49 leaves R clear - which
     * quietly stored rax and rcx instead of r8 and r9. The trace line
     * made it visible (a2 = ffffffffffffffff, a3 = the argument
     * itself), and those two values are handed to the engine's head
     * call, so this was the ring carrying two wrong arguments. */
    s[o++] = 0x4D; s[o++] = 0x89; s[o++] = 0x84;    /* mov [r10+rdx*8+a2], */
    s[o++] = 0xD2;
    *(uint32_t *)(s + o) = (uint32_t)(RING_SLOTS * 8); o += 4;   /* r8  */
    s[o++] = 0x4D; s[o++] = 0x89; s[o++] = 0x8C;    /* mov [r10+rdx*8+a3], */
    s[o++] = 0xD2;
    *(uint32_t *)(s + o) = (uint32_t)(RING_SLOTS * 16); o += 4;  /* r9  */
    /* And the frame it was captured in: a plain load of a counter this
     * side owns, then a plain store. No read-modify-write in the stub -
     * that is the instruction every earlier crash landed on. r11 is
     * volatile in the x64 convention, so borrowing it costs nothing. */
    s[o++] = 0x49; s[o++] = 0xBB;                   /* mov r11, imm64      */
    *(uint64_t *)(s + o) = (uint64_t)(uintptr_t)&g_ringStamp; o += 8;
    s[o++] = 0x4D; s[o++] = 0x8B; s[o++] = 0x1B;    /* mov r11,[r11]       */
    s[o++] = 0x4D; s[o++] = 0x89; s[o++] = 0x9C;    /* mov [r10+rdx*8+t],  */
    s[o++] = 0xD2;
    *(uint32_t *)(s + o) = (uint32_t)(RING_SLOTS * 24); o += 4;  /* r11 */
    s[o++] = 0x5A;                                  /* pop rdx             */
    o = EmitJmp(s, o, (uint64_t)(uintptr_t)s + o, S_ARGS_FN);
    if (o < 0) return 0;
    FlushInstructionCache(GetCurrentProcess(), s, (size_t)o);

    /* The stub's own bytes, so a crash dump at the fault has
     * something authoritative to be checked against. */
    for (i = 0; i < o; i += 24) {
        char hex[80];
        int j, k = 0;

        for (j = i; j < o && j < i + 24; j++)
            k += snprintf(hex + k, sizeof(hex) - k, " %02X", s[j]);
        Log("args stub@%03d:%s", i, hex);
    }

    /* What the site calls, before trusting it. After a game update this is
     * the check that fails, and the two addresses in the log say whether
     * the site moved or the function it calls did. */
    Log("args site: call -> %llX, want %llX %s",
        (unsigned long long)SiteTarget(S_ARGS),
        (unsigned long long)S_ARGS_FN,
        SiteTarget(S_ARGS) == S_ARGS_FN ? "ok" : "MISMATCH");

    if (!PatchCall(S_ARGS, S_ARGS_FN, s)) return 0;

    /* Read the site back. The patch was written, and this says
     * it survived - a rel32 that points anywhere but the stub
     * is a bug caught at install time, not a mystery later. */
    {
        const uint8_t *at = (const uint8_t *)(uintptr_t)S_ARGS;
        int32_t rel = *(const int32_t *)(at + 1);
        uint64_t got = S_ARGS + 5 + (int64_t)rel;

        Log("args site: E8 rel->%llX (stub %llX) %s",
            (unsigned long long)got,
            (unsigned long long)(uintptr_t)s,
            got == (uint64_t)(uintptr_t)s ? "ok" : "MISMATCH");
    }
    return 1;
}

/* ---- the gates -------------------------------------------
 * Each one replays the instruction it displaced, then records
 * what that instruction told us. rax is borrowed and restored;
 * where two scratch registers are needed r11 is borrowed too,
 * since a stub in the middle of a function may not disturb
 * anything the surrounding code still has live.
 */
static int InstallMenu1(void) {
    static const uint8_t sig[6] = { 0xFF, 0x87, 0x6C, 0x05, 0x00, 0x00 };
    uint8_t *s;
    int o = 0;

    if (!Check(S_MENU1, 6, sig)) return 0;
    s = NewStub(S_MENU1);
    if (!s) return 0;

    s[o++] = 0x50;                                  /* push rax */
    memcpy(s + o, sig, 6); o += 6;                  /* inc [rdi+56C] */
    EmitMov64(s, &o, 0xB8, (uint64_t)(uintptr_t)&g_fp.skip[0]);
    s[o++] = 0xFE; s[o++] = 0x00;                   /* inc byte [rax] */
    EmitMov64(s, &o, 0xB8, (uint64_t)(uintptr_t)&g_fp.bodyVis);
    s[o++] = 0xFE; s[o++] = 0x00;
    s[o++] = 0x58;                                  /* pop rax  */
    o = EmitJmp(s, o, (uint64_t)(uintptr_t)s + o, S_MENU1 + 6);
    if (o < 0) return 0;

    return PatchJmp(S_MENU1, 6, s);
}

/* Shared by the two decrementing sites: the counter is read
 * back after the store, which is what the table does, so the
 * gate holds the live count rather than a running tally. */
static int InstallMenu(uint64_t site, int clear) {
    static const uint8_t sig[6] = { 0xFF, 0x8B, 0x6C, 0x05, 0x00, 0x00 };
    uint8_t *s;
    int o = 0;

    if (!Check(site, 6, sig)) return 0;
    s = NewStub(site);
    if (!s) return 0;

    s[o++] = 0x50;                                  /* push rax */
    s[o++] = 0x41; s[o++] = 0x53;                   /* push r11 */
    memcpy(s + o, sig, 6); o += 6;                  /* dec [rbx+56C] */
    s[o++] = 0x0F; s[o++] = 0xB6;                   /* movzx eax,   */
    s[o++] = 0x83;                                  /*   byte [rbx+56C] */
    *(uint32_t *)(s + o) = 0x0000056Cu; o += 4;
    s[o++] = 0x49; s[o++] = 0xBB;                   /* mov r11, imm */
    *(uint64_t *)(s + o) = (uint64_t)(uintptr_t)&g_fp.skip[0]; o += 8;
    s[o++] = 0x41; s[o++] = 0x88; s[o++] = 0x03;    /* mov [r11],al */
    s[o++] = 0x49; s[o++] = 0xBB;
    *(uint64_t *)(s + o) = (uint64_t)(uintptr_t)&g_fp.bodyVis; o += 8;
    s[o++] = 0x41; s[o++] = 0xFE; s[o++] = 0x03;    /* inc byte [r11] */
    if (clear) {
        /* A menu that just closed leaves the capture stale:
         * drop the saved argument and the head pointer with
         * it, so the next frame waits for a fresh one. */
        s[o++] = 0x49; s[o++] = 0xBB;
        *(uint64_t *)(s + o) = (uint64_t)(uintptr_t)&g_fp.headArgPrev;
        o += 8;
        s[o++] = 0x49; s[o++] = 0xC7; s[o++] = 0x03;/* mov qword [r11],0 */
        *(uint32_t *)(s + o) = 0; o += 4;
        s[o++] = 0x49; s[o++] = 0xBB;
        *(uint64_t *)(s + o) = (uint64_t)(uintptr_t)&g_fp.headPtr; o += 8;
        s[o++] = 0x49; s[o++] = 0xC7; s[o++] = 0x03;
        *(uint32_t *)(s + o) = 0; o += 4;
    }
    s[o++] = 0x41; s[o++] = 0x5B;                   /* pop r11 */
    s[o++] = 0x58;                                  /* pop rax  */
    o = EmitJmp(s, o, (uint64_t)(uintptr_t)s + o, site + 6);
    if (o < 0) return 0;

    return PatchJmp(site, 6, s);
}

static int InstallDrone(void) {
    static const uint8_t sig[6] = { 0x88, 0x9F, 0x1A, 0x18, 0x00, 0x00 };
    uint8_t *s;
    int o = 0;

    if (!Check(S_DRONE, 6, sig)) return 0;
    s = NewStub(S_DRONE);
    if (!s) return 0;

    s[o++] = 0x50;
    memcpy(s + o, sig, 6); o += 6;                  /* mov [rdi+181A],bl */
    EmitMov64(s, &o, 0xB8, (uint64_t)(uintptr_t)&g_fp.skip[1]);
    s[o++] = 0x88; s[o++] = 0x18;                   /* mov [rax],bl */
    s[o++] = 0x58;
    o = EmitJmp(s, o, (uint64_t)(uintptr_t)s + o, S_DRONE + 6);
    if (o < 0) return 0;

    return PatchJmp(S_DRONE, 6, s);
}

/* Both aim sites call the same visibility function; the dl
 * leaving the engine is the aim state, so it is simply read
 * off on the way through.
 *
 * own is where this site's own copy of that byte goes, so the
 * measurement can tell the two apart - see Meas. Nothing reads it
 * outside that block. */
static int InstallAds(uint64_t site, volatile uint8_t *own) {
    static const uint8_t sig[1] = { 0xE8 };
    uint8_t *s;
    int o = 0;

    if (!Check(site, 1, sig)) return 0;
    s = NewStub(site);
    if (!s) return 0;

    s[o++] = 0x50;
    EmitMov64(s, &o, 0xB8, (uint64_t)(uintptr_t)&g_fp.skip[3]);
    s[o++] = 0x88; s[o++] = 0x10;                   /* mov [rax],dl */
    /* MEASURE 2026-09-29: the same dl, on this site's own byte. */
    EmitMov64(s, &o, 0xB8, (uint64_t)(uintptr_t)own);
    s[o++] = 0x88; s[o++] = 0x10;                   /* mov [rax],dl */
    /* While first person holds the head down, this call is
     * also the engine speaking its own mind about the head -
     * and on the way out of an aim that mind says visible,
     * which is a frame of skull before our next frame says
     * otherwise. The call goes through either way; only its
     * answer is ours to correct. */
    EmitMov64(s, &o, 0xB8, (uint64_t)(uintptr_t)&g_headShow);
    s[o++] = 0x80; s[o++] = 0x38; s[o++] = 0x00;    /* cmp byte [rax],0 */
    s[o++] = 0x75; s[o++] = 0x02;                   /* jne +2           */
    s[o++] = 0xB2; s[o++] = 0x01;                   /* mov dl,1         */
    /* The engine names the head right here: this call takes it
     * in rcx. Remembering it beats walking the chain ourselves,
     * which is a guess about a layout that only the engine
     * knows for certain. */
    EmitMov64(s, &o, 0xB8, (uint64_t)(uintptr_t)&g_fp.headPtr);
    s[o++] = 0x48; s[o++] = 0x89; s[o++] = 0x08;    /* mov [rax],rcx */
    s[o++] = 0x58;
    o = EmitJmp(s, o, (uint64_t)(uintptr_t)s + o, FN_VIS);
    if (o < 0) return 0;

    return PatchCall(site, FN_VIS, s);
}

/* The body reading, kept to vet the head against. xmm0 is
 * borrowed and put back: at this call site it may well be an
 * argument, and the table's willingness to clobber it is not
 * something we need to copy. */
static int InstallBody(void) {
    static const uint8_t sig[1] = { 0xE8 };
    uint8_t *s;
    int o = 0;

    if (!Check(S_BODY, 1, sig)) return 0;
    s = NewStub(S_BODY);
    if (!s) return 0;

    s[o++] = 0x50;                                  /* push rax */
    s[o++] = 0x48; s[o++] = 0x83; s[o++] = 0xEC;    /* sub rsp,10 */
    s[o++] = 0x10;
    s[o++] = 0x0F; s[o++] = 0x11;                   /* movups [rsp],xmm0 */
    s[o++] = 0x04; s[o++] = 0x24;
    s[o++] = 0x48; s[o++] = 0x8B;                   /* mov rax,[rcx+3F8] */
    s[o++] = 0x81;
    *(uint32_t *)(s + o) = 0x000003F8u; o += 4;
    s[o++] = 0x48; s[o++] = 0x85; s[o++] = 0xC0;    /* test rax,rax */
    s[o++] = 0x74; s[o++] = 0x11;                   /* je +17       */
    s[o++] = 0x0F; s[o++] = 0x10;                   /* movups xmm0, */
    s[o++] = 0x40; s[o++] = 0x20;                   /*   [rax+20]   */
    EmitMov64(s, &o, 0xB8, (uint64_t)(uintptr_t)&g_fp.bodyPos);
    s[o++] = 0x0F; s[o++] = 0x11; s[o++] = 0x00;    /* movups [rax],xmm0 */
    s[o++] = 0x0F; s[o++] = 0x10;                   /* movups xmm0,[rsp] */
    s[o++] = 0x04; s[o++] = 0x24;
    s[o++] = 0x48; s[o++] = 0x83; s[o++] = 0xC4;    /* add rsp,10 */
    s[o++] = 0x10;
    s[o++] = 0x58;                                  /* pop rax     */
    s[o++] = 0x50;                                  /* push rax    */
    EmitMov64(s, &o, 0xB8, (uint64_t)(uintptr_t)&g_fp.bodyOk);
    s[o++] = 0xC6; s[o++] = 0x00; s[o++] = 0x01;    /* mov byte [rax],1 */
    s[o++] = 0x58;                                  /* pop rax     */
    o = EmitJmp(s, o, (uint64_t)(uintptr_t)s + o, S_BODY_FN);
    if (o < 0) return 0;

    return PatchCall(S_BODY, S_BODY_FN, s);
}

/* Body visibility. The table lets one frame through marked
 * invisible so the engine builds the pointers it will later
 * be asked about; that is what the flicker counter is for. */
static int InstallVis(void) {
    static const uint8_t sig[8] = {
        0x0F, 0xB6, 0x40, 0x58, 0x48, 0x83, 0xC4, 0x20
    };
    uint8_t *s;
    int o = 0;

    if (!Check(S_VIS, 8, sig)) return 0;
    s = NewStub(S_VIS);
    if (!s) return 0;

    s[o++] = 0x51;                                  /* push rcx */
    s[o++] = 0x0F; s[o++] = 0xB6;                   /* movzx eax,     */
    s[o++] = 0x40; s[o++] = 0x58;                   /*   byte [rax+58] */
    s[o++] = 0x48; s[o++] = 0xB9;                   /* mov rcx, imm  */
    *(uint64_t *)(s + o) = (uint64_t)(uintptr_t)&g_fp.bodyVis; o += 8;
    s[o++] = 0x80; s[o++] = 0x39; s[o++] = 0x00;    /* cmp byte [rcx],0 */
    s[o++] = 0x74; s[o++] = 0x07;                   /* je +7        */
    s[o++] = 0xB8;                                  /* mov eax,1    */
    *(uint32_t *)(s + o) = 1u; o += 4;
    s[o++] = 0xFE; s[o++] = 0x09;                   /* dec byte [rcx] */
    s[o++] = 0x59;                                  /* pop rcx     */
    s[o++] = 0x48; s[o++] = 0x83; s[o++] = 0xC4;    /* add rsp,20  */
    s[o++] = 0x20;
    o = EmitJmp(s, o, (uint64_t)(uintptr_t)s + o, S_VIS + 8);
    if (o < 0) return 0;

    return PatchJmp(S_VIS, 8, s);
}

/* ---- placement ------------------------------------------- */

typedef void (__attribute__((ms_abi)) *HeadFn)(uint64_t arg,
                                               uint64_t out,
                                               uint64_t a2,
                                               uint64_t a3);
typedef void (__attribute__((ms_abi)) *VisFn)(uint64_t head, uint64_t hide);

/* [[[arg]] + 0x238], the transform the table reads the head
 * position out of. Guarded all the way: a stale argument is
 * the ordinary case after a respawn, not an error. */
static uint64_t HeadTransform(uint64_t arg) {
    uint64_t a = arg;
    int i;

    /* Two plain dereferences, then the one that carries the
     * 0x238: the table walks [[[rcx]]+0x238], and a third
     * plain step here reads past the transform into whatever
     * the object keeps next - which came out of the log as a
     * code address full of int3 padding. */
    for (i = 0; i < 2; i++) {
        a = RdQ(a);
        if (!a) return 0;
    }
    return RdQ(a + 0x238);
}

/* Of the captures in the ring, the one that sits where the local
 * player is. In a squad the ring holds the whole squad, so metres
 * from the player is the test; alone it is trivially the only entry.
 * Returns 0 when nothing is close enough, which is the caller's cue
 * to fall back to the capture it already had.
 *
 * The radius is tight on purpose. It used to be 12 m, which is wider
 * than a squad walks apart: whenever the local capture was missing
 * from the ring for a frame, the nearest entry was a teammate and the
 * eye went to them and back - the co-op flicker of 2026-09-18, which
 * stopped as soon as the squad moved away. A local head sits about
 * head-height from the position the game reports, so a few metres is
 * all the room this needs.
 */
#define PICK_LOCAL_M 4.0f

/* The capture the picker last chose, and when. A frame in which the
 * ring holds nothing that resolves is not a reason to lose the eye: the
 * capture used a moment ago is still this player's own character, and
 * being without it for that frame is what the field reads as a flicker.
 *
 * Three things guard it and all three must hold before it is used: the
 * generation (so it can never reach across the world change that the
 * 2026-09-18 crash was), the window (a capture that was retired stays
 * retired), and HeadTransform (a freed object is never handed to the
 * engine). g_pickHolds counts the uses, because that count is what says
 * whether keeping the ring entries actually removed the flicker. */
#define PICK_HOLD_MS 1500
static volatile uint64_t g_pickHold;
static volatile uint64_t g_pickHoldA2;
static volatile uint64_t g_pickHoldA3;
static volatile uint64_t g_pickHoldAt;
static volatile uint32_t g_pickHoldGen;
static volatile uint64_t g_pickHolds;

/* The eye the placement last wrote, and when, so that a frame with no
 * capture at all can draw the one before it instead of the engine's.
 *
 * The field report of 2026-09-26 is "aiming, and one frame of something
 * else flashes past", and the 2026-09-28 log names the shape exactly: the
 * two lines
 *
 *   [2026-09-28 15:46:20.329] bow: ours        -> no-argument
 *   [2026-09-28 15:46:20.346] bow: no-argument -> ours
 *
 * are 17 ms apart - one frame - and the direction is ours, then the frame
 * handed away, then ours again. That middle frame is one the engine's own
 * camera drew, and on screen it is "another picture" for 1/60 s: the
 * flicker the report describes. All three capture fallbacks failed on it -
 * the ring had nothing that resolved (PickLocalCapture), the last pick was
 * outside its window (g_pickHold), and the vetted argument was gone too
 * (g_fp.headArgPrev) - so the code below bowed out and left the frame to
 * the engine.
 *
 * One frame of difference is invisible here. Running crosses about 0.13 m
 * in a frame, so redrawing the previous eye two or three frames running
 * moves the camera by a few centimetres - a hidden step, not a picture.
 * And it is our own previous frame: this is NOT the latch that was tried
 * and reverted on 2026-09-21, which held the frame with the ENGINE and so
 * showed the engine's own way out of the aim in third person. Here the
 * frame is never handed over; it is the frame we drew, drawn once more.
 *
 * Bounded on purpose, and this is the whole of it: g_eyeReplayAt is
 * stamped only by a placement that used a real capture, never by a replay
 * itself, so a run of replays is anchored to the last real one and dies
 * EYE_REPLAY_MS later however many of them there were. A capture that is
 * gone stays gone; what is replayed cannot outlive the window by a frame,
 * let alone latch. The menu, the drone and a replaced world clear it (see
 * Fp2DropEyeReplay), because those frames are the engine's on purpose and
 * what is remembered behind them is stale.
 *
 * g_eyeReplays counts the replays, the way g_pickHolds counts the holds:
 * the count is what says whether this removed the flicker or only moved
 * it. Read out through ShFp2AimProbe(11).
 */
static volatile float    g_eyeReplay[3];
static volatile uint64_t g_eyeReplayAt;
static volatile uint64_t g_eyeReplays;

/* How long after the last real placement the eye may be redrawn. 80 ms is
 * two to three frames at 30 to 60 fps: long enough to span the run of
 * frames in which every fallback is missing at once, short enough that the
 * step it leaves behind is a few centimetres at a sprint - see the note
 * above. */
#define EYE_REPLAY_MS 80u

/* Forget the eye the placement last wrote. Called wherever the frame is
 * deliberately the engine's - a menu, the drone, a world replaced - so
 * that the first frame after it is placed by this session and not by the
 * one before. See g_eyeReplay. */
static void Fp2DropEyeReplay(void) {
    g_eyeReplayAt = 0;
}

static uint64_t PickLocalCapture(const ShVec3 *me,
                                 uint64_t *outA2, uint64_t *outA3) {
    uint64_t arg = 0, a2 = 0, a3 = 0;
    uint64_t cur = g_ringStamp;      /* one read for the whole sweep */
    float best = PICK_LOCAL_M * PICK_LOCAL_M;
    int i;

    for (i = 0; i < RING_SLOTS; i++) {
        uint64_t a = g_ring.a0[i];
        uint64_t t2;
        float p[3], dx, dy, dz, d;

        if (!a) continue;
        /* Retired with its generation: the world this slot was written in
         * has been replaced since. Age is deliberately not a test here -
         * the engine writes a slot once per character as the world is
         * built, so an entry an hour old is still this session's own, and
         * a window that rejected it was what left the eye with nothing on
         * 2026-09-18 and again on 09-19. */
        if ((g_ring.t[i] >> RING_GEN_SHIFT) != (cur >> RING_GEN_SHIFT))
            continue;
        t2 = HeadTransform(a);
        if (!t2) continue;
        if (!RdF(t2, p, 3)) continue;
        if (p[0] != p[0] || p[1] != p[1]) continue;
        dx = p[0] - me->x;
        dy = p[1] - me->y;
        dz = p[2] - me->z;
        d = dx * dx + dy * dy + dz * dz;
        if (d >= best) continue;
        best = d;
        arg = a;
        a2 = g_ring.a2[i];
        a3 = g_ring.a3[i];
    }
    *outA2 = a2;
    *outA3 = a3;
    return arg;
}

/* Once every couple of seconds while nothing is being picked,
 * say what the ring actually holds - so an empty ring, a chain
 * that will not resolve and a distance that vetoes are told
 * apart in the log instead of all reading as the same zero. */
static void RingTrace(const ShVec3 *me) {
    static uint64_t lastAt;
    uint64_t now = GetTickCount64(), cur = g_ringStamp;
    int slot[8], used = 0, i, n;
    char line[300];

    if (now - lastAt < 2000) return;
    lastAt = now;

    for (i = 0; i < RING_SLOTS && used < 8; i++)
        if (g_ring.a0[i]) slot[used++] = i;

    n = snprintf(line, sizeof line,
                 "ring me=%.1f,%.1f,%.1f live=%d:", me->x, me->y, me->z,
                 used);
    for (i = 0; i < used && n > 0; i++)
        n += snprintf(line + n, sizeof(line) - n, " %d:%llX",
                      slot[i], (unsigned long long)g_ring.a0[slot[i]]);
    if (n > 0) Log("%s", line);

    /* Each entry's own reason, so the three ways of picking nothing -
     * retired, unresolvable, too far - are told apart in the log
     * instead of all reading as the same zero. */
    n = snprintf(line, sizeof line, "ring p:");
    for (i = 0; i < used && n > 0; i++) {
        uint64_t a = g_ring.a0[slot[i]], t = g_ring.t[i], t2;
        float p[3] = { 0, 0, 0 };

        if ((t >> RING_GEN_SHIFT) != (cur >> RING_GEN_SHIFT)) {
            n += snprintf(line + n, sizeof(line) - n, " retired(%ums)",
                          (unsigned)(cur - t));
            continue;
        }
        t2 = HeadTransform(a);
        if (t2 && RdF(t2, p, 3))
            n += snprintf(line + n, sizeof(line) - n,
                          " %.1f,%.1f,%.1f@%ums", p[0], p[1], p[2],
                          (unsigned)(cur - t));
        else
            n += snprintf(line + n, sizeof(line) - n, " -@%ums",
                          (unsigned)(cur - t));
    }
    if (n > 0) Log("%s", line);
}

/* The chain is walked one step at a time and each step is named,
 * because "the head could not be found" is not something that can
 * be acted on: which link came back empty is. Time throttled,
 * not counted: a count of eight was spent in the first second
 * of one bad state and every state after it went unlogged. */
static uint64_t g_hpLogAt;

static int HpLogReady(void) {
    uint64_t now = GetTickCount64();

    if (now - g_hpLogAt < 2000) return 0;
    g_hpLogAt = now;
    return 1;
}

static uint64_t HeadPtrStop(int step) {
    if (HpLogReady())
        Log("headptr: nothing at step %d", step);
    return 0;
}

/* The visibility call writes into the node it is handed. A
 * node that cannot take that write is not a node this state
 * should be talking to - handing one over is what crashed
 * the map screens. */
static int Writable(uint64_t addr, size_t len) {
    MEMORY_BASIC_INFORMATION mbi;

    if (!addr) return 0;
    if (!VirtualQuery((void *)(uintptr_t)addr, &mbi, sizeof(mbi)))
        return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    if (!(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY)))
        return 0;
    /* ...and it has to be the engine's own heap. An address inside a
     * loaded module is not a node, and an image's data sections are
     * writable, so the protection test above waves them through: two
     * crashes in the head-visibility call (GRW.exe+0x14ED39CD, the
     * `and [node+0x54], bx`) were handed exactly that - an address in
     * GRW.exe's own image, from a tag that named a slot the chain had no
     * business trusting. Engine objects are the process's own private
     * memory. */
    if (mbi.Type != MEM_PRIVATE) return 0;
    return (uint64_t)(uintptr_t)mbi.BaseAddress
           + mbi.RegionSize >= addr + len;
}

/* What the visibility call touches. The flag it writes sits at +0x54, so
 * the checked range has to reach past +0x40 - otherwise the check passes
 * on a node whose page ends in between and the write faults anyway. */
#define HEAD_NODE_WRITE 0x60u

/* The highest slot tag the head chain will act on. The table is small and
 * per state (0x0F on foot, 0x15 around vehicles); a read that lands far
 * outside it is a stale layout, not a state. */
#define HEAD_TAG_MAX 0x20u

/* The head, for the visibility call. A chain the table walks
 * by hand; it ends in a small table indexed by a type tag the
 * site has to match, and a miss is simply "not now". */
static uint64_t HeadPtrChain(void) {
    uint64_t a, c;
    uint16_t tag;

    a = RdQ(HEAD_ROOT); if (!a) return HeadPtrStop(0);
    a = RdQ(a + 0x10);  if (!a) return HeadPtrStop(1);
    a = RdQ(a + 0x10);  if (!a) return HeadPtrStop(2);
    a = RdQ(a);         if (!a) return HeadPtrStop(3);
    a = RdQ(a + 0x78);  if (!a) return HeadPtrStop(4);
    c = RdQ(a + 0x10);  if (!c) return HeadPtrStop(5);
    c = RdQ(c + 0x10);  if (!c) return HeadPtrStop(6);
    if (!ShReadableAddr(c + 3, 2)) return HeadPtrStop(7);
    memcpy(&tag, (const void *)(uintptr_t)(c + 3), 2);
    tag &= 0xFFu;
    /* The tag names the slot. 0x0F on foot and 0x15 around vehicles are
     * the ones that have ever been seen to answer; the table's own check
     * reads "15", which in Cheat Engine's assembler is hex.
     *
     * 0xC7 was taken for a third state because it showed up after a map
     * screen - and that is what has been crashing the game. It indexes the
     * table 199 entries deep, past everything the table holds, and what
     * comes back is not a head node: handed to the visibility call the
     * engine reads a sub-object out of it, gets an address inside GRW.exe's
     * image, and faults writing the flag at +0x54. Four crash reports, and
     * every one of them resolved this tag as 199. A tag that far out is a
     * stale read of a layout that has already gone, not a state: the frame
     * does without instead. */
    if (tag > HEAD_TAG_MAX) {
        if (HpLogReady())
            Log("headptr: tag %u is out of range", (unsigned)tag);
        return 0;
    }
    a = RdQ(a + 0x27);  if (!a) return HeadPtrStop(9);
    a = RdQ(a + (uint64_t)tag * 8u);
    if (!a) {
        if (HpLogReady())
            Log("headptr: tag %u, empty slot", (unsigned)tag);
        return 0;
    }
    if (!Writable(a, HEAD_NODE_WRITE)) {
        if (HpLogReady())
            Log("headptr: tag %u, node not writable", (unsigned)tag);
        return 0;
    }
    return a;
}

/* Only the chain is trusted. It re-derives the node from the engine's live
 * state on every attempt, so what it names is a node as of this frame.
 *
 * There used to be a second answer: the last node that worked, handed back
 * when the chain did not resolve - "the ordinary state in a menu rather
 * than an error". That is what crashed the game on the way into a menu. The
 * engine frees the node when the world goes away, the memory stops reading
 * as a node, and FN_VIS walked into it and faulted writing the flag at
 * +0x54 (AV on write, GRW.exe+0x14ED39CD; three reports, the last one from
 * a perfectly ordinary-looking heap address). No page test can tell a live
 * node from a freed one, so nothing remembered is handed over any more: the
 * frame does without, and the engine's own state governs the head until the
 * chain resolves again - which is exactly what the show window is for.
 *
 * What is remembered is only "the chain last named a node", for
 * ShFp2HeadOk. */
static uint64_t HeadPtr(void) {
    uint64_t a = HeadPtrChain();

    if (!a || !Writable(a, HEAD_NODE_WRITE)) {
        if (a && HpLogReady())
            Log("headptr: %016llX is not a node this frame",
                (unsigned long long)a);
        g_fp.headPtr = 0;
        return 0;
    }
    g_fp.headPtr = a;
    return a;
}

/* Why the last frame placed nothing, said where it happens.
 *
 * A frame that is neither ours nor one of the deliberate handovers is a frame
 * the engine's own camera draws - which is what a flicker in the view IS. The
 * plugin polls this state every 60 ms and cannot see a one-frame value at all,
 * and the field report of 2026-09-26 ("aiming, and one frame of something else
 * flashes past") is exactly that shape. Written only when the value on either
 * side of the change is one of those frames, so the line is never about the
 * ordinary walk between ours, ads, menu and drone.
 *
 * This runs at the top of the camera frame, before the placement of the same
 * frame, so what it compares is last frame's answer. */
static int BowBad(int v) {
    /* OFF is in here too: it is what a frame is called when first person is not
     * asked for ON THAT FRAME, so the switch flickering for one frame is an
     * engine-camera frame like any other. The ordinary case - the player has
     * first person off for the whole session - then reads as a line at each end
     * of it, which is the price of catching the flicker. */
    return v == BOW_STALE || v == BOW_ARG || v == BOW_BAD || v == BOW_OFF;
}

static const char *BowName(int v) {
    switch (v) {
    case BOW_NONE:  return "ours";
    case BOW_OFF:   return "off";
    case BOW_MENU:  return "menu";
    case BOW_DRONE: return "drone";
    case BOW_ADS:   return "ads";
    case BOW_STALE: return "stale";
    case BOW_ARG:   return "no-argument";
    case BOW_BAD:   return "not-a-position";
    }
    return "?";
}

/* The camera frame, whether first person runs or not: hold the
 * head down while first person has it, and restate "visible"
 * through the show window after a handover. */
void ShFp2HeadFrame(void) {
    {
        static int last = -1;

        if (g_bow != last) {
            int was = last;

            last = g_bow;
            if (BowBad(g_bow) || BowBad(was))
                Log("bow: %s -> %s (a frame the engine's own camera drew)",
                    BowName(was), BowName(g_bow));
        }
    }

    if (g_headShow) {
        if (g_showUntil) {
            if (GetTickCount64() < g_showUntil) HeadVis(0);
            else g_showUntil = 0;
        }
    }
}

/* Two readings that should describe the same place. A
 * transform left over from before a respawn agrees with
 * nothing, and a camera placed on it is a camera in the sky. */
static int Sane(uint64_t tf) {
    float h[4];

    if (!g_fp.bodyOk) return 1;      /* nothing to compare against yet */
    if (!RdF(tf, h, 4)) return 0;
    if (h[0] != h[0] || h[1] != h[1]) return 0;
    if (fabsf(h[0] - g_fp.bodyPos[0]) > SANE_TOL) return 0;
    if (fabsf(h[1] - g_fp.bodyPos[1]) > SANE_TOL) return 0;
    return 1;
}

/* The visibility call, every frame, the way the table does it.
 *
 * Throttling it was the obvious thrift and it is what made the
 * head flicker: the engine reasserts its own idea of the head
 * constantly, so a missed frame is a frame the head is back.
 * One call a frame is the price of a head that stays away.
 */
static void HeadVis(int hide) {
    uint64_t head;

    /* Only inside the world. Entering a menu is exactly where the chain
     * goes stale - the tag reads 199 there - and it is where every crash
     * into this call has been. While a screen is up the head is the
     * engine's own business, and it shows it on its own when first person
     * lets go; a show window with nothing to show costs nothing. */
    if (!ShInLivePlay()) return;

    head = HeadPtr();
    if (!head) return;
    g_fp.headPtr = head;
    ((VisFn)(uintptr_t)FN_VIS)(head, hide ? 1u : 0u);
    g_fp.hidden = (uint8_t)(hide ? 1 : 0);
    g_fp.visAt = GetTickCount64();
}

/* The arguments this session remembered, without the ring.
 *
 * For the frames that are not ours to place but in which the world has
 * not been replaced: a pause, the map, a load. Those are most of what
 * "not live" means, and retiring the ring for them is what the
 * 2026-09-19 log caught - every ring trace read `retired`, the ring
 * stayed dead, and the eye stayed in the engine's camera for minutes.
 * The ring is the scarce thing here: the engine writes a slot only when
 * it runs the head call for a character - once per character, not once
 * per frame - and the log shows minutes between two of those.
 */
static void Fp2DropRemembered(void) {
    g_fp.headArg = 0;
    g_fp.headArg8 = 0;
    g_fp.headArg9 = 0;
    g_fp.headArgPrev = 0;
    g_fp.headPtr = 0;
    /* The eye as well: those are the frames on which the world may have
     * moved under us (a pause, a load, a replaced world), and redrawing an
     * eye from before the change is a camera in the old place for one
     * frame. See g_eyeReplay. */
    Fp2DropEyeReplay();
}

/* Everything remembered about the session that is being left behind,
 * ring included. This is for the two cases that mean the world itself
 * is gone rather than merely out of sight: a run of frames with no
 * player position, and a loading screen seen on the way out (see
 * g_worldSuspect).
 *
 * Past that point a capture that still resolves may belong to an object
 * the game is already tearing down, and the head call built on it is
 * the 2026-09-18 crash - the game re-set its game mode, the player
 * position read 0, and two seconds later the call went in with the
 * character from the session before. The ring goes with it, by
 * generation rather than by emptying it - see the body. */
static void Fp2Forget(void) {
    Fp2DropRemembered();
    g_pickHold = 0;

    /* Retired, not emptied (2026-09-19). Emptying it left the picker
     * with nothing until the engine ran the head call for a character
     * again, and the field log of that evening shows what that gap
     * costs: frames in which the eye has no capture at all, which the
     * player reads as a flicker. Every entry carries the generation it
     * was written under, so the bump below retires all of them at once:
     * a retired slot is simply not picked, and the engine overwrites it
     * the next time it runs the call for that character.
     *
     * What the paragraph above insists on is unchanged - nothing
     * remembered may be used across a world change. Only the cases that
     * count as one, and the way it is enforced, changed. */
    g_ringGen++;
    RingStampNow();
}

/* A loading screen is the one thing outside the world that says the
 * world itself is being replaced. A pause and the map are not live
 * either, but they leave the world alone - which is why the ring has to
 * survive those and must not survive this. Set on the way out, acted on
 * the first frame we are live again. */
static volatile int g_worldSuspect = 0;

/* How long a run of frames without a player position is a stutter rather
 * than a world being replaced. Five of them in an eight minute session
 * (2026-09-19), four of which were a trace away from working again; the
 * 2026-09-18 crash was two seconds into such a run. Half a second covers
 * the stutter and is a quarter of the way into the crash, which is what
 * the long run is there to catch. */
#define NO_POS_GRACE_MS 500
static volatile uint64_t g_noPosAt;       /* first frame of the run        */
static volatile int      g_noPosTold;     /* the long run is said once     */
static volatile uint64_t g_noPosHolds;    /* stutters carried by the hold  */
static volatile uint64_t g_noPosLogAt;

/* One line every time the eye changes character, and the running
 * count in every line after it.
 *
 * Which capture gets used is the whole question when the view
 * flickers between players, and the ring makes it invisible: the old
 * trace stopped after the first 24 placements, and the 2026-09-18
 * report had nothing in it to look at. So a change is logged, with
 * how far the new capture sits from the local player and whether the
 * game could tell us where that is at all ("UNKNOWN" is the state
 * that made the old code reuse whatever it had).
 *
 * Throttled, because a bad state changes partner every frame and 60
 * lines a second would bury the pattern; the count keeps climbing
 * whether a line is written or not, so the throttle cannot hide how
 * often it happened.
 */
static uint64_t g_pickLogAt;
static uint32_t g_pickFlips;

static void PickNote(uint64_t arg, int haveMe, const ShVec3 *me) {
    static uint64_t last;
    uint64_t now;
    float dist = -1.0f;

    if (arg == last) return;
    last = arg;
    g_pickFlips++;
    now = GetTickCount64();
    if (now - g_pickLogAt < 250) return;
    g_pickLogAt = now;
    if (arg && haveMe && me) {
        uint64_t t2 = HeadTransform(arg);
        float p[3];

        if (t2 && RdF(t2, p, 3)) {
            float dx = p[0] - me->x, dy = p[1] - me->y, dz = p[2] - me->z;

            dist = sqrtf(dx * dx + dy * dy + dz * dz);
        }
    }
    Log("pick: %llx (me %s, %.2f m away) - %u change(s) so far",
        (unsigned long long)arg, haveMe ? "known" : "UNKNOWN",
        dist, g_pickFlips);
}

/* The aim gate's two edges, and the gap between them.
 *
 * An aim hands the frame to the engine's own aim camera the instant the
 * gate says so (see the branch in ShFp2PlaceEye) and takes it back the
 * instant the gate drops. Both are logged with the gap since the last edge,
 * because what the gate does between two aims is the one thing the plugin's
 * once-a-second beat cannot show: whether an aim is one window or a burst of
 * them.
 *
 * Measured 2026-09-20 (firstperson.log, co-op): some aims are as short as
 * 61 ms, and the gate changes hands every 60-250 ms in bursts. Every aim in
 * that log took its own branch correctly - BOW_ADS going in, BOW_NONE coming
 * out - so where a sitting aim's missing raise animation comes from is still
 * open, and this is the line that will say it: a burst arrives here as
 * "started after" a few tens of milliseconds.
 *
 * A hold that kept the frame with the engine for a while after the gate
 * dropped was tried and reverted on 2026-09-21: the engine animates the
 * sights DOWN as soon as the gate drops, so holding the frame shows exactly
 * that - a beat of third person with the head still hidden, then a jump back
 * to the eye. It traded a small artifact for a worse one.
 */
static uint64_t g_adsEdgeAt;    /* last time the gate changed hands */
static int      g_adsEdgeSeen;

static void AimEdge(const char *what) {
    uint64_t now = GetTickCount64();

    Log("aim: %s after %llu ms", what,
        (unsigned long long)(g_adsEdgeSeen ? now - g_adsEdgeAt : 0));
    g_adsEdgeAt = now;
    g_adsEdgeSeen = 1;
}

/* ---- MEASURE 2026-09-29: the aim gate, and what else knows -----
 * Temporary, and nothing here decides anything.
 *
 * The aim's ownership is decided per frame from one byte, and that byte changes
 * hands every 60 to 450 ms in bursts (measured 09-20, and again in the 09-29
 * log). While the two sides own different cameras every burst is a flash: the
 * 09-29 log has the engine's own camera alternating between two fixed poses two
 * metres apart for fifteen seconds.
 *
 * The question this block was written to answer - is the byte, or is the fov,
 * the signal the ownership should follow - is settled: the user's decision is
 * the community table, which reads that byte raw and hands the frame over on
 * it, whatever the fov says. So this block now only observes the candidates
 * side by side, for the record; the ownership itself is the branch in
 * ShFp2PlaceEye. The candidates are:
 *
 *   gate    the byte ownership is read from today;
 *   adsIn   what the S_ADS_IN site last wrote to it, and
 *   adsOut  what S_ADS_OUT wrote - a burst from one site is a different animal
 *           from the two sites disagreeing with each other;
 *   fov     the engine's own fov, which is what the zoom-optic rule already
 *           trusts and which should hold still for a whole aim;
 *   mode    the camera manager's own mode at +0x6C - the field
 *           scripthook_camera.c reads as MGR_MODE ("the mode at +0x6C reads 3
 *           in gameplay"), read here off the manager this frame was called
 *           with, which is the same object.
 *
 * Bounded on purpose: one line per gate edge, and one every MEAS_MS while the
 * gate is up or a zoom optic is in the fov, then it stops for the session at
 * MEAS_LINES - a long session must not be able to fill the log (see the
 * 28,496 lines in fourteen minutes note on TRACE_JUMP_M for how that goes).
 *
 * Delete this block, FP_CAM_MODE, MEAS_MS, MEAS_LINES, Meas itself, the two
 * adsIn/adsOut fields in FpState and the second store in InstallAds.
 */
#define FP_CAM_MODE  0x6C
#define MEAS_MS      400u
#define MEAS_LINES   700u

/* MEASURE 2026-09-29: the gate the decision actually reads, which is
 * the engine's byte unless the probe is holding it at a value. -1 is
 * "not held"; 0 and 1 are the two states, held from outside so the
 * question "does the flash follow this byte" can be answered while
 * the player aims instead of one rebuild and restart per guess.
 *
 * g_lastCm is the manager the last frame ran on, kept so the probe can
 * read the mode out of it from a plugin thread - the frame itself has
 * it in hand, nothing else does.
 */
static volatile int      g_aimHold = -1;
static volatile uint64_t g_lastCm;

/* MEASURE 2026-09-29: the aim rig's share of the eye, and the engine's own
 * two ends of the transition that carries it in.
 *
 * What the day settled: the weapon and the optic are placed from the
 * character's aim rig - the shoulder and weapon - and with the frame handed
 * to the engine the picture IS that rig, so the sights are exact; with the
 * eye held on the head the picture is 191 mm off it (measured twice, 190 and
 * 191 mm, on a settled aim; 1873 mm at the hip, where the engine is still
 * behind the shoulder in its third-person camera), and the sights read that
 * error. Nothing the frame does to the manager's position, to the manager's
 * second copy at +0x1E0, or to xmm2 at the camera build changes it: the rig
 * is computed in the aim path from the character, and in this mode the
 * engine does not even compute the camera for it.
 *
 * So the eye is moved onto the rig instead, and moved by the engine's own
 * transition: the fov it drives from the hip's 0.83 to the sights' 0.49 is
 * the progress, so the shift grows from nothing as the aim comes up and
 * shrinks to nothing as it goes down. There is no switch between two cameras
 * anywhere in this - one source, scaled - which is what keeps it off the list
 * of turns that flashed (see the note at g_aimArm).
 *
 * g_aimRig is the vector from the head to the rig, in the eye's own axes:
 * right along the camera's right, forward along its forward flattened, up in
 * world Z - the same axes as the player's offset, so the two simply add.
 * Calibrated from the live pair on 2026-09-29 (see docs).
 */
static volatile int   g_aimShift = 1;      /* on: the picture follows the rig */
/* 2026-09-30: the constant g_aimRig offset and the FOV_HIP/FOV_SIGHT pair
 * went with the cleanup. They were the 09-29 stand-in for an alignment the
 * aim rig in scripthook_camera.c now measures per weapon instead, and no
 * line has read them since. g_aimAdd stays: the measured path still adds
 * through it. */
static volatile float g_aimAdd[3];

/* The transition-settled detector: the running minimum of the fov while an
 * aim is up, and how many frames it has held still. A monotonic fall, so a
 * minimum that stops moving is the end of the walk - weapon independent.
 * See the hand-over branch in ShFp2PlaceEye. */
static volatile float g_fovMin = 9.9f;
static volatile int   g_fovStill;

static int AimGateNow(void) {
    return g_aimHold >= 0 ? g_aimHold : (g_fp.skip[3] ? 1 : 0);
}

static void Meas(uint64_t cm, int force) {
    static uint64_t lastAt;
    static uint32_t n;
    uint64_t now = GetTickCount64();
    uint32_t mode = 0;

    if (n >= MEAS_LINES) return;
    if (!force && now - lastAt < MEAS_MS) return;
    lastAt = now;
    if (cm && ShReadableAddr(cm + FP_CAM_MODE, 4))
        mode = *(volatile uint32_t *)(uintptr_t)(cm + FP_CAM_MODE);
    Log("meas: %s gate=%d raw=%d hold=%d fov=%.4f mode=%u adsIn=%u "
        "adsOut=%u bow=%d t=%llu",
        force ? "edge" : "beat", AimGateNow(), (int)g_fp.skip[3],
        g_aimHold, (double)ShFovEngine(), (unsigned)mode,
        (unsigned)g_fp.adsIn, (unsigned)g_fp.adsOut, g_bow,
        (unsigned long long)now);
    if (++n == MEAS_LINES)
        Log("meas: %u lines - this session's aim measurement is done",
            (unsigned)n);
}

/* Called from the camera manager's own frame, in the same
 * place the table hooks: the engine has just written the
 * position it computed, and this is the last moment at which
 * replacing it still reaches the render camera.
 *
 * Returns 1 when the position was taken over. Anything else
 * leaves the frame alone, and the caller falls back.
 */
/* ---- the frame-to-frame jump trace ---------------------------------------
 *
 * A flash that lasts one frame IS a discontinuity, and this is where one can be
 * seen without a per-frame log: a sprint moves the camera about 0.13 m per
 * frame, so a quarter of a metre between two frames is either a handover the
 * design makes on purpose - ours to the engine's aim camera, and back - or the
 * thing a report calls "one frame of something else". Both are written, with
 * the values on either side and who owned the frame they came from, so the
 * deliberate ones are read past in a glance and anything else is the flash.
 *
 * The engine's value is what it hands in (before this function writes over it),
 * ours is what it was given. Nothing here changes anything: it is a compare and
 * a line, and only on a jump. Reported 2026-09-26: "with first person on,
 * aiming, sometimes one frame of another picture flashes past".
 */
/* How far the engine's camera, or the eye, has to move between two frames
 * before that is worth a line.
 *
 * It was 0.25 m, which is below what ordinary play moves: running crosses 25 cm
 * inside a frame at anything under about 20 fps, so the line fired on ordinary
 * movement - 28,496 lines in fourteen minutes on 2026-09-27, two per frame (the
 * camera and the eye), 99 percent of the whole logs folder and one fflush per
 * frame on the game thread. What the line exists for is the handover cut, and
 * that is metre-scale (the aim's own frame travels about 1.8 m, see the note at
 * ShFp2PlaceEye), so a metre is the threshold that keeps the events and drops
 * the traffic. */
#define TRACE_JUMP_M 1.0f

/* Engine fov values under this are a zoom optic at work - the line the fov
 * module itself uses (see scripthook_fov.c): a gameplay fov is 0.78 to 0.83
 * radians, and a magnified optic computes far below it. Only the Meas
 * heartbeat reads it. The camera POSITION does not depend on the fov at all,
 * and since 2026-09-29 it does not depend on the engine's ADS byte either
 * unless g_aimHold has been set from outside - see the handover branch. */
#define FOV_ZOOM_RAD 0.5f
static float g_trEng[3], g_trOurs[3];
static int   g_trEngHave, g_trOursHave;

/* The aim's last frame, read off that trace.
 *
 * The engine's own aim camera is not ours to sit in: over an aim it travels
 * about 1.8 m (measured 2026-09-26), the eye sits about 0.36 m from where it
 * started, and the frame the aim ends on used to cut from one to the other - a
 * jump of a metre and a half, one frame long, which is the flash the field
 * reports as "one frame of another picture".
 *
 * What removes it is the handover itself, and two attempts at softening it were
 * made and taken out the same day (2026-09-26), because both of them show the
 * player something worse than the cut:
 *
 *   - a latch that kept the frame with the engine for a moment after the gate
 *     cleared. The engine starts its own way out of the aim the instant the
 *     gate clears, so those frames are the THIRD person one - headless, with the
 *     whole exit animation - and the player who has just let go is looking
 *     straight at it.
 *
 *   - easing the eye back from where the engine's camera was. Its first frame
 *     is that camera, which the log shows 1.85 m from the eye, so it opens on a
 *     third-person headless frame and then glides home: the same fault, spread
 *     over 150 ms instead of one frame.
 *
 * Both were driven by the same wrong idea - that the engine's aim camera is a
 * place to come from. It is not: it is the place the player was already
 * looking through, and the only frame that can follow it without showing
 * anything new is the one the eye is in. So: the cut, which is one frame, and
 * nothing else.
 */

static void TraceJump(const char *what, const float *now, float *last,
                      int *have) {
    if (*have) {
        float dx = now[0] - last[0];
        float dy = now[1] - last[1];
        float dz = now[2] - last[2];

        if (dx * dx + dy * dy + dz * dz > TRACE_JUMP_M * TRACE_JUMP_M) {
            /* A jump whose last frame was one of ours is the steady state
             * rather than news: say it once a session, and let the three that
             * mean something (ads, menu, stale) speak every time. See
             * TRACE_JUMP_M. */
            const char *was = BowName(g_bow);
            static int saidOurs;

            if (!was || strcmp(was, "ours") != 0) {
                Log("fp: %s jumped %.2f,%.2f,%.2f -> %.2f,%.2f,%.2f (last "
                    "frame was %s)", what, last[0], last[1], last[2],
                    now[0], now[1], now[2], was);
            } else if (!saidOurs) {
                saidOurs = 1;
                Log("fp: %s jumped %.2f,%.2f,%.2f -> %.2f,%.2f,%.2f (last "
                    "frame was %s; further jumps like this are not logged "
                    "again this session)", what, last[0], last[1], last[2],
                    now[0], now[1], now[2], was);
            }
        }
    } else {
        *have = 1;
    }
    last[0] = now[0];
    last[1] = now[1];
    last[2] = now[2];
}

/* Put the eye onto the frame, and say the frame was placed.
 *
 * One writer for both the ordinary placement and the replay below, so the
 * two cannot drift: the camera matrix (m[12..14], with the w it has), the
 * camera position (p[0..2], p[3] zeroed as the table leaves it), the eye
 * trace, and the stamp ShFp2Age reads. Only the caller's own book-keeping
 * differs - the ordinary path remembers the eye for a replay and this one
 * does not - which is exactly why that is at the call sites. */
static void EyeWrite(float *m, float *p, const float *out) {
    m[12] = out[0];
    m[13] = out[1];
    m[14] = out[2];
    m[15] = 1.0f;
    p[0] = out[0];
    p[1] = out[1];
    p[2] = out[2];
    p[3] = 0.0f;

    TraceJump("the eye", out, g_trOurs, &g_trOursHave);
    g_fp.placedAt = GetTickCount64();
    g_bow = BOW_NONE;
}

int ShFp2PlaceEye(uint64_t cm, float *m, float *p) {
    /* The table's own buffer is 32 bytes and the store into
     * it is an aligned one, so ours is too: a 16 byte array
     * here is a stack the engine happily writes straight
     * through. */
    SH_ALIGNED(16) float out[8];
    uint64_t arg, tf, a2, a3;
    ShVec3 me;
    int haveMe, tr, held = 0;

    (void)cm;
    tr = (g_trace < 24);
    /* One frame, one stamp for the ring: see g_ringStamp. Here rather
     * than in the camera callbacks so that it is written on the same
     * path that reads the ring, which is what makes the age mean
     * something. */
    RingStampNow();
    if (!g_ready) { g_bow = BOW_OFF; return 0; }

    /* The world checks run whatever first person is doing - before the
     * "do we want the camera" gate, not after it (2026-09-19). The ring
     * has to be right by the time it is asked for, and first person off
     * is exactly when a change is easiest to miss: this used to return at
     * the gate below, so a session joined while the switch was off went
     * unnoticed and the switch-on path cleared the ring for it - retiring
     * entries the engine writes once per world and never rewrites. */
    if (!ShInLivePlay()) {
        if (ShGetUiState() & SH_UI_LOADING) g_worldSuspect = 1;
        Fp2DropRemembered();
        g_bow = g_fp.want ? BOW_STALE : BOW_OFF;
        return 0;
    }
    if (g_worldSuspect) {
        /* The load is over: the world every slot was written in is gone. */
        g_worldSuspect = 0;
        Fp2Forget();
        g_bow = g_fp.want ? BOW_STALE : BOW_OFF;
        return 0;
    }
    if (!g_fp.want)   { g_bow = BOW_OFF;   return 0; }

    /* Past this point the frame is first person's business, so this is where
     * the engine's own camera is read for the trace - before any of the
     * handovers below replace it with nothing. See TraceJump. */
    TraceJump("the engine's camera", p, g_trEng, &g_trEngHave);

    /* Gates that bow out of placing the eye. The head is none
     * of these branches' business: it does what ShFp2HeadWant
     * said, at the top of this function, every frame. */
    /* The menu and the drone take the frame for as long as they last - a menu
     * is minutes - so the remembered capture's window is stamped here for the
     * same reason as the aim's below: an expired one has nothing to place on
     * the frame we come back, and that frame is the engine's own camera. The
     * log of 2026-09-26 00:47:28 has it as the one "menu -> stale" line of the
     * session, on the frame the menu closed. */
    if (g_fp.skip[0]) {
        g_pickHoldAt = GetTickCount64();
        Fp2DropEyeReplay();             /* the eye it remembers */
        g_bow = BOW_MENU;
        return 0;
    }
    if (g_fp.skip[1]) {
        g_pickHoldAt = GetTickCount64();
        Fp2DropEyeReplay();
        g_bow = BOW_DRONE;
        return 0;
    }

    /* While first person holds the camera the head goes, and
     * it goes every frame: the engine reasserts its own idea
     * of the head constantly, and a missed frame is a frame
     * the head is back. An aim keeps it away too - that is
     * what keeps the sights from filling with a skull. In a
     * menu or the drone the branches above left already, so
     * the engine's own state shows it again there. */
    if (!g_headShow) HeadVis(1);

    /* ---- the aim: the engine's own camera, handed over the same frame -----
     * This is the community table, line for line. Its camera hook is:
     *
     *     mov r11l,[skipFirstPersonByte+3]   ; 1 = the player is in ADS
     *     test r11l,r11l
     *     jne  headPositionSkip              ; aiming -> skip recomputing the
     *     ...                                ;   position and writing it
     *   headPositionSkip:
     *     movaps [rax+00000170],xmm2         ; the store still happens; xmm2 is
     *                                        ;   the engine's own value, i.e.
     *                                        ;   "do not overwrite"
     *
     * So while aiming the engine's own aim camera stands, and the decision is
     * the one byte the engine writes at its own two ADS call sites - the same
     * byte captured below as g_fp.skip[3] (see InstallAds). The switch is on
     * the frame itself: the byte goes up this frame and the handover is this
     * frame, with no lag and no look-ahead. The head was hidden just above, so
     * an aim's frame still reads headless - the table does the same.
     *
     * Do NOT bring any of these three back, in any spelling:
     *
     *   - a grace or sustain ("hold the handover for N ms"). The engine starts
     *     its own way out of the aim the instant the byte drops, so the frames
     *     a grace holds are third person - headless, exit animation and all -
     *     straight at the player who just let go (tried and reverted
     *     2026-09-26).
     *
     *   - a threshold that can drift (fov, a per-frame distance, a smoothed
     *     signal). A threshold on a per-frame value is crossed by ordinary
     *     play, and a one-frame move between two cameras is a flash: the fov
     *     hysteresis here flashed at each edge on 2026-09-26, and the borrowed
     *     position switched in and out every 60-450 ms on 2026-09-29.
     *
     *   - borrowing the engine's aim position as a middle form. It is still a
     *     switch between two position sources, so it still needs a threshold,
     *     and the flash still comes back with whatever reads it (2026-09-29).
     *
     * The cost is deliberate and is the user's call: while aiming the camera
     * belongs to the engine, so the player's per-category offsets do not apply
     * on those frames. First person's feel is what is being protected here,
     * and the table pays exactly the same price.
     *
     * The two edges still go to the log (the plugin's report is read from
     * them). g_bow = BOW_ADS is the truth for the handover: the frame was left
     * to the engine.
     *
     * 2026-09-29, later that same day - the floor. The handover is OFF by
     * default: the frame never changes hands, so the one-frame cut at each end
     * of an aim cannot happen at all - by construction, not by a threshold that
     * ordinary play can cross. What that buys and what it costs, in the field's
     * own numbers:
     *
     *   bought   no cut at either end. The picture at the press is the picture
     *            that was already there, and the burst of hand-offs inside one
     *            aim the field reports cannot come back either, because there
     *            is no hand-off to burst.
     *   paid     through an aim the eye stands where the hip eye stood, and the
     *            engine's weapon and optic alignment is computed against its
     *            OWN aim camera - measured 230 mm away on a settled aim and
     *            1873 mm at the hip (ShFp2AimProbe(8) on the 2026-09-29 trace).
     *            The sights still line up; a long lever off them, a pistol's
     *            suppressed barrel at about 0.8 m, reads as the drift the field
     *            reports. A rifle's front sight at about 0.25 m does not.
     *
     * The order was the field's own: take the flash away first, because a
     * one-frame jump between two pictures is not softened by anything, then
     * earn the alignment back by letting the engine's own consumers read OUR
     * eye - the second way in, MGR_STORE in scripthook_camera.c, kept for that
     * purpose since the same day. Until that is done the drift is the known
     * price of the floor.
     *
     * The table's behaviour is still reachable live, for an A/B inside one
     * session, through the knob the probes already own: ShFp2AimHold 1 hands
     * the frame over on the raw byte exactly as it did, ShFp2AimHold 0 or -1
     * (the default, which is what a fresh session and a fresh ini have) keeps
     * the eye. Nothing but this line and ShFp2AimProbe(7)/(0) reads g_aimHold,
     * and the aim's two edges still reach the log either way. */
    /* MEASURE 2026-09-29: hand over when the engine's own ADS transition is
     * done, not the instant the byte goes up.
     *
     * The cut that flashes is the entry one, and it is the engine's camera
     * that makes it: at the press that camera is still 1873 mm behind the
     * head - its third-person seat - and it walks to the aim rig (191 mm off
     * the eye, measured twice) over the transition. Handing over at the press
     * therefore cuts 1.87 m in one frame; handing over at the end of the walk
     * cuts 191 mm, ten times less, and the sights are exact from then on.
     *
     * The end of the walk is read off the fov rather than a fixed number,
     * because a weapon's settled fov differs by weapon (a 4x optic measured
     * 0.4916, iron sights sit higher). Settled here means the fov has stopped
     * falling for a few frames - the transition is monotonic, so the running
     * minimum holding still is the end of it, whatever the weapon.
     *
     * g_aimHold still overrides everything, for an A/B in one session:
     * > 0 hands over at the press (the old behaviour, 1.87 m cut),
     *   0 never hands over, -1 (the default) is this. */
    if (!g_fp.skip[3]) {
        g_fovMin = 9.9f;
        g_fovStill = 0;
    } else {
        float fv = ShFovEngine();

        if (fv > 0.0f && fv < g_fovMin - 0.005f) {
            g_fovMin = fv;
            g_fovStill = 0;
        } else if (fv > g_fovMin + 0.008f) {
            /* The engine has started walking its camera back out. Hand the
             * frame back NOW, and not when its ADS byte finally drops: the
             * byte outlives most of that walk, and by the time it does drop
             * the engine's camera is metres away - the 2026-09-29 beta log
             * has the cut at 2.3 m on the frame the byte went down. Our eye
             * walks back off the same fov (see the apply in
             * scripthook_camera.c), so the two are within a hand's width
             * however early this fires. -1000 is the "it rose" mark: the
             * condition below wants a settled fov, and no settled count is
             * negative. g_fovStill's own reset above clears it for the next
             * aim. */
            g_fovStill = -1000;
        } else if (g_fovStill < 8) {
            g_fovStill++;
        }
    }
    if (g_aimHold > 0) { g_bow = BOW_ADS; return 0; }          /* forced  */
    if (g_aimHold != 0 && g_fp.skip[3] && g_fovStill >= 3) {
        g_bow = BOW_ADS;
        return 0;                                             /* settled */
    }

    g_lastCm = cm;                          /* MEASURE 2026-09-29 */
    /* ---- the aim, for the probes only ------------------------------------
     * g_aimArm is armed by the fov here so ShFp2AimProbe(10) and the Meas
     * heartbeat have the aim's edges to read; it decides NOTHING about the
     * camera. The ownership above is the raw byte, and no line of camera code
     * may read g_aimArm - see the note at its declaration. */
    {
        float fv = ShFovEngine();

        if (fv > 0.0f && fv < FOV_AIM_IN)      g_aimArm = 1;
        else if (fv >= FOV_AIM_OUT)            g_aimArm = 0;
    }
    {
        static int aiming;

        if (AimGateNow() != aiming) {
            aiming = AimGateNow();
            AimEdge(aiming ? "started" : "ended");
            Meas(cm, 1);                    /* MEASURE 2026-09-29 */
        }
    }
    /* MEASURE 2026-09-29: a beat while an aim, or a zoom optic, is in
     * play. Logs at MEAS_MS, decides nothing. */
    {
        float fv = ShFovEngine();

        if (AimGateNow() || (fv > 0.0f && fv < FOV_ZOOM_RAD))
            Meas(cm, 0);
    }

    /* The world checks are at the top of this function now: they have to
     * run whatever first person is doing, and this is the point past
     * which the frame is ours. What remains here is the placement's own
     * work. */

    /* Pick the capture that belongs to the local player. The
     * engine runs the head call once per character, so the ring
     * holds the whole squad; no match falls back to the last
     * capture that vetted - but only while that one still
     * resolves. After a respawn the remembered argument points
     * at a freed object, and the head call below would be a
     * call into nothing: the whole chain has to answer before
     * it is used.
     *
     * That fallback is safe because only a capture that passed the
     * tight radius is ever written back as the remembered one, so a
     * squadmate cannot get into it. The pick itself is noted on every
     * change - see PickNote, which is what the next field report will
     * be read from. */
    haveMe = ShGetPlayerPosition(&me);
    if (haveMe) {
        g_noPosAt = 0;
        g_noPosTold = 0;
        a2 = g_fp.headArg8;
        a3 = g_fp.headArg9;
        arg = PickLocalCapture(&me, &a2, &a3);
        if (!arg && g_pickHold && g_pickHoldGen == g_ringGen &&
            GetTickCount64() - g_pickHoldAt <= PICK_HOLD_MS &&
            HeadTransform(g_pickHold)) {
            /* The ring had nothing this frame. The capture this player
             * was using a moment ago is still theirs, and handing the
             * frame back to the engine for it is the flicker - so keep
             * it, and count it: the count is what says whether retiring
             * the ring only for a world change removed the flicker or
             * only hid it. */
            static uint64_t lastAt;
            uint64_t now = GetTickCount64();

            arg = g_pickHold;
            a2 = g_pickHoldA2;
            a3 = g_pickHoldA3;
            held = 1;
            g_pickHolds++;
            if (now - lastAt >= 2000) {
                lastAt = now;
                Log("pick: ring had nothing, kept the last capture "
                    "(%llu time(s) so far)",
                    (unsigned long long)g_pickHolds);
            }
        }
        if (!arg && g_fp.headArgPrev &&
            HeadTransform(g_fp.headArgPrev))
            arg = g_fp.headArgPrev;
        PickNote(arg, 1, &me);
        if (!arg) {
            /* Nothing picked, and this is the frame the field calls the
             * flicker: all three fallbacks missed, so the frame would go
             * to the engine's own camera and the screen would show it for
             * one frame. Draw the eye this placement last wrote instead,
             * while that is recent enough to stand in - see g_eyeReplay
             * for what "recent enough" is and why it cannot latch.
             *
             * The memory is per placement, not per capture: nothing about
             * which argument wrote the eye is needed here, only where it
             * ended up, so the aim's own follow/offset state is already in
             * the three numbers and is not tracked again. */
            uint64_t now = GetTickCount64();

            if (g_eyeReplayAt && now - g_eyeReplayAt <= EYE_REPLAY_MS) {
                SH_ALIGNED(16) float re[3];

                re[0] = g_eyeReplay[0];
                re[1] = g_eyeReplay[1];
                re[2] = g_eyeReplay[2];
                /* The same write path a placement takes, so the frame is
                 * placed - m, p, the trace and g_fp.placedAt - and only
                 * the capture behind it is missing. Deliberately NOT
                 * stamping g_eyeReplayAt: the window is measured from the
                 * last real placement, and a replay that renewed it would
                 * be the unbounded latch this must never become. */
                EyeWrite(m, p, re);
                g_eyeReplays++;
                {
                    static uint64_t lastAt;

                    if (now - lastAt >= 2000) {
                        lastAt = now;
                        Log("pick: no capture, replayed the last eye "
                            "(%llu frame(s) so far)",
                            (unsigned long long)g_eyeReplays);
                    }
                }
                return 1;
            }
            /* Nothing picked and the last eye is too old to stand in. Four
             * ways to get here and the log has to say which: the ring was
             * never written (the stub is not running), nothing in it
             * resolves to a transform, everything is too far away, or every
             * slot in it was retired. */
            g_bow = BOW_ARG;
            RingTrace(&me);
            return 0;
        }
    } else {
        /* No position this frame. The game does this through a session
         * change - see logs\scripthook_api.log, "no player position" a
         * few seconds before the 2026-09-18 join crash - and it also
         * does it for a frame or two of a state flip: five such runs in
         * an eight minute session on 2026-09-19, four of which were a
         * trace away from working again.
         *
         * A long run is the crash's own signature, and there the answer
         * is the one this branch has always given: forget everything,
         * place nothing, and leave the frame to the engine until a
         * capture from the new world arrives. That crash came two
         * seconds into such a run, so a run of NO_POS_GRACE_MS is
         * already the world being replaced, not a stutter.
         *
         * A short one is this player's own world seen through a stutter,
         * and handing the frame back for it is the flicker the field
         * reports. Keep the capture they were already using - the ring
         * is not consulted, because with no position nothing in it can
         * be measured - and count it, so the next log says which of the
         * two this was. */
        uint64_t now = GetTickCount64();

        if (g_noPosAt == 0) g_noPosAt = now;
        if (now - g_noPosAt >= NO_POS_GRACE_MS) {
            if (!g_noPosTold) {
                g_noPosTold = 1;
                Log("pick: no player position for %llu ms - the world is "
                    "being replaced, forgetting the session",
                    (unsigned long long)(now - g_noPosAt));
            }
            Fp2Forget();
            g_bow = BOW_STALE;
            PickNote(0, 0, 0);
            return 0;
        }
        arg = (g_pickHold && g_pickHoldGen == g_ringGen) ? g_pickHold : 0;
        if (!arg || !HeadTransform(arg)) {
            g_bow = BOW_STALE;
            PickNote(0, 0, 0);
            return 0;
        }
        a2 = g_pickHoldA2;
        a3 = g_pickHoldA3;
        held = 1;
        g_noPosHolds++;
        if (now - g_noPosLogAt >= 2000) {
            g_noPosLogAt = now;
            Log("pick: no position this frame, kept the last capture "
                "(%llu time(s) so far)",
                (unsigned long long)g_noPosHolds);
        }
        PickNote(arg, 0, 0);
    }

    /* Vetted readings are remembered as the last known good
     * one, for the frame where a capture goes missing.
     *
     * A reading that does not vet is not swapped out for it,
     * though. The argument was handed to us by the engine this
     * very frame, which makes it a better bet than one from
     * some earlier frame, and swapping is what quietly stopped
     * everything: with the body hook in place the comparison
     * never agreed, so every frame fell back, the eye was never
     * placed and the head was never hidden. The check is a
     * note, not a veto. */
    tf = HeadTransform(arg);
    if (tf && Sane(tf)) {
        g_fp.headArgPrev = arg;
        g_fp.headArg8 = a2;
        g_fp.headArg9 = a3;
    }
    /* Remembered for the frames in which the ring has nothing - see
     * PICK_HOLD_MS. Written whether or not the reading vets: this one
     * came from the tight radius, so it is this player's own character,
     * and Sane is - as the paragraph above says - a note, not a veto.
     *
     * Not written when the capture being used is the held one itself. A
     * hold that renews its own stamp never expires, and this one has to
     * be measured from the last real pick: a stutter that goes on for a
     * minute in short runs must not keep a capture alive through it. */
    if (!held) {
        g_pickHoldA2 = a2;
        g_pickHoldA3 = a3;
        g_pickHoldAt = GetTickCount64();
        g_pickHoldGen = g_ringGen;
        g_pickHold = arg;
    }

    memset(out, 0, sizeof(out));
    if (tr)
        Log("#%u arg=%llx a2=%llx a3=%llx prev=%llx", g_trace,
            (unsigned long long)arg,
            (unsigned long long)a2,
            (unsigned long long)a3,
            (unsigned long long)g_fp.headArgPrev);
    ((HeadFn)(uintptr_t)FN_HEAD)(arg, (uint64_t)(uintptr_t)out,
                                 a2, a3);
    if (tr)
        Log("#%u out %.2f %.2f %.2f", g_trace,
            out[0], out[1], out[2]);
    g_trace++;
    {
        float ox, oy, oz;
        uint32_t s0, s1;
        int spin = 0;
        float fx, fy, fl;

        /* The offset as one set, not three reads, see
         * ShFp2SetOffset. A torn set is caught by the stamp
         * and simply read again. */
        do {
            s0 = g_offSeq;
            ox = g_fp.off[0];
            oy = g_fp.off[1];
            oz = g_fp.off[2];
            s1 = g_offSeq;
        } while ((s0 != s1 || (s0 & 1u)) && ++spin < 8);

        /* MEASURE 2026-09-29: the aim rig's share, in the same axes as the
         * player's offset and added to it, so both go through one code path
         * and one set of axes. Written from the aim branch on this same
         * thread, so there is nothing to publish behind a counter here. */
        ox += g_aimAdd[0];
        oy += g_aimAdd[1];
        oz += g_aimAdd[2];

        /* The offset is in the eye's own axes, not the
         * world's: right along the camera's right, forward
         * along its forward flattened to the horizon, and up
         * in world Z. Flattened because a downward view would
         * otherwise walk the eye into the chest, and the
         * horizon is the one direction that does not change
         * with where the player is looking. */
        fx = m[4];
        fy = m[5];
        fl = sqrtf(fx * fx + fy * fy);
        if (fl > 0.01f) {
            fx /= fl;
            fy /= fl;
        } else {
            fx = 0.0f;
            fy = 1.0f;
        }
        out[0] += m[0] * ox + fx * oy;
        out[1] += m[1] * ox + fy * oy;
        out[2] += m[2] * ox + oz;
    }

    if (out[0] != out[0] || out[1] != out[1] || out[2] != out[2]) {
        g_bow = BOW_BAD;
        return 0;
    }
    if (fabsf(out[0]) > 1e6f || fabsf(out[1]) > 1e6f ||
        fabsf(out[2]) > 1e6f) {
        g_bow = BOW_BAD;
        return 0;
    }

    /* This is the ordinary frame's placement: out[] - the eye plus the user's
     * offset - goes straight over the engine's position. Nothing else reaches
     * here. An aim never gets this far (the branch at the top returned), so
     * there is no position switch in this function and no threshold anywhere:
     * on an aim's frame the engine's own aim camera simply stands. See the note
     * at g_aimArm for why that shape - and only that shape - is the fix. */

    /* Measure how far the engine's own camera sits from the eye, in
     * millimetres, on this frame - NOW, just before out[] goes over p[], so
     * that p[] is still the engine's position and out[] is already the eye. A
     * pure reading: the sum of squares is formed and stored, and neither array
     * is touched by it. It runs every frame that reaches here, an aim's frames
     * and a hip shot's alike, because the value is only worth having if it is
     * unfiltered - see the note at g_lastGapMm, and do not let anything read it
     * back. */
    {
        float dx = out[0] - p[0];
        float dy = out[1] - p[1];
        float dz = out[2] - p[2];
        float sq = dx * dx + dy * dy + dz * dz;

        /* 1e9 mm^2 is a 1000 m gap: past that the two are in different worlds
         * (a load, a cutscene) and the number says nothing, so it is dropped
         * rather than stored. The bound is tested on the sum of squares, so it
         * costs one comparison; the square root below only ever runs on a sum
         * already known to be under the bound, which is also what keeps the
         * millimetre value clear of an int's range. */
        g_lastGapMm = (sq < 1.0e9f) ? (int)(sqrtf(sq) * 1000.0f) : -1;
    }

    EyeWrite(m, p, out);
    /* Remember the eye for the frames in which no capture can be found -
     * see g_eyeReplay. Stamped from the EyeWrite above rather than a second
     * GetTickCount64, so the window is measured from exactly the placement
     * that wrote the three numbers next to it. */
    g_eyeReplay[0] = out[0];
    g_eyeReplay[1] = out[1];
    g_eyeReplay[2] = out[2];
    g_eyeReplayAt = g_fp.placedAt;
    return 1;
}

/* ---- the engine's own position, captured once per frame, before us -----
 *
 * MEASURE 2026-10-01, the capture pollution. The manager's transform and its
 * position vector are the SAME memory we write the eye into, so any reading of
 * them taken after a placement is a reading of our own write. The rig learning
 * in scripthook_camera.c measured its residual that way - "the seat" minus
 * "our eye" - and with our own eye standing in for the seat the residual came
 * back the same size every frame and was added every frame: the 2026-10-01
 * session walked the rifle's offset from (83, -88, -65) mm to (632, -871,
 * -455) mm in two minutes, doubling whole entries on the way ((48.5, 202.4,
 * -59.7) -> (96.3, 412.6, -121.0)), which is the same defect the note at the
 * accumulator describes and the same one Firejumper93's VR mod records as
 * "half the time it reads back its own write, and the error COMPOUNDS when a
 * polluted capture is itself written from".
 *
 * So the engine's position is taken ONCE, by the camera module, at the top of
 * its manager callback - before ShFp2PlaceEye can touch anything - and every
 * later reader in the frame uses this copy. Nothing here decides or writes:
 * it is the frame's baseline, and a negative or denormal value is kept as it
 * came rather than filtered, because a filter on this is a second thing that
 * can drift.
 */
static float g_engRaw[3];
static int   g_engRawHave = 0;

/** Called by scripthook_camera.c at the top of its manager callback, before
 *  the eye is placed. The three numbers are the engine's own position for
 *  this frame. */
void ShFp2EngineRawCapture(const float *pos) {
    if (!pos) { g_engRawHave = 0; return; }
    g_engRaw[0] = pos[0];
    g_engRaw[1] = pos[1];
    g_engRaw[2] = pos[2];
    g_engRawHave = 1;
}

/** 1 when this frame's engine position was captured, with it copied out.
 *  READ ONLY: this is the frame's baseline, never a place to write. */
int ShFp2EngineRaw(float *out) {
    if (!g_engRawHave || !out) return 0;
    out[0] = g_engRaw[0];
    out[1] = g_engRaw[1];
    out[2] = g_engRaw[2];
    return 1;
}

/* ---- install and the plugin facing API ------------------- */

/* The eye, the head and the gates. These are the table: without
 * them there is no first person at all. */
static int InstallCore(void) {
    uint32_t miss = 0;

    if (!InstallArgs())           miss |= M_ARGS;
    if (!InstallMenu1())          miss |= M_MENU1;
    if (!InstallMenu(S_MENU2, 0)) miss |= M_MENU2;
    if (!InstallMenu(S_MENU3, 1)) miss |= M_MENU3;
    if (!InstallDrone())          miss |= M_DRONE;
    if (!InstallAds(S_ADS_OUT, &g_fp.adsOut) ||
        !InstallAds(S_ADS_IN, &g_fp.adsIn)) miss |= M_ADS;

    g_miss = miss;
    g_ready = (miss & M_REQUIRED) == 0;
    return g_ready;
}

/* Which of the extra sites to take, one bit each. Asked for one
 * at a time on purpose: they are the sites a build can disagree
 * with, and the only way to find which is to take them alone. */
#define EX_BODY      0x1u   /* the body position hook          */
#define EX_VIS       0x2u   /* body visibility                 */
#define EX_SHOULDER  0x4u   /* always allow a shoulder swap    */
#define EX_WALL      0x8u   /* keep the body against a wall    */

/* The rest of it: two unconditional patches and two more hooks,
 * none of which the eye needs. They change how the engine draws
 * the body whether first person is on or not, so they are kept
 * apart - installed only when asked, and the first thing to
 * suspect when a build disagrees with them. */
static int InstallExtras(uint32_t mask) {
    uint32_t miss = 0;

    if ((mask & EX_BODY) && !InstallBody())  miss |= M_BODY;
    if ((mask & EX_VIS) && !InstallVis())    miss |= M_VIS;
    if (mask & EX_SHOULDER) {
        /* jne, the branch that refuses a shoulder swap. */
        static const uint8_t sg[6] = {
            0x0F, 0x85, 0x87, 0x00, 0x00, 0x00
        };
        if (!Check(S_SHOULDER, 6, sg) || !PatchNop(S_SHOULDER, 6))
            miss |= M_SHOULDER;
    }
    if (mask & EX_WALL) {
        /* mov byte [r13+58],01: the wall push that hides
         * the body. */
        static const uint8_t sg[5] = { 0x41, 0xC6, 0x45, 0x58, 0x01 };
        if (!Check(S_WALL, 5, sg) || !PatchNop(S_WALL, 5))
            miss |= M_WALL;
    }
    g_exMiss = miss;
    return miss == 0;
}

SH_API int ShFp2Install(void) {
    if (g_tried) return g_ready;
    g_tried = 1;
    LogInit("scripthook_fpx.log");
    /* The stamp is set before the stub can run even once, and that is the
     * first half of the 2026-09-19 bug: stamping used to start on the
     * first camera frame, while the engine runs the capture site earlier
     * than that - while the world is being built, before the player is in
     * it. Every entry in the ring therefore carried stamp 0, and stamp 0
     * reads as a generation of its own, so the whole ring was retired for
     * the session: at 04:19 the log had four slots, all live, none
     * usable. */
    RingStampNow();
    InstallCore();
    Log("install miss=%03x ready=%d", (unsigned)g_miss, g_ready);
    if (!g_ready) ShSetError(SH_ERR_HOOK_FAILED);
    return g_ready;
}

/** The body and shoulder patches, on request. mask is one bit
 *  per site: 1 the body position hook, 2 body visibility, 4 the
 *  shoulder swap, 8 the wall push. Returns 1 when every site
 *  that was asked for took.
 */
SH_API int ShFp2InstallExtras(uint32_t mask) {
    int ok;
    if (!g_ready) return 0;
    if (g_exTried) return g_exMiss == 0;
    g_exTried = 1;
    ok = InstallExtras(mask);
    Log("extras mask=%x miss=%03x", (unsigned)mask,
        (unsigned)g_exMiss);
    return ok;
}

SH_API int ShFp2Ready(void) {
    return g_ready;
}

/** Which installs did not take, as a mask of the M_ bits.
 *  0 means every site was found and patched.
 */
SH_API uint32_t ShFp2Missing(void) {
    return g_miss | g_exMiss;
}

/** 1 to take the camera, 0 to hand it back. The head is none
 *  of this call's business: the plugin says what it should be,
 *  through ShFp2HeadWant.
 */
SH_API void ShFp2Enable(int on) {
    int was = g_fp.want;

    g_fp.want = (uint8_t)(on ? 1 : 0);
    /* A fresh claim on the camera drops what was remembered - but not the
     * ring (2026-09-19). The ring is written by the engine, one slot per
     * character as the world is built, and retiring it here retired
     * entries the engine does not write again: that evening's log has
     * four slots, three hours, and a switch flipped a handful of times.
     *
     * A world replaced while the switch was off is caught by the
     * placement instead, which now runs its world checks before the "do
     * we want the camera" gate - see the top of ShFp2PlaceEye. That was
     * the whole reason this clear was here, and it is the better place
     * for it: it also catches a load that happened while first person was
     * on but the player was in a menu. */
    if (on && !was) {
        Fp2DropRemembered();
        g_pickHold = 0;
    }
    /* Taking the camera means taking the head: first person
     * holds it down every frame from here. Handing the camera
     * back hands the head back with it - through a window of
     * restated shows, since the engine's own state lags. */
    g_headShow = on ? 0 : 1;
    if (!on) {
        g_showUntil = GetTickCount64() + 800;
        HeadVis(0);
        /* The frame is the engine's from here, and the eye this session
         * last wrote belongs to it no longer: a replay from it after the
         * switch goes back on would be a camera from the session before.
         * See g_eyeReplay. */
        Fp2DropEyeReplay();
    }
}

/** Force the head visible (non zero) or hidden (0), now and
 *  for every frame until said otherwise. Showing calls the
 *  engine once; hiding is the camera frame's business, one
 *  call a frame for as long as it lasts.
 */
SH_API void ShFp2HeadShow(int show) {
    g_headShow = show ? 1 : 0;
    if (show) {
        g_showUntil = GetTickCount64() + 800;
        HeadVis(0);
    }
}

/** The eye offset, in metres, in the eye's own axes: right
 *  along the camera's right, forward along its forward
 *  flattened to the horizon, and up in world Z.
 *
 *  Written from a plugin thread, read inside the engine's
 *  frame, so the three floats are published behind a counter:
 *  odd while a write is in progress, even when it is done, and
 *  the reader takes the set again if it caught a tear. Three
 *  separate stores are not one store, and the frame that mixed
 *  an old x with a new y would be a visible nudge.
 */
SH_API void ShFp2SetOffset(float x, float y, float z) {
    g_offSeq++;
    g_fp.off[0] = x;
    g_fp.off[1] = y;
    g_fp.off[2] = z;
    g_fp.off[3] = 0.0f;
    g_offSeq++;
}

/* MEASURE 2026-09-30: the live half of the 2026-09-29 measurement lived here
 * - ShFp2AimProbe and ShFp2AimHold, the two the test REPL polled and poked so
 * that a question about an aim could be answered without a restart.
 *
 * The measurement is finished, its numbers are written down in
 * docs/firstperson-aim-flash-and-offset.md, and nothing in the framework or in
 * a plugin ever called either one. Both exports are gone rather than left as
 * an API a plugin could believe in. The gate they poked is not: g_aimHold sits
 * at its default -1 and the ownership branch reads it exactly as before.
 */

/** The live gate bytes: menu count, drone, aim, and whether a
 *  fresh capture is waiting. Any may be NULL.
 */
SH_API void ShFp2Gate(int *menu, int *drone, int *ads, int *fresh) {
    if (menu)  *menu  = g_fp.skip[0];
    if (drone) *drone = g_fp.skip[1];
    if (ads)   *ads   = g_fp.skip[3];
    if (fresh) *fresh = g_fp.skip[2];
}

/** 1 while the head is reachable through the engine's own
 *  visibility call. 0 asks the caller to hide it some other
 *  way, which is what the node scan is for.
 */
SH_API int ShFp2HeadOk(void) {
    return g_fp.headPtr != 0;
}

/** Why the last frame placed nothing: 0 it placed. */
SH_API int ShFp2Bow(void) {
    return g_bow;
}

/** Milliseconds since the last frame the eye was placed. */
SH_API uint32_t ShFp2Age(void) {
    if (!g_fp.placedAt) return 0xFFFFFFFFu;
    return (uint32_t)(GetTickCount64() - g_fp.placedAt);
}


