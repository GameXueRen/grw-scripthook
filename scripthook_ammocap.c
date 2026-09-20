/* Ammo capacity, scaled: this module hooks the one engine function that
 * computes a magazine's capacity and scales what it returns. Which function
 * that is, and why a hook is the only way in, is in
 * docs/ammocapacity-reverse.md (kept out of the repository).
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
#define CAP_RVA   0x616180ULL
#define CAP_MAX   0xFFFFu
#define SCALE_ONE 0x00010001L

/* num in the high 16 bits, den in the low. 0 = never set. */
static volatile LONG g_scale;
/* 0 = no hook, 1 = one thread is installing, 2 = installed. */
static volatile LONG g_installed;
static volatile LONG g_logged;
/* Set once AmmoCapacity.asi is seen, so the check and the log happen once
 * instead of on every refused attempt. */
static volatile LONG g_asiBlocked;

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
 * value * num / den, clamped to what a 16-bit capacity can hold.
 *
 * The read is a plain aligned 32-bit load, not a lock cmpxchg: writers
 * publish through InterlockedExchange, so loading the packed word cannot
 * tear - and this runs on the engine's own capacity path, where a locked
 * read per call was pure overhead. */
static int ScaleValue(int ret) {
    uint32_t num, den, value = (uint32_t)ret & CAP_MAX;
    LONG s = g_scale;                          /* aligned; writers atomic */
    uint64_t out;

    if (!s) return ret;
    num = (uint32_t)s >> 16;
    den = (uint32_t)s & 0xFFFFu;
    if (!num || !den) return ret;
    if (num == 1 && den == 1) return ret;      /* pass through untouched */
    if (!value) return 0;
    out = (uint64_t)value * num / den;
    return out > CAP_MAX ? (int)CAP_MAX : (int)out;
}

/* The last look, for ShGetAmmoLook. Recorded on the engine's own capacity
 * path, so the detour stays four stores, one timer read and one increment:
 * no lock, no callback, no allocation, and nothing that can block the engine's
 * thread. The count is bumped LAST, so a caller that polls and sees it move
 * knows the rest was written before it - which is the whole of the ordering
 * this needs. */
static volatile uint64_t g_lookArgs[4];
static volatile uint32_t g_lookRaw;
static volatile uint64_t g_lookTick;
static volatile LONG     g_lookCount;

/* Which call is the player's, counted in the detour.
 *
 * The calls this function takes are not all the player's: it is the engine's
 * shared magazine call, and other magazines - other entities' - go through it
 * too. The LAST call was therefore not an answer: it tracked the magazine while
 * firing, and after a weapon switch it could be the one just stowed, whose
 * number sits at its full value and reads as a fixed 20 or 30 (measured in
 * logs\AmmoControl.log, 2026-09-20).
 *
 * The player's own weapon is the one the engine asks about most often - every
 * shot asks, and so does whatever redraws the number - so the detour keeps a
 * count per object and the API reads the rounds off the hottest of them. Only
 * eight slots, a linear scan and one increment on the engine's own path: no
 * lock, no allocation, nothing that can block it. */
/* As many as the call trace holds, because the two are about the same set:
 * every distinct weapon the engine asks about. Eight was not enough - the
 * player's own weapon was pushed out of the table by the ones other entities
 * are asked about, and a weapon that is not in the table can never be seen to
 * move, so "what moved" (the first tier) answered nothing for it and the
 * display stayed on whatever had been in hand before. Measured: the secondary
 * followed and the primary did not, in the same session, for exactly that
 * reason (logs\scripthook_ammocap.log, 2026-09-20 17:44-17:45). */
#define LOOK_SLOTS 32

typedef struct {
    volatile uint64_t obj;
    volatile LONG     n;
    /* The reading below belongs to this object. The detour reuses a slot for
     * another object whenever the table is full, so without this the rounds of
     * one weapon were compared against the rounds of another - which reads as
     * a move every time the table turned over, and pinned the answer to
     * whatever object the churn happened to leave there. */
    uint64_t          stateObj;  /* which object the readings below are about */
    uint32_t          last;      /* the rounds it carried on the last read */
    int               seeded;    /* 0 until `last` has one reading in it */
    uint64_t          moved;     /* tick of the last time those rounds moved */
} LookSlot;

static LookSlot g_look[LOOK_SLOTS];

