/* Ammo capacity, scaled - the framework's own replacement for the
 * third-party AmmoCapacity.asi. See docs/ammocapacity-reverse.md for the
 * full derivation; this file is its section 7.1.
 *
 * The fact that made every other approach fail, and the reason this is a
 * hook and not a field write: the magazine capacity is STORED NOWHERE. The
 * game computes it in one function (RVA 0x614CB0 in this build - a thunk in
 * the engine's own jump table) and returns it in the low 16 bits of eax.
 * The old plugin hooks that function and scales what comes back; reading
 * memory finds nothing because there is nothing to read.
 *
 * So: hook it once, scale the return value, hand plugins an API. The hook
 * is NOT installed until a caller asks for a scale other than 1/1, which is
 * the framework's rule for anything that writes engine memory - with the
 * scale left alone there is no hook here at all.
 *
 * The scale is a num/den pair rather than a float: the old plugin's own
 * table is integer (0.50 = 1/2, 0.75 = 3/4, 1.25 = 5/4, 1.50 = 3/2,
 * 2.00 = 2/1; .rdata at RVA 0x33D0 and 0x33E8), and integer arithmetic
 * keeps the result exactly reproducible. The pair lives in one 32-bit word
 * so a reader never sees half an update - the engine calls the hooked
 * function from whichever thread it likes.
 */
#include <windows.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"
#include "log.h"
#include "third_party/minhook/include/MinHook.h"

/* The thunk the old plugin validates against: it scans for a call site's
 * E8 rel32, resolves the target, and requires it to be base + 0x614CB0. */
#define CAP_RVA   0x614CB0ULL
#define CAP_MAX   0xFFFFu
#define SCALE_ONE 0x00010001L

/* num in the high 16 bits, den in the low. 0 = never set. */
static volatile LONG g_scale;
static volatile LONG g_installed;
static volatile LONG g_logged;

typedef int (*CapFn_t)(uint64_t, uint64_t, uint64_t, uint64_t);
/* The original function, through MinHook's trampoline.
 *
 * Its real arguments are not documented and the old plugin never needed
 * them: its stub called the original with the argument registers untouched
 * and only then passed the RETURN value on (mov ecx,eax) to its scaler.
 * Four word arguments cover rcx/rdx/r8/r9, which is everything a function
 * taking a pointer or two uses. A weapon that answers unscaled would be the
 * sign that the engine passes a fifth, stack-born argument; §8 of the
 * reverse notes says where to look if that ever happens.
 */
static CapFn_t g_orig;

extern void ShSetError(int err);
extern int  ShReadableAddr(uint64_t addr, size_t len);
extern int  ShReadBytes(uint64_t addr, void *out, uint32_t len);

/* ---- the scale ------------------------------------------------------- */

static int NumDen(LONG s, uint32_t *num, uint32_t *den) {
    if (!s) return 0;
    *num = (uint32_t)s >> 16;
    *den = (uint32_t)s & 0xFFFFu;
    return (*num && *den);
}

/* The old plugin's algorithm, unchanged: zero in, zero out; otherwise
 * value * num / den, clamped to what a 16-bit capacity can hold. */
static int ScaleValue(int ret) {
    uint32_t num, den, value = (uint32_t)ret & CAP_MAX;
    uint64_t out;

    if (!NumDen(InterlockedCompareExchange(&g_scale, 0, 0), &num, &den))
        return ret;
    if (num == 1 && den == 1) return ret;      /* pass through untouched */
    if (!value) return 0;
    out = (uint64_t)value * num / den;
    return out > CAP_MAX ? (int)CAP_MAX : (int)out;
}

static int CapDetour(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    return ScaleValue(g_orig(a1, a2, a3, a4));
}

/* ---- installation ---------------------------------------------------- */

static void LogOnce(void) {
    if (InterlockedExchange(&g_logged, 1)) return;
    LogInit("scripthook_ammocap.log");
    Log("ammocap module up (built " __DATE__ ")");
}

static int Install(void) {
    void *tgt = (void *)(uintptr_t)SH_IMG(CAP_RVA);
    MH_STATUS s;
    uint8_t bytes[5];

    LogOnce();

    /* The old plugin hooks the very same byte. Two inline hooks on one entry
     * is a race worth losing before it starts, so the framework stays out
     * and says why. */
    if (GetModuleHandleA("AmmoCapacity.asi")) {
        Log("ammocap: AmmoCapacity.asi is loaded and owns this hook - "
            "remove that plugin to use the framework's scale");
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }

    if (!ShReadableAddr((uint64_t)(uintptr_t)tgt, sizeof(bytes)) ||
        !ShReadBytes((uint64_t)(uintptr_t)tgt, bytes, sizeof(bytes))) {
        Log("ammocap: target %p (rva 0x%llx) is not readable - wrong build?",
            tgt, (unsigned long long)CAP_RVA);
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    /* An E9 is what the engine's jump table carries here; anything else
     * means a different build or another hook. Logged, not refused: the
     * old plugin's own check is stricter because it patches by hand,
     * while MinHook relocates whatever it finds. */
    Log("ammocap: target %p bytes %02X %02X %02X %02X %02X", tgt,
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);

    s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        Log("ammocap: MH_Initialize failed (%s)", MH_StatusToString(s));
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    s = MH_CreateHook(tgt, (LPVOID)CapDetour, (LPVOID *)&g_orig);
    if (s != MH_OK) {
        Log("ammocap: MH_CreateHook failed (%s)", MH_StatusToString(s));
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    s = MH_EnableHook(tgt);
    if (s != MH_OK) {
        Log("ammocap: MH_EnableHook failed (%s)", MH_StatusToString(s));
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    Log("ammocap: hooked rva 0x%llx (%p), original at %p",
        (unsigned long long)CAP_RVA, tgt, (void *)g_orig);
    return 1;
}

/* ---- public API ------------------------------------------------------ */

SH_API int ShSetAmmoScale(int num, int den) {
    if (num <= 0 || den <= 0 || num > 0xFFFF || den > 0xFFFF) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    /* Asking for 1/1 before anything else is the "leave it alone" case: no
     * hook is installed for it, the game keeps its own number. */
    if (num == 1 && den == 1 && !InterlockedCompareExchange(&g_installed, 0, 0)) {
        InterlockedExchange(&g_scale, SCALE_ONE);
        ShSetError(SH_OK);
        return 1;
    }
    if (!InterlockedCompareExchange(&g_installed, 0, 0)) {
        if (!Install()) return 0;
        InterlockedExchange(&g_installed, 1);
    }
    InterlockedExchange(&g_scale, (LONG)(((uint32_t)num << 16) | (uint32_t)den));
    LogOnce();
    Log("ammocap: scale set to %d/%d", num, den);
    ShSetError(SH_OK);
    return 1;
}

SH_API void ShGetAmmoScale(int *num, int *den) {
    uint32_t n, d;
    LONG s = InterlockedCompareExchange(&g_scale, 0, 0);

    if (!NumDen(s, &n, &d)) { n = 1; d = 1; }
    if (num) *num = (int)n;
    if (den) *den = (int)d;
}

SH_API int ShAmmoScaleActive(void) {
    LONG s = InterlockedCompareExchange(&g_scale, 0, 0);

    return (s && s != SCALE_ONE) ? 1 : 0;
}
