/* The shared hook layer: one MinHook state, one hook per target, for the
 * whole DLL.
 *
 * ---- why this exists ---------------------------------------------------
 *
 * MinHook keeps one hook per target per module. That is a property of the
 * library, not a policy anyone chose, and it is why every plugin that used
 * to carry MinHook of its own was a land mine: the second installer of a
 * target gets MH_ERROR_ALREADY_CREATED and silently does nothing, and the
 * first one to shut MinHook down takes every other hook in the DLL with it.
 * Both failure modes are invisible until they are not.
 *
 * This layer owns both problems so no plugin has to think about them:
 *
 *   1. one hook per target, globally. A target is recorded here the moment
 *      it is hooked, by tag. A second request for the same target - from
 *      another plugin, or from this one twice - is answered SH_HOOK_DUP with
 *      a log line naming the tag that got there first, instead of a refusal
 *      from inside MinHook that means the same thing and says nothing.
 *
 *   2. one MinHook state, owned here. MH_Initialize is called once and
 *      ALREADY_INITIALIZED is the normal answer, because the framework's
 *      own hook owners (the file layer, the forge I/O layer, the overlay)
 *      initialize it before any plugin runs. MH_Uninitialize is never
 *      called: MinHook's state is per DLL, and shutting it down would take
 *      the framework's hooks with it. What a plugin installs, it can take
 *      back with ShHookRemove - its own hooks only, never anyone else's.
 *
 * ---- what a caller gets -------------------------------------------------
 *
 * The plugin owns the detour and the feature; this layer owns the install.
 * The contract, in full:
 *
 *   - ShHookAdd installs and enables, atomically. `orig` is the trampoline
 *     to call through; it is valid from the moment the call returns OK.
 *   - Per-address enable, never MH_ALL_HOOKS: MH_ALL_HOOKS would also
 *     switch on hooks another module of this DLL created and deliberately
 *     left disabled (the file layer says the same in its own comment).
 *   - The same tag twice is idempotent and reference counted: two users of
 *     one tag share one hook, and the hook leaves when the last one calls
 *     ShHookRemove.
 *   - ShHookAddLate waits for a module that is not loaded yet, on the
 *     calling thread, for up to about five minutes. It exists for the
 *     loader DLLs (uplay_r1_loader64.dll and friends) that arrive whenever
 *     the game gets round to them.
 *
 * ---- what a caller must not do ------------------------------------------
 *
 * Do not call any of this from inside a hook detour: creating a hook
 * allocates and writes code pages, and doing that from inside another
 * hook's detour is how a process deadlocks against its own loader lock.
 * ShHookRemove is for teardown, not for toggling a feature off: a detour
 * that checks its own flag and calls through is the cheap way to be off,
 * and it is what every plugin here does.
 *
 * ---- who uses it --------------------------------------------------------
 *
 * Everyone who hooks anything, since 2026-10-16: the plugin set (micfix,
 * AllLanguages, NoCorruptionPopup, NvFilter), the framework's own hook
 * owners (the file layer, the CPU-scheduling spoof, the mode machine, the
 * ammo-capacity hook, the overlay's Present code hook), and - by claim
 * rather than by hook - the nine modules that patch game code by hand.
 * That is the whole point of the registry: there is one answer to "what is
 * hooked in this process", and it is ShHookStatus.
 *
 * Log: logs\scripthook_hooks.log, one line per install, refusal, duplicate,
 * claim, enable/disable and removal. Floor level, so a player who never
 * raised the log level still has the record of what was hooked on their
 * machine.
 */
#define SH_BUILD 1
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <psapi.h>

#include "scripthook.h"
#include "log.h"
#include "third_party/minhook/include/MinHook.h"

/* ---- the registry ------------------------------------------------------ */

