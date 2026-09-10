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
#define S_ARGS      SH_IMG(0x9A51930)
#define S_ARGS_FN   SH_IMG(0x188BA20)

/* The menu counter at +0x56C, pushed from three places. The
 * table tracks it because a menu renders the body too, and a
 * hidden head in a menu is a headless loadout screen. */
#define S_MENU1     SH_IMG(0x12710681)   /* inc [rdi+56C] */
#define S_MENU2     SH_IMG(0x126FFCD5)   /* dec [rbx+56C] */
#define S_MENU3     SH_IMG(0x126FC16C)   /* dec [rbx+56C] */

/* mov [rdi+181A],bl: 1 while the tactical drone flies. */
#define S_DRONE     SH_IMG(0x1153DF45)

/* call GRW.exe+2A25600 on entering and leaving aim down
 * sight. The dl the engine passes is the ADS state. */
#define S_ADS_OUT   SH_IMG(0x14E18C63)
#define S_ADS_IN    SH_IMG(0x14E18C79)

/* call GRW.exe+2A183E0: carries the body position, which the
 * table keeps to vet the head reading against. */
#define S_BODY      SH_IMG(0x14EB425A)
#define S_BODY_FN   SH_IMG(0x2A183E0)

/* movzx eax,[rax+58] / add rsp,20: body visibility, taken so
 * the body can be told to stay visible. */
#define S_VIS       SH_IMG(0x14EBAB15)

/* jne: refuses a shoulder swap while aiming. Nopped so the
 * swap always answers. */
#define S_SHOULDER  SH_IMG(0x140B101B)

/* mov byte [r13+58],01: hides the body when it is pushed
 * against a wall. Nopped so the body stays. */
#define S_WALL      SH_IMG(0x14F1EA9A)

/* The two engine calls the whole thing rests on. */
#define FN_HEAD     SH_IMG(0x188BA20)
#define FN_VIS      SH_IMG(0x2A25600)

/* Head of the chain that names the head for the visibility
 * call. Read once per attempt, not once per frame. */
#define HEAD_ROOT   SH_IMG(0x4B905B8)

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
 * Layout, fixed so the stub can address it without help:
 *   +0x000  uint64 rcx of the calls that hashed here
 *   +0x200  uint64 r8 of the same calls
 *   +0x400  uint64 r9 of the same calls
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
 */
#define RING_SLOTS   64
static volatile struct {
    uint64_t a0[RING_SLOTS];     /* +0x000 */
    uint64_t a2[RING_SLOTS];     /* +0x200 */
    uint64_t a3[RING_SLOTS];     /* +0x400 */
} g_ring;

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
    s[o++] = 0x83; s[o++] = 0xE2; s[o++] = 0x3F;    /* and edx,63   slot   */
    s[o++] = 0x49; s[o++] = 0x89; s[o++] = 0x0C;    /* mov [r10+rdx*8],    */
    s[o++] = 0xD2;                                  /*   rcx               */
    s[o++] = 0x49; s[o++] = 0x89; s[o++] = 0x84;    /* mov [r10+rdx*8+200],*/
    s[o++] = 0xD2;
    *(uint32_t *)(s + o) = 0x200; o += 4;           /*   r8                */
    s[o++] = 0x49; s[o++] = 0x89; s[o++] = 0x8C;    /* mov [r10+rdx*8+400],*/
    s[o++] = 0xD2;
    *(uint32_t *)(s + o) = 0x400; o += 4;           /*   r9                */
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

/* Of the last few captures, the one that sits where the local
 * player is. In a squad the ring holds the whole squad, so
 * metres from the player is the test; alone it is trivially
 * the only entry. Returns 0 when nothing is close enough,
 * which is the caller's cue to fall back.
 */
#define PICK_MAX_M   12.0f

