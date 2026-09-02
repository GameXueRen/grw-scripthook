/* Vehicle spawning. The engine has no SpawnVehicle, it
 * has a spec and a spawner the manager owns.
 */
#include <windows.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"
#include "log.h"

/* RVAs, so this survives a relocated image. */
#define RVA_MGR_GETTER   0x916DC40
#define RVA_SPAWN        0x916D5E0
#define RVA_COMMIT       0x916E590

/* A vehicle is named by a masked handle: the kind hash
 * below, with the vehicle id in the high dword.
 */
#define VEH_KIND_HASH    0x8F2CBBBAu
#define SPEC_VTABLE      SH_IMG(0x394A1E0)
#define SPEC_HANDLE_OFF  0x28
#define COMMIT_MODE      7
#define SPAWN_MODE       1

extern int ShReadableAddr(uint64_t addr, size_t len);
extern uint64_t ShReadQ(uint64_t addr);
extern int ShReadMem(uint64_t addr, void *out, size_t len);
extern void ShSetError(int err);
extern int ShRequireInGame(void);
extern int ShGetPlayer(ShPlayer *out);

typedef uint64_t (__attribute__((ms_abi)) *MgrGet_t)(void);
typedef uint64_t (__attribute__((ms_abi)) *Spawn_t)(uint64_t, int,
                                                    const void *);
typedef uint64_t (__attribute__((ms_abi)) *Commit_t)(uint64_t, int,
                                                     uint64_t);

static uint64_t ImgAddr(uint64_t rva) {
    return (uint64_t)(uintptr_t)GetModuleHandleA(NULL) + rva;
}

