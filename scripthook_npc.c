/* NPC spawning, mirrored from the engine's spawn director
 * (FUN_148AE7AA0). Same pump model as the vehicle spawner.
 */
#include <windows.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"
#include "log.h"

/* RVAs, so this survives a relocated image. Re-pinned for the 2026-09 build
 * one at a time - the whole block except the last three was still the
 * previous build's, which is why NPC spawning stopped working while vehicle
 * spawning did not (its own block was re-pinned and is live verified).
 *
 * How each one was pinned, so the next update can be done the same way:
 *   MGR_GETTER  the old and new bodies are the same eight bytes,
 *               mov rax,[rip+..]; ret - and spawn.c, which uses the same
 *               manager and works in game, carries the same address
 *   SPAWN       spawn.c carries the same address (0x990B0B0)
 *   COMMIT      spawn.c carries the same address; body 96% by .pdata match
 *   SET_CATEGORY byte search for the setter's own body: unique in both
 *   SET_174     byte search for mov [rcx+0x174],edx;ret: unique in both
 *   POP_REGISTER 32 bytes identical to the old body
 *   COLLECT     identical to the old body except the rel32 of one call
 *   RETIRE      no call site to vote with and no unique body shape; the only
 *               .pdata candidate at 80%. Its failure is logged, not silent.
 *   KIND / POOL_FIND / SPEC_OF / NPC_SPEC_VTABLE were re-pinned earlier and
 *               checked against the old build then.
 *
 * The data globals below (POOL, POPMGR, CONTEXT, REGISTRY, ARCH_DESC,
 * NULL_BLOCK) have no rip-relative reference in the old image to map, so
 * they cannot be checked offline: the module now prints what it reads from
 * each one and a session says whether they are still the right slots.
 */
#define RVA_MGR_GETTER   0x990BAB0
#define RVA_SPAWN        0x990B0B0
#define RVA_COMMIT       0x990CA70
#define RVA_SET_CATEGORY 0xA9E51B0
#define RVA_SET_174      0xA9E63B0
#define RVA_POP_REGISTER 0x8AE8EA0
#define RVA_COLLECT      0xC1F5BA0
#define RVA_KIND         0x89372E0
#define RVA_POOL_FIND    0xE2E0780

/* Despawn, from the Domino UnspawnFromEntity node: the
 * entity's spawning spec, then retire it. Verified live. */
#define RVA_SPEC_OF      0xA9C3F80
#define RVA_RETIRE       0x99FDBB0

/* The two bootstrap slots, re-pinned off the engine's own call path:
 * every place the engine feeds the catalogue collector loads the registry
 * from one rip relative slot, and two of them, in different functions,
 * decode to the same address (0x3FC4F0 + disp and 0x235B541 + disp both
 * land on 4BC1878). The same decode on the old build lands exactly on the
 * value this constant used to carry, 4BC17F8, so the slot moved by the
 * +0x80 this data family moved by - the same shift the spawn manager slot
 * shows through MGR_GETTER. ARCH_DESC keeps the shape it has in the old
 * build (its +0x20 tail is byte for byte the same) at +0x10.
 *
 * POOL / CONTEXT / NULL_BLOCK have no anchor in the image to decode, and
 * each is now either unused or read only. The population manager does have
 * one: it is the argument of the register call, which the engine loads from
 * a slot at five separate sites - four of them agree on it, and that is what
 * POP_MGR below is. It is still taken only if it looks like an engine
 * object, because that call has crashed the game twice. */
#define RVA_POOL         0x4D89080
/* The population manager, decoded off the engine's own register calls rather
 * than guessed - see PopManager(). */
#define RVA_POP_MGR      0x4B98FA8
/* The context the spawn takes its Camp and Job from: two 32 bit ids at
 * +0x1B4 and +0x1B8, and what makes a spawn an agent of the faction the
 * engine is playing rather than a body standing in the world. It was
 * carried as 0x4B98F98 once, where the readings came back as float bit
 * patterns (0.4f, 5.0f); that was taken for the meaning being wrong and
 * the slot was retired. The slot was what was wrong. GhostHook (the NPC
 * spawner author's own framework, TU25) and the upstream September port
 * both carry 0x4B90288 for it, and it reads back small integers. */
#define RVA_CONTEXT      0x4B90288
#define RVA_REGISTRY     0x4BC1878
#define RVA_ARCH_DESC    0x42C2570
#define RVA_NULL_BLOCK   0x4D89068
#define NPC_SPEC_VTABLE  SH_IMG(0x394A4E0)

#define COMMIT_MODE      7
/* The second argument of SPAWN. SPAWN stores it in the spec itself
 * (spec+0x148 comes back holding it verbatim) and passes it on to the
 * engine's create call (990B130 "mov r8d, esi" feeding the VM entry at
 * A99A97C), where it picks which set of behaviour functions the spec gets
 * wired to - spec+0x68, +0x78 and +0x1D8 all move with it.
 *
 * It is not an "is it hostile" switch: 0, 1 and 2 all produce a spec, and
 * both 0 and 1 were observed with NPCs that engage. 1 is the value every
 * build that worked has used, so it stays. See ModeProbe for how the three
 * were compared, and spec+0x14C for the field that is NOT this one: it is
 * the constant 0x16, already set by the engine by the time SPAWN returns. */
#define SPAWN_MODE       1
#define NPC_CATEGORY     3
#define NPC_MAX          1024
/* A camp and a job are small ids. Anything larger read at those offsets
 * is a different slot talking, not a camp, and is refused. */
#define CAMP_JOB_MAX     0xFFFFu
/* How far above the probed surface a formation point is placed, in
 * centimetres, settable from [npc] ground_lift_cm in scripthook.ini.
 *
 * The probe is the engine's own ray, so what it returns is the surface
 * itself; the lift only has to keep the odd point from starting a
 * centimetre inside it - a flat foot on a slope, a ray that lands on the
 * near edge of a step. It was a flat 0.5 m, and that read as "the NPC is
 * dropped in from the air" rather than placed, so it is 10 cm now and 0 is
 * allowed for anyone who wants the point exactly on the surface. */
#define NPC_GROUND_LIFT_CM_DEF  10
#define NPC_GROUND_LIFT_CM_MAX  100

extern int ShReadableAddr(uint64_t addr, size_t len);
extern uint64_t ShReadQ(uint64_t addr);
extern int ShReadMem(uint64_t addr, void *out, size_t len);
extern void ShSetError(int err);
extern int ShRequireInGame(void);
extern const void *ShSpawnBuildMatrix(const ShVec3 *pos);

