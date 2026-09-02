/* Field of view, changed from the menu and held every
 * frame by the ScriptHook's camera override.
 */
/* Linked against the ScriptHook, so the API is called
 * directly. See src/README.md for how that works.
 */
#include <windows.h>
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
    if (!ShIsInGame()) return;
    if (!ShGetCamera(&c)) return;
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

    memset(&o, 0, sizeof(o));
    o.apply = SH_CAM_FOV;
    o.fov = g_deg * DEG2RAD;
    return ShCameraApply(&o);
}

static void Report(void) {
    if (g_on)
        ShMenuStatusF(g_menu, "%.0f deg, game default %.0f",
                      g_deg, DefaultRad() * RAD2DEG);
    else
        ShMenuStatusF(g_menu, "off, game is %.0f deg",
                      DefaultRad() * RAD2DEG);
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
            ShMenuStatus(g_menu, "the camera is not ready");
            return;
        }
    } else {
        g_on = 0;
        ShCameraReleaseFields(SH_CAM_FOV);
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
        int in = ShIsInGame();

        LearnDefault();
        if (g_on && in && !wasIn) Push();
        wasIn = in;
        Sleep(TICK_MS);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);

        g_deg = FALLBACK * RAD2DEG;
        g_menu = ShMenuCreate("Field of view");
        ShMenuToggle(g_menu, "Override", 0, OnToggle, NULL);
        ShMenuNumber(g_menu, "Vertical fov", g_deg, DEG_MIN, DEG_MAX,
                     DEG_STEP, OnFov, NULL);
        ShMenuAction(g_menu, "Back to the game default", OnReset,
                     NULL);
        Report();

        CreateThread(NULL, 0, TickThread, NULL, 0, NULL);
    }
    return TRUE;
}
