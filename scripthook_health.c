/* Player health: protected value, flags, engine calls.
 * See HEALTH.md. Build pinned, base 0x140000000.
 */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

#define OFF_OWNER      0x028
#define OFF_MAXHP      0x0F0
#define OFF_HP         0x0F8
#define OFF_GODMODE    0x201
#define OFF_NODAMAGE   0x202
#define OFF_NODEATH    0x203
#define COMP_SIZE      0x3A0

extern int ShReadableAddr(uint64_t addr, size_t len);
extern uint64_t ShReadQ(uint64_t addr);
extern int ShReadMem(uint64_t addr, void *out, size_t len);
extern void ShSetError(int err);
extern int ShRequireInGame(void);

static uint64_t g_comp = 0;
static uint64_t g_compOwner = 0;

/* The protected int codec lives in scripthook_stat.c now,
 * exposed as a general stat API. Health is one caller.
 */
extern int ShStatRead(uint64_t stat, uint32_t *out);
extern int ShStatWrite(uint64_t stat, uint32_t value);

#define ProtGet ShStatRead
#define ProtSet ShStatWrite

/* A component is valid when its owner matches and the
 * protected value decodes to a sane reading.
 */
static int CompValid(uint64_t comp, uint64_t owner) {
    uint32_t mx = 0, cur = 0;
    if (!comp || !ShReadableAddr(comp, COMP_SIZE)) return 0;
    if (ShReadQ(comp + OFF_OWNER) != owner) return 0;
    if (!ShReadableAddr(comp + OFF_MAXHP, 4)) return 0;
    memcpy(&mx, (void *)(uintptr_t)(comp + OFF_MAXHP), 4);
    if (mx == 0 || mx > 100000) return 0;
    if (!ProtGet(comp + OFF_HP, &cur)) return 0;
    return cur <= mx;
}

/* The engine's own lookup. It binary searches the
 * entity's component array, so nothing is scanned.
 */
#define SH_GET_COMPONENT   SH_IMG(0xC5D2BE0)
#define SH_HEALTH_DESC     SH_IMG(0x499FCB0)

typedef uint64_t (__attribute__((ms_abi)) *GetComponent_t)(uint64_t,
                                                           uint64_t);

/* Health is a SUB component, absent from the entity's
 * array, reached through the holder below at +0x88.
 */
#define SUB_WINDOW        0x200
#define OFF_ENT_COMPS     0x78
#define OFF_ENT_NCOMPS    0x82

static int EngineComponent(uint64_t owner, uint64_t *out) {
    uint64_t arr;
    uint16_t n = 0;
    uint16_t i;

    if (!owner || !ShReadableAddr(owner, 0x90)) return 0;
    arr = ShReadQ(owner + OFF_ENT_COMPS);
    if (!arr || !ShReadableAddr(owner + OFF_ENT_NCOMPS, 2)) return 0;
    memcpy(&n, (void *)(uintptr_t)(owner + OFF_ENT_NCOMPS), 2);
    if (!n || n > 512) return 0;

    /* Holder classes differ per entity type, so match the
     * structure instead: a sub component owning us.
     */
    for (i = 0; i < n; i++) {
        uint64_t c = ShReadQ(arr + (uint64_t)i * 8);
        uint64_t o;
        if (!c || !ShReadableAddr(c, SUB_WINDOW)) continue;
        for (o = 0; o + 8 <= SUB_WINDOW; o += 8) {
            uint64_t comp = ShReadQ(c + o);
            if (!comp) continue;
            if (CompValid(comp, owner)) {
                *out = comp;
                return 1;
            }
        }
    }
    return 0;
}

/* One heap pass looking for component+0x28 == owner. */
static int FindComponent(uint64_t owner, uint64_t *out) {
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = (uint8_t *)0x1000000;

    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;
        /* Same ceiling bug as the spawn scan: Windows puts the
         * heap above 60GB, so a short scan finds nothing.
         */
        if ((uint64_t)(uintptr_t)mbi.BaseAddress
            >= 0x800000000000ULL) break;
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
                    if (v != owner) continue;
                    {
                        uint64_t comp =
                            (uint64_t)(uintptr_t)(b + o + k) - OFF_OWNER;
                        if (CompValid(comp, owner)) {
                            *out = comp;
                            return 1;
                        }
                    }
                }
            }
        }
        scan = next;
    }
    return 0;
}