typedef uint64_t (__attribute__((ms_abi)) *MgrGet_t)(void);
typedef uint64_t (__attribute__((ms_abi)) *Spawn_t)(uint64_t, int,
                                                    const void *);
typedef uint64_t (__attribute__((ms_abi)) *Commit_t)(uint64_t, int,
                                                     uint64_t);
typedef void (__attribute__((ms_abi)) *SetI_t)(uint64_t, int);
typedef void (__attribute__((ms_abi)) *Reg_t)(uint64_t, uint64_t);
typedef void (__attribute__((ms_abi)) *Collect_t)(uint64_t, uint64_t,
                                                  void *);
typedef int (__attribute__((ms_abi)) *Kind_t)(uint64_t);
typedef uint64_t (__attribute__((ms_abi)) *PoolFind_t)(uint64_t,
                                                       uint64_t, int);

static uint64_t ImgAddr(uint64_t rva) {
    return (uint64_t)(uintptr_t)GetModuleHandleA(NULL) + rva;
}

/* Handle block: object +0, refcount +8, flags +0xC (bit 31
 * valid), id +0x10. */
static uint64_t BlockObj(uint64_t blk) {
    uint32_t fl;
    if (!blk || !ShReadableAddr(blk, 0x18)) return 0;
    fl = *(volatile uint32_t *)(uintptr_t)(blk + 0xC);
    if (!(fl & 0x80000000u)) return 0;
    return ShReadQ(blk);
}

/* ---- catalogue, read on the game thread once ---- */

static ShNpcArchetype g_npcs[NPC_MAX];
/* The catalogue's archetype blocks, kept beside the public {id, kind}. The
 * scan that produced an id already holds that archetype's own block, so a
 * spawn can be made from here - which is what takes the pool lookup out of
 * the spawn path entirely. The public struct is {id, kind} and stays that
 * way; this is the module's own copy. */
static uint64_t g_npcBlk[NPC_MAX];
static int g_npcCount = 0;
static volatile int g_listWanted = 0;
static volatile int g_listDone = 0;

/* The collector writes an array record here. It is bigger
 * than the three fields we read, and a tight buffer let the
 * engine walk off the end and corrupt the stack. */
typedef struct {
    uint64_t ptr;
    uint16_t cap;
    uint16_t cnt;
    uint8_t  spare[0x38];
} ArchList;

/* This module had no log at all, and every way it can fail is silent: the
 * catalogue comes back empty, or a spawn returns early, and the caller only
 * sees one generic error. That is how "cannot summon" reached the game
 * without naming the step. One line, first outcome wins: the addresses in it
 * are what re-pins a data slot that moved, which cannot be done offline -
 * these globals have no rip-relative reference in the old image to match. */
static void NpcWhy(const char *why, uint64_t a, uint64_t b) {
    LogFirst("scripthook_npc.log", "npc: %s (%llX %llX)", why,
             (unsigned long long)a, (unsigned long long)b);
}


static void ListOnGameThread(void) {
    ArchList hdr;
    uint64_t reg = ShReadQ(ImgAddr(RVA_REGISTRY));
    int i, n = 0;

    memset(&hdr, 0, sizeof(hdr));
    if (!reg) {
        NpcWhy("registry slot holds nothing", ImgAddr(RVA_REGISTRY), 0);
        return;
    }
    ((Collect_t)ImgAddr(RVA_COLLECT))(reg, ImgAddr(RVA_ARCH_DESC), &hdr);
    if (!hdr.ptr || hdr.cnt > 0x4000 ||
        !ShReadableAddr(hdr.ptr, (size_t)hdr.cnt * 8)) {
        NpcWhy("collector gave no list", hdr.ptr, hdr.cnt);
        return;
    }
    for (i = 0; i < hdr.cnt && n < NPC_MAX; i++) {
        uint64_t blk = ShReadQ(hdr.ptr + (uint64_t)i * 8);
        uint64_t obj = BlockObj(blk);
        if (!obj) continue;
        g_npcs[n].id = ShReadQ(blk + 0x10);
        g_npcs[n].kind = ((Kind_t)ImgAddr(RVA_KIND))(obj);
        g_npcBlk[n] = blk;
        n++;
    }
    /* The collector's array stays with the engine's pool;
     * a few KB once per session. */
    g_npcCount = n;
    /* What the scan made of it: a count of zero with entries collected is a
     * different fault from an empty collector, and the two want different
     * constants re-pinned. */
    LogFirst("scripthook_npc.log",
             "npc: catalogue %d of %u collected (registry %llX)",
             n, (unsigned)hdr.cnt, (unsigned long long)reg);
}

/* ---- one spawn, on the game thread ---- */

/* The pump signals this, so a spawn costs the frame the
 * engine needs and no polling granularity on top. */
static HANDLE g_pumpEvent;

/* The pump's wake event, created once. Check-then-create was not atomic:
 * two threads could each make one and the second store dropped the first -
 * and a thread already waiting on the dropped handle would sit out its whole
 * timeout instead of being woken by the SetEvent that went to the newer one.
 * One compare-and-swap; the loser closes its own copy. Both callers take the
 * handle through here, so every SetEvent and every wait are on the same
 * object and a signal can no longer be missed.
 */
static HANDLE GetPumpEvent(void) {
    HANDLE h = g_pumpEvent;

    if (h) return h;
    h = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!h) return NULL;
    if (InterlockedCompareExchangePointer((PVOID volatile *)&g_pumpEvent,
                                          h, NULL) != NULL) {
        CloseHandle(h);              /* another thread won; use its handle */
        h = g_pumpEvent;
    }
    return h;
}

/* Process detach only: the event outlives every waiter by design, so it is
 * not released in the middle of a session. */
void ShNpcShutdown(void) {
    HANDLE h = (HANDLE)InterlockedExchangePointer(
        (PVOID volatile *)&g_pumpEvent, NULL);

    if (h) CloseHandle(h);
}

static volatile uint64_t g_pendId = 0;
static const void *g_pendMtx = NULL;
static volatile uint64_t g_pendSpec = 0;
static volatile int g_pendDone = 0;
static volatile int g_pendErr = 0;

/* The archetype's own block, out of the catalogue.
 *
 * This used to go through the engine's pool - POOL + 0x100 and then the
 * pool's find - two more pinned slots that cannot be checked offline, on
 * the path that has to work before anything else can. The scan that
 * produced the id already held the block, so the lookup is local now: same
 * answer, no bootstrap slots, and a miss says so. */