/* The last calls that changed WHICH object the engine was asking about.
 *
 * A weapon switch runs both the weapon going down and the one coming up
 * through this function, and nothing about those objects says which is which:
 * no owner (+0x250 resolves to the object itself) and no reference from the
 * player (measured - see §9.9). So the ORDER they were asked in is the only
 * evidence there is, and this records it: a compare and two stores on the
 * engine's own path, nothing that blocks. */
#define TRACE_SLOTS 32

typedef struct {
    volatile uint64_t obj;
    volatile uint64_t tick;
} LookCall;

static LookCall g_trace[TRACE_SLOTS];
static volatile LONG g_traceN;      /* transitions seen since the hook went in */
static uint64_t g_traceObj;         /* what the call before this one carried */

static void TraceCall(uint64_t obj) {
    LONG n;
    int i;

    if (obj == g_traceObj) return;  /* transitions only */
    g_traceObj = obj;
    n = InterlockedIncrement(&g_traceN) - 1;
    i = (int)(n & (TRACE_SLOTS - 1));
    g_trace[i].obj = obj;
    g_trace[i].tick = (uint64_t)GetTickCount64();
}

/* Called on the engine's capacity path for every call, with the object it was
 * given. The coldest slot is given up when the table is full, so the eight
 * never become a permanent set: an object that stops being called stops being
 * counted, and the API ages what is left. */
static void NoteCall(uint64_t obj) {
    int i, freeSlot = -1, lowSlot = 0;
    LONG low = 0x7FFFFFFF;

    if (obj < 0x10000ULL || obj >= 0x800000000000ULL || (obj & 7)) return;
    for (i = 0; i < LOOK_SLOTS; i++) {
        uint64_t o = g_look[i].obj;

        if (o == obj) {
            if (g_look[i].n < 0x7FFFFFFF) g_look[i].n++;
            return;
        }
        if (!o) {
            if (freeSlot < 0) freeSlot = i;
            continue;
        }
        if (g_look[i].n < low) { low = g_look[i].n; lowSlot = i; }
    }
    i = (freeSlot >= 0) ? freeSlot : lowSlot;
    g_look[i].n = 1;
    g_look[i].obj = obj;
}

