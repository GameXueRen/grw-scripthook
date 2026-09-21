/* First person the way the Cheat Engine table does it.
 *
 * The engine already computes where the head is: one call,
 * GRW.exe+188BA20, is handed an argument and writes a world
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
 */

/* call GRW.exe+188BA20: the capture site. This is where the
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

/* call GRW.exe+2A25600 on entering and leaving aim down
 * sight. The dl the engine passes is the ADS state. */
#define S_ADS_OUT   SH_IMG(0x147FF653)
#define S_ADS_IN    SH_IMG(0x147FF669)

/* call GRW.exe+2A183E0: carries the body position, which the
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
 * off on the way through. */
static int InstallAds(uint64_t site) {
    static const uint8_t sig[1] = { 0xE8 };
    uint8_t *s;
    int o = 0;

    if (!Check(site, 1, sig)) return 0;
    s = NewStub(site);
    if (!s) return 0;

    s[o++] = 0x50;
    EmitMov64(s, &o, 0xB8, (uint64_t)(uintptr_t)&g_fp.skip[3]);
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

/* The camera frame, whether first person runs or not: hold the
 * head down while first person has it, and restate "visible"
 * through the show window after a handover. */
void ShFp2HeadFrame(void) {
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

/* Called from the camera manager's own frame, in the same
 * place the table hooks: the engine has just written the
 * position it computed, and this is the last moment at which
 * replacing it still reaches the render camera.
 *
 * Returns 1 when the position was taken over. Anything else
 * leaves the frame alone, and the caller falls back.
 */
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

    /* Gates that bow out of placing the eye. The head is none
     * of these branches' business: it does what ShFp2HeadWant
     * said, at the top of this function, every frame. */
    if (g_fp.skip[0]) { g_bow = BOW_MENU;  return 0; }
    if (g_fp.skip[1]) { g_bow = BOW_DRONE; return 0; }

    /* While first person holds the camera the head goes, and
     * it goes every frame: the engine reasserts its own idea
     * of the head constantly, and a missed frame is a frame
     * the head is back. An aim keeps it away too - that is
     * what keeps the sights from filling with a skull. In a
     * menu or the drone the branches above left already, so
     * the engine's own state shows it again there. */
    if (!g_headShow) HeadVis(1);
    /* An aim hands the frame to the engine's own aim camera,
     * the instant it starts - the table's behaviour, and the
     * only one this design has: the engine's aim transition
     * runs the whole time, so a window that keeps writing over
     * it does not blend anything, it hides the transition and
     * then reveals it in one jump when the window closes.
     * That is a pull, and it was measured as one. */
    if (g_fp.skip[3]) {
        if (g_bow != BOW_ADS) AimEdge("started");
        g_bow = BOW_ADS;
        return 0;
    }
    if (g_bow == BOW_ADS) AimEdge("ended");

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
            /* Nothing picked. Four ways to get here and the log
             * has to say which: the ring was never written (the
             * stub is not running), nothing in it resolves to a
             * transform, everything is too far away, or every
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

    m[12] = out[0];
    m[13] = out[1];
    m[14] = out[2];
    m[15] = 1.0f;
    p[0] = out[0];
    p[1] = out[1];
    p[2] = out[2];
    p[3] = 0.0f;

    g_fp.placedAt = GetTickCount64();
    g_bow = BOW_NONE;
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
    if (!InstallAds(S_ADS_OUT) || !InstallAds(S_ADS_IN)) miss |= M_ADS;

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