/* Named by eye, one spawn at a time. Ids are stable. */
static const ShVehicle g_vehicles[] = {
    { 0x40062BA8, "Off road bike, civilian" },
    { 0x4007466A, "Tommy bike, civilian" },
    { 0x4007466B, "Tommy bike, rebels" },
    { 0x40081214, "Alpaca, static, FREEZES ON ENTRY" },
    { 0x40727FB6, "Tractor, civilian" },
    { 0x40807B6A, "4x4, Santa Blanca" },
    { 0x408240A5, "Buggy, Unidad" },
    { 0x40828265, "DXI sedan, civilian" },
    { 0x40893339, "4x4, Unidad" },
    { 0x408A2E76, "SUV, civilian" },
    { 0x408BEE7D, "Sumitzu car, civilian" },
    { 0x408F9C00, "Minibus, rebels" },
    { 0x40904800, "Minibus, civilian" },
    { 0x40919FB5, "Sumitzu 200GT, civilian" },
    { 0x4092D408, "Sumitzu hatchback, civilian" },
    { 0x4093151B, "BLOCK pickup, civilian" },
    { 0x40949CF8, "Sumitzu Carry Ace 250 van" },
    { 0x40957471, "Technical, rebels" },
    { 0x409668C2, "Paranero, Santa Blanca" },
    { 0x409668C3, "Paranero, Santa Blanca, default" },
    { 0x409C1F36, "Landrock armed, Santa Blanca" },
    { 0x409C4000, "Minibus, civilian, white" },
    { 0x409E634C, "Trophy truck, Santa Blanca" },
    { 0x40A0F748, "4x4 armed, Unidad" },
    { 0x40A2E34A, "Chobolet sedan, Santa Blanca" },
    { 0x40A4CB08, "AMV, Unidad" },
    { 0x40A7371F, "Mercedes style sedan, Santa Blanca" },
    { 0x40A8DDD4, "Nakahawa pickup, civilian" },
    { 0x40A90268, "Monster truck, civilian" },
    { 0x40ADABD8, "Decussine sedan, civilian" },
    { 0x40AF4531, "Decussine SUV, Santa Blanca" },
    { 0x40B177BF, "Landrock van, civilian" },
    { 0x40B42154, "Decussine 90s, Santa Blanca" },
    { 0x40B48DBC, "HELICOPTER" },
    { 0x40B51B8B, "Decussine SUV, civilian" },
    { 0x40BA6E9D, "Monster, unused, FREEZES ON ENTRY" },
    { 0x40BC24EC, "Zeus pickup, Santa Blanca" },
    { 0x40C08000, "MRAP, Unidad" },
    { 0x40C1D4CE, "Wooden boat, Last Rites" },
    { 0x40D7800E, "Fohd pickup, Unidad" },
    { 0x40D99000, "Brubeck tow truck, Los Penitentes" },
    { 0x40D994DE, "Brubeck tow truck, rebels" },
    { 0x40DA98C4, "Armoured ambulance, cut but driveable" },
    { 0x40DF16D3, "Dinghy, Santa Blanca" },
    { 0x40E7D196, "Scoossna 171, plane, Santa Blanca" },
    { 0x40E9BEE5, "Convoy ambulance, Santa Blanca" },
    { 0x40F0E42D, "Mama Cocha advert truck, Unidad" },
    { 0x40F2361A, "Brubeck oil truck, Los Penitentes" },
    { 0x40FCBA9C, "APC, Santa Blanca" },
    { 0x40FCC288, "APC, Unidad" },

    /* The 0x41 block, missed by the first scan because it
     * filtered on a 0x40 prefix it had only assumed.
     */
    { 0x4102F196, "GUNSHIP, Unidad" },
    { 0x41033AB7, "Boxcar truck, Santa Blanca" },
    { 0x410420B6, "Barracks truck, Unidad" },
    { 0x410654CC, "Boxcar, Santa Blanca" },
    { 0x410654CD, "Murder disposal truck, Santa Blanca" },
    { 0x410654CE, "Rancho Luna advert truck, Santa Blanca" },
    { 0x410824BE, "Digger, civilian" },
    { 0x4108BE91, "KILLDOZER, armoured" },
    { 0x410B93B6, "Gunboat, Unidad" },
    { 0x4126AEDF, "Gunboat, Santa Blanca" },
    { 0x41322E4B, "Convoy comms truck, Santa Blanca" },
    { 0x41378E20, "Yacht, honks at itself, civilian" },
    { 0x4139EEFE, "Classic airplane, civilian" },
    { 0x4146EAF5, "Cossna 172, civilian" },
    { 0x4159D6C8, "UH-60, Santa Blanca" },
};

#define VEHICLE_COUNT \
    ((int)(sizeof(g_vehicles) / sizeof(g_vehicles[0])))

/* Spec addresses move each session, so resolve once on
 * demand and remember. Bounded, never on a hot path.
 */
static volatile uint64_t g_specCache[VEHICLE_COUNT];
/* A vehicle whose spec was not found in the walk is marked
 * tried, so the next request for it does not re-walk the whole
 * address space looking again. */
static volatile unsigned char g_specTried[VEHICLE_COUNT];

/* FindAllSpecs runs once on a background thread (ShSpawnWarm)
 * and once on demand; the lock keeps the two from colliding.
 * The pending spawn request is handed to the physics hook
 * through a lock and a request sequence, so a request can
 * never be mistaken for an earlier one that already ran. */
static CRITICAL_SECTION g_specLock;
static CRITICAL_SECTION g_pendLock;
static volatile LONG g_locksInit = 0;

static void EnsureLocks(void) {
    while (!g_locksInit) {
        if (InterlockedCompareExchange(&g_locksInit, 2, 0) == 0) {
            InitializeCriticalSection(&g_specLock);
            InitializeCriticalSection(&g_pendLock);
            LogInit("scripthook_spawn.log");
            InterlockedExchange(&g_locksInit, 1);
        }
    }
}

/* Transform buffers. A single one handed to the engine and
 * then rewritten froze the game, so rotate.
 */
#define MTX_RING 64
static SH_ALIGNED(16) uint8_t g_mtx[MTX_RING][64];
static int g_mtxNext = 0;

