/* First person. The camera sits at the player's eye and the
 * head is hidden, so the body and weapon stay drawn.
 */
/* Binds late by choice. Plugins may import the ScriptHook
 * directly instead, since the loader loads them from a
 * thread rather than from DllMain. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "scripthook.h"

#define TICK_MS     250
#define MAX_PARTS   64

/* The player position is already at head height, so forward
 * is the only offset needed to clear the face.
 */
#define FWD_DEF     15.0f
#define FWD_MIN     0.0f
#define FWD_MAX     60.0f
#define FWD_STEP    5.0f
#define UP_DEF      0.0f
#define UP_MIN      -30.0f
#define UP_MAX      30.0f
#define UP_STEP     5.0f

typedef int (*IsInGame_t)(void);
typedef int (*GetPlayer_t)(ShPlayer *);
typedef int (*FirstPerson_t)(float, float);
typedef void (*Release_t)(uint32_t);
typedef int (*SetBlur_t)(int);
typedef int (*HeadNodes_t)(uint64_t, uint64_t *, int);
typedef int (*SetVisible_t)(uint64_t, uint64_t, int, int);
typedef uint32_t (*MenuCreate_t)(const char *);
typedef int (*MenuToggle_t)(uint32_t, const char *, int,
                            ShMenuFn, void *);
typedef int (*MenuNumber_t)(uint32_t, const char *, float, float,
                            float, float, ShMenuFn, void *);
typedef int (*MenuStatus_t)(uint32_t, const char *);

static IsInGame_t   g_inGame;
static GetPlayer_t  g_getPlayer;
static FirstPerson_t g_fp;
static Release_t    g_release;
static HeadNodes_t  g_headNodes;
static SetVisible_t g_setVisible;
static MenuStatus_t g_status;
static SetBlur_t    g_setBlur;

static uint32_t g_menu = 0;
static volatile int   g_on = 0;
static volatile int   g_wantHide = 1;
static volatile float g_fwd = FWD_DEF;
static volatile float g_up = UP_DEF;

static uint64_t g_root = 0;
static uint64_t g_parts[MAX_PARTS];
static int      g_nparts = 0;

static uint64_t PlayerRoot(void) {
    ShPlayer p;

    memset(&p, 0, sizeof(p));
    if (!g_getPlayer || !g_getPlayer(&p)) return 0;
    return p.root ? p.root : p.entity;
}

/* The eye follows the head bone inside the engine's own
 * frame, so nothing here runs per frame.
 */
static void PushCamera(void) {
    if (g_fp) g_fp(g_fwd / 100.0f, g_up / 100.0f);
}

static void ShowHead(void) {
    int i;

    for (i = 0; i < g_nparts; i++)
        if (g_setVisible) g_setVisible(g_root, g_parts[i], 1, 0);
    g_nparts = 0;
}

/* The aim group that names the head parts appears the first
 * time the player aims, so this keeps trying until it does.
 */
static int HideHead(uint64_t root) {
    int n, i;

    if (!g_headNodes || !g_setVisible) return 0;
    n = g_headNodes(root, g_parts, MAX_PARTS);
    if (n <= 0) return 0;

    for (i = 0; i < n; i++)
        g_setVisible(root, g_parts[i], 0, 1);
    g_nparts = n;
    return n;
}

static void Report(void) {
    char line[96];

    if (!g_status) return;
    if (!g_on)
        snprintf(line, sizeof(line), "off");
    else if (!g_wantHide)
        snprintf(line, sizeof(line), "on, head left visible");
    else if (g_nparts > 0)
        snprintf(line, sizeof(line), "on, head hidden (%d parts)",
                 g_nparts);
    else
        snprintf(line, sizeof(line), "on, aim once to hide the head");
    g_status(g_menu, line);
}

