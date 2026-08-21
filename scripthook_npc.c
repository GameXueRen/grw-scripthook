/* NPC spawning, mirrored from the engine's spawn director
 * (FUN_148AE7AA0). Same pump model as the vehicle spawner.
 */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

/* RVAs, so this survives a relocated image. */
#define RVA_MGR_GETTER   0x916DC40
#define RVA_SPAWN        0x916D5E0
#define RVA_COMMIT       0x916E590
#define RVA_SET_CATEGORY 0xA62A8A0
#define RVA_SET_174      0xA62B3B0
#define RVA_POP_REGISTER 0x85B7240
#define RVA_COLLECT      0xC41D6C0
#define RVA_KIND         0x83B1390
#define RVA_POOL_FIND    0xDF4EE30

/* Despawn, from the Domino UnspawnFromEntity node: the
 * entity's spawning spec, then retire it. Verified live. */
#define RVA_SPEC_OF      0xA604700
#define RVA_RETIRE       0x921A2F0

#define RVA_POOL         0x4D89000
#define RVA_POPMGR       0x4B98F18
#define RVA_CONTEXT      0x4B90208
#define RVA_REGISTRY     0x4BC17F8
#define RVA_ARCH_DESC    0x42C2560
#define RVA_NULL_BLOCK   0x4D88FE8
#define NPC_SPEC_VTABLE  SH_IMG(0x394A660)

#define COMMIT_MODE      7
#define SPAWN_MODE       1
#define NPC_CATEGORY     3
#define NPC_MAX          1024

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

static void ListOnGameThread(void) {
    ArchList hdr;
    uint64_t reg = ShReadQ(ImgAddr(RVA_REGISTRY));
    int i, n = 0;

    memset(&hdr, 0, sizeof(hdr));
    if (!reg) return;
    ((Collect_t)ImgAddr(RVA_COLLECT))(reg, ImgAddr(RVA_ARCH_DESC), &hdr);
    if (!hdr.ptr || hdr.cnt > 0x4000 ||
        !ShReadableAddr(hdr.ptr, (size_t)hdr.cnt * 8))
        return;
    for (i = 0; i < hdr.cnt && n < NPC_MAX; i++) {
        uint64_t blk = ShReadQ(hdr.ptr + (uint64_t)i * 8);
        uint64_t obj = BlockObj(blk);
        if (!obj) continue;
        g_npcs[n].id = ShReadQ(blk + 0x10);
        g_npcs[n].kind = ((Kind_t)ImgAddr(RVA_KIND))(obj);
        n++;
    }
    /* The collector's array stays with the engine's pool;
     * a few KB once per session. */
    g_npcCount = n;
}

/* ---- one spawn, on the game thread ---- */

/* The pump signals this, so a spawn costs the frame the
 * engine needs and no polling granularity on top. */
static HANDLE g_pumpEvent;

static volatile uint64_t g_pendId = 0;
static const void *g_pendMtx = NULL;
static volatile uint64_t g_pendSpec = 0;
static volatile int g_pendDone = 0;
static volatile int g_pendErr = 0;

static uint64_t ArchetypeBlock(uint64_t id) {
    uint64_t map = ShReadQ(ImgAddr(RVA_POOL) + 0x100), cell;
    if (!map) return 0;
    cell = ((PoolFind_t)ImgAddr(RVA_POOL_FIND))(map, id, 0);
    return cell ? ShReadQ(cell) : 0;
}

static void SpawnOnGameThread(uint64_t id, const void *mtx) {
    uint64_t mgr, arch, archBlk, csBlk, cs, spec, old, ctx, pop;
    uint32_t camp = 0xFFFFFFFFu, job = 0xFFFFFFFFu;

    g_pendErr = SH_ERR_NO_CANDIDATE;
    archBlk = ArchetypeBlock(id);
    arch = BlockObj(archBlk);
    if (!arch) return;
    csBlk = ShReadQ(arch + 0x48);
    cs = BlockObj(csBlk);
    if (!cs) return;
    mgr = ((MgrGet_t)ImgAddr(RVA_MGR_GETTER))();
    if (!mgr) return;

    spec = ((Spawn_t)ImgAddr(RVA_SPAWN))(cs, SPAWN_MODE, mtx);
    if (!spec) return;

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

    pop = ShReadQ(ImgAddr(RVA_POPMGR));
    if (pop) ((Reg_t)ImgAddr(RVA_POP_REGISTER))(pop, spec);

    ((Commit_t)ImgAddr(RVA_COMMIT))(mgr, COMMIT_MODE, spec);
    g_pendSpec = spec;
    g_pendErr = 0;
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
    if (!spec) return;
    if (!ShReadableAddr(spec, 0x180)) return;
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
    if (did && g_pumpEvent) SetEvent(g_pumpEvent);
}

static int WaitFlag(volatile int *flag, int ms) {
    DWORD end = GetTickCount() + (DWORD)ms;

    if (!g_pumpEvent)
        g_pumpEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    while (!*flag) {
        DWORD now = GetTickCount();
        if (now >= end) break;
        if (g_pumpEvent)
            WaitForSingleObject(g_pumpEvent, end - now);
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
