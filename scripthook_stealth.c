/* Visibility, from the trainer's stealth toggle. */
/* The engine computes an Awareness value driving detection
 * range. This scales it: 0 hides, 1 is normal. */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

/* mulss xmm8, xmm9, the awareness scale in the detector. */
#define VIS_SITE  SH_IMG(0x14393508)
#define VIS_LEN   5

extern void ShSetError(int err);
extern void *ShAllocNear(uint64_t target);
extern int ShReadableAddr(uint64_t addr, size_t len);

static uint8_t *g_stub = NULL;
static float   *g_factor = NULL;
static int      g_hooked = 0;

/* Cave: stolen mulss, then mulss xmm8, [factor], then back.
 * The factor float lives at the tail of the cave.
 */
static int BuildStub(void) {
    uint8_t *s = (uint8_t *)ShAllocNear(VIS_SITE);
    int64_t back;
    int o = 0, dispAt, factorAt;

    if (!s) return 0;
    memset(s, 0xCC, 0x1000);

    memcpy(s + o, (const void *)(uintptr_t)VIS_SITE, VIS_LEN);
    o += VIS_LEN;

    s[o++] = 0xF3; s[o++] = 0x44; s[o++] = 0x0F;   /* mulss    */
    s[o++] = 0x59; s[o++] = 0x05;                  /* xmm8,[m] */
    dispAt = o; o += 4;

    s[o++] = 0xE9;                                 /* jmp back */
    back = (int64_t)(VIS_SITE + VIS_LEN)
         - ((int64_t)(uintptr_t)(s + o) + 4);
    if (back > 0x7FFFFFFFLL || back < -0x7FFFFFFFLL) return 0;
    *(int32_t *)(s + o) = (int32_t)back; o += 4;

    factorAt = o;
    *(float *)(s + o) = 1.0f; o += 4;

    *(int32_t *)(s + dispAt) = factorAt - (dispAt + 4);
    g_factor = (float *)(s + factorAt);
    g_stub = s;
    return 1;
}

static int Patch(void) {
    uint8_t *at = (uint8_t *)(uintptr_t)VIS_SITE;
    int64_t rel = (int64_t)(uintptr_t)g_stub - ((int64_t)VIS_SITE + 5);
    uint8_t patch[VIS_LEN];
    DWORD old;

    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) return 0;
    patch[0] = 0xE9;
    memcpy(patch + 1, &rel, 4);

    if (!VirtualProtect(at, VIS_LEN, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(at, patch, VIS_LEN);
    VirtualProtect(at, VIS_LEN, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, VIS_LEN);
    return 1;
}

static int Install(void) {
    uint8_t cur[VIS_LEN];

    if (g_hooked) return 1;
    if (!ShReadableAddr(VIS_SITE, VIS_LEN)) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    memcpy(cur, (const void *)(uintptr_t)VIS_SITE, VIS_LEN);

    /* Refuse a build whose instruction we do not know. */
    if (cur[0] != 0xF3 || cur[1] != 0x45 || cur[2] != 0x0F ||
        cur[3] != 0x59 || cur[4] != 0xC1) {
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

/* 1.0 normal, 0.0 invisible, 0.5 halves detection range.
 * Above 1 makes the player easier to spot. */
SH_API int ShSetVisibility(float factor) {
    if (factor < 0.0f) factor = 0.0f;
    if (!Install()) return 0;
    *g_factor = factor;
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShGetVisibility(float *out) {
    if (!out) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    *out = g_factor ? *g_factor : 1.0f;
    ShSetError(SH_OK);
    return 1;
}
