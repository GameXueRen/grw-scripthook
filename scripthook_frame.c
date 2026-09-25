/* A per frame hook on the game thread, one call a frame.
 *
 * scripthook.h has carried the ShRegisterFrameCallback declaration since
 * the API was written, and nothing ever implemented it: a plugin that
 * bound it got nothing back, and one that imported it failed to load at
 * all. This is the implementation.
 *
 * The site is the engine's own spawn-director update, which runs once a
 * frame on the game thread; its first six bytes are what are taken. The
 * stub puts every register the caller owns back before the engine's own
 * instructions run, which is what makes a hook at +0 safe: at that offset
 * rcx/rdx/r8/r9 hold the arguments and the XMM registers may hold live
 * values.
 *
 * The site, the stolen length and the two byte sequences the stub carries
 * come from GhostHook (https://github.com/IHateHUDClutter/GhostHook, GPL-3.0
 * like this tree), the NPC spawner author's own framework, where this hook
 * was first used and validated on the same TU25 build.
 *
 * Nothing is patched until someone registers, and the site is checked byte
 * for byte first: a stale RVA is then a logged no-op rather than a jump
 * into whatever moved there.
 *
 * Logs to <gamedir>\logs\scripthook_frame.log
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"
#include "log.h"

/* TU25 Ai::SpawningManagerUpdate. */
#define FRAME_RVA  0x0BF42B60u
#define STOLEN     6
#define CB_MAX     16

extern void *ShAllocNear(uint64_t target);
extern int   ShReadableAddr(uint64_t addr, size_t len);

/* The prologue the site must have before anything is written. Thirty
 * bytes, so it covers the six that are taken and the call that follows
 * them - a shape nothing else in the image shares by accident. */
static const uint8_t kSite[] = {
    0x40,0x57,0x48,0x83,0xec,0x50,0x48,0x89,0xcf,0x48,0x8b,0x49,0x08,
    0x48,0x85,0xc9,0x0f,0x84,0x19,0x01,0x00,0x00,0x48,0x89,0x5c,0x24,
    0x60,0xe8,0x80,0x79
};

static ShFrameFn_t volatile g_cb[CB_MAX];
static void        *g_user[CB_MAX];
static uint8_t     *g_stub;
static DWORD        g_tls = TLS_OUT_OF_INDEXES;
static volatile LONG g_installing;
static volatile LONG g_said;

/* What the stub runs around the call: save rcx/rdx/r8/r9/r10/r11/rax and
 * xmm0 to xmm5, and put every one of them back. */
static const uint8_t kSave[] = {
    0x48,0x81,0xec,0xd0,0x00,0x00,0x00,0x48,0x89,0x4c,0x24,0x20,
    0x48,0x89,0x54,0x24,0x28,0x4c,0x89,0x44,0x24,0x30,0x4c,0x89,
    0x4c,0x24,0x38,0x4c,0x89,0x54,0x24,0x40,0x4c,0x89,0x5c,0x24,
    0x48,0x48,0x89,0x44,0x24,0x50,0x0f,0x11,0x44,0x24,0x60,0x0f,
    0x11,0x4c,0x24,0x70,0x0f,0x11,0x94,0x24,0x80,0x00,0x00,0x00,
    0x0f,0x11,0x9c,0x24,0x90,0x00,0x00,0x00,0x0f,0x11,0xa4,0x24,
    0xa0,0x00,0x00,0x00,0x0f,0x11,0xac,0x24,0xb0,0x00,0x00,0x00,
    0x48,0x8b,0x4c,0x24,0x20
};

static const uint8_t kRest[] = {
    0x0f,0x10,0xac,0x24,0xb0,0x00,0x00,0x00,0x0f,0x10,0xa4,0x24,
    0xa0,0x00,0x00,0x00,0x0f,0x10,0x9c,0x24,0x90,0x00,0x00,0x00,
    0x0f,0x10,0x94,0x24,0x80,0x00,0x00,0x00,0x0f,0x10,0x4c,0x24,
    0x70,0x0f,0x10,0x44,0x24,0x60,0x48,0x8b,0x44,0x24,0x50,0x4c,
    0x8b,0x5c,0x24,0x48,0x4c,0x8b,0x54,0x24,0x40,0x4c,0x8b,0x4c,
    0x24,0x38,0x4c,0x8b,0x44,0x24,0x30,0x48,0x8b,0x54,0x24,0x28,
    0x48,0x8b,0x4c,0x24,0x20,0x48,0x81,0xc4,0xd0,0x00,0x00,0x00
};

static void RunCallbacks(void) {
    int i;

    for (i = 0; i < CB_MAX; i++) {
        ShFrameFn_t fn = g_cb[i];

        if (fn) fn(g_user[i]);
    }
}

/* The stub's landing point. A callback that reaches the site again - it
 * asks the engine for something that updates the spawn director, say -
 * must not start a second round under the first, so the TLS slot is the
 * re-entry guard. The engine's own error value is carried across, because
 * this runs inside its code and it may be relying on it. */
static void FrameDispatch(void) {
    DWORD error = GetLastError();

    if (!TlsGetValue(g_tls) && TlsSetValue(g_tls, (void *)1)) {
        RunCallbacks();
        TlsSetValue(g_tls, NULL);
    }
    SetLastError(error);
}