/* Sized for the whole DLL, not for one plugin's handful: the framework's own
 * owners want about thirty-five (the file layer's twenty-one, corefix's
 * sixteen, playmode's two, ammocap's one, the overlay's one), the nine
 * hand-patched modules claim a dozen more, and the plugin set adds another
 * seventeen. A registry that runs out refuses installs - which is correct
 * behaviour and a very bad state to be in, because half a hook set is not
 * the hook set anybody tested. So the number is generous on purpose, and
 * the refusal says what it means. */
#define HOOK_MAX 128

typedef struct {
    char     tag[HOOK_TAG_MAX];   /* the caller's name for this hook       */
    char     label[96];           /* "user32!MessageBoxW", or the caller's */
    void    *target;              /* the address that was hooked           */
    void    *orig;                /* the trampoline, to call through       */
    LONG     refs;                /* how many Add calls under this tag     */
    int      used;
    int      state;               /* HST_* - see below                     */
    int      claimed;             /* 1: owned, patched by hand elsewhere   */
} HookRec;

/* A slot is reserved *before* MinHook is called, and MinHook is called with
 * the registry lock released. Without the reservation two installers of one
 * target would both pass FindTarget (neither is recorded yet) and create two
 * hooks on the same address; with it the second one is answered DUP by the
 * same rule as always, and the window is honest about what it is. */
#define HST_FREE    0
#define HST_INSTALL 1             /* reserved; MinHook is being called      */
#define HST_ACTIVE  2             /* installed (or claimed)                 */
#define HST_REMOVE  3             /* teardown in flight                     */

static HookRec          g_hooks[HOOK_MAX];
static CRITICAL_SECTION g_lock;
static volatile LONG    g_lockReady;
static volatile LONG    g_mhReady;      /* MinHook initialized, once        */
static volatile LONG    g_logMade;

/* The one serialized log in the framework: every module that owns a log
 * writes to it from a single thread, but this file is written by the loader
 * thread, by each plugin's own startup thread and by the supervisor threads
 * - and log.h's writer emits a line as four separate calls (the timestamp,
 * the text, the newline, the flush), so two threads' lines interleave in the
 * middle of each other without this. The floor path in log.h has the same
 * note for the same reason; this layer is where it actually matters. */
static CRITICAL_SECTION g_logLock;
static volatile LONG    g_logLockReady;

static void HLog(const char *fmt, ...) {
    va_list ap;
    int     held = 0;

    if (!g_logMade && InterlockedCompareExchange(&g_logMade, 1, 0) == 0) {
        LogInitAlways("scripthook_hooks.log");
        InitializeCriticalSection(&g_logLock);
        InterlockedExchange(&g_logLockReady, 1);
    }
    if (g_logLockReady) {
        EnterCriticalSection(&g_logLock);
        held = 1;
    }
    va_start(ap, fmt);
    Logv(fmt, ap);
    va_end(ap);
    if (held) LeaveCriticalSection(&g_logLock);
}

static void Lock(void)   { if (g_lockReady) EnterCriticalSection(&g_lock); }
static void Unlock(void) { if (g_lockReady) LeaveCriticalSection(&g_lock); }

/* Once, and tolerantly: ALREADY_INITIALIZED is the common case in this DLL,
 * because the framework's own hook owners get there first. Anything else is
 * a refusal, and the caller's hook simply does not go in. */
static int MinHookReady(void) {
    MH_STATUS s;

    if (InterlockedCompareExchange(&g_mhReady, 0, 0)) return 1;
    s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        HLog("hooks: MH_Initialize failed (%s) - nothing is installed",
             MH_StatusToString(s));
        return 0;
    }
    InterlockedExchange(&g_mhReady, 1);
    return 1;
}

/* The record for a target, whatever tag owns it. NULL when the target is
 * free. The lock must be held. */
static HookRec *FindTarget(void *target) {
    int i;

    for (i = 0; i < HOOK_MAX; i++)
        if (g_hooks[i].used && g_hooks[i].target == target)
            return &g_hooks[i];
    return NULL;
}