int ShVehicleCount(void) { return VEHICLE_COUNT; }

const ShVehicle *ShVehicleAt(int index) {
    if (index < 0 || index >= VEHICLE_COUNT) return NULL;
    return &g_vehicles[index];
}

const char *ShVehicleName(uint32_t vehicleId) {
    int i;
    for (i = 0; i < VEHICLE_COUNT; i++)
        if (g_vehicles[i].id == vehicleId)
            return g_vehicles[i].name;
    return NULL;
}

/* One bounded walk of committed memory that resolves EVERY
 * unresolved vehicle spec at once. A per-vehicle walk is the
 * same cost as this whole walk (the read time dominates), so
 * doing it once for all vehicles is what makes repeated spawns
 * fast instead of re-walking the address space for each new
 * vehicle. The spec starts SPEC_HANDLE_OFF below the handle.
 *
 * The address space is split across a few worker threads: a full
 * scan reads tens of gigabytes of committed writable memory, and
 * a single thread took ~13s even with direct reads.
 */
#define SCAN_WORKERS 4

/* Shared "wanted spec" state for the parallel scan. The low dword
 * (VEH_KIND_HASH) rejects almost every memory value in one compare;
 * only hash hits take the lock. */
static volatile uint64_t g_scanWants[VEHICLE_COUNT];
static volatile int     g_scanIdx[VEHICLE_COUNT];
static volatile int     g_scanLeft = 0;
static uint8_t g_scanBuf[SCAN_WORKERS][0x10000];
static volatile LONG    g_scanBusy = 0;

typedef struct { uintptr_t lo, hi; int id; } ScanSeg;

/* The region was just reported committed and writable by
 * VirtualQuery, so a direct read is safe; SEH catches a page freed
 * in the tiny gap instead of killing the game. ReadProcessMemory
 * per 64KB chunk costs a kernel call and made a full scan take
 * ~21s. */
static int ChunkRead(uint8_t *dst, const void *src, size_t n) {
#if defined(_MSC_VER)
    int ok = 0;
    __try {
        memcpy(dst, src, n);
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }
    return ok;
#else
    return ShReadMem((uint64_t)(uintptr_t)src, dst, n);
#endif
}

static DWORD WINAPI ScanWorker(LPVOID p) {
    ScanSeg *seg = (ScanSeg *)p;
    uint8_t *scan = (uint8_t *)seg->lo;
    uint8_t *buf = g_scanBuf[seg->id];
    MEMORY_BASIC_INFORMATION mbi;

    while ((uintptr_t)scan < seg->hi &&
           VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        size_t sz, o, k, got;

        if (next <= scan) break;
        if ((uintptr_t)mbi.BaseAddress >= seg->hi) break;
        if (g_scanLeft <= 0) break;
        if (mbi.State != MEM_COMMIT ||
            !(mbi.Protect & PAGE_READWRITE) ||
            (mbi.Protect & PAGE_GUARD))
        {
            scan = next;
            continue;
        }

        sz = mbi.RegionSize;
        if ((uintptr_t)mbi.BaseAddress + sz > seg->hi)
            sz = seg->hi - (uintptr_t)mbi.BaseAddress;

        /* Chunked kernel reads: a page freed mid scan skips
         * instead of faulting. Chunks overlap so no candidate
         * spans a seam. */
        for (o = 0; o + 8 <= sz && g_scanLeft > 0;
             o += sizeof(g_scanBuf[0]) - 8) {
            got = sz - o;
            if (got > sizeof(g_scanBuf[0])) got = sizeof(g_scanBuf[0]);
            if (!ChunkRead(buf, (const void *)((uint8_t *)mbi.BaseAddress + o),
                           got))
                continue;
            for (k = 0; k + 8 <= got && g_scanLeft > 0; k += 8) {
                uint64_t v;
                int w;
                memcpy(&v, buf + k, 8);
                if ((uint32_t)v != VEH_KIND_HASH) continue;
                EnterCriticalSection(&g_specLock);
                if (g_scanLeft <= 0) {
                    LeaveCriticalSection(&g_specLock);
                    return 0;
                }
                for (w = 0; w < g_scanLeft; w++) {
                    if (v != g_scanWants[w]) continue;
                    {
                        uint64_t base =
                            (uint64_t)(uintptr_t)((uint8_t *)mbi.BaseAddress
                                                  + o + k)
                            - SPEC_HANDLE_OFF;
                        if (ShReadQ(base) == SPEC_VTABLE) {
                            g_specCache[g_scanIdx[w]] = base;
                            g_scanWants[w] = g_scanWants[g_scanLeft - 1];
                            g_scanIdx[w] = g_scanIdx[g_scanLeft - 1];
                            g_scanLeft--;
                            w--; /* recheck swapped entry */
                        }
                    }
                }
                LeaveCriticalSection(&g_specLock);
            }
        }
        scan = next;
    }
    return 0;
}

