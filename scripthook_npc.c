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
 * POOL / POPMGR / CONTEXT / NULL_BLOCK have no anchor in the image to
 * decode, and each is now either unused or reached only through a shape
 * check: the spawn path no longer needs the pool (the catalogue already
 * hands back each archetype's own block) or the pinned POPMGR slot (the
 * population manager comes from the engine's own three call sites, and is
 * used only if it looks like an engine object). It was the stale POPMGR
 * that crashed the game. */
#define RVA_POOL         0x4D89000
#define RVA_POPMGR       0x4B98F18
#define RVA_CONTEXT      0x4B90208
#define RVA_REGISTRY     0x4BC1878
#define RVA_ARCH_DESC    0x42C2570
#define RVA_NULL_BLOCK   0x4D88FE8
#define NPC_SPEC_VTABLE  SH_IMG(0x394A4E0)

#define COMMIT_MODE      7
#define SPAWN_MODE       1
#define NPC_CATEGORY     3
#define NPC_MAX          1024
/* How far above the probed surface a formation point is placed. Small on
 * purpose: the NPC materialises and settles on its own, and all the lift
 * has to do is keep the replacement from starting inside the ground. */
#define NPC_GROUND_LIFT  0.5f

extern int ShReadableAddr(uint64_t addr, size_t len);
extern uint64_t ShReadQ(uint64_t addr);
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

/* The population manager, for the one call the framework has to make with
 * an argument of its own: the engine's three call sites to the register
 * function each load it from a different global. Decoding those three
 * (each is a mov rcx,[rip+disp32] right before the call) gives the slots
 * below; which of them belongs to the mode in play is not knowable from
 * outside, so each is taken only if it looks like an engine object - a
 * readable pointer whose first word is a vtable in the image - and the one
 * that answers is the one used.
 *
 * This is the call that crashed the game: a stale slot produced a pointer
 * that was not a manager, and the register function wrote through it. */
static const uint64_t g_popSlots[3] = { 0x4BACFA8, 0x4B957A8, 0x4B99BA8 };

static uint64_t PopManager(void) {
    size_t i;

    for (i = 0; i < 3; i++) {
        uint64_t p = ShReadQ(ImgAddr(g_popSlots[i]));

        if (p && ShReadableAddr(p, 0x40) && ShInImage(ShReadQ(p)))
            return p;                 /* logged by the caller, with the spec */
    }
    return 0;
}