static HookRec *FindTag(const char *tag) {
    int i;

    for (i = 0; i < HOOK_MAX; i++)
        if (g_hooks[i].used && strcmp(g_hooks[i].tag, tag) == 0)
            return &g_hooks[i];
    return NULL;
}

/* ---- is the entry point already somebody else's? ------------------------
 *
 * Read only: twelve bytes of the target, a log line, and then the install
 * goes ahead exactly as it did before. Nothing here can change a result,
 * and no return value is touched.
 *
 * What it buys is a name for the one state this registry cannot see. It
 * knows every hook installed through ShHookAdd*, and MinHook answers for
 * the ones the framework's own layers create behind the registry's back
 * (MH_ERROR_ALREADY_CREATED). What neither can see is a plugin - or a mod
 * that is not built against this DLL at all - patching the function's first
 * bytes with VirtualProtect. Measured 2026-10-27: none of the sixteen
 * .asi in plugins\third_plugin carries MinHook, they all patch bytes
 * directly. So the two collision cases are:
 *
 *   they patched first   our FindTarget has no record, the install below
 *                        succeeds on top of their jump, and the two detours
 *                        end up chained in an order nobody chose.
 *   we hooked first      their patch lands on the entry later and our
 *                        detour stops running. No install happens then, so
 *                        nothing in this file can see it; that half is
 *                        unobservable from here by construction.
 *
 * This is the first case only, and it cannot be more than a suspicion: a
 * function may genuinely begin with a jump (a hotpatch thunk, a linker
 * /Gy stub, call/pop). The bytes are logged with it so a support case can
 * tell the two apart without a debugger. */
extern int ShReadableAddr(uint64_t addr, size_t len);

/* Sampling and reporting are split because the caller must not hold the
 * registry lock while a log line is written: HLog takes the log lock and
 * flushes, and a slow disk would then block every ShHookAdd and
 * ShHookStatus for the duration. Sampling is twelve bytes and no I/O. */
