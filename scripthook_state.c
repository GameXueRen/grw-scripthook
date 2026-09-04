/* GameFlow state, read through a static global.
 * See FINDINGS.md for the derivation.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

/* FUN_146E02C00 returns *(DAT_144B87978 + 0x20), the
 * GR_GameFlow machine. Current state is machine+0x260.
 */
#define GAMEFLOW_HOLDER SH_IMG(0x4B87978)
#define OFF_HOLDER_FLOW 0x020
#define OFF_STATE_OWNER 0x018
#define OFF_CURSTATE    0x260

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

/* The engine puts these objects high, well above the
 * heap window ShReadableAddr allows, so query directly.
 */
static int Readable(uint64_t addr, size_t len) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!Sane(addr)) return 0;
    if (!VirtualQuery((void *)(uintptr_t)addr, &mbi, sizeof(mbi)))
        return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize
           >= addr + len;
}

static uint64_t ReadQ(uint64_t addr) {
    uint64_t v = 0;
    if (!Readable(addr, 8)) return 0;
    memcpy(&v, (void *)(uintptr_t)addr, 8);
    return v;
}

uint64_t ShGetStateMachine(void) {
    uint64_t holder, m;

    if (!Readable(GAMEFLOW_HOLDER, 8)) return 0;
    holder = ReadQ(GAMEFLOW_HOLDER);
    if (!Sane(holder)) return 0;
    if (!Readable(holder + OFF_HOLDER_FLOW, 8)) return 0;
    m = ReadQ(holder + OFF_HOLDER_FLOW);
    if (!Sane(m)) return 0;
    return m;
}

/* The machine and its state must agree, which rejects a
 * half built or reallocated flow.
 */
static uint64_t CurrentState(void) {
    uint64_t m = ShGetStateMachine(), s;

    if (!m || !Readable(m + OFF_CURSTATE, 8)) return 0;
    s = ReadQ(m + OFF_CURSTATE);
    if (!Sane(s) || !Readable(s + OFF_STATE_OWNER, 8)) return 0;
    if (ReadQ(s + OFF_STATE_OWNER) != m) return 0;
    return s;
}

static uint32_t StateHash(uint64_t state) {
    uint64_t vt, fn, desc;
    uint32_t h = 0;

    if (!state) return 0;
    vt = ReadQ(state);
    if (!ShInImage(vt)) return 0;
    if (!Readable(vt + VT_GETDESC, 8)) return 0;
    fn = ReadQ(vt + VT_GETDESC);
    if (!fn) return 0;
    desc = ((GetDesc_t)fn)(state);
    if (!desc || !Readable(desc + OFF_DESC_HASH, 4)) return 0;
    memcpy(&h, (void *)(uintptr_t)(desc + OFF_DESC_HASH), 4);
    return h;
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

SH_API uint32_t ShGetUiState(void) {
    uint32_t bits = 0;
    size_t i;
    for (i = 0; i < sizeof(g_uiBits) / sizeof(g_uiBits[0]); i++)
        if (ShGameSceneActive(g_uiBits[i].scene)) bits |= g_uiBits[i].bit;
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

static int Paused(void) {
    if (ShGetUiState() & SH_UI_PAUSED) return 1;
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
        else if (Paused()) snprintf(buf, len, "Paused%s", sub);
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

/* Everything cached belongs to the old session, and the
 * physics world has to be ready before plugins call.
 */
static void OnStateChanged(uint32_t h) {
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
    static uint32_t prev = 0;
    static volatile LONG busy = 0;

    if (h == prev || !h) return;
    if (InterlockedCompareExchange(&busy, 1, 0) != 0) return;
    prev = h;
    OnStateChanged(h);
    InterlockedExchange(&busy, 0);
}

SH_API int ShGetGameState(void) {
    uint32_t h = StateHash(CurrentState());

    TrackState(h);
    if (h == HASH_PLAYING || h == HASH_INGAME) {
        /* the flow stays Playing through all of these */
        uint32_t ui = ShGetUiState();
        int cam;
        if (ui & SH_UI_LOADING) return SH_STATE_RELOADING;
        if (ui & SH_UI_GAMEOVER) return SH_STATE_GAMEOVER;
        /* Ahead of the pause check: the drone is live play,
         * so it reports as the drone, never as paused. */
        cam = EngineCamera(ui);
        if (cam) return cam;
        if (Paused()) return SH_STATE_PAUSED;
        return SH_STATE_INGAME;
    }
    if (h == HASH_MENU) return SH_STATE_MENU;
    if (h == HASH_LOADING) return SH_STATE_LOADING;
    if (h == HASH_ENDOFGAME) return SH_STATE_MENU;
    return SH_STATE_UNKNOWN;
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
        TrackState(StateHash(CurrentState()));
        if (++tick >= 20) { tick = 0; ShCrashRearm(); }
        Sleep(100);
    }
    return 0;
}

void ShStateStartup(void) {
    CreateThread(NULL, 0, StateWatchThread, NULL, 0, NULL);
}

int ShRequireInGame(void) {
    if (ShIsInGame()) return 1;
    ShSetError(SH_ERR_NOT_IN_GAME);
    return 0;
}
