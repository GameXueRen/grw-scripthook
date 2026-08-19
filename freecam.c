/* Free camera. Toggled from the menu, flown with WASD.
 * The character keeps responding to input, by design.
 */
/* Binds late by choice. Plugins may import the ScriptHook
 * directly instead, since the loader loads them from a
 * thread rather than from DllMain. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "scripthook.h"

#define KEY_UP      'E'
#define KEY_DOWN    'Q'
#define TICK_MS     16
#define LOOK_RATE   1.6f
#define PITCH_LIMIT 1.5f

typedef int (*IsInGame_t)(void);
typedef int (*GetCam_t)(ShCamera *);
typedef int (*Angles_t)(float *, float *);
typedef int (*Free_t)(const ShVec3 *, float, float);
typedef void (*Release_t)(uint32_t);
typedef uint32_t (*MenuCreate_t)(const char *);
typedef int (*MenuToggle_t)(uint32_t, const char *, int,
                            ShMenuFn, void *);
typedef int (*MenuNumber_t)(uint32_t, const char *, float, float,
                            float, float, ShMenuFn, void *);
typedef int (*MenuAction_t)(uint32_t, const char *, ShMenuFn, void *);
typedef int (*MenuStatus_t)(uint32_t, const char *);
typedef void (*MenuOpen_t)(int);
typedef uint32_t (*HudCreate_t)(const char *, int, int);
typedef int (*HudSet_t)(uint32_t, const char *);
typedef int (*HudColour_t)(uint32_t, uint32_t);

static IsInGame_t   g_inGame;
static GetCam_t     g_getCam;
static Angles_t     g_angles;
static Free_t       g_free;
static Release_t    g_release;
static MenuStatus_t g_status;
static MenuOpen_t   g_menuOpen;
static HudSet_t     g_hudSet;
static HudColour_t  g_hudColour;

static uint32_t g_menu = 0;
static uint32_t g_hud = 0;
static volatile int   g_on = 0;
static volatile float g_speed = 12.0f;
static ShVec3 g_pos;
static float  g_yaw = 0.0f, g_pitch = 0.0f;

static int Down(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

/* Seed from wherever the engine's camera is, so detaching
 * never jumps the view.
 */
static int Detach(void) {
    ShCamera c;

    if (!g_getCam(&c)) return 0;
    g_pos = c.pos;
    g_angles(&g_yaw, &g_pitch);
    return 1;
}

static void OnToggle(uint32_t menu, uint32_t item, int value,
                     void *user) {
    (void)item; (void)user;
    if (value) {
        if (!Detach()) {
            g_status(menu, "no camera yet");
            return;
        }
        g_on = 1;
        g_status(menu, "flying, WASD QE arrows");
        /* Closed so the arrow keys fly instead of moving
         * the selection.
         */
        if (g_menuOpen) g_menuOpen(0);
    } else {
        g_on = 0;
        g_release(SH_CAM_POS | SH_CAM_ROT);
        g_status(menu, "camera returned to the game");
    }
}

static void OnSpeed(uint32_t menu, uint32_t item, int value,
                    void *user) {
    (void)menu; (void)item; (void)user;
    g_speed = (float)value;
}

static void OnRecentre(uint32_t menu, uint32_t item, int value,
                       void *user) {
    (void)item; (void)value; (void)user;
    if (Detach()) g_status(menu, "recentred on the game camera");
}

/* Game basis: x right, y forward, z up. Same convention the
 * API rebuilds the pose from.
 */
static void Step(float dt) {
    float cy = (float)cos(g_yaw), sy = (float)sin(g_yaw);
    float cp = (float)cos(g_pitch), sp = (float)sin(g_pitch);
    float fx = sy * cp, fy = cy * cp, fz = sp;
    float rx = cy, ry = -sy;
    float move = g_speed * dt;
    float look = LOOK_RATE * dt;

    if (Down(VK_SHIFT)) move *= 5.0f;
    if (Down(VK_CONTROL)) move *= 0.2f;

    if (Down(VK_LEFT))  g_yaw -= look;
    if (Down(VK_RIGHT)) g_yaw += look;
    if (Down(VK_UP))    g_pitch += look;
    if (Down(VK_DOWN))  g_pitch -= look;
    if (g_pitch > PITCH_LIMIT) g_pitch = PITCH_LIMIT;
    if (g_pitch < -PITCH_LIMIT) g_pitch = -PITCH_LIMIT;

    if (Down('W')) { g_pos.x += fx * move; g_pos.y += fy * move;
                     g_pos.z += fz * move; }
    if (Down('S')) { g_pos.x -= fx * move; g_pos.y -= fy * move;
                     g_pos.z -= fz * move; }
    if (Down('D')) { g_pos.x += rx * move; g_pos.y += ry * move; }
    if (Down('A')) { g_pos.x -= rx * move; g_pos.y -= ry * move; }
    if (Down(KEY_UP)) g_pos.z += move;
    if (Down(KEY_DOWN)) g_pos.z -= move;

    g_free(&g_pos, g_yaw, g_pitch);
}