static uint64_t ArchetypeBlock(uint64_t id) {
    int i;

    for (i = 0; i < g_npcCount; i++)
        if (g_npcs[i].id == id) return g_npcBlk[i];
    return 0;
}

/* The population manager, for the one call the framework has to make with an
 * argument of its own.
 *
 * It is decoded, not guessed. The engine calls the register function from
 * five places and every one of them loads the argument from a global right
 * before the call:
 *
 *   48 8B 0D <disp32>   mov rcx,[rip+disp32]    the manager slot
 *   48 89 C2            mov rdx,rax             the spec
 *   E8 <rel32>          call register
 *
 * Decoding that disp32 at each of the five sites in the current build:
 *
 *   775B74D -> 4B98FA8    9202385 -> 4B98FA8    9EAB00D -> 4B98FA8
 *   13053BBB -> 4B98FA8   718E325 -> 4B9A7A8   (same block, another mode)
 *
 * Four independent sites agree, which no lookalike can do, so that is the
 * slot - and it is 0x10 past the retired context slot above, in the same
 * block, which is where the whole family sits.
 *
 * The five values that used to sit here came from an earlier decode and not
 * one of them was the manager: in the campaign the slot in play read empty,
 * so the registration was skipped on every spawn and the completion line
 * said NOT registered each time.
 *
 * This is the call that has crashed the game twice - once with a stale slot
 * from before the decode, once after the check below had been relaxed to
 * "readable" on the belief that a slot the engine fills must hold a manager.
 * Readable is not the same as correct; the vtable test is what does the
 * work, and it stays. */
static uint64_t PopManager(void) {
    uint64_t p = ShReadQ(ImgAddr(RVA_POP_MGR));

    /* An engine object: readable, and its first word is a vtable in the
     * image. */
    if (p && ShReadableAddr(p, 0x40) && ShInImage(ShReadQ(p)))
        return p;                     /* logged by the caller, with the spec */

    /* Worth a line, because the two ways this fails lead opposite ways: an
     * empty slot says the address above is wrong and has to move, while a
     * slot holding something that is not an engine object says the address
     * is right and the cast is wrong. Once per session. */
    {
        static volatile LONG said;

        if (InterlockedExchange(&said, 1) == 0) {
            if (!g_logFile) LogInit("scripthook_npc.log");
            Log("npc: population slot %llX holds %llX, no manager there",
                (unsigned long long)ImgAddr(RVA_POP_MGR),
                (unsigned long long)p);
        }
    }
    return 0;
}

/* ---- what SPAWN's second argument means --------------------------------
 *
 * SPAWN passes it straight through to the engine's own create call
 * (990B130 "mov r8d, esi" feeding Core(mgr, ctx, mode) at A99A97C), and that
 * call is a VM entry - so the meaning is not in the image to be read. The
 * engine's own spawn points do not settle it either: one passes 0 outright
 * (AB30B8 "xor edx, edx"), one passes a value it just computed (47A62F),
 * and an internal wrapper uses 2 (A99AA13 "mov r8d, 2").
 *
 * So ask the engine instead: one context, spawned three times with the
 * argument at 0, 1 and 2, each returned spec snapshotted, the three compared
 * word by word. A field that the argument actually drives shows up as a
 * column that differs, and no guess about which field is "the faction" is
 * needed to find it.
 *
 * Nothing is committed, registered or written into any of the three, so a
 * run leaves no NPC behind - only three specs the engine handed out and was
 * never told to place. It runs once per session, behind [npc] mode_probe in
 * scripthook.ini, and is off by default.
 */
#define PROBE_SPAN 0x2E0

/* [npc] ground_lift_cm, read once. The value is in centimetres because the
 * config API has no float reader, and 10 cm is fine enough for a step that
 * the eye has to notice. */
static float NpcGroundLift(void) {
    static volatile LONG got;
    static float lift;
    int cm;

    if (InterlockedCompareExchange(&got, 0, 0)) return lift;
    cm = ShConfigGetInt("npc", "ground_lift_cm", NPC_GROUND_LIFT_CM_DEF);
    if (cm < 0) cm = 0;
    if (cm > NPC_GROUND_LIFT_CM_MAX) cm = NPC_GROUND_LIFT_CM_MAX;
    lift = (float)cm / 100.0f;
    InterlockedExchange(&got, 1);
    return lift;
}

static void ModeProbe(uint64_t cs, const void *mtx) {
    static volatile LONG done;
    uint8_t snap[3][PROBE_SPAN];
    uint64_t spec[3];
    int i, off, differ = 0;

    if (InterlockedExchange(&done, 1)) return;

    if (!g_logFile) LogInit("scripthook_npc.log");
    Log("probe: mode 0/1/2 on context %llX, matrix %llX - nothing committed",
        (unsigned long long)cs, (unsigned long long)(uintptr_t)mtx);

    memset(snap, 0, sizeof snap);
    memset(spec, 0, sizeof spec);
    for (i = 0; i < 3; i++) {
        spec[i] = ((Spawn_t)ImgAddr(RVA_SPAWN))(cs, i, mtx);
        if (!spec[i]) {
            Log("probe: mode %d -> no spec (refused)", i);
            continue;
        }
        if (!ShReadableAddr(spec[i], PROBE_SPAN) ||
            !ShReadMem(spec[i], snap[i], PROBE_SPAN)) {
            Log("probe: mode %d -> spec %llX unreadable", i,
                (unsigned long long)spec[i]);
            continue;
        }
        Log("probe: mode %d -> spec %llX, vtable %llX", i,
            (unsigned long long)spec[i],
            (unsigned long long)ShReadQ(spec[i]));
    }

    /* Only the lines that differ: the answer is the column that moves. */
    for (off = 0; off < PROBE_SPAN; off += 8) {
        uint64_t a = *(uint64_t *)(uintptr_t)(snap[0] + off);
        uint64_t b = *(uint64_t *)(uintptr_t)(snap[1] + off);
        uint64_t c = *(uint64_t *)(uintptr_t)(snap[2] + off);

        if (a == b && b == c) continue;
        differ++;
        Log("probe: +%03X  m0=%016llX  m1=%016llX  m2=%016llX", off,
            (unsigned long long)a, (unsigned long long)b,
            (unsigned long long)c);
    }
    Log("probe: %d of %d word(s) differ across 0/1/2 (span %X)",
        differ, PROBE_SPAN / 8, PROBE_SPAN);
}

