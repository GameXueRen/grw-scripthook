/* Reflected objects: the engine's own method tables. */
/* An object is [vtable, methodTable, ...]. The table is
 * 32 byte entries of crc32 name, index and function, so
 * any method is reachable by name. See FINDINGS.md. */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

#define OFF_METHODS     0x08
#define ENTRY_SIZE      0x20
#define ENTRY_FN        0x10
#define MAX_METHODS     512

/* vt+0x30 returns the class descriptor, hash at +0x24. */
#define VT_GETDESC      0x30
#define OFF_DESC_HASH   0x24

/* GR_GameFlow: sub objects at +0x1d8, HybridMenu +0x30. */
#define OFF_FLOW_OBJS   0x1D8
#define FLOW_OBJ_COUNT  17
#define OFF_FLOW_HYBRID 0x30
#define FLOW_METHODS    SH_IMG(0x483B650)
#define HYBRID_VTABLE   SH_IMG(0x3B5ADD8)

#define CALL_WAIT_MS    2000

extern uint64_t ShGetStateMachine(void);
extern void ShSetError(int err);
extern int ShQueueCall(uint64_t fn, uint64_t a0, uint64_t a1,
                       uint64_t a2, uint64_t a3);
extern int ShQueueResult(uint64_t *outRet);

typedef uint64_t (__attribute__((ms_abi)) *GetDesc_t)(uint64_t);

static int Sane(uint64_t p) {
    return p >= 0x10000ULL && p < 0x800000000000ULL;
}

/* Objects live above the heap window the shared reader
 * allows, so query the page directly.
 */
static int Readable(uint64_t addr, size_t len) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!Sane(addr)) return 0;
    if (!VirtualQuery((void *)(uintptr_t)addr, &mbi, sizeof(mbi)))
        return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize
           >= addr + len;
}

static uint64_t ReadQ(uint64_t addr) {
    uint64_t v = 0;
    if (!Readable(addr, 8)) return 0;
    memcpy(&v, (void *)(uintptr_t)addr, 8);
    return v;
}

static uint32_t ReadD(uint64_t addr) {
    uint32_t v = 0;
    if (!Readable(addr, 4)) return 0;
    memcpy(&v, (void *)(uintptr_t)addr, 4);
    return v;
}

/* Every reflected class starts its table with this entry.
 * An image vtable alone proves nothing: calling vt+0x30 on
 * an object without this signature crashed the game once. */
#define SIGNATURE_HASH  0x090A5560u

/* Entries carry their own index, which bounds the walk:
 * the first one that disagrees belongs to the next class.
 */
static uint64_t MethodTable(uint64_t obj) {
    uint64_t mt;

    if (!Readable(obj, 16)) return 0;
    if (!ShInImage(ReadQ(obj))) return 0;
    mt = ReadQ(obj + OFF_METHODS);
    if (!ShInImage(mt) || !Readable(mt, ENTRY_SIZE)) return 0;
    if (ReadD(mt) != SIGNATURE_HASH || ReadD(mt + 4) != 0) return 0;
    return mt;
}

SH_API int ShReflectMethod(uint64_t obj, uint32_t nameHash,
                           uint64_t *outFn) {
    uint64_t mt = MethodTable(obj);
    int i;

    if (!mt) { ShSetError(SH_ERR_NOT_ENTITY); return 0; }
    for (i = 0; i < MAX_METHODS; i++) {
        uint64_t e = mt + (uint64_t)i * ENTRY_SIZE;
        if (!Readable(e, ENTRY_SIZE)) break;
        if (ReadD(e + 4) != (uint32_t)i) break;
        if (ReadD(e) == nameHash) {
            uint64_t fn = ReadQ(e + ENTRY_FN);
            if (!ShInImage(fn)) break;
            if (outFn) *outFn = fn;
            ShSetError(SH_OK);
            return 1;
        }
    }
    ShSetError(SH_ERR_NO_CANDIDATE);
    return 0;
}

