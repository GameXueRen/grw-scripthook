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
static uint64_t g_specCache[VEHICLE_COUNT];

/* Transform buffers. A single one handed to the engine and
 * then rewritten froze the game, so rotate.
 */
#define MTX_RING 64
static uint8_t g_mtx[MTX_RING][64] __attribute__((aligned(16)));
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

/* One bounded walk of committed memory for the handle that
 * names this vehicle. The spec starts 0x28 below it.
 */
static uint64_t FindSpec(uint32_t vehicleId) {
    MEMORY_BASIC_INFORMATION mbi;
    uint64_t want = ((uint64_t)vehicleId << 32) | VEH_KIND_HASH;
    uint8_t *scan = (uint8_t *)0x10000000;

    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;
        /* Wine keeps the heap low, Windows does not. Stopping
         * at 60GB found nothing there and every spawn failed.
         */
        if ((uint64_t)(uintptr_t)mbi.BaseAddress >= 0x800000000000ULL)
            break;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & PAGE_READWRITE) &&
            !(mbi.Protect & PAGE_GUARD))
        {
            uint8_t *b = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize, o, k, got;

            /* Chunked kernel reads: a page freed mid scan
             * skips instead of faulting. Chunks overlap so
             * no candidate spans a seam. */
            static uint8_t buf[0x10000];

            for (o = 0; o + 8 <= sz; o += sizeof(buf) - 8) {
                got = sz - o;
                if (got > sizeof(buf)) got = sizeof(buf);
                if (!ShReadMem((uint64_t)(uintptr_t)(b + o), buf, got))
                    continue;
                for (k = 0; k + 8 <= got; k += 8) {
                    uint64_t v;
                    memcpy(&v, buf + k, 8);
                    if (v != want) continue;
                    {
                        uint64_t base =
                            (uint64_t)(uintptr_t)(b + o + k)
                            - SPEC_HANDLE_OFF;
                        if (ShReadQ(base) == SPEC_VTABLE) return base;
                    }
                }
            }
        }
        scan = next;
    }
    return 0;
}

static uint64_t SpecFor(uint32_t vehicleId) {
    int i;
    for (i = 0; i < VEHICLE_COUNT; i++) {
        if (g_vehicles[i].id != vehicleId) continue;
        if (g_specCache[i] &&
            ShReadQ(g_specCache[i]) == SPEC_VTABLE)
            return g_specCache[i];
        g_specCache[i] = FindSpec(vehicleId);
        return g_specCache[i];
    }
    return 0;
}

void ShSpawnInvalidate(void) {
    memset(g_specCache, 0, sizeof(g_specCache));
}

/* Build a transform from the player's own, so the basis is
 * whatever the engine considers upright here.
 */
static const void *BuildMatrix(const ShVec3 *pos) {
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

/* Runs on the game thread, queued by ShSpawnVehicle. */
static volatile uint64_t g_pendSpec = 0;
static const void *g_pendMtx = NULL;
static volatile int g_pendDone = 0;

void ShSpawnPump(void) {
    uint64_t spec = g_pendSpec, mgr, spawner;
    const void *mtx = g_pendMtx;

    if (!spec || !mtx) return;
    g_pendSpec = 0;

    mgr = ((MgrGet_t)ImgAddr(RVA_MGR_GETTER))();
    if (mgr) {
        spawner = ((Spawn_t)ImgAddr(RVA_SPAWN))(spec, SPAWN_MODE,
                                                mtx);
        /* The commit is required, or nothing ever appears. */
        if (spawner)
            ((Commit_t)ImgAddr(RVA_COMMIT))(mgr, COMMIT_MODE,
                                            spawner);
    }
    g_pendDone = 1;
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
    int waited;

    if (!pos) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (!ShRequireInGame()) return 0;

    spec = SpecFor(vehicleId);
    if (!spec) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }

    mtx = BuildMatrix(pos);
    if (!mtx) { ShSetError(SH_ERR_NO_ROOT); return 0; }

    g_pendDone = 0;
    g_pendMtx = mtx;
    g_pendSpec = spec;

    /* The pump runs in the physics hook, so wait for it. */
    for (waited = 0; waited < 300 && !g_pendDone; waited++)
        Sleep(10);
    if (!g_pendDone) {
        g_pendSpec = 0;
        ShSetError(SH_ERR_NO_PHYSICS);
        return 0;
    }

    for (waited = 0; waited < 60 && !ent; waited++) {
        ent = EntityNear(pos, 8.0f);
        if (!ent) Sleep(50);
    }
    if (!ent) ShSetError(SH_ERR_NO_CANDIDATE);
    return ent;
}