static void SpawnOnGameThread(uint64_t id, const void *mtx) {
    uint64_t mgr, arch, archBlk, csBlk, cs, spec, old, ctx, pop;
    static volatile LONG campJobSaid = 0;

    g_pendErr = SH_ERR_NO_CANDIDATE;
    archBlk = ArchetypeBlock(id);
    arch = BlockObj(archBlk);
    if (!arch) {
        /* The id came out of the catalogue, so a miss here means the scan
         * and the request disagree; the count says how much catalogue there
         * was to disagree with. */
        NpcWhy("no archetype block for the id", id, (uint64_t)g_npcCount);
        return;
    }
    csBlk = ShReadQ(arch + 0x48);
    cs = BlockObj(csBlk);
    if (!cs) { NpcWhy("archetype has no spec block", arch, csBlk); return; }
    mgr = ((MgrGet_t)ImgAddr(RVA_MGR_GETTER))();
    if (!mgr || !ShReadableAddr(mgr, 0x40)) {
        NpcWhy("no usable spawn manager", ImgAddr(RVA_MGR_GETTER), mgr);
        return;
    }

    /* Off by default and once a session: [npc] mode_probe in scripthook.ini.
     * It spawns three specs that are never committed, which is what makes
     * the meaning of SPAWN's second argument visible without having to guess
     * at a field. See ModeProbe. */
    if (ShConfigGetBool("npc", "mode_probe", 0)) ModeProbe(cs, mtx);

    /* Step markers, on the game thread. A spawn that never returns leaves
     * no trace at all - the frame rate goes to zero and the only thing the
     * log shows is the wave that asked for it, seconds earlier. These say
     * which of the three engine calls it went into, so the next freeze is
     * a diagnosis instead of a guess. Each one is one flushed line; the
     * order they stop in is the answer. */
    /* The log has to be open before the first marker: it used to be opened
     * at the end of this function, which the markers would be written
     * before - and the one that matters is the line before a freeze. */
    if (!g_logFile) LogInit("scripthook_npc.log");
    Log("npc: step SPAWN  in  (cs %llX mode %d)", (unsigned long long)cs,
        SPAWN_MODE);
    spec = ((Spawn_t)ImgAddr(RVA_SPAWN))(cs, SPAWN_MODE, mtx);
    Log("npc: step SPAWN  out (%llX)", (unsigned long long)spec);
    if (!spec) { NpcWhy("spawn() gave back nothing", cs, SPAWN_MODE); return; }
    /* Everything below writes through this pointer, so it is checked as a
     * spec before any of it: a stale SPAWN would otherwise have us write
     * 0x2D8 bytes into whatever it returned. */
    if (!ShReadableAddr(spec, 0x2D8) || ShReadQ(spec) != NPC_SPEC_VTABLE) {
        NpcWhy("spawn() did not give back a spec", spec, NPC_SPEC_VTABLE);
        return;
    }

    ((SetI_t)ImgAddr(RVA_SET_CATEGORY))(spec, NPC_CATEGORY);

    /* Archetype handle: take a reference, swap, drop the old
     * one (the null sentinel on a fresh spec). */
    InterlockedIncrement((volatile LONG *)(uintptr_t)(archBlk + 8));
    old = ShReadQ(spec + 0x2B8);
    *(volatile uint64_t *)(uintptr_t)(spec + 0x2B8) = archBlk;
    if (old) InterlockedDecrement((volatile LONG *)(uintptr_t)(old + 8));

    ((SetI_t)ImgAddr(RVA_SET_174))(spec, 0);

    /* Camp and Job: the two ids that make this spawn an agent of the
     * faction the engine is playing. They are what "its AI runs like a
     * native spawn" has always meant in scripthook.h, and they live at
     * +0x1B4 and +0x1B8 of the context RVA_CONTEXT names.
     *
     * They were written here once and then dropped, because the values
     * that came back read as 0.4f and 5.0f and the write was blamed for
     * a crash. The slot was the problem. Float bit patterns are what a
     * WRONG slot reads as, not what a camp id reads as; 0x4B98F98 is
     * not the context. Both GhostHook (the NPC spawner author's own
     * framework, TU25) and the upstream September port carry 0x4B90288
     * for it, and that one reads back small integers.
     *
     * Written as two 32 bit ids, exactly as GhostHook writes them. The
     * range guard is the part that is ours: this is the field the crash
     * was blamed on, and a camp or a job is a small number, so anything
     * else means the slot moved again - refused, and said once, rather
     * than handed to the register function. */
    ctx = ShReadQ(ImgAddr(RVA_CONTEXT));
    if (ctx && ShReadableAddr(ctx + 0x1B4, 8)) {
        uint32_t camp = *(volatile uint32_t *)(uintptr_t)(ctx + 0x1B4);
        uint32_t job  = *(volatile uint32_t *)(uintptr_t)(ctx + 0x1B8);

        if (camp <= CAMP_JOB_MAX && job <= CAMP_JOB_MAX) {
            *(volatile uint32_t *)(uintptr_t)(spec + 0x2D0) = camp;
            *(volatile uint32_t *)(uintptr_t)(spec + 0x2D4) = job;
            Log("npc: camp %u job %u (ctx %llX)", camp, job,
                (unsigned long long)ctx);
        } else if (InterlockedExchange(&campJobSaid, 1) == 0) {
            Log("npc: camp/job read as %u/%u at ctx %llX, which is not a "
                "camp and a job - nothing was written, the slot has moved",
                camp, job, (unsigned long long)ctx);
        }
    } else if (InterlockedExchange(&campJobSaid, 1) == 0) {
        Log("npc: no context to take camp/job from (ctx %llX)",
            (unsigned long long)ctx);
    }

    pop = PopManager();
    Log("npc: step REG    in  (pop %llX)", (unsigned long long)pop);
    if (pop) ((Reg_t)ImgAddr(RVA_POP_REGISTER))(pop, spec);
    else NpcWhy("no usable population manager; registration skipped", 0,
                ImgAddr(RVA_POP_REGISTER));
    Log("npc: step REG    out");

    Log("npc: step COMMIT in");
    ((Commit_t)ImgAddr(RVA_COMMIT))(mgr, COMMIT_MODE, spec);
    Log("npc: step COMMIT out");
    g_pendSpec = spec;
    g_pendErr = 0;
    /* Written per spawn, not once: which data slot answered, and whether the
     * population registration went through, are what a session has to say -
     * none of these slots can be checked offline. The catalogue line already
     * holds the module's once-only flag, and log.h keeps its handle per
     * translation unit, so the file is opened only if the catalogue has not
     * opened it already (reopening would truncate what was written). */
    if (!g_logFile) LogInit("scripthook_npc.log");
    Log("npc: spawn id %llX -> spec %llX (mgr %llX pop %llX), %s",
        (unsigned long long)id, (unsigned long long)spec,
        (unsigned long long)mgr, (unsigned long long)pop,
        pop ? "registered" : "NOT registered");
}

