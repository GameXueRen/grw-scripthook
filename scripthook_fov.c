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
#include "log.h"

/* mov [rax+0x180], ecx   rax is the camera manager. */
#define FOV_SITE  SH_IMG(0x81E0C22)
#define FOV_LEN   6

/* Engine values under 0.5 rad are zoom optics at work:
 * scopes and binoculars compute far below the 0.78 to
 * 0.83 gameplay range, and they keep their own fov. */
#define FOV_PASS_BITS 0x3F000000u

extern void ShSetError(int err);
extern void *ShAllocNear(uint64_t target);
extern int ShReadableAddr(uint64_t addr, size_t len);

/* Read by the stub: enabled, the value as bits, the engine's own last
 * value, then the pin. The engine value is kept so a plugin can tell a
 * narrowed aim apart from a zoom optic, and the pin is the no-zoom
 * option: with it set the engine's value is replaced whatever it is, so
 * an aim that would narrow the view keeps the one the override carries.
 */
#define ST_ENABLE 0
#define ST_VALUE  1
#define ST_ENGINE 2
#define ST_PIN    3
static volatile uint32_t g_state[4] = { 0, 0, 0, 0 };

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
    /* The engine's own value, kept for the menu: it is the only
     * thing that says whether this frame is a zoom optic. */
    s[o++] = 0x41; s[o++] = 0x89; s[o++] = 0x4A;   /* mov [r10+8],ecx */
    s[o++] = 0x08;
    s[o++] = 0x41; s[o++] = 0x83; s[o++] = 0x3A;   /* cmp [r10],0 */
    s[o++] = 0x00;
    s[o++] = 0x74; s[o++] = 0x13;                  /* je +19     */

    /* The pin replaces whatever the engine computed - that is what
     * keeps an aim from narrowing the view. */
    s[o++] = 0x41; s[o++] = 0x83; s[o++] = 0x7A;   /* cmp [r10+C],0 */
    s[o++] = 0x0C;
    s[o++] = 0x00;
    s[o++] = 0x75; s[o++] = 0x08;                  /* jne +8     */

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

/* Called by the camera hook's own install, and not only on the first
 * override. The engine's value has to be readable (ShFovEngine) before any
 * value is pushed, and a caller that has to push a fov first to learn what
 * the engine computed cannot tell a sight from the hip - which is the whole
 * basis of no zoom on iron sights. With nothing overriding, the stub simply
 * passes the engine's own value through, so installing it costs nothing.
 */
int ShFovInstall(void) {
    if (g_hooked) return 1;
    if (!ShReadableAddr(FOV_SITE, FOV_LEN)) {
        LogFirst("scripthook_fov.log", "fov site %llX is not readable",
                 (unsigned long long)FOV_SITE);
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    memcpy(g_orig, (const void *)(uintptr_t)FOV_SITE, FOV_LEN);

    /* Refuse anything but the store we expect, so a build
     * we do not know is left alone. This is the one failure the module
     * cannot report any other way: the caller sees SH_ERR_NO_CANDIDATE and
     * nothing says which site it was or what is there now.
     */
    if (g_orig[0] != 0x89 || g_orig[1] != 0x88) {
        LogFirst("scripthook_fov.log",
                 "fov site %llX holds %02X %02X %02X %02X %02X %02X, wanted "
                 "89 88 80 01 00 00 - stale constant?",
                 (unsigned long long)FOV_SITE, g_orig[0], g_orig[1],
                 g_orig[2], g_orig[3], g_orig[4], g_orig[5]);
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    LogFirst("scripthook_fov.log",
             "fov site %llX matched (89 88 80 01 00 00)",
             (unsigned long long)FOV_SITE);
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
    if (!ShFovInstall()) return 0;
    memcpy(&bits, &radians, 4);
    g_state[ST_VALUE] = bits;
    g_state[ST_ENABLE] = 1;
    return 1;
}

void ShFovClear(void) {
    g_state[ST_ENABLE] = 0;
    g_state[ST_PIN] = 0;
}

/* Pin the override: while it is set the engine's own value is replaced
 * whatever it is, so an aim that would narrow the view keeps the fov the
 * override carries. Cleared with the override, because a pin with nothing
 * to pin is a value nobody owns.
 */
SH_API void ShFovPin(int on) {
    g_state[ST_PIN] = on ? 1u : 0u;
}

/* The engine's own value of the last frame, radians, before any
 * replacement - 0 until the engine has run the site once. A plugin uses
 * it to tell a narrowed aim (a mild zoom, still in the gameplay range)
 * apart from a magnified optic, which computes far below it.
 */
SH_API float ShFovEngine(void) {
    uint32_t bits = g_state[ST_ENGINE];
    float f;

    memcpy(&f, &bits, 4);
    return f;
}

int ShFovActive(void) {
    return (int)g_state[ST_ENABLE];
}