static void FindAllSpecs(void) {
    ScanSeg seg[SCAN_WORKERS];
    HANDLE th[SCAN_WORKERS];
    uintptr_t lo = 0x10000000, span;
    int remaining, i, nTh = 0;

    /* Already scanning (warm thread): wait for it and leave. The
     * caller then re-checks the cache. */
    if (InterlockedCompareExchange(&g_scanBusy, 1, 0)) {
        while (InterlockedCompareExchange(&g_scanBusy, 0, 0))
            Sleep(5);
        return;
    }

    EnterCriticalSection(&g_specLock);
    /* Collect what is still unresolved. */
    remaining = 0;
    for (i = 0; i < VEHICLE_COUNT; i++) {
        if (g_specCache[i] && ShReadQ(g_specCache[i]) == SPEC_VTABLE)
            continue;
        g_scanWants[remaining] = ((uint64_t)g_vehicles[i].id << 32)
                               | VEH_KIND_HASH;
        g_scanIdx[remaining] = i;
        remaining++;
    }
    if (remaining == 0) {
        for (i = 0; i < VEHICLE_COUNT; i++) g_specTried[i] = 1;
        LeaveCriticalSection(&g_specLock);
        InterlockedExchange(&g_scanBusy, 0);
        return;
    }
    g_scanLeft = remaining;
    LeaveCriticalSection(&g_specLock);

    /* Split the address space into worker ranges. */
    span = (uintptr_t)0x800000000000ULL - lo;
    for (i = 0; i < SCAN_WORKERS; i++) {
        seg[i].lo = lo + span / SCAN_WORKERS * (uintptr_t)i;
        seg[i].hi = lo + span / SCAN_WORKERS * (uintptr_t)(i + 1);
        seg[i].id = i;
        th[i] = CreateThread(NULL, 0, ScanWorker, &seg[i], 0, NULL);
        if (th[i]) nTh++;
    }
    if (nTh == 0) {
        /* Worker creation failed; do it inline as a fallback. */
        seg[0].lo = lo;
        seg[0].hi = (uintptr_t)0x800000000000ULL;
        seg[0].id = 0;
        ScanWorker(&seg[0]);
    } else {
        for (i = 0; i < SCAN_WORKERS; i++)
            if (th[i]) {
                WaitForSingleObject(th[i], INFINITE);
                CloseHandle(th[i]);
            }
    }

    EnterCriticalSection(&g_specLock);
    remaining = g_scanLeft;
    for (i = 0; i < VEHICLE_COUNT; i++) g_specTried[i] = 1;
    LeaveCriticalSection(&g_specLock);
    InterlockedExchange(&g_scanBusy, 0);
    Log("spawn: spec scan done, %d unresolved", remaining);
}