/* ---- despawn ---- */

typedef uint64_t (__attribute__((ms_abi)) *SpecOf_t)(uint64_t);
typedef int (__attribute__((ms_abi)) *Retire_t)(uint64_t);

static volatile uint64_t g_killEnt = 0;
static volatile int g_killDone = 0;
static volatile int g_killOk = 0;

static void DespawnOnGameThread(uint64_t entity) {
    uint64_t spec;

    g_killOk = 0;
    spec = ((SpecOf_t)ImgAddr(RVA_SPEC_OF))(entity);
    if (!spec) { NpcWhy("no spec for the entity to unspawn", entity, 0); return; }
    if (!ShReadableAddr(spec, 0x180)) {
        NpcWhy("spec is not readable", spec, ImgAddr(RVA_SPEC_OF));
        return;
    }
    ((Retire_t)ImgAddr(RVA_RETIRE))(spec);
    g_killOk = 1;
}

/* Called from the physics hook, next to ShSpawnPump.
 *
 * Every slot is taken with an interlocked exchange rather than read
 * and then cleared. The September updates put the physics callbacks on
 * worker threads, so this pump can be entered by two of them at once -
 * and with a read-then-clear both would see the same request. For a
 * spawn that means two SpawnOnGameThread calls with the SAME matrix:
 * two NPCs stacked on one point, the archetype's refcount taken twice
 * and the spec's fields written twice over each other, which is a
 * batch that arrives overlapping and half wired up. An exchange makes
 * it one request, one spawn. g_pendMtx is deliberately still a plain
 * read: the publisher fills it (and the spec) before the id, and the
 * id is what claims the request. */
void ShNpcPump(void) {
    int did = 0;
    uint64_t e = (uint64_t)InterlockedExchange64(
        (volatile LONG64 *)&g_killEnt, 0);

    if (e) {
        DespawnOnGameThread(e);
        g_killDone = 1;
        did = 1;
    }

    if (InterlockedExchange((volatile LONG *)&g_listWanted, 0)) {
        ListOnGameThread();
        g_listDone = 1;
        did = 1;
    }
    {
        uint64_t id = (uint64_t)InterlockedExchange64(
            (volatile LONG64 *)&g_pendId, 0);

        if (id) {
            const void *mtx = g_pendMtx;

            if (mtx) SpawnOnGameThread(id, mtx);
            g_pendDone = 1;
            did = 1;
        }
    }
    if (did) {
        HANDLE ev = GetPumpEvent();

        if (ev) SetEvent(ev);
    }
}

static int WaitFlag(volatile int *flag, int ms) {
    DWORD end = GetTickCount() + (DWORD)ms;
    HANDLE ev = GetPumpEvent();

    while (!*flag) {
        DWORD now = GetTickCount();
        if (now >= end) break;
        if (ev)
            WaitForSingleObject(ev, end - now);
        else
            Sleep(1);
    }
    return *flag;
}

static int EnsureList(void) {
    if (g_npcCount) return 1;
    if (!ShRequireInGame()) return 0;
    g_listDone = 0;
    g_listWanted = 1;
    if (!WaitFlag(&g_listDone, 3000)) {
        g_listWanted = 0;
        ShSetError(SH_ERR_NO_PHYSICS);
        return 0;
    }
    return g_npcCount > 0;
}

int ShNpcCount(void) {
    return EnsureList() ? g_npcCount : 0;
}

const ShNpcArchetype *ShNpcAt(int index) {
    if (!EnsureList()) return NULL;
    if (index < 0 || index >= g_npcCount) return NULL;
    return &g_npcs[index];
}

/* The entity lands at spec+0xA8 once the factory has built
 * it, a frame or two after the commit. */
static uint64_t SpecEntity(uint64_t spec) {
    uint64_t blk;
    if (!ShReadableAddr(spec, 0x1A0)) return 0;
    if (ShReadQ(spec) != NPC_SPEC_VTABLE) return 0;
    blk = ShReadQ(spec + 0xA8);
    if (blk == ImgAddr(RVA_NULL_BLOCK)) return 0;
    return BlockObj(blk);
}

/* Any spawn system entity, NPC or vehicle. Entities built
 * outside that road have no spec and refuse. */
int ShDespawn(uint64_t entity) {
    if (!entity) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (!ShRequireInGame()) return 0;

    g_killDone = 0;
    g_killEnt = entity;
    if (!WaitFlag(&g_killDone, 3000)) {
        g_killEnt = 0;
        ShSetError(SH_ERR_NO_PHYSICS);
        return 0;
    }
    if (!g_killOk) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    return 1;
}

uint64_t ShSpawnNpc(uint64_t archetypeId, const ShVec3 *pos) {
    const void *mtx;
    uint64_t ent = 0, spec;
    int waited;

    if (!pos || !archetypeId) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (!ShRequireInGame()) return 0;

    mtx = ShSpawnBuildMatrix(pos);
    if (!mtx) { ShSetError(SH_ERR_NO_ROOT); return 0; }

    g_pendDone = 0;
    g_pendSpec = 0;
    g_pendMtx = mtx;
    g_pendId = archetypeId;
    if (!WaitFlag(&g_pendDone, 3000)) {
        g_pendId = 0;
        ShSetError(SH_ERR_NO_PHYSICS);
        return 0;
    }
    if (g_pendErr) { ShSetError(g_pendErr); return 0; }

    spec = g_pendSpec;
    for (waited = 0; waited < 60 && !ent; waited++) {
        ent = SpecEntity(spec);
        if (!ent) Sleep(50);
    }
    if (!ent) ShSetError(SH_ERR_NO_CANDIDATE);
    return ent;
}