static int CapDetour(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    int ret = g_orig(a1, a2, a3, a4);

    g_lookArgs[0] = a1;
    g_lookArgs[1] = a2;
    g_lookArgs[2] = a3;
    g_lookArgs[3] = a4;
    g_lookRaw = (uint32_t)ret & CAP_MAX;
    g_lookTick = (uint64_t)GetTickCount64();
    InterlockedIncrement(&g_lookCount);
    NoteCall(a1);
    TraceCall(a1);
    return ScaleValue(ret);
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
    if (InterlockedCompareExchange(&g_asiBlocked, 0, 0) ||
        GetModuleHandleA("AmmoCapacity.asi")) {
        if (!InterlockedExchange(&g_asiBlocked, 1))
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
        /* Do not leave a created-but-disabled hook behind: the trampoline
         * it handed us goes away with it. */
        MH_RemoveHook(tgt);
        g_orig = NULL;
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    Log("ammocap: hooked rva 0x%llx (%p), original at %p",
        (unsigned long long)CAP_RVA, tgt, (void *)g_orig);
    return 1;
}

/* ---- the installer's signal ------------------------------------------ */

/* A second caller used to sit in a Sleep(1) spin for up to five seconds while
 * the first one installed the hook - and the loader thread is one of those
 * callers: five seconds of a start-up thread, for a hook that takes
 * microseconds to set. Waiters sleep on this instead, with the same five
 * seconds kept as the outer bound. Nothing waits when the event could not be
 * made: that path keeps the bounded sleep. */
static HANDLE        g_instEvent;
static volatile LONG g_instEventMade;   /* 0 no, 1 yes, 2 being made, -1 no */

static HANDLE InstalledEvent(void) {
    for (;;) {
        LONG s = InterlockedCompareExchange(&g_instEventMade, 0, 0);

        if (s == 1) return g_instEvent;
        if (s == -1) return NULL;
        if (s == 2) { Sleep(0); continue; }     /* another thread is making it */
        if (InterlockedCompareExchange(&g_instEventMade, 2, 0) == 0) {
            HANDLE h = CreateEvent(NULL, TRUE, FALSE, NULL);

            g_instEvent = h;
            InterlockedExchange(&g_instEventMade, h ? 1 : -1);
            return h;
        }
    }
}

/* Called by whoever finished the install, either way. The installer gets here
 * before a waiter can be waiting on the handle, and this is manual-reset: an
 * event set before the wait still wakes the next waiter, so the signal cannot
 * be missed and nobody sleeps the timeout out over a finished install. */
static void InstalledSignal(void) {
    HANDLE h = InstalledEvent();

    if (h) SetEvent(h);
}

/* 1 when the install finished, 0 when it did not within ms. */
static int InstalledWait(DWORD ms) {
    HANDLE h = InstalledEvent();

    if (!h) {
        int spins = 0;

        while (InterlockedCompareExchange(&g_installed, 0, 0) == 1 &&
               spins++ < (int)ms)
            Sleep(1);
    } else {
        WaitForSingleObject(h, ms);
    }
    return InterlockedCompareExchange(&g_installed, 0, 0) == 2;
}

/* ---- public API ------------------------------------------------------ */

SH_API int ShSetAmmoScale(int num, int den) {
    LONG packed;

    if (num <= 0 || den <= 0 || num > 0xFFFF || den > 0xFFFF) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    packed = (LONG)(((uint32_t)num << 16) | (uint32_t)den);
    /* Asking for 1/1 before anything else is the "leave it alone" case: no
     * hook is installed for it, the game keeps its own number. */
    if (packed == SCALE_ONE && InterlockedCompareExchange(&g_installed, 0, 0) == 0) {
        InterlockedExchange(&g_scale, SCALE_ONE);
        ShSetError(SH_OK);
        return 1;
    }
    /* One installer at a time: two plugins asking at the same instant used
     * to both call Install(), and the loser was handed a hook failure even
     * though the hook had just been installed. */
    if (InterlockedCompareExchange(&g_installed, 0, 0) != 2) {
        LONG prev = InterlockedCompareExchange(&g_installed, 1, 0);
        if (prev == 0) {
            if (!Install()) {
                InterlockedExchange(&g_installed, 0);
                InstalledSignal();
                return 0;
            }
            InterlockedExchange(&g_installed, 2);
            InstalledSignal();
        } else if (prev == 1) {
            /* Someone else is installing. This used to be a Sleep(1) spin for
             * up to 5000 rounds - five seconds of a blocked caller thread
             * (the loader's, at start up) for a hook that takes microseconds
             * to set. It waits on the installer's signal now, with the same
             * five seconds as the outer bound. */
            if (!InstalledWait(5000) ||
                InterlockedCompareExchange(&g_installed, 0, 0) != 2)
                return 0;
        }
        /* prev == 2: another thread finished the install while this one was
         * reading g_installed above, which is the success case, not a
         * failure - it is the same hook, and the caller's request is met. */
    }
    /* Log only a real change: the setter can be called in a loop. */
    if (InterlockedExchange(&g_scale, packed) != packed) {
        LogOnce();
        Log("ammocap: scale set to %d/%d", num, den);
    }
    ShSetError(SH_OK);
    return 1;
}

SH_API void ShGetAmmoScale(int *num, int *den) {
    uint32_t n, d;
    LONG s = g_scale;

    if (!NumDen(s, &n, &d)) { n = 1; d = 1; }
    if (num) *num = (int)n;
    if (den) *den = (int)d;
}

SH_API int ShAmmoScaleActive(void) {
    LONG s = g_scale;

    return (s && s != SCALE_ONE) ? 1 : 0;
}

/* ---- the last look, for a probe -------------------------------------- */

/* ---- the rounds ------------------------------------------------------- */

/* Where the rounds sit on the weapon the engine hands its own capacity
 * function: the offsets the removed ammo module used (OFF_AMMO and its
 * alternative). The object they belong to is the one thing that module could
 * not reach cheaply - it swept all of memory for a class - and the engine
 * hands it over by itself now. See docs/ammocapacity-reverse.md, section 9. */
#define OFF_ROUNDS     0x180
#define OFF_ROUNDS_ALT 0x130

/* The weapon in the last call is not the whole story - see the note at the
 * detour, where calls are counted per object instead. A search for a pointer
 * to a weapon inside the player's own objects was tried first and came back
 * empty ("no link (0 pointer(s) to 0 of 1 known weapons across 96 player
 * objects)" in logs\scripthook_ammocap.log), so the counts are what is left,
 * and they say what the engine's own call pattern says. */

static uint64_t g_lastObj;              /* the object the last call carried */
static uint64_t g_handObj;              /* the weapon firing last vouched for */
static uint64_t g_handLogged;           /* the last weapon reported as in hand */
static DWORD    g_diagAt;               /* when the hot list was last logged */

/* A magazine holds rounds, not kilobytes: the protected-int decode can come
 * out positive on memory that is not one of its ints at all, and it did -
 * three objects in one session decoded to 814535360 and to negative numbers
 * that churned on every poll (logs\scripthook_ammocap.log, 2026-09-20
 * 17:53-17:54), and one of those numbers reached the plugin as a magazine
 * count. Anything past this is not a count. */
#define ROUNDS_MAX 0x800

/* The two candidate offsets, each read on its own: a weapon that keeps its
 * count at +0x130 usually has something else at +0x180, and a reading that
 * looks like a number there would otherwise be mistaken for the count. */
static int RoundsAt180(uint64_t obj, uint32_t *out) {
    uint32_t v = 0;

    if (!obj || !ShReadableAddr(obj, OFF_ROUNDS + 4)) return 0;
    if (!ShStatRead(obj + OFF_ROUNDS, &v) || v > ROUNDS_MAX) return 0;
    *out = v;
    return 1;
}

static int RoundsAt130(uint64_t obj, uint32_t *out) {
    uint32_t v = 0;

    if (!obj || !ShReadableAddr(obj, OFF_ROUNDS_ALT + 4)) return 0;
    if (!ShStatRead(obj + OFF_ROUNDS_ALT, &v) || v > ROUNDS_MAX) return 0;
    *out = v;
    return 1;
}

/* The rounds off one object: +0x180, or +0x130 when that answers nothing
 * while this one carries a number - the rule the removed module applied. */
static int RoundsAt(uint64_t obj, uint32_t *out) {
    uint32_t main = 0, alt = 0;
    int haveMain, haveAlt;

    haveMain = RoundsAt180(obj, &main);
    haveAlt  = RoundsAt130(obj, &alt);
    if (!haveMain && !haveAlt) return 0;
    if (haveMain && (!haveAlt || main || !alt)) *out = main;
    else *out = alt;
    return 1;
}

/* ---- whose weapon is it ---------------------------------------------- */

/* The owner handle the removed ammo module insisted on, unchanged: +0x250
 * holds a pointer to a masked handle slot, the object sits at +0x00 of that
 * slot and its flags at +0x0C, and the handle is live when the flags are
 * negative.
 *
 * Measured 2026-09-20 17:37:40 in logs\AmmoProbe.log: of the eight weapons the
 * engine asked about in one burst, exactly ONE resolved to the player. The
 * others are other entities' - which is why the last call alone shows a number
 * that is not the player's, and why this test is worth the few reads. */
#define OFF_OWNER 0x250

static uint64_t PlayerRoot(void) {
    ShPlayer p;

    memset(&p, 0, sizeof(p));
    if (!ShGetPlayer(&p)) return 0;
    return p.root ? p.root : p.entity;
}

static uint64_t ResolveHandle(uint64_t slot) {
    uint64_t val = 0;
    int32_t flags = 0;

    if (!slot || !ShReadableAddr(slot, 0x10)) return 0;
    if (!ShReadBytes(slot, &val, 8)) return 0;
    if (!ShReadBytes(slot + 0xC, &flags, 4)) return 0;
    if (flags >= 0) return 0;
    return (val >= 0x10000ULL && val < 0x800000000000ULL) ? val : 0;
}

static int OwnedByPlayer(uint64_t obj) {
    uint64_t root, handle;

    if (!obj || !ShReadableAddr(obj + OFF_OWNER, 8)) return 0;
    if (!ShReadBytes(obj + OFF_OWNER, &handle, 8)) return 0;
    root = PlayerRoot();
    return root && ResolveHandle(handle) == root;
}

/* The object the engine asked about most recently that belongs to the player,
 * read back out of the call trace. This is what a weapon switch and the first
 * moment in-game fall back on: both ask about several weapons, and this picks
 * his out of them. */
static uint64_t LastOwnedCall(void) {
    LONG total = g_traceN, i;

    for (i = total - 1; i >= 0 && i >= total - TRACE_SLOTS; i--) {
        uint64_t obj = g_trace[(int)(i & (TRACE_SLOTS - 1))].obj;

        if (OwnedByPlayer(obj)) return obj;
    }
    return 0;
}

/* Age the counts once a second: what was asked in the last few seconds is
 * still visible, and an object nobody asks about drops out on its own. Aging
 * per read collapsed every count to one within a quarter second - the hot list
 * then said nothing at all (logs\scripthook_ammocap.log, 16:00-16:02, every
 * line n=1), which is why the counts are only a diagnostic now. */
static void AgeCalls(void) {
    static DWORD lastAt;
    DWORD now = GetTickCount();
    int i;

    if ((DWORD)(now - lastAt) < 1000) return;
    lastAt = now;
    for (i = 0; i < LOOK_SLOTS; i++)
        if (g_look[i].n > 1) g_look[i].n >>= 1;
}

/* The object the engine has been asking about most, and how many times. */
static uint64_t HottestObject(LONG *count) {
    uint64_t best = 0;
    LONG bestN = 0;
    int i;

    for (i = 0; i < LOOK_SLOTS; i++) {
        if (!g_look[i].obj) continue;
        if (g_look[i].n > bestN) { bestN = g_look[i].n; best = g_look[i].obj; }
    }
    if (count) *count = bestN;
    return best;
}

/* The candidates the decision was made from, with their counts and their
 * rounds. When the number is wrong, this is what says why: which objects were
 * in the running and what each of them carried. */
static void LogHottest(void) {
    uint64_t done[3] = { 0, 0, 0 };
    int k, i, printed = 0;

    Log("ammo: hot objects (calls kept, rounds):");
    for (k = 0; k < 3; k++) {
        uint64_t best = 0;
        LONG bestN = 0;
        uint32_t v;

        for (i = 0; i < LOOK_SLOTS; i++) {
            uint64_t o = g_look[i].obj;
            int d, skip = 0;

            if (!o) continue;
            for (d = 0; d < printed; d++)
                if (done[d] == o) skip = 1;
            if (skip) continue;
            if (g_look[i].n > bestN) { bestN = g_look[i].n; best = o; }
        }
        if (!best) return;
        done[printed++] = best;
        if (RoundsAt(best, &v))
            Log("ammo:   %016llX n=%ld rounds=%u owner=%s",
                (unsigned long long)best, (long)bestN, v,
                OwnedByPlayer(best) ? "player" : "other");
        else
            Log("ammo:   %016llX n=%ld rounds=? owner=%s",
                (unsigned long long)best, (long)bestN,
                OwnedByPlayer(best) ? "player" : "other");
    }
}

/* The weapon in hand, by what moved: firing and reloading both move the
 * rounds of the weapon being used, and nothing moves the rounds of the ones
 * stowed - or of anyone else's weapons, which is why their numbers sat still
 * for whole minutes in the logs of 2026-09-20 17:30-17:31 while the fired one
 * walked 20 -> 12.
 *
 * The value tracked is the one RoundsAt reads: +0x180, or +0x130 for the
 * weapons that keep the count there. Only that one - tracking both was tried
 * and backfired, because a weapon's other offset can carry a value that moves
 * on its own, which made weapons nobody was holding look like the ones being
 * used (logs\scripthook_ammocap.log, 2026-09-21 00:47-00:48, where the number
 * flapped between 20 and 44 with nothing fired).
 *
 * Only the player's own weapons are considered, and among those the newest
 * move wins. The caller falls back to LastOwnedCall() when none of them has
 * moved - which is the case on entering the game and for the moment after a
 * weapon switch. */
static uint64_t HandObject(uint32_t *out) {
    uint64_t best = 0, bestMoved = 0, now = (uint64_t)GetTickCount64();
    uint32_t bestVal = 0;
    int i;

    for (i = 0; i < LOOK_SLOTS; i++) {
        uint64_t obj = g_look[i].obj;
        uint32_t v;

        if (!obj) continue;
        if (g_look[i].stateObj != obj) {           /* the slot was reused */
            g_look[i].stateObj = obj;
            g_look[i].seeded = 0;
            g_look[i].moved = 0;
        }
        if (!RoundsAt(obj, &v)) continue;
        if (!g_look[i].seeded) {
            g_look[i].seeded = 1;                  /* first sight is not a move */
            g_look[i].last = v;
        } else if (v != g_look[i].last) {
            g_look[i].last = v;
            g_look[i].moved = now;
        }
        /* His own weapon, and only one that has actually MOVED.
         *
         * A candidate that has not moved is not eligible - that turns the
         * answer into "whichever object the table happens to hold first", a
         * different weapon every few seconds (2026-09-20) - and one that is
         * not his is not eligible either: other entities' magazines do not
         * move, but the offsets tracked here can carry values that churn on
         * objects that are not magazines at all.
         *
         * Nothing eligible here means the caller keeps the weapon it already
         * had - see ShGetAmmoRounds. */
        if (!g_look[i].moved || !OwnedByPlayer(obj)) continue;
        if (g_look[i].moved > bestMoved) {
            bestMoved = g_look[i].moved;
            bestVal = v;
            best = obj;
        }
    }
    /* Only while it is current. A dry fire - pulling the trigger on an empty
     * weapon - moves nothing, so the memory would otherwise stay on the weapon
     * fired last and never notice that the one now in hand is at zero: the
     * engine asks about that one, the last call is it, and letting the memory
     * go quiet hands the answer back to that call. */
    if (bestMoved && now - bestMoved > 2000) return 0;
    if (out) *out = bestVal;
    return best;
}

SH_API int ShGetAmmoRounds(int *rounds) {
    ShAmmoLook look;
    uint64_t obj;
    uint32_t v;
    int ok = 0, tier = 0;

    if (!rounds) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    LogOnce();
    AgeCalls();

    if (ShGetAmmoLook(&look)) {
        obj = look.args[0];
        if (obj && obj != g_lastObj) {
            g_lastObj = obj;
            Log("ammo: the last call carried %016llX (raw %u)",
                (unsigned long long)obj, (unsigned)look.raw);
        }
    }

    /* Four steps, most specific first: the weapon that just moved, then the
     * weapon that last moved (the one he was holding), then the last weapon
     * the engine asked about that is his own, then the last call.
     *
     * The second step is what makes this stable. Every one of the player's
     * weapons answers the ownership test, so asking the engine which object it
     * asked about last picks between them at random - and it asks about
     * several at every switch (logs\scripthook_ammocap.log, 2026-09-21
     * 00:47-00:48: the number flapped 20 -> 44 -> 20 inside five seconds with
     * nothing fired). What firing proved is kept instead: after a switch the
     * number stays on the weapon that was in hand, until firing says
     * otherwise. */
    obj = HandObject(&v);
    if (obj) {
        tier = 1;
        g_handObj = obj;
        if (obj != g_handLogged) {         /* one line per weapon, not per shot */
            g_handLogged = obj;
            Log("ammo: in hand %016llX, %u rounds", (unsigned long long)obj, v);
        }
    }
    if (tier == 1) {
        *rounds = (int)v;
        ok = 1;
    } else if (g_handObj && RoundsAt(g_handObj, &v)) {
        *rounds = (int)v;
        ok = 1;
        tier = 1;
    } else if ((obj = LastOwnedCall()) != 0 && RoundsAt(obj, &v)) {
        *rounds = (int)v;
        ok = 1;
        tier = 2;
    } else if (RoundsAt(g_lastObj, &v)) {
        *rounds = (int)v;
        ok = 1;
        tier = 3;
    }

    /* The hot list is the evidence for a fallback. While the first tier
     * answers, the objects in the running are not interesting, and printing
     * them every few seconds buried the lines that matter. */
    if (tier != 1 && (DWORD)(GetTickCount() - g_diagAt) >= 5000) {
        g_diagAt = GetTickCount();
        LogHottest();
    }
    if (!ok) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShGetAmmoCalls(ShAmmoCall *out, int max) {
    LONG total, first;
    int i, n = 0;

    if (!out || max < 1) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    LogOnce();
    total = g_traceN;
    if (total > TRACE_SLOTS) first = total - TRACE_SLOTS;
    else first = 0;
    for (i = (int)first; i < (int)total && n < max; i++) {
        int k = (int)(i & (TRACE_SLOTS - 1));

        out[n].obj = g_trace[k].obj;
        out[n].tick = g_trace[k].tick;
        out[n].seq = (uint64_t)i + 1;
        n++;
    }
    if (!n) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    ShSetError(SH_OK);
    return n;
}

SH_API int ShGetAmmoLook(ShAmmoLook *out) {
    int i;

    if (!out) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    /* No hook, no looks. A scale of 1/1 keeps the game alone and installs
     * nothing, so a caller that wants these numbers has to ask for a scale
     * first - which is what the probe does, and what this says when it has
     * not. */
    if (InterlockedCompareExchange(&g_installed, 0, 0) != 2) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    for (i = 0; i < 4; i++) out->args[i] = g_lookArgs[i];
    out->raw = g_lookRaw;
    out->count = (uint32_t)InterlockedCompareExchange(&g_lookCount, 0, 0);
    out->tick = g_lookTick;
    if (!out->count) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    ShSetError(SH_OK);
    return 1;
}
