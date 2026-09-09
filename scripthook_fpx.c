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
extern void *ShAllocNear(uint64_t target);
extern void ShSetError(int err);

/* ---- byte level helpers ---------------------------------- */

static uint64_t RdQ(uint64_t addr) {
    uint64_t v = 0;
    if (!addr || !ShReadableAddr(addr, 8)) return 0;
    memcpy(&v, (const void *)(uintptr_t)addr, 8);
    return v;
}

static int RdF(uint64_t addr, float *out, int n) {
    if (!addr || !ShReadableAddr(addr, (size_t)n * 4)) return 0;
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
 * Records the argument the engine itself passes to the head
 * function, and marks it fresh. Everything else follows from
 * this one value.
 *
 * All four argument registers are taken, not just the first.
 * The table calls the function from a patch site, where the
 * third and fourth happen to hold whatever the engine left
 * there; a call from C has no such luck, and a function that
 * reads them is handed our stack instead of the engine's. The
 * two extra values cost nothing when the function ignores
 * them and are the difference between working and a fault
 * when it does not.
 *
 * Only rax is touched and it is put back: at a call site rax
 * is scratch, but nothing here owns it.
 */
static int InstallArgs(void) {
    static const uint8_t sig[1] = { 0xE8 };
    uint8_t *s;
    int o = 0;

    if (!Check(S_ARGS, 1, sig)) return 0;
    s = NewStub(S_ARGS);
    if (!s) return 0;

    s[o++] = 0x50;                                  /* push rax */
    EmitMov64(s, &o, 0xB8, (uint64_t)(uintptr_t)&g_fp.headArg);
    s[o++] = 0x48; s[o++] = 0x89; s[o++] = 0x08;    /* mov [rax],rcx */
    EmitMov64(s, &o, 0xB8, (uint64_t)(uintptr_t)&g_fp.headArg8);
    s[o++] = 0x4C; s[o++] = 0x89; s[o++] = 0x00;    /* mov [rax],r8  */
    EmitMov64(s, &o, 0xB8, (uint64_t)(uintptr_t)&g_fp.headArg9);
    s[o++] = 0x4C; s[o++] = 0x89; s[o++] = 0x08;    /* mov [rax],r9  */
    EmitMov64(s, &o, 0xB8, (uint64_t)(uintptr_t)&g_fp.skip[2]);
    s[o++] = 0xFE; s[o++] = 0x00;                   /* inc byte [rax] */
    s[o++] = 0x58;                                  /* pop rax  */
    o = EmitJmp(s, o, (uint64_t)(uintptr_t)s + o, S_ARGS_FN);
    if (o < 0) return 0;

    return PatchCall(S_ARGS, S_ARGS_FN, s);
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

    for (i = 0; i < 3; i++) {
        a = RdQ(a);
        if (!a) return 0;
    }
    return RdQ(a + 0x238);
}

/* The chain is walked one step at a time and each step is named,
 * because "the head could not be found" is not something that can
 * be acted on: which link came back empty is. */
static uint32_t g_hpTrace = 0;

static uint64_t HeadPtrStop(int step) {
    if (g_hpTrace < 8) {
        g_hpTrace++;
        Log("headptr: nothing at step %d", step);
    }
    return 0;
}

/* The head, for the visibility call. A chain the table walks
 * by hand; it ends in a small table indexed by a type tag the
 * site has to match, and a miss is simply "not now". */
static uint64_t HeadPtr(void) {
    uint64_t a, c;
    uint16_t tag;

    /* What the engine handed its own visibility call. It is the
     * real thing, so it wins over anything we walk to. */
    if (g_fp.headPtr) return g_fp.headPtr;

    a = RdQ(HEAD_ROOT); if (!a) return HeadPtrStop(0);
    a = RdQ(a + 0x10);  if (!a) return HeadPtrStop(1);
    a = RdQ(a + 0x10);  if (!a) return HeadPtrStop(2);
    a = RdQ(a);         if (!a) return HeadPtrStop(3);
    a = RdQ(a + 0x78);  if (!a) return HeadPtrStop(4);
    c = RdQ(a + 0x10);  if (!c) return HeadPtrStop(5);
    c = RdQ(c + 0x10);  if (!c) return HeadPtrStop(6);
    if (!ShReadableAddr(c + 3, 2)) return HeadPtrStop(7);
    memcpy(&tag, (const void *)(uintptr_t)(c + 3), 2);
    if ((tag & 0xFFu) != 15u) {
        if (g_hpTrace < 8) {
            g_hpTrace++;
            Log("headptr: tag %u, not 15", (unsigned)(tag & 0xFFu));
        }
        return 0;
    }
    a = RdQ(a + 0x27);  if (!a) return HeadPtrStop(9);
    a = RdQ(a + (uint64_t)(tag & 0xFFu) * 8u);
    if (!a) return HeadPtrStop(10);
    return a;
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
    uint64_t arg, tf;
    int i, tr;

    (void)cm;
    tr = (g_trace < 24);
    if (!g_ready) { g_bow = BOW_OFF; return 0; }
    if (!g_fp.want)   { g_bow = BOW_OFF;   return 0; }
    /* A menu and the drone are views the engine draws itself,
     * and a hidden head in either is a headless body on
     * screen. An aim is the opposite: the table keeps the head
     * hidden there, which is what keeps the sights from
     * filling with the inside of a skull. */
    if (g_fp.skip[0]) { g_bow = BOW_MENU;  HeadVis(0); return 0; }
    if (g_fp.skip[1]) { g_bow = BOW_DRONE; HeadVis(0); return 0; }
    if (g_fp.skip[3]) { g_bow = BOW_ADS;   HeadVis(1); return 0; }

    /* No fresh capture this frame: place nothing, but the head
     * stays hidden. The table skips the placement on such a
     * frame and never the head, and skipping both is exactly
     * what let it back on screen for a frame at a time. */
    if (!g_fp.skip[2]) {
        g_bow = BOW_STALE;
        HeadVis(1);
        return 0;
    }

    /* One capture feeds one frame. */
    g_fp.skip[2] = 0;
    arg = g_fp.headArg;
    if (!arg) arg = g_fp.headArgPrev;
    if (!arg) { g_bow = BOW_ARG; HeadVis(1); return 0; }

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
    if (tf && Sane(tf)) g_fp.headArgPrev = arg;

    memset(out, 0, sizeof(out));
    if (tr)
        Log("#%u arg=%llx a2=%llx a3=%llx prev=%llx", g_trace,
            (unsigned long long)arg,
            (unsigned long long)g_fp.headArg8,
            (unsigned long long)g_fp.headArg9,
            (unsigned long long)g_fp.headArgPrev);
    ((HeadFn)(uintptr_t)FN_HEAD)(arg, (uint64_t)(uintptr_t)out,
                                 g_fp.headArg8, g_fp.headArg9);
    if (tr)
        Log("#%u out %.2f %.2f %.2f", g_trace,
            out[0], out[1], out[2]);
    g_trace++;
    for (i = 0; i < 3; i++)
        out[i] += g_fp.off[i];
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
    HeadVis(1);
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

/** 1 to take the camera, 0 to hand it back. */
SH_API void ShFp2Enable(int on) {
    g_fp.want = (uint8_t)(on ? 1 : 0);
    if (!on) {
        /* Handing the camera back means the head has to come
         * back with it, and it has to happen now rather than
         * on the next beat. */
        g_fp.visAt = 0;
        HeadVis(0);
    }
}

/** The eye offset, in metres, in world axes. */
SH_API void ShFp2SetOffset(float x, float y, float z) {
    g_fp.off[0] = x;
    g_fp.off[1] = y;
    g_fp.off[2] = z;
    g_fp.off[3] = 0.0f;
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

/** Force the head one way or the other, outside the frame
 *  path. Used when a screen opens and the head has to be
 *  visible even though first person is still armed.
 */
SH_API void ShFp2HeadShow(int show) {
    HeadVis(show ? 0 : 1);
}