/* ---- archetype groups --------------------------------------------
 *
 * Which faction an archetype belongs to is the engine's own business and it
 * keeps it private: ShNpcArchetype carries {id, kind} and no name, so kind is
 * the one value the framework can read. Kind is therefore what decides the
 * group, measured on this build rather than looked up in a table:
 *
 *   kind 3         Santa Blanca
 *   kind 5         Unidad
 *   kind 6, 7      Rebels
 *   kind 0, 1      Civilians
 *   kind 4         Special
 *   anything else  no group (-1)
 *
 * Older versions of this module also carried four per-archetype id tables,
 * with a blacklist and an explicitly listed Unidad id. They had come from a
 * third-party plugin and were removed at its author's request on 2026-09-20;
 * what that costs is written down at ShNpcGroupOfArchetype below.
 */

static const char *g_groupNames[SH_NPC_GROUP_MAX] = {
    "Santa Blanca", "Unidad", "Rebels", "Civilians", "Special"
};

SH_API int ShNpcGroupCount(void) {
    return SH_NPC_GROUP_MAX;
}

SH_API const char *ShNpcGroupName(int group) {
    if (group < 0 || group >= SH_NPC_GROUP_MAX) return "";
    return g_groupNames[group];
}

/* One archetype, one group, decided by the engine's own kind - see the note
 * above. There is no id table any more, so an archetype whose kind does not
 * name its faction reads as the group its kind names; that is what most of
 * the catalogue has always done, and it is the honest answer given that the
 * engine keeps the faction itself private. */
SH_API int ShNpcGroupOfArchetype(const ShNpcArchetype *a) {
    int kind;

    if (!a) return -1;
    kind = a->kind;

    if (kind == 3) return SH_NPC_GROUP_SANTA_BLANCA;
    if (kind == 5) return SH_NPC_GROUP_UNIDAD;
    if (kind == 6 || kind == 7) return SH_NPC_GROUP_REBELS;
    if (kind <= 1) return SH_NPC_GROUP_CIVILIANS;
    if (kind == 4) return SH_NPC_GROUP_SPECIAL;
    return -1;
}

SH_API int ShNpcGroupOf(uint64_t archetypeId) {
    int n, i;

    if (!archetypeId) return -1;
    n = ShNpcCount();
    if (n <= 0) return -1;
    for (i = 0; i < n; i++) {
        const ShNpcArchetype *a = ShNpcAt(i);
        if (a && a->id == archetypeId) return ShNpcGroupOfArchetype(a);
    }
    return -1;
}

SH_API int ShNpcGroupSize(int group) {
    int n, i, c = 0;

    if (group < 0 || group >= SH_NPC_GROUP_MAX) return 0;
    n = ShNpcCount();
    if (n <= 0) return 0;
    for (i = 0; i < n; i++)
        if (ShNpcGroupOfArchetype(ShNpcAt(i)) == group) c++;
    return c;
}

SH_API int ShNpcAtInGroup(int group, int index, ShNpcArchetype *out) {
    int n, i, k = 0;

    if (group < 0 || group >= SH_NPC_GROUP_MAX || index < 0) return -1;
    n = ShNpcCount();
    if (n <= 0) return -1;
    for (i = 0; i < n; i++) {
        const ShNpcArchetype *a = ShNpcAt(i);
        if (ShNpcGroupOfArchetype(a) != group) continue;
        if (k == index) {
            if (out) *out = *a;
            return i;
        }
        k++;
    }
    return -1;
}

/* ---- formations -------------------------------------------------- */

#define NPC_PI_F   3.14159265f
#define NPC_TAU_F  6.28318531f

/* One point of a layout, in the plane ShNpcPlanFormation then rotates by the
 * player's yaw. Distances are metres, and every shape is the plain reading of
 * its name:
 *
 *   line         abreast, 2.5 m apart, centred on the aim point
 *   spread       a three column grid at 3 m, every row centred on its own
 *   semicircle   an arc in front of the aim point, radius 4.5 m
 *   circle       a ring around it, radius 3.5 m
 *   random       a ring at 2-6.5 m, each spawn jittered inside its own slice
 *
 * The jitter is one step of xorshift32 per point, seeded by the caller: the
 * same seed lays out the same batch, and each point keeps to its own slice of
 * the ring, so a batch never lands two NPCs on one spot. */
static void FormationPoint(int formation, int i, int n,
                           unsigned *seed, float *ox, float *oy) {
    float a;

    *ox = 0.0f;
    *oy = 0.0f;
    if (n <= 1) return;

    switch (formation) {
    case SH_NPC_FORMATION_LINE:
        *ox = ((float)i - (float)(n - 1) * 0.5f) * 2.5f;
        break;

    case SH_NPC_FORMATION_SPREAD: {
        /* A three column grid, every row centred on its own. */
        int q = i / 3, r = i % 3;
        int rowLen = n - 3 * q;
        int rows = (n + 2) / 3;
        *ox = ((float)r - (float)(rowLen - 1) * 0.5f) * 3.0f;
        *oy = ((float)q - ((float)rows - 1.0f) * 0.5f) * 3.0f;
        break;
    }

    case SH_NPC_FORMATION_SEMICIRCLE:
        a = NPC_PI_F * ((float)i / (float)(n - 1)) - NPC_PI_F * 0.5f;
        *ox = 4.5f * cosf(a);
        *oy = 4.5f * sinf(a);
        break;

    case SH_NPC_FORMATION_CIRCLE:
        a = NPC_TAU_F * (float)i / (float)n;
        *ox = 3.5f * cosf(a);
        *oy = 3.5f * sinf(a);
        break;

    default: {
        /* Random: the low 16 bits of the step jitter the point inside its
         * slice, the next 16 pick the radius in [2, 6.5). Zero is the one
         * seed xorshift32 cannot leave, so it is replaced by a constant. */
        unsigned s = *seed;
        float jitter, rad;

        if (!s) s = 0x9E3779B9u;
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        *seed = s;

        jitter = (float)(s & 0xFFFFu) * (1.0f / 65536.0f);
        rad = 2.0f + 4.5f * (float)((s >> 16) & 0xFFFFu) * (1.0f / 65536.0f);

        a = (NPC_TAU_F / (float)n) * ((float)i + jitter);
        *ox = rad * cosf(a);
        *oy = rad * sinf(a);
        break;
    }
    }
}