SH_API int ShReflectMethods(uint64_t obj, ShMethod *out, int max) {
    uint64_t mt = MethodTable(obj);
    int i, n = 0;

    if (!mt || !out || max < 1) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    for (i = 0; i < MAX_METHODS && n < max; i++) {
        uint64_t e = mt + (uint64_t)i * ENTRY_SIZE;
        if (!Readable(e, ENTRY_SIZE)) break;
        if (ReadD(e + 4) != (uint32_t)i) break;
        out[n].nameHash = ReadD(e);
        out[n].index = i;
        out[n].fn = ReadQ(e + ENTRY_FN);
        n++;
    }
    ShSetError(SH_OK);
    return n;
}

/* vt+0x30 is the descriptor getter only on full objects,
 * the ones whose table also carries common entries 1 and 2.
 * A one method class has another vtable shape entirely. */
#define COMMON1_HASH    0x8AD4C430u
#define COMMON2_HASH    0x5D8A1965u

SH_API uint32_t ShReflectClassHash(uint64_t obj) {
    uint64_t vt, fn, desc, mt = MethodTable(obj);

    if (!mt || !Readable(mt, 3 * ENTRY_SIZE)) return 0;
    if (ReadD(mt + ENTRY_SIZE) != COMMON1_HASH) return 0;
    if (ReadD(mt + 2 * ENTRY_SIZE) != COMMON2_HASH) return 0;
    vt = ReadQ(obj);
    if (!ShInImage(vt) || !Readable(vt + VT_GETDESC, 8)) return 0;
    fn = ReadQ(vt + VT_GETDESC);
    if (!ShInImage(fn)) return 0;
    desc = ((GetDesc_t)fn)(obj);
    if (!desc) return 0;
    return ReadD(desc + OFF_DESC_HASH);
}

/* Queue on the game thread and wait, so two calls in a
 * row cannot collide on the single slot.
 */
SH_API int ShReflectCall(uint64_t obj, uint32_t nameHash,
                         uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t *outRet) {
    uint64_t fn = 0;
    int waited = 0;

    if (!ShReflectMethod(obj, nameHash, &fn)) return 0;
    while (!ShQueueCall(fn, obj, a1, a2, a3)) {
        if (++waited > CALL_WAIT_MS) {
            ShSetError(SH_ERR_HOOK_FAILED);
            return 0;
        }
        Sleep(1);
    }
    while (!ShQueueResult(outRet)) {
        if (++waited > CALL_WAIT_MS) {
            ShSetError(SH_ERR_HOOK_FAILED);
            return 0;
        }
        Sleep(1);
    }
    ShSetError(SH_OK);
    return 1;
}

/* A scene is any reflected object with Enter and Exit.
 * Each call checks the other exists, so it can be undone.
 */
SH_API int ShSceneEnter(uint64_t obj) {
    uint64_t fn;
    if (!ShReflectMethod(obj, SH_HASH_EXIT, &fn)) return 0;
    return ShReflectCall(obj, SH_HASH_ENTER, 0, 0, 0, NULL);
}

SH_API int ShSceneExit(uint64_t obj) {
    uint64_t fn;
    if (!ShReflectMethod(obj, SH_HASH_ENTER, &fn)) return 0;
    return ShReflectCall(obj, SH_HASH_EXIT, 0, 0, 0, NULL);
}

SH_API uint64_t ShGameFlow(void) {
    uint64_t m = ShGetStateMachine();

    if (!m || ReadQ(m + OFF_METHODS) != FLOW_METHODS) {
        ShSetError(SH_ERR_NO_GLOBAL);
        return 0;
    }
    return m;
}

SH_API uint64_t ShGameFlowObject(int slot) {
    uint64_t m = ShGameFlow(), o;

    if (!m) return 0;
    if (slot < 0 || slot >= FLOW_OBJ_COUNT) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    o = ReadQ(m + OFF_FLOW_OBJS + (uint64_t)slot * 8);
    if (!MethodTable(o)) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    return o;
}

SH_API uint64_t ShHybridMenu(void) {
    uint64_t m = ShGameFlow(), h;

    if (!m) return 0;
    h = ReadQ(m + OFF_FLOW_HYBRID);
    if (!Readable(h, 8) || ReadQ(h) != HYBRID_VTABLE) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    return h;
}

SH_API int ShTriggerGameOver(int reason) {
    uint64_t m = ShGameFlow();

    if (!m) return 0;
    return ShReflectCall(m, SH_HASH_GAMEOVER,
                         (uint64_t)(uint32_t)reason, 0, 0, NULL);
}