static uint64_t SpecFor(uint32_t vehicleId) {
    int i, need = 0;

    for (i = 0; i < VEHICLE_COUNT; i++) {
        if (g_vehicles[i].id != vehicleId) continue;
        if (g_specCache[i] &&
            ShReadQ(g_specCache[i]) == SPEC_VTABLE)
            return g_specCache[i];
        if (!g_specTried[i]) need = 1;
        break;
    }
    if (need) FindAllSpecs();
    if (g_specCache[i] &&
        ShReadQ(g_specCache[i]) == SPEC_VTABLE)
        return g_specCache[i];
    return 0;
}

void ShSpawnInvalidate(void) {
    int i;
    EnsureLocks();
    EnterCriticalSection(&g_specLock);
    /* Keep the cached addresses: a world reload usually leaves the
     * spec objects where they were, so FindAllSpecs only rescans
     * the entries whose vtable check fails. Clearing the cache here
     * forced a full ~13s scan on every reload for no reason. */
    for (i = 0; i < VEHICLE_COUNT; i++) g_specTried[i] = 0;
    LeaveCriticalSection(&g_specLock);
}

/* Transform from the player's own, so the basis is what
 * the engine calls upright. Shared with the NPC spawner.
 */
const void *ShSpawnBuildMatrix(const ShVec3 *pos) {
    ShPlayer p;
    uint8_t *slot;
    float t[4];

    if (!ShGetPlayer(&p) || !p.root) return NULL;
    if (!ShReadableAddr(p.root + 0x20, 64)) return NULL;

    slot = g_mtx[g_mtxNext];
    g_mtxNext = (g_mtxNext + 1) % MTX_RING;

    memcpy(slot, (const void *)(uintptr_t)(p.root + 0x20), 64);
    t[0] = pos->x; t[1] = pos->y; t[2] = pos->z; t[3] = 1.0f;
    memcpy(slot + 0x30, t, 16);
    return slot;
}

/* Runs on the game thread, queued by ShSpawnVehicle. The pump
 * takes the newest pending request under the lock and records
 * the request sequence it consumed; a caller waits for its own
 * sequence, so a request overtaken by a newer one is handled by
 * the next pump instead of being lost. */
static volatile uint64_t g_pendSpec = 0;
static volatile const void *g_pendMtx = NULL;
static volatile uint32_t g_pendSeq = 0;
static volatile uint32_t g_doneSeq = 0;

void ShSpawnPump(void) {
    uint64_t spec = 0, mgr, spawner;
    const void *mtx = NULL;
    uint32_t my = 0;

    EnsureLocks();
    EnterCriticalSection(&g_pendLock);
    spec = g_pendSpec;
    mtx = (const void *)g_pendMtx;
    if (spec && mtx) {
        my = g_pendSeq;
        g_pendSpec = 0;
        g_pendMtx = NULL;
    }
    LeaveCriticalSection(&g_pendLock);

    if (!spec) {
        /* Nothing queued: let every waiter through so a request
         * already consumed by an earlier pump is not stuck. */
        g_doneSeq = g_pendSeq;
        return;
    }

    mgr = ((MgrGet_t)ImgAddr(RVA_MGR_GETTER))();
    if (mgr) {
        spawner = ((Spawn_t)ImgAddr(RVA_SPAWN))(spec, SPAWN_MODE,
                                                mtx);
        /* The commit is required, or nothing ever appears. */
        if (spawner)
            ((Commit_t)ImgAddr(RVA_COMMIT))(mgr, COMMIT_MODE,
                                            spawner);
        Log("pump: seq=%u spawner=%llx", my,
            (unsigned long long)spawner);
    } else {
        Log("pump: seq=%u no manager", my);
    }
    g_doneSeq = my;
}

/* The entity arrives a frame or two after the commit, so
 * look for the vehicle nearest where we asked for it.
 */