/* Internal. Plugins never see a component pointer. */
static int ResolveComponent(uint64_t root, uint64_t *out) {
    if (!root) { ShSetError(SH_ERR_NO_ROOT); return 0; }
    /* Ask the engine first. It is cheap and current, so
     * the cache cannot go stale behind a respawn.
     */
    if (EngineComponent(root, &g_comp)) {
        g_compOwner = root;
        *out = g_comp;
        ShSetError(SH_OK);
        return 1;
    }
    if (g_comp && g_compOwner == root && CompValid(g_comp, root)) {
        *out = g_comp;
        return 1;
    }
    if (!FindComponent(root, &g_comp)) {
        g_comp = 0;
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    g_compOwner = root;
    *out = g_comp;
    ShSetError(SH_OK);
    return 1;
}

static int PlayerComponent(uint64_t *out) {
    ShPlayer p;
    uint64_t root = 0;
    if (!ShGetPlayer(&p)) return 0;
    if (!ShWalkToRoot(p.entity, &root)) return 0;
    return ResolveComponent(root, out);
}

/* Entity targeted: walk to the root, then resolve. */
static int EntityComponent(uint64_t entity, uint64_t *out) {
    uint64_t root = 0;
    if (!entity) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (!ShWalkToRoot(entity, &root)) return 0;
    if (EngineComponent(root, out)) {
        ShSetError(SH_OK);
        return 1;
    }
    if (root == g_compOwner && g_comp && CompValid(g_comp, root)) {
        *out = g_comp;
        return 1;
    }
    if (!FindComponent(root, out)) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}

static int ReadHealth(uint64_t comp, uint32_t *cur, uint32_t *max) {
    uint32_t c = 0, m = 0;
    memcpy(&m, (void *)(uintptr_t)(comp + OFF_MAXHP), 4);
    if (!ProtGet(comp + OFF_HP, &c)) {
        ShSetError(SH_ERR_UNWRITABLE);
        return 0;
    }
    if (cur) *cur = c;
    if (max) *max = m;
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShGetHealthEntity(uint64_t entity, uint32_t *cur,
                             uint32_t *max) {
    uint64_t comp = 0;
    if (!ShRequireInGame()) return 0;
    if (!EntityComponent(entity, &comp)) return 0;
    return ReadHealth(comp, cur, max);
}

SH_API int ShSetHealthEntity(uint64_t entity, uint32_t value) {
    uint64_t comp = 0;
    if (!ShRequireInGame()) return 0;
    if (!EntityComponent(entity, &comp)) return 0;
    if (!ProtSet(comp + OFF_HP, value)) {
        ShSetError(SH_ERR_UNWRITABLE);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShSetGodModeEntity(uint64_t entity, int on) {
    uint64_t comp = 0;
    uint8_t v = on ? 1 : 0;
    if (!ShRequireInGame()) return 0;
    if (!EntityComponent(entity, &comp)) return 0;
    if (!ShReadableAddr(comp + OFF_GODMODE, 1)) {
        ShSetError(SH_ERR_UNWRITABLE);
        return 0;
    }
    *(uint8_t *)(uintptr_t)(comp + OFF_GODMODE) = v;
    *(uint8_t *)(uintptr_t)(comp + OFF_NODAMAGE) = v;
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShGetHealthPlayer(uint32_t *cur, uint32_t *max) {
    uint64_t comp = 0;
    uint32_t c = 0, m = 0;

    if (!ShRequireInGame()) return 0;
    if (!PlayerComponent(&comp)) return 0;
    memcpy(&m, (void *)(uintptr_t)(comp + OFF_MAXHP), 4);
    if (!ProtGet(comp + OFF_HP, &c)) {
        ShSetError(SH_ERR_NO_POSITION);
        return 0;
    }
    if (cur) *cur = c;
    if (max) *max = m;
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShSetHealthPlayer(uint32_t value) {
    uint64_t comp = 0;
    if (!ShRequireInGame()) return 0;
    if (!PlayerComponent(&comp)) return 0;
    if (!ProtSet(comp + OFF_HP, value)) {
        ShSetError(SH_ERR_UNWRITABLE);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShSetGodModePlayer(int on) {
    uint64_t comp = 0;
    uint8_t v = on ? 1 : 0;
    if (!ShRequireInGame()) return 0;
    if (!PlayerComponent(&comp)) return 0;
    if (!ShReadableAddr(comp + OFF_GODMODE, 1)) {
        ShSetError(SH_ERR_UNWRITABLE);
        return 0;
    }
    *(uint8_t *)(uintptr_t)(comp + OFF_GODMODE) = v;
    *(uint8_t *)(uintptr_t)(comp + OFF_NODAMAGE) = v;
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShSetCannotDiePlayer(int on) {
    uint64_t comp = 0;
    if (!ShRequireInGame()) return 0;
    if (!PlayerComponent(&comp)) return 0;
    if (!ShReadableAddr(comp + OFF_NODEATH, 1)) {
        ShSetError(SH_ERR_UNWRITABLE);
        return 0;
    }
    *(uint8_t *)(uintptr_t)(comp + OFF_NODEATH) = on ? 1 : 0;
    ShSetError(SH_OK);
    return 1;
}

SH_API void ShInvalidateHealth(void) {
    g_comp = 0;
    g_compOwner = 0;
}