SH_API int ShNpcPlanFormation(int formation, int count, float distance,
                              const ShVec3 *origin, float yaw,
                              ShVec3 *out, int max) {
    unsigned seed;
    float cy, sy, bx, by;
    int i, n;

    if (!origin || !out || max <= 0 || count < 1) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    if (formation < SH_NPC_FORMATION_LINE ||
        formation > SH_NPC_FORMATION_RANDOM) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }

    n = count > max ? max : count;
    if (n > SH_NPC_SPAWN_MAX) n = SH_NPC_SPAWN_MAX;

    seed = (unsigned)GetTickCount() ^ (unsigned)(uintptr_t)out
           ^ (unsigned)(uintptr_t)origin;
    cy = cosf(yaw);
    sy = sinf(yaw);
    bx = origin->x + distance * cy;
    by = origin->y + distance * sy;

    for (i = 0; i < n; i++) {
        float ox, oy;
        FormationPoint(formation, i, n, &seed, &ox, &oy);
        out[i].x = bx + ox * sy + oy * cy;
        out[i].y = by - ox * cy + oy * sy;
        out[i].z = origin->z;
    }
    ShSetError(SH_OK);
    return n;
}

/* ---- the layout policy -------------------------------------------
 *
 * Where a batch lands and which way it looks are the request's own
 * business - until someone wants to look at a batch. With the player
 * standing still every call lands on the same spot, and a spawn that
 * faces the player is a spawn whose reaction cannot be told from its
 * patience. So a caller may leave a policy here instead: each batch a
 * step further round the player than the last, and a heading drawn by
 * one of the modes in scripthook.h.
 *
 * It is deliberately not a field of ShNpcSpawnRequest: that struct is
 * shared with plugins and with jobs that are already running, and a
 * caller that never installs a policy has to see exactly what it saw
 * before.
 */
static volatile LONG g_layoutOn = 0;
static volatile LONG g_spreadTick = 0;
static float g_spreadStepDeg = 0.0f;               /* degrees, as given */
static float g_spreadStep = 0.0f;                  /* radians */
static int   g_facingMode = SH_NPC_FACING_MODE_PLAYER;
static float g_facingAngle = 0.0f;                 /* radians */

SH_API int ShNpcSpawnSetLayout(const ShNpcSpawnLayout *layout) {
    if (!layout) {
        InterlockedExchange(&g_layoutOn, 0);
        ShSetError(SH_OK);
        return 1;
    }
    if (layout->facing_mode < SH_NPC_FACING_MODE_PLAYER ||
        layout->facing_mode > SH_NPC_FACING_MODE_SPIN) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }

    /* A step that did not change is not a new walk. The caller this
     * exists for re-installs its policy before every batch, and
     * resetting the count here would pin every batch to the same
     * bearing - the very thing the spread is for. */
    if (g_spreadStepDeg != layout->spread_step_deg) {
        g_spreadStepDeg = layout->spread_step_deg;
        g_spreadStep = layout->spread_step_deg * (NPC_PI_F / 180.0f);
        InterlockedExchange(&g_spreadTick, 0);
    }
    g_facingMode = layout->facing_mode;
    g_facingAngle = layout->facing_angle_deg * (NPC_PI_F / 180.0f);
    InterlockedExchange(&g_layoutOn, 1);
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShNpcSpawnGetLayout(ShNpcSpawnLayout *out) {
    if (!out) { ShSetError(SH_ERR_BAD_ARG); return 0; }

    if (!InterlockedCompareExchange(&g_layoutOn, 0, 0)) {
        out->spread_step_deg = 0.0f;
        out->facing_mode = SH_NPC_FACING_MODE_PLAYER;
        out->facing_angle_deg = 0.0f;
    } else {
        out->spread_step_deg = g_spreadStepDeg;
        out->facing_mode = g_facingMode;
        out->facing_angle_deg = g_facingAngle * (180.0f / NPC_PI_F);
    }
    ShSetError(SH_OK);
    return 1;
}

/* A heading for one spawn that no policy fixed, in radians. Xorshift
 * over an interlocked counter: no seeding, no library state, safe
 * from any thread. */
static float NextFacingJitter(void) {
    static volatile LONG s = 0x1F123BB5;
    unsigned x = (unsigned)InterlockedIncrement(&s) * 2654435761u;

    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return (float)x * (NPC_TAU_F / 4294967296.0f);
}

/* ---- one batch --------------------------------------------------- */

/* Runs on whichever thread calls it. progress, when it is not
 * NULL, is republished after every NPC so a poll can watch the
 * batch fill up; cancel is read between NPCs. Two rules are its own:
 * it stops while the world is loading (an NPC handed to a world that is
 * not there yet is a crash waiting to happen), and it drops an NPC it
 * spawned just as the world went away. */
