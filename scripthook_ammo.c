/* Ammo, by weapon slot. */
/* The weapon inventory is found by its vtable and kept only
 * when its owner handle at +0x250 resolves to the player.
 * Ammo per slot is a protected int at slot*0x28 + 0x180. */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

#define INV_VTABLE   SH_IMG(0x3905CF0)
#define OFF_OWNER    0x250
#define SLOT_STRIDE  0x28
#define OFF_AMMO     0x180
#define OFF_AMMO_ALT 0x130
#define MAX_SLOT     8

extern int ShReadableAddr(uint64_t addr, size_t len);
extern uint64_t ShReadQ(uint64_t addr);
extern int ShReadMem(uint64_t addr, void *out, size_t len);
extern void ShSetError(int err);
extern int ShStatRead(uint64_t stat, uint32_t *out);
extern int ShStatWrite(uint64_t stat, uint32_t value);

static uint64_t g_inv = 0;

static int Sane(uint64_t p) {
    return p >= 0x10000ULL && p < 0x800000000000ULL;
}

/* Masked handle: pointer at slot, flags dword at slot+0xC,
 * live when the flags are negative.
 */
static uint64_t ResolveHandle(uint64_t slot) {
    uint64_t val;
    int32_t flags;

    if (!ShReadableAddr(slot, 0x10)) return 0;
    val = ShReadQ(slot);
    memcpy(&flags, (void *)(uintptr_t)(slot + 0xC), 4);
    return (flags < 0 && Sane(val)) ? val : 0;
}

/* +0x250 holds a pointer to the owner handle, so there is
 * one more hop before the masked handle resolves.
 */
static int OwnedByPlayer(uint64_t inv, uint64_t root) {
    uint64_t handle;

    if (!inv || !root || !ShReadableAddr(inv + OFF_OWNER, 8)) return 0;
    handle = ShReadQ(inv + OFF_OWNER);
    if (!Sane(handle)) return 0;
    return ResolveHandle(handle) == root;
}

/* Only a few of these exist, so the sweep is short and the
 * answer is cached until the owner stops matching.
 */
static uint64_t FindInventory(uint64_t root) {
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = (uint8_t *)0x10000;

    if (OwnedByPlayer(g_inv, root)) return g_inv;
    g_inv = 0;

    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;
        if ((uint64_t)(uintptr_t)mbi.BaseAddress >= 0x800000000000ULL)
            break;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & PAGE_READWRITE) &&
            !(mbi.Protect & PAGE_GUARD)) {
            uint8_t *b = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize, o, k, got;

            /* Chunked kernel reads: a page freed mid scan
             * skips instead of faulting. Chunks overlap so
             * no candidate spans a seam. */
            static uint8_t buf[0x10000];

            for (o = 0; o + 0x260 <= sz; o += sizeof(buf) - 0x260) {
                got = sz - o;
                if (got > sizeof(buf)) got = sizeof(buf);
                if (!ShReadMem((uint64_t)(uintptr_t)(b + o), buf, got))
                    continue;
                for (k = 0; k + 0x260 <= got; k += 8) {
                    uint64_t obj = (uint64_t)(uintptr_t)(b + o + k);
                    uint64_t vt;
                    memcpy(&vt, buf + k, 8);
                    if (vt != INV_VTABLE) continue;
                    if (OwnedByPlayer(obj, root)) {
                        g_inv = obj;
                        return obj;
                    }
                }
            }
        }
        scan = next;
    }
    return 0;
}

static uint64_t PlayerInv(void) {
    ShPlayer p;

    memset(&p, 0, sizeof(p));
    if (!ShGetPlayer(&p)) return 0;
    return FindInventory(p.root ? p.root : p.entity);
}

/* Ammo sits at +0x180 for most weapons and +0x130 for some,
 * so read the one that decodes.
 */
static uint64_t AmmoAddr(uint64_t inv, int slot) {
    uint64_t base = inv + (uint64_t)slot * SLOT_STRIDE;
    uint32_t tmp;

    if (ShStatRead(base + OFF_AMMO, &tmp)) return base + OFF_AMMO;
    if (ShStatRead(base + OFF_AMMO_ALT, &tmp)) return base + OFF_AMMO_ALT;
    return 0;
}

SH_API int ShGetAmmo(int slot, uint32_t *out) {
    uint64_t inv, a;

    if (slot < 0 || slot >= MAX_SLOT || !out) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    inv = PlayerInv();
    if (!inv) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    a = AmmoAddr(inv, slot);
    if (!a) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    return ShStatRead(a, out);
}

SH_API int ShSetAmmo(int slot, uint32_t value) {
    uint64_t inv, a;

    if (slot < 0 || slot >= MAX_SLOT) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    inv = PlayerInv();
    if (!inv) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    a = AmmoAddr(inv, slot);
    if (!a) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    return ShStatWrite(a, value);
}
