/* GameFlow state, read through a static global.
 * See FINDINGS.md for the derivation.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "scripthook_tick.h"
#include "image.h"
#include "log.h"

/* FUN_146E02C00 returns *(DAT_144B87978 + 0x20), the
 * GR_GameFlow machine. Current state is machine+0x260.
 */
#define GAMEFLOW_HOLDER SH_IMG(0x4B879F8)
#define OFF_HOLDER_FLOW 0x020
#define OFF_STATE_OWNER 0x018
#define OFF_CURSTATE    0x260

/* The active input context: what the player is doing right
 * now, one of the twelve indices in ShInputContextIdx.
 * Read from the InputContextDispatcher singleton, derived in
 * the AnvilNext notes (file 12). The root global 0x4D84E98
 * is shared with the player slot lookup in scripthook_api.c.
 */
#define OFF_CTX_DISPATCHER 0x258
#define OFF_CTX_ACTIVE     0x9C0

/* vt+0x30 returns the class descriptor, hash at +0x24. */
#define VT_GETDESC      0x30
#define OFF_DESC_HASH   0x24

/* Class hashes, confirmed live in game. */
#define HASH_MENU       0xD638A0D9u
#define HASH_PLAYING    0x8816ABC6u
#define HASH_INGAME     0xB5888AF6u
#define HASH_LOADING    0x72270FACu
#define HASH_ENDOFGAME  0x90B56ECBu

extern int ShReadableAddr(uint64_t addr, size_t len);
extern uint64_t ShReadQ(uint64_t addr);
extern void ShSetError(int err);
extern void ShReflectNoteFlow(uint64_t machine);
extern int ShInPauseMenu(void);
extern void ShPhysicsOnEnterPlaying(void);
extern void ShCameraOnEnterPlaying(void);
extern void ShHeadOnEnterPlaying(void);
extern void ShMenuOnEnterPlaying(void);
extern void ShSpawnInvalidate(void);
extern void ShSpawnOnEnterPlaying(void);
extern void ShUiOnEnterPlaying(void);
extern void ShInvalidate(void);
extern void ShInvalidateHealth(void);

typedef uint64_t (__attribute__((ms_abi)) *GetDesc_t)(uint64_t);

static int Sane(uint64_t p) {
    return p >= 0x10000ULL && p < 0x800000000000ULL;
}

/* Reads that tolerate a bad address, without asking the kernel first.
 *
 * This file used to put VirtualQuery in front of every read, one per hop of a
 * pointer chain. Measured 2026-10-27: VirtualQuery costs about 8us in this
 * process, the state read made seven of them, and that was 70us of the 25us
 * a call the framework had left (and 190-280us before the descriptor cache).
 * What the query was buying - "do not fault on a freed or unmapped page" - is
 * what __try gives for nothing when no fault happens, and the callers check
 * the pointers they get back anyway (Sane, the owner field, ShInImage).
 *
 * A faulting read here is not an error path worth optimising: it happens on
 * the frame a world is torn down and answers 0, which is what the old code
 * did too. */