static int SpawnBatch(const ShNpcSpawnRequest *req, uint64_t *out,
                      int maxOut, volatile LONG *cancel,
                      volatile LONG *progress) {
    ShPlayer pl;
    ShVec3 pp, tmp, pos[SH_NPC_SPAWN_MAX];
    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
    int n, i, got = 0, count;

    if (!req || !out || maxOut <= 0) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }

    count = req->count;
    if (count > SH_NPC_SPAWN_MAX) count = SH_NPC_SPAWN_MAX;
    if (count > maxOut) count = maxOut;
    if (count < 1) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }

    if (!ShRequireInGame()) return 0;
    if (!ShGetPlayer(&pl) || !ShGetPlayerPosition(&pp) ||
        !ShGetEntityTransform(pl.entity, &tmp, &yaw, &pitch, &roll)) {
        ShSetError(SH_ERR_NO_POSITION);
        return 0;
    }

    /* The layout policy, when one is installed: each batch goes one
     * step further round the player than the last, so a run of them
     * spreads out instead of stacking on one spot. The first batch
     * keeps the straight-ahead heading the planner has always given
     * it. */
    if (InterlockedCompareExchange(&g_layoutOn, 0, 0)) {
        float step = g_spreadStep;

        if (step != 0.0f)
            yaw += (float)(InterlockedIncrement(&g_spreadTick) - 1) * step;
    }

    n = ShNpcPlanFormation(req->formation, count, req->distance, &pp, yaw,
                           pos, SH_NPC_SPAWN_MAX);

    for (i = 0; i < n; i++) {
        uint64_t e;
        int st;
        float gz;

        st = ShGetGameState();
        if (st == SH_STATE_LOADING || st == SH_STATE_RELOADING) break;
        if (cancel && *cancel) break;

        /* Put the point on the ground first.
         *
         * The planner leaves z at the origin's own, and that is what buries
         * NPCs: a formation is metres across,
         * so on a slope its far points sit above or below the surface the
         * player is standing on. The probe is the engine's own ray, hinted
         * with the planned height so it finds the surface there - a bridge
         * deck if the player is under one - and a sweep from altitude is
         * tried when the hint misses, which is what a point over a drop
         * needs. A point with no ground under it is not spawned: better a
         * batch of four that stand than five with one under the terrain.
         */
        if (!ShGroundHeightFrom(pos[i].x, pos[i].y, pos[i].z, &gz) &&
            !ShGroundHeight(pos[i].x, pos[i].y, &gz)) {
            NpcWhy("no ground under a formation point; it was skipped",
                   (uint64_t)(unsigned)i, (uint64_t)(unsigned)n);
            continue;
        }
        pos[i].z = gz + NpcGroundLift();

        e = ShSpawnNpc(req->id, &pos[i]);
        if (!e) continue;

        st = ShGetGameState();
        if (st == SH_STATE_LOADING || st == SH_STATE_RELOADING) continue;
        if (cancel && *cancel) break;

        out[got++] = e;
        if (progress) InterlockedExchange(progress, got);

        {
            /* Which way it looks: the policy when one is installed,
             * the request's own facing field when not. FORWARD is the
             * one mode that writes nothing - the spawn already
             * carries the player's heading. */
            int   mode = InterlockedCompareExchange(&g_layoutOn, 0, 0)
                       ? g_facingMode : -1;
            int   want = 0;
            float face = 0.0f;

            if (mode == SH_NPC_FACING_MODE_PLAYER ||
                (mode < 0 && req->facing == SH_NPC_FACING_PLAYER)) {
                want = 1;
                face = atan2f(pp.y - pos[i].y, pp.x - pos[i].x);
            } else if (mode == SH_NPC_FACING_MODE_RANDOM) {
                want = 1;
                face = NextFacingJitter();
            } else if (mode == SH_NPC_FACING_MODE_FIXED) {
                want = 1;
                face = g_facingAngle;
            } else if (mode == SH_NPC_FACING_MODE_SPIN) {
                want = 1;
                face = g_facingAngle * (float)got;
            }

            if (want) ShQueueTransform(e, &pos[i], face, 0.0f, 0.0f);
        }
    }

    if (got > 0) ShSetError(SH_OK);
    else if (ShIsInGame()) ShSetError(SH_ERR_NO_CANDIDATE);
    return got;
}

SH_API int ShNpcSpawnFormation(const ShNpcSpawnRequest *req,
                               uint64_t *out, int maxOut) {
    return SpawnBatch(req, out, maxOut, NULL, NULL);
}

/* ---- batches in flight ------------------------------------------- */

typedef struct {
    volatile LONG   used;
    volatile LONG   cancel;
    volatile LONG   done;
    volatile LONG   count;
    ShNpcSpawnRequest req;
    uint64_t ent[SH_NPC_SPAWN_MAX];
} NpcJob;

static NpcJob g_jobs[SH_NPC_SPAWN_JOBS];
static CRITICAL_SECTION g_jobLock;
static volatile LONG g_jobInit = 0;

static void EnsureJobLock(void) {
    while (!g_jobInit) {
        if (InterlockedCompareExchange(&g_jobInit, 2, 0) == 0) {
            InitializeCriticalSection(&g_jobLock);
            InterlockedExchange(&g_jobInit, 1);
        }
    }
}

static NpcJob *JobOf(uint32_t job) {
    if (job == 0 || job > SH_NPC_SPAWN_JOBS) return NULL;
    if (!g_jobs[job - 1].used) return NULL;
    return &g_jobs[job - 1];
}

static DWORD WINAPI JobThread(LPVOID p) {
    NpcJob *j = (NpcJob *)p;

    SpawnBatch(&j->req, j->ent, SH_NPC_SPAWN_MAX, &j->cancel, &j->count);
    InterlockedExchange(&j->done, 1);
    return 0;
}

SH_API uint32_t ShNpcSpawnBegin(const ShNpcSpawnRequest *req) {
    NpcJob *j;
    int i, slot = -1;

    if (!req) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    EnsureJobLock();

    EnterCriticalSection(&g_jobLock);
    for (i = 0; i < SH_NPC_SPAWN_JOBS; i++)
        if (!g_jobs[i].used) { slot = i; g_jobs[i].used = 1; break; }
    LeaveCriticalSection(&g_jobLock);

    if (slot < 0) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }

    j = &g_jobs[slot];
    j->cancel = 0;
    j->done = 0;
    j->count = 0;
    j->req = *req;

    {
        /* Closed at once: the job is polled through j->done and never
         * joined, so keeping the thread object would leak one kernel
         * handle per spawn request for the rest of the session. */
        HANDLE th = CreateThread(NULL, 0, JobThread, j, 0, NULL);

        if (!th) {
            j->used = 0;
            ShSetError(SH_ERR_NO_CANDIDATE);
            return 0;
        }
        CloseHandle(th);
    }
    ShSetError(SH_OK);
    return (uint32_t)(slot + 1);
}

SH_API int ShNpcSpawnPoll(uint32_t job, uint64_t *out, int maxOut,
                          int *done) {
    NpcJob *j = JobOf(job);
    int n, i, w;

    if (!j) { ShSetError(SH_ERR_BAD_ARG); return -1; }

    n = (int)j->count;
    if (n < 0) n = 0;
    if (n > SH_NPC_SPAWN_MAX) n = SH_NPC_SPAWN_MAX;

    if (out && maxOut > 0) {
        w = n > maxOut ? maxOut : n;
        for (i = 0; i < w; i++) out[i] = j->ent[i];
    }
    if (done) *done = j->done ? 1 : 0;
    ShSetError(SH_OK);
    return n;
}

SH_API int ShNpcSpawnCancel(uint32_t job) {
    NpcJob *j = JobOf(job);

    if (!j) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    InterlockedExchange(&j->cancel, 1);
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShNpcSpawnEnd(uint32_t job) {
    NpcJob *j = JobOf(job);

    if (!j) { ShSetError(SH_ERR_BAD_ARG); return 0; }

    /* Still running: ask it to stop and let the caller come back
     * once it has. Freeing the slot now would pull it out from
     * under the worker. */
    if (!j->done) {
        InterlockedExchange(&j->cancel, 1);
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }

    EnterCriticalSection(&g_jobLock);
    j->used = 0;
    LeaveCriticalSection(&g_jobLock);
    ShSetError(SH_OK);
    return 1;
}