static int SampleEntryOwner(void *target, unsigned char *b) {
    uint64_t a = (uint64_t)(uintptr_t)target;

    if (!a || !ShReadableAddr(a, 12)) return 0;
    __try {
        memcpy(b, (void *)(uintptr_t)a, 12);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (b[0] == 0xE9) return 1;
    if (b[0] == 0xEB) return 2;
    if (b[0] == 0xCC) return 3;
    if (b[0] == 0x48 && b[1] == 0xB8 && b[10] == 0xFF && b[11] == 0xE0) return 4;
    return 0;
}

static void ReportEntryOwner(const char *tag, const char *label, int what,
                             const unsigned char *b) {
    static const char *const kWhat[5] = {
        "", "a rel32 jump (E9)", "a rel8 jump (EB)", "int3 (CC)",
        "a 64-bit jump (mov rax,imm64; jmp rax)"
    };

    if (what < 1 || what > 4) return;
    HLog("hooks: %s -> %s starts with %s before this install - the entry is "
         "already owned by something this registry cannot see (a framework "
         "layer, or a mod that patches bytes itself: the ones in "
         "plugins\\third_plugin carry no MinHook). Installing anyway; if this "
         "feature or that one stops working, that is why. bytes %02X %02X "
         "%02X %02X %02X %02X %02X %02X",
         tag, label, kWhat[what], b[0], b[1], b[2], b[3], b[4], b[5], b[6],
         b[7]);
}

/* ---- install ----------------------------------------------------------- */

/* Shared by Add and by the Late waiter once the module has arrived. The
 * lock must NOT be held. Returns SH_HOOK_OK / SH_HOOK_DUP / SH_HOOK_ERR. */
static int InstallLocked(const char *tag, const char *label,
                         void *target, void *detour, void **orig) {
    HookRec *r;
    MH_STATUS s, srem;
    unsigned char bytes[12];
    void    *tramp = NULL;
    int      i, slot = -1, what;
    LONG     refs = 0;
    char     ltag[HOOK_TAG_MAX], llabel[96], owner[HOOK_TAG_MAX];

    /* Everything the log lines below print is copied out while the lock is
     * held: the record it comes from can be cleared by a concurrent
     * ShHookRemove the moment the lock is released. */
    snprintf(ltag, sizeof(ltag), "%s", tag);
    snprintf(llabel, sizeof(llabel), "%s", label);
    owner[0] = 0;

    Lock();

    r = FindTag(tag);
    if (r) {
        if (r->state == HST_INSTALL) {
            /* Another thread is inside MH_CreateHook for this very tag. Its
             * trampoline does not exist yet, so there is nothing honest to
             * hand back here; answering OK with a NULL orig is how a caller
             * ends up calling through nothing. */
            Unlock();
            if (orig) *orig = NULL;
            HLog("hooks: %s is being installed on another thread - not "
                 "installed a second time", ltag);
            return SH_HOOK_ERR;
        }
        /* The same tag twice: one hook, counted twice. The trampoline is
         * handed back again so both callers call through the same one. */
        r->refs++;
        refs = r->refs;
        tramp = r->orig;
        Unlock();
        if (orig) *orig = tramp;
        HLog("hooks: %s -> %s (already in place under this tag, refs=%ld)",
             ltag, llabel, refs);
        return SH_HOOK_OK;
    }

    r = FindTarget(target);
    if (r) {
        /* Somebody else got here first - installed, or reserved a moment ago
         * by a thread that is inside MinHook right now. Not installed, and
         * the caller's detour is NOT in the chain - the log names the owner
         * so the next question ("why is my detour not running") has an
         * answer without a debugger. */
        snprintf(owner, sizeof(owner), "%s", r->tag);
        Unlock();
        if (orig) *orig = NULL;
        HLog("hooks: %s -> %s is already owned by %s - not installed",
             ltag, llabel, owner);
        return SH_HOOK_DUP;
    }

    for (i = 0; i < HOOK_MAX; i++) {
        if (!g_hooks[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        Unlock();
        HLog("hooks: %s -> %s REFUSED: the registry is full (%d) - HOOK_MAX "
             "is too small for this build, and every install after this one "
             "will be refused too", ltag, llabel, HOOK_MAX);
        return SH_HOOK_ERR;
    }

    if (!MinHookReady()) {
        Unlock();
        if (orig) *orig = NULL;
        return SH_HOOK_ERR;
    }

    /* Reserve the slot, then let go of the lock. Reserving first is what
     * makes the window safe: a second installer of this target meets
     * FindTarget (DUP), and a second caller of this tag meets the
     * HST_INSTALL branch above, instead of both racing into MinHook. */
    r = &g_hooks[slot];
    memset(r, 0, sizeof(*r));
    snprintf(r->tag, sizeof(r->tag), "%s", ltag);
    snprintf(r->label, sizeof(r->label), "%s", llabel);
    r->target = target;
    r->state  = HST_INSTALL;
    r->used   = 1;
    Unlock();

    /* Last look before the bytes are written: says so in the log when the
     * entry already jumps somewhere this registry does not know about. Read
     * only, and outside the lock - the report needs the log lock. */
    what = SampleEntryOwner(target, bytes);
    if (what) ReportEntryOwner(ltag, llabel, what, bytes);

    /* MH_CreateHook writes into the target's prologue and keeps the
     * trampoline in MinHook's own pool; it takes no registry lock of ours,
     * and holding one across it is what lets a slow install block every
     * other module's ShHookStatus. */
    s = MH_CreateHook(target, detour, orig);
    if (s != MH_OK) {
        Lock();
        memset(&g_hooks[slot], 0, sizeof(g_hooks[slot]));
        Unlock();
        if (orig) *orig = NULL;
        if (s == MH_ERROR_ALREADY_CREATED) {
            /* A hook nobody registered here: the framework's own layers
             * still install theirs directly. Same answer as a DUP, with the
             * reason this one is different spelled out. */
            HLog("hooks: %s -> %s is already hooked outside this registry "
                 "(a framework layer owns it) - not installed", ltag,
                 llabel);
            return SH_HOOK_DUP;
        }
        HLog("hooks: %s -> %s refused: MH_CreateHook %s", ltag, llabel,
             MH_StatusToString(s));
        return SH_HOOK_ERR;
    }
    tramp = orig ? *orig : NULL;

    /* Per address, never MH_ALL_HOOKS - see the file head. A refusal here
     * leaves a created-but-disabled hook behind, which is the one state
     * MinHook cannot recover from by itself, so it is removed again - and
     * whether that removal worked is part of the answer, because a hook left
     * in that state makes the next installer of this target report "already
     * hooked outside this registry" and send the next person looking in the
     * wrong place. */
    s = MH_EnableHook(target);
    if (s != MH_OK) {
        srem = MH_RemoveHook(target);
        Lock();
        memset(&g_hooks[slot], 0, sizeof(g_hooks[slot]));
        Unlock();
        if (orig) *orig = NULL;
        HLog("hooks: %s -> %s refused: MH_EnableHook %s; the created hook "
             "was %s", ltag, llabel, MH_StatusToString(s),
             srem == MH_OK ? "removed again"
                           : MH_StatusToString(srem));
        return SH_HOOK_ERR;
    }

    Lock();
    r = &g_hooks[slot];
    r->orig  = tramp;
    r->refs  = 1;
    r->state = HST_ACTIVE;
    Unlock();
    HLog("hooks: %s -> %s at %p (trampoline %p)", ltag, llabel, target,
         tramp);
    return SH_HOOK_OK;
}

SH_API int ShHookAdd(const char *tag, HMODULE mod, const char *fn,
                     void *detour, void **orig) {
    void *target;
    char modname[64];
    char label[160];

    if (!tag || !*tag || !mod || !fn || !detour) {
        HLog("hooks: ShHookAdd called with a null argument - nothing done");
        return SH_HOOK_ERR;
    }
    target = (void *)GetProcAddress(mod, fn);
    if (!target) {
        if (orig) *orig = NULL;
        HLog("hooks: %s -> the export %s was not found - nothing installed",
             tag, fn);
        return SH_HOOK_ERR;
    }
    modname[0] = 0;
    GetModuleBaseNameA(GetCurrentProcess(), mod, modname,
                       (DWORD)sizeof(modname));
    if (!modname[0])
        snprintf(modname, sizeof(modname), "%p", (void *)mod);
    snprintf(label, sizeof(label), "%s!%s", modname, fn);

    return InstallLocked(tag, label, target, detour, orig);
}

SH_API int ShHookAddAt(const char *tag, void *target, const char *label,
                       void *detour, void **orig) {
    char mine[96];

    if (!tag || !*tag || !target || !detour) {
        HLog("hooks: ShHookAddAt called with a null argument - nothing done");
        return SH_HOOK_ERR;
    }
    /* A caller that has no label to offer gets the address: the log and the
     * status line still say which target this is, which is the whole point
     * of the registry. */
    if (!label || !*label) {
        snprintf(mine, sizeof(mine), "0x%p", target);
        label = mine;
    }
    return InstallLocked(tag, label, target, detour, orig);
}

SH_API int ShHookClaim(const char *tag, void *target, const char *label) {
    HookRec *r;
    char     mine[96];
    int      i, slot = -1;

    if (!tag || !*tag || !target) {
        HLog("hooks: ShHookClaim called with a null argument - nothing done");
        return SH_HOOK_ERR;
    }
    if (!label || !*label) {
        snprintf(mine, sizeof(mine), "0x%p", target);
        label = mine;
    }

    Lock();
    r = FindTag(tag);
    if (r) {
        r->refs++;
        Unlock();
        HLog("hooks: %s -> %s (already claimed under this tag, refs=%ld)",
             tag, label, r->refs);
        return SH_HOOK_OK;
    }
    r = FindTarget(target);
    if (r) {
        Unlock();
        HLog("hooks: %s -> %s is already owned by %s - not claimed",
             tag, label, r->tag);
        return SH_HOOK_DUP;
    }
    for (i = 0; i < HOOK_MAX; i++) {
        if (!g_hooks[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        Unlock();
        HLog("hooks: %s -> %s refused: the registry is full (%d)",
             tag, label, HOOK_MAX);
        return SH_HOOK_ERR;
    }

    /* No MinHook here, and that is the point: a claim records that this
     * address is spoken for by a hand-written patch, so the next caller
     * asking for it is told who owns it instead of silently writing over
     * somebody else's jump. The patch itself stays exactly where it was. */
    r = &g_hooks[slot];
    memset(r, 0, sizeof(*r));
    snprintf(r->tag, sizeof(r->tag), "%s", tag);
    snprintf(r->label, sizeof(r->label), "%s", label);
    r->target = target;
    r->orig    = NULL;
    r->refs    = 1;
    r->used    = 1;
    r->state   = HST_ACTIVE;
    r->claimed = 1;
    Unlock();
    HLog("hooks: %s -> %s claimed (patched by hand, not hooked here)",
         tag, label);
    return SH_HOOK_OK;
}

SH_API int ShHookAddLate(const char *tag, const char *modname,
                         const char *fn, void *detour, void **orig) {
    HMODULE mod;
    int     i, waited = 0;

    if (!tag || !*tag || !modname || !*modname || !fn || !detour) {
        HLog("hooks: ShHookAddLate called with a null argument - nothing "
             "done");
        return SH_HOOK_ERR;
    }

    /* The wait is the whole point of this entry point: a loader DLL that is
     * not in the process yet arrives whenever the game gets round to it,
     * and a hook that goes in before the game takes the pointer it wants
     * is the only kind that always works (AllLanguages lost a week to
     * learning that the other kind is a race). The policy is the one that
     * plugin used: a quarter second for the first minute, a second after
     * that, about five minutes in total. */
    for (i = 0; i < 480; i++) {
        mod = GetModuleHandleA(modname);
        if (mod) break;
        if (i == 0)
            HLog("hooks: %s is waiting for %s to be loaded", tag, modname);
        Sleep(i < 240 ? 250 : 1000);
        waited = 1;
    }
    if (!mod) {
        if (orig) *orig = NULL;
        HLog("hooks: %s gave up waiting for %s - nothing installed",
             tag, modname);
        return SH_HOOK_ERR;
    }
    if (waited)
        HLog("hooks: %s found %s after %d poll(s)", tag, modname, i);

    return ShHookAdd(tag, mod, fn, detour, orig);
}

SH_API int ShHookEnable(const char *tag, int on) {
    HookRec  *r;
    MH_STATUS s;
    void     *target;
    char      llabel[96];

    if (!tag || !*tag) return SH_HOOK_ERR;

    /* Hidden enable/disable is part of owning a hook: a hot-path detour
     * that is off most of the session should cost nothing while it is off,
     * and a detour per call is not nothing. MinHook keeps the trampoline
     * valid across both, so a caller that disables and enables at will can
     * still call through the pointer it was handed at install time. */
    Lock();
    r = FindTag(tag);
    if (!r) {
        Unlock();
        HLog("hooks: %s is not installed - nothing to %s", tag,
             on ? "enable" : "disable");
        return SH_HOOK_ERR;
    }
    if (r->claimed) {
        Unlock();
        HLog("hooks: %s is a claim (patched by hand), not a hook - nothing "
             "to %s", tag, on ? "enable" : "disable");
        return SH_HOOK_ERR;
    }
    target = r->target;
    /* The record can be cleared by a concurrent ShHookRemove the moment the
     * lock goes, so what the log prints has to be a copy, not a pointer. */
    snprintf(llabel, sizeof(llabel), "%s", r->label);
    Unlock();

    s = on ? MH_EnableHook(target) : MH_DisableHook(target);
    if (s != MH_OK) {
        HLog("hooks: %s -> %s %s failed (%s)", tag, llabel,
             on ? "enable" : "disable", MH_StatusToString(s));
        return SH_HOOK_ERR;
    }
    HLog("hooks: %s -> %s %s", tag, llabel, on ? "enabled" : "disabled");
    return SH_HOOK_OK;
}

SH_API int ShHookRemove(const char *tag) {
    HookRec *r;
    void    *target;
    int      slot;

    if (!tag || !*tag) return SH_HOOK_ERR;

    Lock();
    r = FindTag(tag);
    if (!r) {
        Unlock();
        HLog("hooks: %s is not installed - nothing to remove", tag);
        return SH_HOOK_ERR;
    }
    if (r->state == HST_INSTALL) {
        /* Its trampoline is still being made on another thread. Refusing is
         * the only answer that cannot leave MinHook holding a hook nobody
         * has a record of. */
        Unlock();
        HLog("hooks: %s is being installed on another thread - not removed",
             tag);
        return SH_HOOK_ERR;
    }
    r->refs--;
    if (r->refs > 0) {
        LONG left = r->refs;

        Unlock();
        HLog("hooks: %s released (refs=%ld left)", tag, left);
        return SH_HOOK_OK;
    }

    /* Last one out. The slot is marked first and the record is cleared
     * before the lock goes back on, so a detour mid-flight reads its own
     * orig pointer and never the next record's. MinHook itself is called
     * with the lock released: MH_DisableHook suspends every other thread to
     * rewrite the prologue, and a thread parked in ShHookStatus holding
     * this lock would deadlock against it. Teardown only - to be off
     * without being gone, the detour checks its own flag. */
    target = r->target;
    r->state = HST_REMOVE;
    slot = (int)(r - g_hooks);
    Unlock();

    MH_DisableHook(target);
    MH_RemoveHook(target);

    Lock();
    memset(&g_hooks[slot], 0, sizeof(g_hooks[slot]));
    Unlock();
    HLog("hooks: %s removed from %p", tag, target);
    return SH_HOOK_OK;
}

SH_API int ShHookStatus(char *buf, int n) {
    char tags[512];
    int  i, count = 0, tlen = 0;

    if (!buf || n <= 0) return 0;
    buf[0]  = 0;
    tags[0] = 0;

    Lock();
    for (i = 0; i < HOOK_MAX; i++) {
        if (!g_hooks[i].used) continue;
        count++;
        tlen += snprintf(tags + tlen, sizeof(tags) - (size_t)tlen, "%s%s",
                         tlen ? ", " : "", g_hooks[i].tag);
        if (tlen >= (int)sizeof(tags) - 1) break;
    }
    Unlock();

    if (!count) {
        snprintf(buf, (size_t)n, "no hooks");
        return (int)strlen(buf);
    }
    {
        int hooked = 0, claimed = 0;

        Lock();
        for (i = 0; i < HOOK_MAX; i++) {
            if (!g_hooks[i].used) continue;
            if (g_hooks[i].claimed) claimed++; else hooked++;   /* INSTALL and ACTIVE both */
        }
        Unlock();
        snprintf(buf, (size_t)n, "%d hooked, %d claimed: %s", hooked, claimed,
                 tags);
    }
    return (int)strlen(buf);
}

/* ---- startup ----------------------------------------------------------- */

/* Called from the loader thread once, before the plugins load: the layer
 * has no work of its own until a plugin asks for a hook, and the lock it
 * hands out has to exist before the first ask, which can come from any
 * thread at any time after this. */
void ShHookStartup(void) {
    if (InterlockedExchange(&g_lockReady, 1)) return;
    InitializeCriticalSection(&g_lock);
    LogInitAlways("scripthook_hooks.log");
    /* The log's own lock is made by the first HLog call (see above), so the
     * registry lock stays the only thing this function owns. */
    HLog("--- the shared hook layer ---");
    HLog("build " __DATE__ " " __TIME__);
    HLog("one hook per target, one MinHook state per DLL; MH_Uninitialize "
         "is never called");
}