static uint64_t ReadQ(uint64_t addr) {
    uint64_t v = 0;

    if (!addr) return 0;
    __try {
        memcpy(&v, (void *)(uintptr_t)addr, 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = 0;
    }
    return v;
}

/* Same, 4 bytes, and tells the caller whether it got them: the descriptor's
 * hash is the one read where "0" is also a legal value. */
static int ReadD(uint64_t addr, uint32_t *out) {
    uint32_t v = 0;
    int ok = 1;

    if (!addr) return 0;
    __try {
        memcpy(&v, (void *)(uintptr_t)addr, 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }
    if (ok && out) *out = v;
    return ok;
}

/* Set once, when the flow machine has been found and reported. Interlocked
 * because ShGetStateMachine is called from the state watch thread and from
 * every plugin thread that asks for the game state. */
static volatile LONG g_flowNoted;

uint64_t ShGetStateMachine(void) {
    uint64_t holder, m;

    /* GAMEFLOW_HOLDER is a fixed address in the game image, mapped for the
     * life of the process; the two hops after it are checked by Sane. */
    holder = ReadQ(GAMEFLOW_HOLDER);
    if (!Sane(holder)) return 0;
    m = ReadQ(holder + OFF_HOLDER_FLOW);
    if (!Sane(m)) return 0;

    /* The machine's method table at +8 is pinned in reflect.c, behind a
     * check that only runs if something asks for the flow - and nothing
     * that ships does. So the machine reports itself: once, the first time
     * there is one to read, the value it holds and whether it is still the
     * pinned one. This runs from the state watch thread, so it costs one
     * interlocked exchange per call and one log line per session. */
    if (InterlockedCompareExchange(&g_flowNoted, 1, 0) == 0)
        ShReflectNoteFlow(m);
    return m;
}

/* The machine and its state must agree, which rejects a
 * half built or reallocated flow.
 */
static uint64_t CurrentState(void) {
    uint64_t m = ShGetStateMachine(), s;

    if (!m) return 0;
    s = ReadQ(m + OFF_CURSTATE);
    if (!Sane(s)) return 0;
    if (ReadQ(s + OFF_STATE_OWNER) != m) return 0;
    return s;
}

/* ---- the descriptor cache -----------------------------------------------
 *
 * GetDesc is a virtual call into the game, and it is not cheap: measured
 * 2026-10-27, with every other cost in this file already cut down, it was
 * 190-280us per call while the state entry is asked about 90 times a second -
 * 17-25 ms a second, the largest single cost the framework had left, and the
 * one that showed up in every plugin's ShGetPlayer and ShIsInGame too.
 *
 * What it returns is a descriptor object belonging to the *state object*, and
 * the state object only changes when the flow moves. So the pair (state
 * object, descriptor) is remembered. The hash itself is NOT cached: it is read
 * out of the descriptor on every call, so anything the game changes inside
 * that descriptor is still seen on the next call. What is saved is the call
 * that hands the descriptor over.
 *
 * Three guards, because a stale descriptor means a wrong game state, and a
 * wrong state moves the world under a plugin's feet:
 *   - the pair is published under a sequence number, so no reader can see a
 *     half-written (state, desc) pair;
 *   - the descriptor must still be readable before it is used, so one the game
 *     freed falls back to a fresh resolve instead of being read anyway;
 *   - one call in 256 resolves again and compares. If the same state object
 *     ever answers a different descriptor, that is logged, taken, and the
 *     assumption is disproved in the field rather than in theory.
 */
static struct {
    uint64_t state;
    uint64_t desc;
    LONG     seq;
} g_desc;
static volatile LONG g_descTick;
static volatile LONG g_descDrift;
/* One writer at a time. Two threads that both missed would otherwise
 * interleave the two stores and publish a pair that belongs to neither -
 * state A with descriptor B - which reads as a perfectly valid answer and is
 * exactly the failure this cache must not introduce. Held only on a miss or
 * on the one-in-256 check, never on the hit path. */
static volatile LONG g_descBusy;

/* The call itself, unchanged - the only part that costs. */
static uint64_t DescResolve(uint64_t state) {
    uint64_t vt, fn;

    vt = ReadQ(state);
    if (!ShInImage(vt)) return 0;
    fn = ReadQ(vt + VT_GETDESC);
    if (!fn) return 0;
    return ((GetDesc_t)fn)(state);
}

static uint32_t StateHash(uint64_t state) {
    uint64_t desc;
    uint32_t h = 0;
    int seq0, seq1, verify, ok = 0;

    if (!state) return 0;

    verify = ((InterlockedIncrement(&g_descTick) & 0xFF) == 0);

    seq0 = (int)InterlockedCompareExchange(&g_desc.seq, 0, 0);
    desc = (!(seq0 & 1) && g_desc.state == state) ? g_desc.desc : 0;
    seq1 = (int)InterlockedCompareExchange(&g_desc.seq, 0, 0);

    /* The hit: no kernel call, no game call - read the hash out of the
     * descriptor and go. `verify` skips the shortcut on one call in 256 so
     * the assumption behind the cache is re-tested rather than trusted. */
    if (desc && seq0 == seq1 && !verify)
        ok = ReadD(desc + OFF_DESC_HASH, &h);

    if (!ok) {
        int spins = 0, got = 0;

        while (InterlockedCompareExchange(&g_descBusy, 1, 0) != 0) {
            YieldProcessor();
            if (++spins > 20000) break;
        }
        got = spins <= 20000;

        /* Somebody may have filled exactly this one while we waited - but not
         * on a verify pass, which has to do the work to have answered. */
        if (got && !verify && !(g_desc.seq & 1) && g_desc.state == state &&
            g_desc.desc && ReadD(g_desc.desc + OFF_DESC_HASH, &h)) {
            desc = g_desc.desc;
            ok = 1;
        } else {
            uint64_t fresh = DescResolve(state);

            if (verify && desc && fresh && fresh != desc && !g_descDrift &&
                InterlockedExchange(&g_descDrift, 1) == 0)
                LogFirst("state-desc-drift",
                         "state: the same state object answered a different "
                         "descriptor (%llx -> %llx); the cache follows it - "
                         "the assumption behind caching it is approximate "
                         "rather than exact",
                         (unsigned long long)desc, (unsigned long long)fresh);
            if (got) {
                InterlockedIncrement(&g_desc.seq);      /* odd: updating    */
                g_desc.state = state;
                g_desc.desc  = fresh;
                InterlockedIncrement(&g_desc.seq);      /* even: published  */
            }
            desc = fresh;
            ok = desc && ReadD(desc + OFF_DESC_HASH, &h);
        }
        if (got) InterlockedExchange(&g_descBusy, 0);
    }

    return ok ? h : 0;
}

SH_API uint32_t ShGetGameStateHash(void) {
    return StateHash(CurrentState());
}

/* UI state: the named scenes the game drew last frame */
extern int ShGameSceneActive(const char *name);

static const struct { const char *scene; uint32_t bit; } g_uiBits[] = {
    { "HUD_Drone",          SH_UI_DRONE },
    { "HUD_Binocular",      SH_UI_BINOCULAR },
    { "HUD_Vehicle",        SH_UI_VEHICLE },
    { "Menu_TabbedPage",    SH_UI_PAUSE },
    { "MENU_LoadoutV2",     SH_UI_LOADOUT },
    { "MENU_Map_Cursor",    SH_UI_MAP },
    { "MENU_Skills",        SH_UI_SKILLS },
    { "HUD_ComWheel",       SH_UI_COMWHEEL },
    { "MENU_GameOver",      SH_UI_GAMEOVER },
    { "MENU_LoadingScreen", SH_UI_LOADING },
    { "HUD_Cinematic",      SH_UI_CINEMATIC },
    { "Menu_Popup",         SH_UI_POPUP },
};

/* One pass over the scenes the game drew, not one pass per bit.
 *
 * The old shape asked ShGameSceneActive() fourteen times, and each of those
 * walked the whole active list and compared the name of every scene in it:
 * 14 x 14 lookups, each a linear scan of the 64-slot name cache. Measured on
 * 2026-10-27 that was the bulk of ShGetGameState's 60-100us, which is the
 * entry point plugins poll 222 times a second.
 *
 * Now the fourteen names are hashed once into a fixed table and the scenes
 * are walked once: 14 hash lookups instead of 196 string comparisons. The
 * table is built on first use; a caller that arrives while it is being built
 * falls back to the old loop rather than reading a half-filled table, because
 * a missing bit here is read as "that screen is not up" and a wrong answer
 * moves the game state under a plugin's feet. */
extern const char *ShSceneNameRef(uint64_t scene);

static uint32_t UiNameHash(const char *s) {
    uint32_t h = 2166136261u;

    while (s && *s) { h ^= (uint32_t)(unsigned char)*s++; h *= 16777619u; }
    return h;
}

#define UI_HASH 64        /* 14 names, open addressing, never more than 1/4 full */
static struct { uint32_t hash; uint32_t bit; } g_uiTab[UI_HASH];
static volatile LONG g_uiBuilt;   /* 0 = no, 1 = built, -1 = being built */

static void UiTabBuild(void) {
    size_t i;

    for (i = 0; i < sizeof(g_uiBits) / sizeof(g_uiBits[0]); i++) {
        uint32_t h = UiNameHash(g_uiBits[i].scene);
        uint32_t j = h & (UI_HASH - 1);

        while (g_uiTab[j].hash && g_uiTab[j].hash != h) j = (j + 1) & (UI_HASH - 1);
        g_uiTab[j].hash = h;
        g_uiTab[j].bit  = g_uiBits[i].bit;
    }
}

static int UiTabReady(void) {
    LONG s = InterlockedCompareExchange(&g_uiBuilt, 0, 0);

    if (s == 1) return 1;
    if (s == 0 && InterlockedCompareExchange(&g_uiBuilt, -1, 0) == 0) {
        UiTabBuild();
        InterlockedExchange(&g_uiBuilt, 1);
        return 1;
    }
    return 0;
}

/* The old shape, kept for the once-in-a-session window while the table is
 * being built on another thread. Correct, just slower. */
static uint32_t UiStateByAsking(void) {
    uint32_t bits = 0;
    size_t i;

    for (i = 0; i < sizeof(g_uiBits) / sizeof(g_uiBits[0]); i++)
        if (ShGameSceneActive(g_uiBits[i].scene)) bits |= g_uiBits[i].bit;
    return bits;
}

SH_API uint32_t ShGetUiState(void) {
    uint32_t bits = 0;
    int n, i;

    if (!UiTabReady()) return UiStateByAsking();

    n = ShGameSceneCount();
    for (i = 0; i < n; i++) {
        uint64_t sc = ShGameSceneAt(i);
        const char *nm;
        uint32_t h, j;

        if (!sc) continue;
        nm = ShSceneNameRef(sc);
        if (!nm || !nm[0]) continue;
        h = UiNameHash(nm);
        j = h & (UI_HASH - 1);
        while (g_uiTab[j].hash) {
            if (g_uiTab[j].hash == h) { bits |= g_uiTab[j].bit; break; }
            j = (j + 1) & (UI_HASH - 1);
        }
    }
    return bits;
}

SH_API int ShInDrone(void)     { return (ShGetUiState() & SH_UI_DRONE) != 0; }
SH_API int ShInLoadout(void)   { return (ShGetUiState() & SH_UI_LOADOUT) != 0; }
SH_API int ShInMap(void)       { return (ShGetUiState() & SH_UI_MAP) != 0; }
SH_API int ShInBinocular(void) { return (ShGetUiState() & SH_UI_BINOCULAR) != 0; }

/* The screens that stop play: the pause tabs and the pages
 * reached from them. SH_UI_VEHICLE is the vehicle HUD, so
 * it is normal play and deliberately absent. */
#define SH_UI_PAUSED   (SH_UI_PAUSE | SH_UI_LOADOUT | SH_UI_MAP | \
                        SH_UI_SKILLS | SH_UI_POPUP)

static int Paused(uint32_t ui) {
    if (ui & SH_UI_PAUSED) return 1;
    return ShInPauseMenu();
}

/* Live play where the engine owns the camera. The game is
 * still running, so none of these is a pause. */
static int EngineCamera(uint32_t ui) {
    if (ui & SH_UI_DRONE) return SH_STATE_DRONE;
    if (ui & SH_UI_BINOCULAR) return SH_STATE_BINOCULAR;
    if (ui & SH_UI_CINEMATIC) return SH_STATE_CINEMATIC;
    return 0;
}

/* Is the session in live play right now: in the world, with no screen up
 * that keeps the player out of it.
 *
 * Deliberately NOT ShGetGameState: that one tracks the state, and tracking
 * can run OnStateChanged, which calls back into the player scanner. This is
 * asked from inside that scanner's own lock, where anything recursive is a
 * deadlock, so it is read only - same inputs, no side effects, no lock. */
int ShInLivePlay(void) {
    uint32_t h = StateHash(CurrentState());
    uint32_t ui;

    if (h != HASH_PLAYING && h != HASH_INGAME) return 0;
    ui = ShGetUiState();
    if (ui & SH_UI_LOADING) return 0;
    if (ui & SH_UI_GAMEOVER) return 0;
    /* Drone, binoculars and cinematics are the engine holding the camera
     * of a player who is still there, so they count as play. */
    return Paused(ui) ? 0 : 1;
}

SH_API int ShGetGameStateName(char *buf, int len) {
    uint64_t s;
    uint32_t h;

    if (!buf || len < 2) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    s = CurrentState();
    if (!s) {
        snprintf(buf, len, "no machine");
        ShSetError(SH_ERR_NOT_IN_GAME);
        return 0;
    }
    h = StateHash(s);
    if (h == HASH_PLAYING) {
        uint32_t ui = ShGetUiState();
        const char *sub = "";
        if (ui & SH_UI_DRONE) sub = " (drone)";
        else if (ui & SH_UI_BINOCULAR) sub = " (binocular)";
        else if (ui & SH_UI_LOADOUT) sub = " (loadout)";
        else if (ui & SH_UI_MAP) sub = " (map)";
        else if (ui & SH_UI_SKILLS) sub = " (skills)";
        else if (ui & SH_UI_VEHICLE) sub = " (vehicle)";
        if (ui & SH_UI_LOADING) snprintf(buf, len, "Reloading");
        else if (ui & SH_UI_GAMEOVER) snprintf(buf, len, "GameOver");
        else if (ui & SH_UI_DRONE) snprintf(buf, len, "Drone");
        else if (ui & SH_UI_BINOCULAR) snprintf(buf, len, "Binocular");
        else if (ui & SH_UI_CINEMATIC) snprintf(buf, len, "Cinematic");
        else if (Paused(ui)) snprintf(buf, len, "Paused%s", sub);
        else snprintf(buf, len, "Playing%s", sub);
    }
    else if (h == HASH_MENU) snprintf(buf, len, "MenuOrLobby");
    else if (h == HASH_INGAME) snprintf(buf, len, "InGame");
    else if (h == HASH_LOADING) snprintf(buf, len, "GameLoading");
    else if (h == HASH_ENDOFGAME) snprintf(buf, len, "EndOfGame");
    else snprintf(buf, len, "class_%08x", h);
    ShSetError(SH_OK);
    return 1;
}

/* The root global below is the local player's controller,
 * shared with the slot index lookup. dispatcher + 0x9C0 is
 * the int32 active context. Wine maps the dispatcher high,
 * so the reads are guarded by __try rather than by a query:
 * see the note on ReadQ above. */
#define SH_INPUT_ROOT SH_IMG(0x4D84F18)

SH_API int ShInputContext(void) {
    uint64_t root, disp;
    uint32_t u32 = 0;
    int32_t idx = -1;

    root = ReadQ(SH_INPUT_ROOT);
    if (!Sane(root)) return -1;
    disp = ReadQ(root + OFF_CTX_DISPATCHER);
    if (!Sane(disp)) return -1;
    if (!ReadD(disp + OFF_CTX_ACTIVE, &u32)) return -1;
    idx = (int32_t)u32;
    if (idx < SH_CTX_EMPTY || idx > SH_CTX_POPUP) return -1;
    return (int)idx;
}

/* Everything cached belongs to the old session, and the
 * physics world has to be ready before plugins call.
 */
/* One read of the state machine per frame, however many threads ask.
 *
 * Measured before this: 66 calls a second from cnchat's 15ms poll, 62 from
 * freecam's 16ms, 30 each from chaos's two loops, 20 from the Ghost Mode
 * pair - every one of them paying StateHash (which calls a virtual function
 * inside the game), TrackState, and twelve scene-name lookups, and every one
 * from a different thread. The answer cannot change between two asks inside
 * one frame, so one 64-bit word carries (frame << 32 | state) and the first
 * asker of the frame does the work. Published with one interlocked exchange:
 * no lock, no torn read, and OnStateChanged clears it so a transition is
 * visible to the very next caller rather than on the next frame.
 *
 * A frame counter that is not moving (no render hook, or a build where the
 * overlay never presents) would freeze the cache - that cannot happen here
 * because the state watch thread runs TrackState every 100ms and clears this
 * on every hash change, which is also the only way the value can go stale. */
static volatile LONG64 g_stateCache;

static void StateCacheClear(void) {
    InterlockedExchange64(&g_stateCache, 0);
}

static void OnStateChanged(uint32_t h) {
    StateCacheClear();
    ShInvalidate();
    ShInvalidateHealth();
    ShUiOnEnterPlaying();
    if (h == HASH_PLAYING || h == HASH_INGAME) {
        ShPhysicsOnEnterPlaying();
        ShCameraOnEnterPlaying();
        ShHeadOnEnterPlaying();
        ShMenuOnEnterPlaying();
        /* The vehicle specs live in the engine heap: a world
         * reload can move them, so drop the cache and rescan. */
        ShSpawnInvalidate();
        ShSpawnOnEnterPlaying();
        /* A new body starts with no aim group yet, so drop the
         * head group cache from the old session. */
        ShHeadInvalidate();
    }
}

static void TrackState(uint32_t h) {
    static volatile LONG prev = 0;
    static volatile LONG busy = 0;

    if (!h) return;
    if ((uint32_t)InterlockedCompareExchange(&prev, 0, 0) == h) return;
    if (InterlockedCompareExchange(&busy, 1, 0) != 0) return;
    /* Published with an interlocked store: the state hash is read and
     * written from the watch thread and from every ShGetGameState caller. */
    InterlockedExchange(&prev, (LONG)h);
    OnStateChanged(h);
    InterlockedExchange(&busy, 0);
}

/* The work: one hash, one tracking call, one UI-state read. Called at most
 * once per frame now - see the cache above. */
static int ComputeGameState(void) {
    uint64_t th = ShApiStatsOn ? ShTickNow() : 0;
    uint32_t h = StateHash(CurrentState());
    int r;

    if (th) ShApiNote(SH_API_STATE_HASH, th);
    TrackState(h);
    if (h == HASH_PLAYING || h == HASH_INGAME) {
        /* the flow stays Playing through all of these; one
         * ShGetUiState per call - it is 12 scene lookups and the
         * chat thread polls here every 15ms */
        uint64_t tu = ShApiStatsOn ? ShTickNow() : 0;
        uint32_t ui = ShGetUiState();
        int cam;

        if (tu) ShApiNote(SH_API_STATE_UI, tu);
        if (ui & SH_UI_LOADING) r = SH_STATE_RELOADING;
        else if (ui & SH_UI_GAMEOVER) r = SH_STATE_GAMEOVER;
        /* Ahead of the pause check: the drone is live play,
         * so it reports as the drone, never as paused. */
        else if ((cam = EngineCamera(ui)) != 0) r = cam;
        else if (Paused(ui)) r = SH_STATE_PAUSED;
        else r = SH_STATE_INGAME;
    } else if (h == HASH_MENU) {
        r = SH_STATE_MENU;
    } else if (h == HASH_LOADING) {
        r = SH_STATE_LOADING;
    } else if (h == HASH_ENDOFGAME) {
        r = SH_STATE_MENU;
    } else {
        r = SH_STATE_UNKNOWN;
    }
    return r;
}

extern uint32_t ShSceneFrame(void);

/* One exit, so the API counter sees every path: this is the entry point the
 * plugins poll the most (66 calls a second, measured) and a counter that
 * only counted the common branch would be worse than none. */
SH_API int ShGetGameState(void) {
    uint64_t t0 = ShApiStatsOn ? ShTickNow() : 0;
    uint32_t f = ShSceneFrame();
    LONG64 c = InterlockedCompareExchange64(&g_stateCache, 0, 0);
    int r;

    if (c && (uint32_t)(c >> 32) == f) {
        r = (int)(uint32_t)c;
        if (t0) ShApiNote(SH_API_STATE_HIT, t0);
    } else {
        r = ComputeGameState();
        if (f) InterlockedExchange64(&g_stateCache,
                                     ((LONG64)f << 32) | (uint32_t)r);
    }
    if (t0) ShApiNote(SH_API_STATE, t0);
    return r;
}

/* Paused still counts: the world is loaded and every read
 * keeps working, so callers must not tear down.
 */
SH_API int ShIsInGame(void) {
    int s = ShGetGameState();
    return s == SH_STATE_INGAME || s == SH_STATE_PAUSED ||
           s == SH_STATE_DRONE || s == SH_STATE_BINOCULAR ||
           s == SH_STATE_CINEMATIC;
}

/* dinput8 watches the state itself. Plugins never have
 * to poll for the API to stay correct.
 */
extern void ShCrashRearm(void);

static DWORD WINAPI StateWatchThread(LPVOID p) {
    int tick = 0;

    (void)p;
    for (;;) {
        ShTickPing(SH_TICK_STATE);
        TrackState(StateHash(CurrentState()));
        if (++tick >= 20) { tick = 0; ShCrashRearm(); }
        Sleep(100);
    }
    return 0;
}

void ShStateStartup(void) {
    HANDLE h = CreateThread(NULL, 0, StateWatchThread, NULL, 0, NULL);

    /* Nobody waits on it, so the thread object is released at once rather
     * than held for the session. */
    if (h) CloseHandle(h);
}

int ShRequireInGame(void) {
    if (ShIsInGame()) return 1;
    ShSetError(SH_ERR_NOT_IN_GAME);
    return 0;
}
