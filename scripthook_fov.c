/* Field of view, taken at source.
 * The engine computes fov from a virtual call on the active
 * camera behaviour, so there is no field to write. */
/* This is where the result enters the camera manager, ahead
 * of the camera build and so ahead of culling. Overriding
 * the camera later pops geometry at the frustum edge. */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

/* mov [rax+0x180], ecx   rax is the camera manager. */
#define FOV_SITE  SH_IMG(0x7E889A2)
#define FOV_LEN   6

/* Engine values under 0.5 rad are zoom optics at work:
 * scopes and binoculars compute far below the 0.78 to
 * 0.83 gameplay range, and they keep their own fov. */
#define FOV_PASS_BITS 0x3F000000u

extern void ShSetError(int err);
extern void *ShAllocNear(uint64_t target);
extern int ShReadableAddr(uint64_t addr, size_t len);

/* Read by the stub: enabled, then the value as bits. */
static volatile uint32_t g_state[2] = { 0, 0 };

static uint8_t *g_stub = NULL;
static uint8_t  g_orig[FOV_LEN];
static int      g_hooked = 0;

static int BuildStub(void) {
    uint8_t *s = (uint8_t *)ShAllocNear(FOV_SITE);
    int64_t back;
    int o = 0;

    if (!s) return 0;
    memset(s, 0xCC, 0x1000);

    s[o++] = 0x41; s[o++] = 0x52;                  /* push r10   */
    s[o++] = 0x49; s[o++] = 0xBA;                  /* mov r10,im */
    *(uint64_t *)(s + o) = (uint64_t)(uintptr_t)g_state;
    o += 8;
    s[o++] = 0x41; s[o++] = 0x83; s[o++] = 0x3A;   /* cmp [r10],0 */
    s[o++] = 0x00;
    s[o++] = 0x74; s[o++] = 0x0C;                  /* je +12     */

    /* Positive floats order like unsigned ints, so one cmp
     * passes a zooming engine value through untouched.
     */
    s[o++] = 0x81; s[o++] = 0xF9;                  /* cmp ecx,im */
    *(uint32_t *)(s + o) = FOV_PASS_BITS;
    o += 4;
    s[o++] = 0x72; s[o++] = 0x04;                  /* jb +4      */
    s[o++] = 0x41; s[o++] = 0x8B; s[o++] = 0x4A;   /* mov ecx,   */
    s[o++] = 0x04;                                 /*   [r10+4]  */
    s[o++] = 0x41; s[o++] = 0x5A;                  /* pop r10    */

    memcpy(s + o, g_orig, FOV_LEN);                /* the store  */
    o += FOV_LEN;

    back = (int64_t)(FOV_SITE + FOV_LEN)
         - ((int64_t)(uintptr_t)(s + o) + 5);
    if (back > 0x7FFFFFFFLL || back < -0x7FFFFFFFLL) return 0;
    s[o++] = 0xE9;
    *(int32_t *)(s + o) = (int32_t)back;
    o += 4;

    g_stub = s;
    return 1;
}

/* Five byte jmp with the sixth left as a nop, so the next
 * instruction boundary is unchanged.
 */
static int Patch(void) {
    uint8_t *at = (uint8_t *)(uintptr_t)FOV_SITE;
    int64_t rel = (int64_t)(uintptr_t)g_stub - ((int64_t)FOV_SITE + 5);
    uint8_t patch[FOV_LEN];
    DWORD old;

    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) return 0;
    patch[0] = 0xE9;
    memcpy(patch + 1, &rel, 4);
    patch[5] = 0x90;

    if (!VirtualProtect(at, FOV_LEN, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(at, patch, FOV_LEN);
    VirtualProtect(at, FOV_LEN, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, FOV_LEN);
    return 1;
}

static int Install(void) {
    if (g_hooked) return 1;
    if (!ShReadableAddr(FOV_SITE, FOV_LEN)) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    memcpy(g_orig, (const void *)(uintptr_t)FOV_SITE, FOV_LEN);

    /* Refuse anything but the store we expect, so a build
     * we do not know is left alone.
     */
    if (g_orig[0] != 0x89 || g_orig[1] != 0x88) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    if (!BuildStub() || !Patch()) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    g_hooked = 1;
    return 1;
}

/* Vertical, radians. Applied by the engine's own
 * propagation, so culling and projection agree. */
int ShFovSet(float radians) {
    uint32_t bits;

    if (!(radians > 0.05f && radians < 3.0f)) return 0;
    if (!Install()) return 0;
    memcpy(&bits, &radians, 4);
    g_state[1] = bits;
    g_state[0] = 1;
    return 1;
}

void ShFovClear(void) {
    g_state[0] = 0;
}

int ShFovActive(void) {
    return (int)g_state[0];
}
