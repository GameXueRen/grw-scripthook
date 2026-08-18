/* Field of view, changed from the menu and held every
 * frame by the ScriptHook's camera override.
 */
/* Plugins load from inside dinput8's DllMain, so a static
 * import on it deadlocks the loader. Bind late.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "scripthook.h"

#define DEG2RAD     0.01745329252f
#define RAD2DEG     57.2957795f

/* The engine's own vertical fov is about 0.815 rad, which
 * is the fallback if we never manage to read it.
 */
#define FALLBACK    0.815f
#define DEG_MIN     30.0f
#define DEG_MAX     140.0f
#define DEG_STEP    2.0f
#define TICK_MS     500

typedef int (*IsInGame_t)(void);
typedef int (*GetCam_t)(ShCamera *);
typedef int (*Apply_t)(const ShCameraOverride *);
typedef void (*Release_t)(uint32_t);
typedef uint32_t (*MenuCreate_t)(const char *);
typedef int (*MenuToggle_t)(uint32_t, const char *, int,
                            ShMenuFn, void *);
typedef int (*MenuNumber_t)(uint32_t, const char *, float, float,
                            float, float, ShMenuFn, void *);
typedef int (*MenuAction_t)(uint32_t, const char *, ShMenuFn, void *);
typedef int (*MenuStatus_t)(uint32_t, const char *);

static IsInGame_t   g_inGame;
static GetCam_t     g_getCam;
static Apply_t      g_apply;
static Release_t    g_release;
static MenuStatus_t g_status;

static uint32_t g_menu = 0;
static volatile int   g_on = 0;
static volatile float g_deg = 0.0f;
static volatile float g_defaultRad = 0.0f;

/* Captured while the override is off, so it is the game's
 * value rather than one of ours read back.
 */
static void LearnDefault(void) {
    ShCamera c;

    if (g_on || g_defaultRad > 0.0f) return;
    if (!g_inGame || !g_inGame()) return;
    if (!g_getCam || !g_getCam(&c)) return;
    if (c.fov > 0.05f && c.fov < 3.0f) {
        g_defaultRad = c.fov;
        if (g_deg <= 0.0f) g_deg = c.fov * RAD2DEG;
    }
}

static float DefaultRad(void) {
    return (g_defaultRad > 0.0f) ? g_defaultRad : FALLBACK;
}

/* One call is enough: the ScriptHook reapplies it inside
 * the engine's own frame until it is released.
 */
static int Push(void) {
    ShCameraOverride o;

    if (!g_apply) return 0;
    memset(&o, 0, sizeof(o));
    o.apply = SH_CAM_FOV;
    o.fov = g_deg * DEG2RAD;
    return g_apply(&o);
}

static void Report(void) {
    char line[96];

    if (!g_status) return;
    if (g_on)
        snprintf(line, sizeof(line), "%.0f deg, game default %.0f",
                 g_deg, DefaultRad() * RAD2DEG);
    else
        snprintf(line, sizeof(line), "off, game is %.0f deg",
                 DefaultRad() * RAD2DEG);
    g_status(g_menu, line);
}

static void OnToggle(uint32_t menu, uint32_t item, int value,
                     void *user) {
    (void)menu; (void)item; (void)user;

    if (value) {
        LearnDefault();
        if (g_deg <= 0.0f) g_deg = DefaultRad() * RAD2DEG;
        g_on = 1;
        if (!Push()) {
            g_on = 0;
            if (g_status) g_status(g_menu, "the camera is not ready");
            return;
        }
    } else {
        g_on = 0;
        if (g_release) g_release(SH_CAM_FOV);
    }
    Report();
}

static void OnFov(uint32_t menu, uint32_t item, int value,
                  void *user) {
    (void)menu; (void)item; (void)user;
    g_deg = (float)value;
    if (g_on) Push();
    Report();
}

static void OnReset(uint32_t menu, uint32_t item, int value,
                    void *user) {
    (void)menu; (void)item; (void)value; (void)user;
    g_deg = DefaultRad() * RAD2DEG;
    if (g_on) Push();
    Report();
}

/* Entering a session reinstalls the camera hook, so the
 * override is pushed again to survive the transition.
 */
static DWORD WINAPI TickThread(LPVOID p) {
    int wasIn = 0;
    (void)p;

    for (;;) {
        int in = (g_inGame && g_inGame());

        LearnDefault();
        if (g_on && in && !wasIn) Push();
        wasIn = in;
        Sleep(TICK_MS);
    }
    return 0;
}

static DWORD WINAPI BindThread(LPVOID p) {
    HMODULE m = NULL;
    MenuCreate_t menuCreate;
    MenuToggle_t menuToggle;
    MenuNumber_t menuNumber;
    MenuAction_t menuAction;
    (void)p;

    while (!m) {
        m = GetModuleHandleA("dinput8.dll");
        if (!m) Sleep(500);
    }
    *(FARPROC *)&g_inGame = GetProcAddress(m, "ShIsInGame");
    *(FARPROC *)&g_getCam = GetProcAddress(m, "ShGetCamera");
    *(FARPROC *)&g_apply = GetProcAddress(m, "ShCameraApply");
    *(FARPROC *)&g_release =
        GetProcAddress(m, "ShCameraReleaseFields");
    *(FARPROC *)&menuCreate = GetProcAddress(m, "ShMenuCreate");
    *(FARPROC *)&menuToggle = GetProcAddress(m, "ShMenuToggle");
    *(FARPROC *)&menuNumber = GetProcAddress(m, "ShMenuNumber");
    *(FARPROC *)&menuAction = GetProcAddress(m, "ShMenuAction");
    *(FARPROC *)&g_status = GetProcAddress(m, "ShMenuStatus");
    if (!g_inGame || !g_getCam || !g_apply || !g_release) return 1;
    if (!menuCreate || !menuToggle || !menuNumber) return 1;
    if (!menuAction || !g_status) return 1;

    g_deg = FALLBACK * RAD2DEG;
    g_menu = menuCreate("Field of view");
    menuToggle(g_menu, "Override", 0, OnToggle, NULL);
    menuNumber(g_menu, "Vertical fov", g_deg, DEG_MIN, DEG_MAX,
               DEG_STEP, OnFov, NULL);
    menuAction(g_menu, "Back to the game default", OnReset, NULL);
    Report();

    CreateThread(NULL, 0, TickThread, NULL, 0, NULL);
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
