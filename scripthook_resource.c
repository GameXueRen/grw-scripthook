/* The four crafting resources, by name. */
/* They live as protected ints in a trie under the resource
 * manager. Each is resolved by its stable spec id, then read
 * or written through the general stat codec. */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

/* *(global) is the manager handle. +0x18 is the base, +0x10
 * off that is the type-0 container holding the trie root.
 */
#define RES_GLOBAL   SH_IMG(0x4B98DF0)
#define OFF_MGRBASE  0x18
#define OFF_CONT0    0x10

/* The leaf packs a count byte, a key-pointer array at +8,
 * then a value array whose word offset is table[count].
 */
#define VALOFF_TBL   SH_IMG(0x3AA1D79)
#define OFF_DEF_SPEC 0x08

/* Skill points are a PLAIN int, not a protected one: the
 * global holds a pointer, the value sits at +0x1C.
 */
#define SKILL_GLOBAL SH_IMG(0x4B98F10)
#define OFF_SKILL    0x1C

extern int ShReadableAddr(uint64_t addr, size_t len);
extern uint64_t ShReadQ(uint64_t addr);
extern void ShSetError(int err);
extern int ShStatRead(uint64_t stat, uint32_t *out);
extern int ShStatWrite(uint64_t stat, uint32_t value);

/* Spec ids, the 0x4000xxxx family, stable like vehicles. */
static const uint32_t g_spec[4] = {
    0x40005815u,  /* food     */
    0x40005817u,  /* gasoline */
    0x40005818u,  /* medicine */
    0x4000581Au   /* comms    */
};

static int Sane(uint64_t p) {
    return p >= 0x10000ULL && p < 0x800000000000ULL;
}

/* Returns the protected int address for a resource spec, or
 * 0 if the manager is not ready or the spec is absent.
 */
static uint64_t Resolve(uint32_t spec) {
    uint64_t handle, mgr, root, base, keyptr;
    uint8_t count, valoff;
    uint32_t sid;
    int i;

    if (!ShReadableAddr(RES_GLOBAL, 8)) return 0;
    handle = ShReadQ(RES_GLOBAL);
    if (!Sane(handle) || !ShReadableAddr(handle + OFF_MGRBASE, 8)) return 0;
    mgr = ShReadQ(handle + OFF_MGRBASE);
    if (!Sane(mgr) || !ShReadableAddr(mgr + OFF_CONT0, 8)) return 0;
    root = ShReadQ(mgr + OFF_CONT0);

    /* A single leaf holds all eight resources. */
    if ((root & 7) != 1) return 0;
    base = root & ~7ULL;
    if (!ShReadableAddr(base, 1)) return 0;
    memcpy(&count, (void *)(uintptr_t)base, 1);
    if (count > 32) return 0;
    if (!ShReadableAddr(VALOFF_TBL + count, 1)) return 0;
    memcpy(&valoff, (void *)(uintptr_t)(VALOFF_TBL + count), 1);

    for (i = 0; i <= (int)count; i++) {
        keyptr = ShReadQ(base + 8 + (uint64_t)i * 8);
        if (!Sane(keyptr) || !ShReadableAddr(keyptr + OFF_DEF_SPEC, 4))
            continue;
        memcpy(&sid, (void *)(uintptr_t)(keyptr + OFF_DEF_SPEC), 4);
        if (sid == spec)
            return ShReadQ(base + ((uint64_t)valoff + i) * 8);
    }
    return 0;
}

SH_API int ShGetResource(int which, uint32_t *out) {
    uint64_t prot;

    if (which < 0 || which > 3 || !out) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    prot = Resolve(g_spec[which]);
    if (!prot) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    return ShStatRead(prot, out);
}

SH_API int ShSetResource(int which, uint32_t value) {
    uint64_t prot;

    if (which < 0 || which > 3) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    prot = Resolve(g_spec[which]);
    if (!prot) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    return ShStatWrite(prot, value);
}

/** Sets all four the same. Returns how many took. */
SH_API int ShSetAllResources(uint32_t value) {
    int i, n = 0;

    for (i = 0; i < 4; i++)
        if (ShSetResource(i, value)) n++;
    ShSetError(n ? SH_OK : SH_ERR_NO_CANDIDATE);
    return n;
}

static uint64_t SkillAddr(void) {
    uint64_t obj;

    if (!ShReadableAddr(SKILL_GLOBAL, 8)) return 0;
    obj = ShReadQ(SKILL_GLOBAL);
    if (!Sane(obj) || !ShReadableAddr(obj + OFF_SKILL, 4)) return 0;
    return obj + OFF_SKILL;
}

SH_API int ShGetSkillPoints(uint32_t *out) {
    uint64_t a = SkillAddr();

    if (!out) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (!a) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    memcpy(out, (void *)(uintptr_t)a, 4);
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShSetSkillPoints(uint32_t value) {
    uint64_t a = SkillAddr();

    if (!a) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    memcpy((void *)(uintptr_t)a, &value, 4);
    ShSetError(SH_OK);
    return 1;
}