static void OnToggle(uint32_t menu, uint32_t item, int value,
                     void *user) {
    (void)menu; (void)item; (void)user;

    if (value) {
        uint64_t root = PlayerRoot();

        g_on = 1;
        PushCamera();
        if (g_setBlur) g_setBlur(0);
        if (root && g_wantHide) {
            g_root = root;
            HideHead(root);
        }
    } else {
        g_on = 0;
        ShowHead();
        if (g_setBlur) g_setBlur(1);
        if (g_release) g_release(SH_CAM_POS);
    }
    Report();
}

static void OnHide(uint32_t menu, uint32_t item, int value,
                   void *user) {
    (void)menu; (void)item; (void)user;
    g_wantHide = value;
    if (value) {
        uint64_t root = PlayerRoot();
        if (g_on && root) HideHead(root);
    } else {
        ShowHead();
    }
    Report();
}

static void OnForward(uint32_t menu, uint32_t item, int value,
                      void *user) {
    (void)menu; (void)item; (void)user;
    g_fwd = (float)value;
    if (g_on) PushCamera();
}

static void OnUp(uint32_t menu, uint32_t item, int value,
                 void *user) {
    (void)menu; (void)item; (void)user;
    g_up = (float)value;
    if (g_on) PushCamera();
}

/* A new body means the old parts are gone, so the hold is
 * dropped and armed again on the new one.
 */
static DWORD WINAPI TickThread(LPVOID p) {
    int said = 0;
    (void)p;

    for (;;) {
        uint64_t root;

        Sleep(TICK_MS);
        if (!g_on || !g_inGame || !g_inGame()) continue;

        root = PlayerRoot();
        if (!root) continue;
        if (root != g_root) {
            g_root = root;
            g_nparts = 0;
            said = 0;
            PushCamera();
        }
        if (g_wantHide && g_nparts == 0 && HideHead(root)) {
            Report();
            said = 0;
        } else if (!said) {
            Report();
            said = 1;
        }
    }
    return 0;
}

static DWORD WINAPI BindThread(LPVOID p) {
    HMODULE m = NULL;
    MenuCreate_t menuCreate;
    MenuToggle_t menuToggle;
    MenuNumber_t menuNumber;
    (void)p;

    while (!m) {
        m = GetModuleHandleA("dinput8.dll");
        if (!m) Sleep(500);
    }
    *(FARPROC *)&g_inGame = GetProcAddress(m, "ShIsInGame");
    *(FARPROC *)&g_getPlayer = GetProcAddress(m, "ShGetPlayer");
    *(FARPROC *)&g_fp = GetProcAddress(m, "ShCameraFirstPerson");
    *(FARPROC *)&g_release =
        GetProcAddress(m, "ShCameraReleaseFields");
    *(FARPROC *)&g_headNodes = GetProcAddress(m, "ShGetHeadNodes");
    *(FARPROC *)&g_setVisible = GetProcAddress(m, "ShSetVisible");
    /* Optional: an older dinput8 just keeps the blur. */
    *(FARPROC *)&g_setBlur = GetProcAddress(m, "ShSetCameraBlur");
    *(FARPROC *)&menuCreate = GetProcAddress(m, "ShMenuCreate");
    *(FARPROC *)&menuToggle = GetProcAddress(m, "ShMenuToggle");
    *(FARPROC *)&menuNumber = GetProcAddress(m, "ShMenuNumber");
    *(FARPROC *)&g_status = GetProcAddress(m, "ShMenuStatus");
    if (!g_inGame || !g_getPlayer || !g_fp || !g_release) return 1;
    if (!g_headNodes || !g_setVisible) return 1;
    if (!menuCreate || !menuToggle || !menuNumber || !g_status)
        return 1;

    g_menu = menuCreate("First person");
    menuToggle(g_menu, "Enabled", 0, OnToggle, NULL);
    menuToggle(g_menu, "Hide head", 1, OnHide, NULL);
    menuNumber(g_menu, "Forward cm", FWD_DEF, FWD_MIN, FWD_MAX,
               FWD_STEP, OnForward, NULL);
    menuNumber(g_menu, "Height cm", UP_DEF, UP_MIN, UP_MAX,
               UP_STEP, OnUp, NULL);
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
