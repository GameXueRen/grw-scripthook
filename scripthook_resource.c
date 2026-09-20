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
#define RES_GLOBAL   SH_IMG(0x4B98E80)
#define OFF_MGRBASE  0x18
#define OFF_CONT0    0x10

/* The leaf packs a count byte, a key-pointer array at +8,
 * then a value array whose word offset is table[count].
 */
#define VALOFF_TBL   SH_IMG(0x3AA1D09)
#define OFF_DEF_SPEC 0x08

/* Skill points are a PLAIN int, not a protected one: the
 * global holds a pointer, the value sits at +0x1C.
 */
#define SKILL_GLOBAL SH_IMG(0x4B98FA0)
#define OFF_SKILL    0x1C

extern int ShReadableAddr(uint64_t addr, size_t len);
extern uint64_t ShReadQ(uint64_t addr);
extern int ShReadMem(uint64_t addr, void *out, size_t len);
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

/* The leaf both callers read: its base, how many entries it holds, and the
 * value-array index its own count maps to. One walk, two users - the spec
 * lookup below and the slot read a probe asks for.
 */
static int Leaf(uint64_t *base, uint8_t *count, uint8_t *valoff) {
    uint64_t handle, mgr, root;

    if (!ShReadableAddr(RES_GLOBAL, 8)) return 0;
    handle = ShReadQ(RES_GLOBAL);
    if (!Sane(handle) || !ShReadableAddr(handle + OFF_MGRBASE, 8)) return 0;
    mgr = ShReadQ(handle + OFF_MGRBASE);
    if (!Sane(mgr) || !ShReadableAddr(mgr + OFF_CONT0, 8)) return 0;
    root = ShReadQ(mgr + OFF_CONT0);

    /* A single leaf holds all eight resources. */
    if ((root & 7) != 1) return 0;
    *base = root & ~7ULL;
    if (!ShReadableAddr(*base, 1)) return 0;
    memcpy(count, (void *)(uintptr_t)*base, 1);
    if (*count > 32) return 0;
    if (!ShReadableAddr(VALOFF_TBL + *count, 1)) return 0;
    memcpy(valoff, (void *)(uintptr_t)(VALOFF_TBL + *count), 1);
    return 1;
}

/* The spec id and the value behind one entry of the leaf. 1 when the row
 * exists - i runs to count INCLUSIVE, which is how the node is laid out
 * (see the loop in Resolve below). */
static int Slot(int i, uint32_t *spec, uint64_t *prot) {
    uint64_t base, keyptr, p;
    uint8_t count, valoff;
    uint32_t sid;

    if (!Leaf(&base, &count, &valoff)) return 0;
    if (i < 0 || i > (int)count) return 0;
    keyptr = ShReadQ(base + 8 + (uint64_t)i * 8);
    if (!Sane(keyptr) || !ShReadableAddr(keyptr + OFF_DEF_SPEC, 4)) return 0;
    memcpy(&sid, (void *)(uintptr_t)(keyptr + OFF_DEF_SPEC), 4);
    p = ShReadQ(base + ((uint64_t)valoff + (uint64_t)i) * 8);
    if (spec) *spec = sid;
    if (prot) *prot = p;
    return 1;
}

/* Returns the protected int address for a resource spec, or
 * 0 if the manager is not ready or the spec is absent.
 */
static uint64_t Resolve(uint32_t spec) {
    uint64_t base, p;
    uint8_t count, valoff;
    uint32_t sid;
    int i;

    if (!Leaf(&base, &count, &valoff)) return 0;

    for (i = 0; i <= (int)count; i++) {
        if (!Slot(i, &sid, &p)) continue;
        if (sid == spec)
            return p;
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

/* A probe's view of the same leaf the four named resources live in: that node
 * holds EIGHT entries (see the comment above), and which four the others are
 * is exactly the kind of question a probe is asked - it is where a count that
 * moves by one per shot would be cheap to reach, no heap scan and no hook.
 *
 * i indexes the leaf in its own order, 0 up to the count the node reports.
 * value is the decoded int behind the entry and prot the address of the
 * protected int itself, either may be NULL. 1 when the row exists, 0 with
 * SH_ERR_NO_CANDIDATE once past the end or while the manager is not up. */
SH_API int ShGetResourceSlot(int i, uint32_t *spec, uint32_t *value,
                             uint64_t *prot) {
    uint64_t p = 0;
    uint32_t sid = 0;

    if (i < 0) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (!Slot(i, &sid, &p)) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    if (spec) *spec = sid;
    if (prot) *prot = p;
    if (value && !ShStatRead(p, value)) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
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
    /* Kernel-mediated read, so a page the engine frees under us costs a
     * failed call instead of a fault. */
    if (!ShReadMem(a, out, 4)) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShSetSkillPoints(uint32_t value) {
    uint64_t a = SkillAddr();

    if (!a) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    if (!WriteProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)a,
                            &value, 4, NULL)) {
        ShSetError(SH_ERR_UNWRITABLE);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}
