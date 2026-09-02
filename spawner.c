/* Vehicle spawner. A submenu of the shared root menu, so
 * navigation, scrolling and drawing belong to the API.
 */
/* Binds late by choice. Plugins may import the ScriptHook
 * directly instead, since the loader loads them from a
 * thread rather than from DllMain. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "scripthook.h"

#define AHEAD  6.0f
#define LIFT   1.0f

typedef struct { uint32_t id; const char *name; } Vehicle;

typedef int (*Count_t)(void);
typedef const Vehicle *(*At_t)(int);
typedef uint64_t (*Spawn_t)(uint32_t, const ShVec3 *);
typedef int (*PlayerPos_t)(ShVec3 *);
typedef uint32_t (*MenuCreate_t)(const char *);
typedef int (*MenuAction_t)(uint32_t, const char *, ShMenuFn, void *);
typedef int (*MenuStatus_t)(uint32_t, const char *);
typedef int (*MenuHint_t)(uint32_t, const char *);
typedef int (*IsInGame_t)(void);
typedef void (*Warm_t)(void);

static Count_t      g_count;
static At_t         g_at;
static Spawn_t      g_spawn;
static PlayerPos_t  g_playerPos;
static MenuStatus_t g_status;
static MenuHint_t   g_hint;
static IsInGame_t   g_isInGame;
static Warm_t       g_warm;

static uint32_t g_menu = 0;
static volatile int g_spawned = 0;

/* Resolve the whole vehicle spec cache once on a background
 * thread, after the world is loaded, so the first spawn does
 * not have to scan the address space synchronously. */
static DWORD WINAPI WarmThread(LPVOID p) {
    (void)p;
    for (;;) {
        if (g_isInGame && g_isInGame()) break;
        Sleep(1000);
    }
    Sleep(2000);
    if (g_warm) g_warm();
    return 0;
}

/* Runs on the menu thread with the API's lock dropped, so a
 * plain API call here is exactly what the ABI expects.
 */
static void OnSpawn(uint32_t menu, uint32_t item, int value,
                    void *user) {
    const Vehicle *v = (const Vehicle *)user;
    ShVec3 pos;
    uint64_t ent;
    char line[96];

    (void)item; (void)value;
    if (!v) return;
    if (!g_playerPos(&pos)) {
        g_status(menu, "no player position");
        return;
    }
    pos.x += AHEAD;
    pos.z += LIFT;

    g_status(menu, "spawning...");
    ent = g_spawn(v->id, &pos);
    if (ent) {
        g_spawned++;
        snprintf(line, sizeof(line), "spawned, %d this session",
                 g_spawned);
    } else {
        snprintf(line, sizeof(line), "nothing appeared");
    }
    g_status(menu, line);
}

static DWORD WINAPI BindThread(LPVOID p) {
    HMODULE m = NULL;
    MenuCreate_t menuCreate;
    MenuAction_t menuAction;
    int n, i;
    (void)p;

    while (!m) {
        m = GetModuleHandleA("dinput8.dll");
        if (!m) Sleep(500);
    }
    *(FARPROC *)&g_count = GetProcAddress(m, "ShVehicleCount");
    *(FARPROC *)&g_at = GetProcAddress(m, "ShVehicleAt");
    *(FARPROC *)&g_spawn = GetProcAddress(m, "ShSpawnVehicle");
    *(FARPROC *)&g_playerPos = GetProcAddress(m,
                                              "ShGetPlayerPosition");
    *(FARPROC *)&menuCreate = GetProcAddress(m, "ShMenuCreate");
    *(FARPROC *)&menuAction = GetProcAddress(m, "ShMenuAction");
    *(FARPROC *)&g_status = GetProcAddress(m, "ShMenuStatus");
    *(FARPROC *)&g_hint = GetProcAddress(m, "ShMenuHint");
    *(FARPROC *)&g_isInGame = GetProcAddress(m, "ShIsInGame");
    *(FARPROC *)&g_warm = GetProcAddress(m, "ShSpawnWarm");
    if (!g_count || !g_at || !g_spawn || !g_playerPos) return 1;
    if (!menuCreate || !menuAction || !g_status || !g_hint) return 1;

    /* The catalogue is static, so every vehicle becomes a
     * row once and the API scrolls them.
     */
    g_menu = menuCreate("Vehicles");
    g_hint(g_menu, "Note: the first summon may take a moment.");
    n = g_count();
    for (i = 0; i < n; i++) {
        const Vehicle *v = g_at(i);
        if (v) menuAction(g_menu, v->name, OnSpawn, (void *)v);
    }

    /* Warm the spec cache in the background once the world is
     * up; the first spawn then queues instead of scanning. */
    if (g_isInGame && g_warm)
        CreateThread(NULL, 0, WarmThread, NULL, 0, NULL);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        CreateThread(NULL, 0, BindThread, NULL, 0, NULL);
    }
    return TRUE;
}