static void SpawnOnGameThread(uint64_t id, const void *mtx) {
    uint64_t mgr, arch, archBlk, csBlk, cs, spec, old, ctx, pop;
    uint32_t camp = 0xFFFFFFFFu, job = 0xFFFFFFFFu;

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

    spec = ((Spawn_t)ImgAddr(RVA_SPAWN))(cs, SPAWN_MODE, mtx);
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

    ctx = ShReadQ(ImgAddr(RVA_CONTEXT));
    if (ctx && ShReadableAddr(ctx + 0x1B4, 8)) {
        camp = *(volatile uint32_t *)(uintptr_t)(ctx + 0x1B4);
        job  = *(volatile uint32_t *)(uintptr_t)(ctx + 0x1B8);
    }
    *(volatile uint32_t *)(uintptr_t)(spec + 0x2D0) = camp;
    *(volatile uint32_t *)(uintptr_t)(spec + 0x2D4) = job;

    pop = PopManager();
    if (pop) ((Reg_t)ImgAddr(RVA_POP_REGISTER))(pop, spec);
    else NpcWhy("no usable population manager; registration skipped", 0,
                ImgAddr(RVA_POP_REGISTER));

    ((Commit_t)ImgAddr(RVA_COMMIT))(mgr, COMMIT_MODE, spec);
    g_pendSpec = spec;
    g_pendErr = 0;
    /* The data slots this path reads, in one line. They cannot be checked
     * offline - no rip-relative reference to them survives in the image to
     * match - so a session is what says which slot still holds what. */
    /* Written per spawn, not once: whether the population registration went
     * through is the one thing this line has to say, and the catalogue line
     * already holds the module's once-only flag. log.h keeps its handle per
     * translation unit, so the file is opened only if the catalogue has not
     * opened it already - reopening would truncate what was written. */
    if (!g_logFile) LogInit("scripthook_npc.log");
    Log("npc: spawn id %llX -> spec %llX (mgr %llX pop %llX ctx %llX, %s)",
        (unsigned long long)id, (unsigned long long)spec,
        (unsigned long long)mgr, (unsigned long long)pop,
        (unsigned long long)ctx, pop ? "registered" : "NOT registered");
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

/* Called from the physics hook, next to ShSpawnPump. */
void ShNpcPump(void) {
    int did = 0;

    if (g_killEnt) {
        uint64_t e = g_killEnt;
        g_killEnt = 0;
        DespawnOnGameThread(e);
        g_killDone = 1;
        did = 1;
    }

    if (g_listWanted) {
        g_listWanted = 0;
        ListOnGameThread();
        g_listDone = 1;
        did = 1;
    }
    if (g_pendId) {
        uint64_t id = g_pendId;
        const void *mtx = g_pendMtx;
        g_pendId = 0;
        if (mtx) SpawnOnGameThread(id, mtx);
        g_pendDone = 1;
        did = 1;
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
 * Recouped from NPCSpawner.asi, which is the only place the
 * grouping has ever been written down: the engine keeps an
 * archetype's faction to itself and ShNpcArchetype has no name.
 * The four id tables win; the engine's kind is the fallback.
 * docs/npcspawner-reverse.md carries the evidence, and the
 * originals of these bytes.
 */

static const char *g_groupNames[SH_NPC_GROUP_MAX] = {
    "Santa Blanca", "Unidad", "Rebels", "Civilians", "Special"
};

/* Never spawned: the original's blacklist. @0x180007C00 */
static const uint64_t g_blacklist[44] = {
    0x5325BD7A52ULL, 0x78FADA79FFULL, 0x5325BD7A4DULL, 0x78FADA4D05ULL,
    0x5325BD7A48ULL, 0x31B65512F7ULL, 0x10F2A192C3DULL, 0x10F2A192C35ULL,
    0x3D66E0ABDFULL, 0x2309B3C694ULL, 0x5325BD7627ULL, 0x2FD455483EULL,
    0x237A1CBFBAULL, 0x237A1CBFB9ULL, 0x10F2A192C39ULL, 0x4738A95B71ULL,
    0x51021FED77ULL, 0x4FEB645A8DULL, 0x4FEB645A8EULL, 0x4AC59FEDD9ULL,
    0x4FEB647516ULL, 0x4FEB6459B2ULL, 0x4AC59FEDD7ULL, 0x4FEB6459EBULL,
    0x4D8AB38F5AULL, 0x4FEB6459ECULL, 0x4AC59FEDD6ULL, 0x4FEB647517ULL,
    0x4AC59FEDD8ULL, 0x4D8AB38F5BULL, 0x4AC59FEDDAULL, 0x4FEB6459B1ULL,
    0x7C33CC49CAULL, 0x3456303A13ULL, 0x5B708516B0ULL, 0x3F73BD8D99ULL,
    0x3F4892BDFDULL, 0x8AFE25F47FULL, 0x6E164F05B3ULL, 0x3F4892BCF7ULL,
    0x3456303D78ULL, 0x2EC3CD6993ULL, 0x2DF374C80CULL, 0xBBE631D833ULL
};

/* Special, explicitly listed. @0x180007D60 */
static const uint64_t g_special[76] = {
    0x1A987A8752DULL, 0x1AFB8794FE6ULL, 0x1B155F83D72ULL, 0x1AFB8751E8EULL,
    0x1A987A5256FULL, 0x1B155F6628CULL, 0x1AFB8794FEAULL, 0x1A987A5FFD7ULL,
    0x183A1B02363ULL, 0x185B81997B0ULL, 0x18173A2EB26ULL, 0x18173A5399AULL,
    0x187334C4DB7ULL, 0x185B81B4750ULL, 0x187E427E5C5ULL, 0x18173A30AC6ULL,
    0x18173A2FCA4ULL, 0x18173A3102FULL, 0x197A932C062ULL, 0x18F3D2B2FA2ULL,
    0x1A16C520991ULL, 0x18D7BD5327EULL, 0x1994707A751ULL, 0x8A9482DAC2ULL,
    0x7C0B092643ULL,  0x8A9482DACCULL, 0x14397E627AEULL, 0x154BBBC37D0ULL,
    0x154BBB495E1ULL, 0x537991063FULL, 0x4AC59FCA66ULL, 0x5379910630ULL,
    0x537991063EULL,  0x4AC59FCA65ULL, 0x5379910631ULL, 0x4AC59FCA64ULL,
    0x68EB25F118ULL,  0x433A9B6E26ULL, 0x68EB25EB16ULL, 0x7D662A1D27ULL,
    0x78C9B348CFULL,  0x45F1E58279ULL, 0x792C60E200ULL, 0x18B72EE403DULL,
    0x198B997684CULL, 0x71CBB77732ULL, 0x71CBB77725ULL, 0x71CBB77721ULL,
    0x71CBB77722ULL,  0x71CBB7772DULL, 0x71CBB77720ULL, 0x71CBB77739ULL,
    0x7929219EF8ULL,  0x45F1E58223ULL, 0x71CBB77729ULL, 0x71CBB77724ULL,
    0x71CBB77727ULL,  0x71CBB77736ULL, 0x147CD1A13C3ULL, 0x82AF1233BCULL,
    0x71CBB77735ULL,  0x71CBB77731ULL, 0x71CBB7772CULL, 0x71CBB77728ULL,
    0x71CBB7772BULL,  0x71CBB77726ULL, 0x71CBB77734ULL, 0x71CBB7772AULL,
    0x71CBB77730ULL,  0x71CBB7773AULL, 0x71CBB77738ULL, 0x7926908976ULL,
    0x71CBB77737ULL,  0xCA9DCD4408ULL, 0x71CBB77723ULL, 0x7C33CCA452ULL
};

/* @0x180007FC0 */
static const uint64_t g_santaBlanca[2] = {
    0xF645ED5E6DULL, 0x154BBBC8ADAULL
};

/* @0x180007FE8 */
static const uint64_t g_civilians[3] = {
    0x1AFB8765956ULL, 0x7DECAB61CDULL, 0x7DECAB1E1DULL
};

/* The one explicitly listed Unidad archetype. */
#define NPC_ID_UNIDAD 0x1A987A7937CULL

static int InTable(const uint64_t *t, int n, uint64_t v) {
    int i;
    for (i = 0; i < n; i++) if (t[i] == v) return 1;
    return 0;
}

SH_API int ShNpcGroupCount(void) {
    return SH_NPC_GROUP_MAX;
}

SH_API const char *ShNpcGroupName(int group) {
    if (group < 0 || group >= SH_NPC_GROUP_MAX) return "";
    return g_groupNames[group];
}

/* The original's 0x1800011C0, reshaped: it asked "is this in
 * group G", which for one archetype answers exactly one group,
 * so one return says the same thing and is cheaper to ask. */
SH_API int ShNpcGroupOfArchetype(const ShNpcArchetype *a) {
    uint64_t id;
    int kind;

    if (!a) return -1;
    id = a->id;
    kind = a->kind;

    if (InTable(g_blacklist, 44, id)) return -1;
    if (InTable(g_santaBlanca, 2, id)) return SH_NPC_GROUP_SANTA_BLANCA;
    if (id == NPC_ID_UNIDAD) return SH_NPC_GROUP_UNIDAD;
    if (InTable(g_civilians, 3, id)) return SH_NPC_GROUP_CIVILIANS;
    if (InTable(g_special, 76, id)) return SH_NPC_GROUP_SPECIAL;

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

#define PI_F      3.14159265f
#define TWO_PI_F  6.28318531f
#define INV_2P24  5.9604645e-8f   /* 2^-24, the PRNG scale */

/* One point, in the plane the caller then rotates by yaw.
 * Mirrors NPCSpawner.asi's 0x180001530. */
static void FormationPoint(int formation, int i, int n,
                           unsigned *seed, float *ox, float *oy) {
    float a;

    *ox = 0.0f;
    *oy = 0.0f;
    if (n <= 1) return;

    switch (formation) {
    case SH_NPC_FORMATION_LINE:
        *ox = ((float)i - (float)(n - 1) * 0.5f) * 3.0f;
        break;

    case SH_NPC_FORMATION_SPREAD: {
        /* A three column grid, every row centred on its own. */
        int q = i / 3, r = i % 3;
        int rowLen = n - 3 * q;
        int rows = (n + 2) / 3;
        *ox = ((float)r - (float)(rowLen - 1) * 0.5f) * 3.5f;
        *oy = ((float)q - ((float)rows - 1.0f) * 0.5f) * 3.5f;
        break;
    }

    case SH_NPC_FORMATION_SEMICIRCLE:
        a = PI_F * ((float)i / (float)(n - 1)) - PI_F * 0.5f;
        *ox = 5.0f * cosf(a);
        *oy = 5.0f * sinf(a);
        break;

    case SH_NPC_FORMATION_CIRCLE:
        a = TWO_PI_F * (float)i / (float)n;
        *ox = 4.0f * cosf(a);
        *oy = 4.0f * sinf(a);
        break;

    default: {
        /* Random: a 24 bit hash of the running seed gives the
         * angle a jitter inside its slice and the radius a value
         * in [2.5, 7.0). */
        unsigned s = *seed;
        unsigned h1 = (s * 0x19660Du + 0x3C6EF35Fu) & 0xFFFFFFu;
        float r1, r2, rad;

        s = s * 0x17385CA9u + 0x47502932u;
        *seed = s;

        r1 = (float)h1 * INV_2P24;
        r2 = (float)(s & 0xFFFFFFu) * INV_2P24;

        a = (TWO_PI_F / (float)n) * ((float)i + r1);
        rad = 2.5f + 4.5f * r2;
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

/* ---- one batch --------------------------------------------------- */

/* Runs on whichever thread calls it. progress, when it is not
 * NULL, is republished after every NPC so a poll can watch the
 * batch fill up; cancel is read between NPCs. Mirrors the
 * original's worker at 0x180002070, including its refusal to
 * work while the world is loading and its habit of dropping an
 * NPC it spawned just as the world went away. */
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
         * The planner leaves z at the origin's own, mirroring the original
         * plugin, and that is what buries NPCs: a formation is metres across,
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
        pos[i].z = gz + NPC_GROUND_LIFT;

        e = ShSpawnNpc(req->id, &pos[i]);
        if (!e) continue;

        st = ShGetGameState();
        if (st == SH_STATE_LOADING || st == SH_STATE_RELOADING) continue;
        if (cancel && *cancel) break;

        out[got++] = e;
        if (progress) InterlockedExchange(progress, got);

        if (req->facing == SH_NPC_FACING_PLAYER) {
            /* Turn it to look at the player, in radians. */
            float dx = pp.x - pos[i].x;
            float dy = pp.y - pos[i].y;
            ShQueueTransform(e, &pos[i], atan2f(dy, dx), 0.0f, 0.0f);
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