static uint64_t PickLocalCapture(const ShVec3 *me,
                                 uint64_t *outA2, uint64_t *outA3) {
    uint64_t arg = 0, a2 = 0, a3 = 0;
    float best = PICK_MAX_M * PICK_MAX_M;
    int i;

    for (i = 0; i < RING_SLOTS; i++) {
        uint64_t a = g_ring.a0[i];
        uint64_t t2;
        float p[3], dx, dy, dz, d;

        if (!a) continue;
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
    uint64_t now = GetTickCount64();
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

    n = snprintf(line, sizeof line, "ring p:");
    for (i = 0; i < used && n > 0; i++) {
        uint64_t t2 = HeadTransform(g_ring.a0[slot[i]]);
        float p[3] = { 0, 0, 0 };

        if (t2 && RdF(t2, p, 3))
            n += snprintf(line + n, sizeof(line) - n,
                          " %.1f,%.1f,%.1f", p[0], p[1], p[2]);
        else
            n += snprintf(line + n, sizeof(line) - n, " -");
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
    return (uint64_t)(uintptr_t)mbi.BaseAddress
           + mbi.RegionSize >= addr + len;
}

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
    /* The tag names the slot, and it is not a constant: the
     * log caught 0x0F on foot, 0x15 around vehicles and 0xC7
     * after a map screen - three states, three slots. The
     * table's own check reads "15", which in Cheat Engine's
     * assembler is hex, and a white list kept missing states.
     * Any of them indexes the same table; what matters is
     * that the slot yields a pointer at all. */
    a = RdQ(a + 0x27);  if (!a) return HeadPtrStop(9);
    a = RdQ(a + (uint64_t)tag * 8u);
    if (!a) {
        if (HpLogReady())
            Log("headptr: tag %u, empty slot", (unsigned)tag);
        return 0;
    }
    if (!Writable(a, 0x40)) {
        if (HpLogReady())
            Log("headptr: tag %u, node not writable", (unsigned)tag);
        return 0;
    }
    return a;
}

/* Chain first, so a respawn picks the fresh pointer the same
 * frame it exists; the last good one only answers when the
 * chain will not resolve right now, which is the ordinary
 * state in a menu rather than an error. */
static uint64_t HeadPtr(void) {
    uint64_t a = HeadPtrChain();

    if (a) {
        g_fp.headPtr = a;
        return a;
    }
    return g_fp.headPtr;
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
    uint64_t head = HeadPtr();

    if (!head) return;
    g_fp.headPtr = head;
    ((VisFn)(uintptr_t)FN_VIS)(head, hide ? 1u : 0u);
    g_fp.hidden = (uint8_t)(hide ? 1 : 0);
    g_fp.visAt = GetTickCount64();
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
    int tr;

    (void)cm;
    tr = (g_trace < 24);
    if (!g_ready) { g_bow = BOW_OFF; return 0; }
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
        g_bow = BOW_ADS;
        return 0;
    }

    /* Pick the capture that belongs to the local player. The
     * engine runs the head call once per character, so the ring
     * holds the whole squad; no match falls back to the last
     * capture that vetted - but only while that one still
     * resolves. After a respawn the remembered argument points
     * at a freed object, and the head call below would be a
     * call into nothing: the whole chain has to answer before
     * it is used. */
    a2 = g_fp.headArg8;
    a3 = g_fp.headArg9;
    arg = ShGetPlayerPosition(&me)
          ? PickLocalCapture(&me, &a2, &a3) : 0;
    if (!arg && g_fp.headArgPrev &&
        HeadTransform(g_fp.headArgPrev))
        arg = g_fp.headArgPrev;
    if (!arg) {
        /* Nothing picked. Three ways to get here and the log
         * has to say which: the ring was never written (the
         * stub is not running), nothing in it resolves to a
         * transform, or everything is too far away. */
        g_bow = BOW_ARG;
        RingTrace(&me);
        return 0;
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
    g_fp.want = (uint8_t)(on ? 1 : 0);
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