/* Is the site still the prologue this was pinned against? */
static int SiteOk(uint64_t *out) {
    uint64_t site = SH_IMG(FRAME_RVA);

    if (!ShReadableAddr(site, sizeof kSite)) return 0;
    if (memcmp((const void *)(uintptr_t)site, kSite, sizeof kSite)) return 0;
    if (out) *out = site;
    return 1;
}

/* Build the stub, then take the six bytes. Once per process: the stub is
 * kept, and a later registration finds the hook already in place. */
static int EnsureHook(void) {
    uint64_t site = 0, fn;
    uint8_t *s;
    uint8_t patch[STOLEN];
    int64_t rel;
    int o = 0;
    DWORD old = 0;

    if (InterlockedCompareExchange(&g_installing, 1, 0)) return 0;
    if (g_stub) { InterlockedExchange(&g_installing, 0); return 1; }

    if (!SiteOk(&site)) {
        if (InterlockedExchange(&g_said, 1) == 0) {
            LogInit("scripthook_frame.log");
            Log("frame: %llX does not hold the pinned prologue - no hook, "
                "and no callback will run",
                (unsigned long long)SH_IMG(FRAME_RVA));
        }
        InterlockedExchange(&g_installing, 0);
        return 0;
    }
    if (g_tls == TLS_OUT_OF_INDEXES) g_tls = TlsAlloc();
    if (g_tls == TLS_OUT_OF_INDEXES) {
        InterlockedExchange(&g_installing, 0);
        return 0;
    }

    s = (uint8_t *)ShAllocNear(site);
    if (!s) {
        InterlockedExchange(&g_installing, 0);
        return 0;
    }
    memset(s, 0xCC, 0x1000);

    s[o++] = 0x9C;                                  /* pushfq            */
    memcpy(s + o, kSave, sizeof kSave); o += sizeof kSave;
    s[o++] = 0x48; s[o++] = 0xB8;                   /* mov rax, imm64    */
    fn = (uint64_t)(uintptr_t)FrameDispatch;
    memcpy(s + o, &fn, 8); o += 8;
    s[o++] = 0xFF; s[o++] = 0xD0;                   /* call rax          */
    memcpy(s + o, kRest, sizeof kRest); o += sizeof kRest;
    s[o++] = 0x9D;                                  /* popfq             */
    memcpy(s + o, kSite, STOLEN); o += STOLEN;      /* the engine's own  */
    s[o++] = 0xFF; s[o++] = 0x25;                   /* jmp [rip+0]       */
    memset(s + o, 0, 4); o += 4;
    fn = site + STOLEN;
    memcpy(s + o, &fn, 8); o += 8;

    if (!FlushInstructionCache(GetCurrentProcess(), s, (size_t)o)) {
        VirtualFree(s, 0, MEM_RELEASE);
        InterlockedExchange(&g_installing, 0);
        return 0;
    }

    /* E9 rel32, then a nop for the sixth byte. The site was checked a
     * moment ago and this is the process's own game thread work; every
     * other hook in this framework patches the same way. */
    rel = (int64_t)(uintptr_t)s - (int64_t)(site + 5);
    if (rel < INT32_MIN || rel > INT32_MAX ||
        !VirtualProtect((void *)(uintptr_t)site, STOLEN,
                        PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(s, 0, MEM_RELEASE);
        InterlockedExchange(&g_installing, 0);
        return 0;
    }
    patch[0] = 0xE9;
    { int32_t r = (int32_t)rel; memcpy(patch + 1, &r, 4); }
    patch[5] = 0x90;
    memcpy((void *)(uintptr_t)site, patch, STOLEN);
    FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)site,
                          STOLEN);
    VirtualProtect((void *)(uintptr_t)site, STOLEN, old, &old);

    g_stub = s;
    InterlockedExchange(&g_installing, 0);

    LogInit("scripthook_frame.log");
    Log("frame: hook at %llX -> stub %llX, one call a frame",
        (unsigned long long)site, (unsigned long long)(uintptr_t)s);
    return 1;
}

SH_API int ShRegisterFrameCallback(ShFrameFn_t fn, void *user) {
    int i;

    if (!fn) return 0;

    /* Already on the list? Then this is a caller arriving twice, which is
     * what the loader does to a plugin set - two "plugin scan done" lines,
     * seconds apart, in every start-up log - and the answer is still yes.
     * One slot, one call a frame: a second one would run the callback
     * twice and make a counted probe report double the frame rate. */
    for (i = 0; i < CB_MAX; i++)
        if (g_cb[i] == fn) return 1;

    if (!EnsureHook()) return 0;

    for (i = 0; i < CB_MAX; i++) {
        if (g_cb[i]) continue;
        /* The user pointer goes in first, so the callback never sees a
         * slot that is live but not yet carrying its own data. */
        g_user[i] = user;
        g_cb[i] = fn;
        return 1;
    }
    return 0;
}

SH_API void ShUnregisterFrameCallback(ShFrameFn_t fn) {
    int i;

    if (!fn) return;
    for (i = 0; i < CB_MAX; i++)
        if (g_cb[i] == fn) g_cb[i] = NULL;
}