/* Live telemetry while the menu is closed, which is the one
 * thing a menu cannot show.
 */
static void Telemetry(void) {
    char line[128];

    if (!g_hud) return;
    if (!g_on) { g_hudSet(g_hud, ""); return; }
    snprintf(line, sizeof(line),
             "FREECAM  %.0f %.0f %.0f   %.0f m/s",
             g_pos.x, g_pos.y, g_pos.z, g_speed);
    g_hudColour(g_hud, 0x78EBFF);
    g_hudSet(g_hud, line);
}

static DWORD WINAPI CamThread(LPVOID p) {
    int tick = 0;
    (void)p;

    for (;;) {
        Sleep(TICK_MS);
        if (!g_inGame()) {
            if (g_on) {
                g_on = 0;
                g_release(SH_CAM_POS | SH_CAM_ROT);
            }
            continue;
        }
        if (g_on) Step((float)TICK_MS / 1000.0f);
        if (++tick >= 8) {
            tick = 0;
            Telemetry();
        }
    }
    return 0;
}

static DWORD WINAPI BindThread(LPVOID p) {
    HMODULE m = NULL;
    MenuCreate_t menuCreate;
    MenuToggle_t menuToggle;
    MenuNumber_t menuNumber;
    MenuAction_t menuAction;
    HudCreate_t hudCreate;
    (void)p;

    while (!m) {
        m = GetModuleHandleA("dinput8.dll");
        if (!m) Sleep(500);
    }
    *(FARPROC *)&g_inGame = GetProcAddress(m, "ShIsInGame");
    *(FARPROC *)&g_getCam = GetProcAddress(m, "ShGetCamera");
    *(FARPROC *)&g_angles = GetProcAddress(m, "ShCameraAngles");
    *(FARPROC *)&g_free = GetProcAddress(m, "ShCameraFree");
    *(FARPROC *)&g_release =
        GetProcAddress(m, "ShCameraReleaseFields");
    *(FARPROC *)&menuCreate = GetProcAddress(m, "ShMenuCreate");
    *(FARPROC *)&menuToggle = GetProcAddress(m, "ShMenuToggle");
    *(FARPROC *)&menuNumber = GetProcAddress(m, "ShMenuNumber");
    *(FARPROC *)&menuAction = GetProcAddress(m, "ShMenuAction");
    *(FARPROC *)&g_status = GetProcAddress(m, "ShMenuStatus");
    *(FARPROC *)&g_menuOpen = GetProcAddress(m, "ShMenuOpen");
    *(FARPROC *)&hudCreate = GetProcAddress(m, "ShHudCreate");
    *(FARPROC *)&g_hudSet = GetProcAddress(m, "ShHudSet");
    *(FARPROC *)&g_hudColour = GetProcAddress(m, "ShHudColour");
    if (!g_inGame || !g_getCam || !g_angles || !g_free) return 1;
    if (!g_release || !menuCreate || !menuToggle) return 1;
    if (!menuNumber || !menuAction || !g_status) return 1;
    if (!hudCreate || !g_hudSet || !g_hudColour) return 1;

    g_hud = hudCreate("freecam", SH_HUD_TOPRIGHT, 20);
    g_menu = menuCreate("Free camera");
    menuToggle(g_menu, "Detached", 0, OnToggle, NULL);
    menuNumber(g_menu, "Speed", 12.0f, 1.0f, 120.0f, 4.0f,
               OnSpeed, NULL);
    menuAction(g_menu, "Recentre on game camera", OnRecentre, NULL);

    CreateThread(NULL, 0, CamThread, NULL, 0, NULL);
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