static uint64_t EntityNear(const ShVec3 *pos, float tol) {
    ShEntity found[48];
    uint64_t best = 0;
    float bd = tol;
    int n, i;

    n = ShFindEntities(SH_KIND_VEHICLE, 120.0f, 0, found, 48);
    for (i = 0; i < n; i++) {
        float dx = found[i].pos.x - pos->x;
        float dy = found[i].pos.y - pos->y;
        float dz = found[i].pos.z - pos->z;
        float d = dx * dx + dy * dy + dz * dz;
        if (d < bd * bd) {
            bd = (float)sqrt((double)d);
            best = found[i].entity;
        }
    }
    return best;
}

uint64_t ShSpawnVehicle(uint32_t vehicleId, const ShVec3 *pos) {
    uint64_t spec, ent = 0;
    const void *mtx;
    uint32_t mySeq;
    int waited;

    if (!pos) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (!ShRequireInGame()) return 0;

    EnsureLocks();
    spec = SpecFor(vehicleId);
    if (!spec) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }

    mtx = ShSpawnBuildMatrix(pos);
    if (!mtx) { ShSetError(SH_ERR_NO_ROOT); return 0; }

    EnterCriticalSection(&g_pendLock);
    g_pendSpec = spec;
    g_pendMtx = mtx;
    mySeq = ++g_pendSeq;
    LeaveCriticalSection(&g_pendLock);
    Log("spawn: id=%x seq=%u", vehicleId, mySeq);

    /* The pump runs in the physics hook, so wait for it. The
     * hook fires every physics frame, so poll tightly. */
    for (waited = 0; waited < 300 && g_doneSeq < mySeq; waited++)
        Sleep(2);
    if (g_doneSeq < mySeq) {
        /* Timed out: pull the request unless a newer one has
         * already replaced it (that one gets its own pump). */
        EnterCriticalSection(&g_pendLock);
        if (g_pendSeq == mySeq) {
            g_pendSpec = 0;
            g_pendMtx = NULL;
        }
        LeaveCriticalSection(&g_pendLock);
        ShSetError(SH_ERR_NO_PHYSICS);
        Log("spawn: id=%x seq=%u timed out, no physics pump",
            vehicleId, mySeq);
        return 0;
    }

    /* The vehicle appears a frame or two after the commit; the
     * first spawn of a model can stream its assets in for a
     * couple of seconds, so poll longer than a vehicle every
     * second and accept a wider radius. */
    for (waited = 0; waited < 120 && !ent; waited++) {
        ent = EntityNear(pos, 20.0f);
        if (!ent) Sleep(20);
    }
    if (!ent) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        Log("spawn: id=%x seq=%u committed, entity not seen in %d ms",
            vehicleId, mySeq, waited * 20);
    } else {
        Log("spawn: id=%x seq=%u entity=%llx after %d ms",
            vehicleId, mySeq, (unsigned long long)ent, waited * 20);
    }
    return ent;
}

/* Warm the spec cache on a background thread so the first spawn
 * does not have to scan the address space synchronously. The
 * framework starts this when the world becomes playable; plugins
 * may also call ShSpawnWarm() at any time. Every run only scans
 * the specs still unresolved. */
static volatile LONG g_warmRunning = 0;

static DWORD WINAPI WarmThread(LPVOID p) {
    DWORD t0;
    (void)p;
    EnsureLocks();
    t0 = GetTickCount();
    Log("warm: scanning for vehicle specs...");
    FindAllSpecs();
    Log("warm: done in %lu ms", (unsigned long)(GetTickCount() - t0));
    InterlockedExchange(&g_warmRunning, 0);
    return 0;
}

void ShSpawnOnEnterPlaying(void) {
    HANDLE h;

    EnsureLocks();
    if (InterlockedCompareExchange(&g_warmRunning, 1, 0)) return;
    h = CreateThread(NULL, 0, WarmThread, NULL, 0, NULL);
    if (!h) InterlockedExchange(&g_warmRunning, 0);
    else CloseHandle(h);
}

SH_API void ShSpawnWarm(void) {
    ShSpawnOnEnterPlaying();
}
