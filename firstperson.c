/* First person. The camera sits at the player's eye and the
 * head is hidden, so the body and weapon stay drawn.
 */
/* Binds late by choice. Plugins may import the ScriptHook
 * directly instead, since the loader loads them from a
 * thread rather than from DllMain. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "scripthook.h"

/* The mode switches on a keypress, so the walk has to keep
 * up with it. 250 left the camera held far too long. */
#define TICK_MS     60
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

/* How long the aim has to hold before the camera is handed
 * over, so the zoom into the body is never on screen. */
#define SETTLE_DEF  600.0f
#define SETTLE_MIN  0.0f
#define SETTLE_MAX  2000.0f
#define SETTLE_STEP 50.0f

typedef int (*IsInGame_t)(void);
typedef int (*GameState_t)(void);
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
typedef int (*MenuAction_t)(uint32_t, const char *, ShMenuFn, void *);
typedef int (*SceneCount_t)(void);
typedef uint64_t (*SceneAt_t)(int);
typedef uint64_t (*SceneRoot_t)(uint64_t);
typedef int (*ChildCount_t)(uint64_t);
typedef uint64_t (*ChildAt_t)(uint64_t, int);
typedef int (*WidgetClass_t)(uint64_t, char *, int);
typedef int (*WidgetGetS_t)(uint64_t, uint32_t, char *, int);
typedef int (*SceneName_t)(uint64_t, char *, int);
typedef int (*WidgetPropType_t)(uint64_t, uint32_t);
typedef int (*LogPath_t)(const char *, char *, int);

static LogPath_t    g_logPath;

static IsInGame_t   g_inGame;
static GameState_t  g_state;
static GetPlayer_t  g_getPlayer;
static FirstPerson_t g_fp;
static Release_t    g_release;
static HeadNodes_t  g_headNodes;
static SetVisible_t g_setVisible;
static MenuStatus_t g_status;
static SetBlur_t    g_setBlur;
static SceneCount_t  g_sceneCount;
static SceneAt_t     g_sceneAt;
static SceneRoot_t   g_sceneRoot;
static ChildCount_t  g_childCount;
static ChildAt_t     g_childAt;
static WidgetClass_t g_widgetClass;
static WidgetGetS_t  g_widgetGetS;
static SceneName_t   g_sceneName;
static WidgetPropType_t g_widgetPropType;

static uint32_t g_menu = 0;
static volatile int   g_on = 0;
static volatile int   g_wantHide = 1;
static volatile float g_fwd = FWD_DEF;
static volatile float g_up = UP_DEF;
static volatile int   g_settleMs = (int)SETTLE_DEF;

/* Counted so the status line can say where the walk got
 * to, rather than only whether it matched. */
static volatile int g_nScenes, g_nWidgets, g_nLabels, g_nText;
static volatile int g_nSights;

static int Aiming(void);

static uint64_t g_root = 0;
static uint64_t g_hideRoot = 0;
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

/* The hook reapplies the eye every frame until it is given
 * back, so a screen the player opens has to release it. The
 * drone owns its own camera and fights us for it. */
static volatile int g_held = 0;

static void Hold(int want) {
    if (want == g_held) return;
    g_held = want;
    if (want) PushCamera();
    else if (g_release) g_release(SH_CAM_POS);
}

/* Entity wide show: releases every hold on the root and
 * unhides all nodes in one deferred call, applied on the
 * game thread against the live node list. */
static void ShowHead(void) {
    if (g_setVisible && g_hideRoot)
        g_setVisible(g_hideRoot, 0, 1, 0);
    g_nparts = 0;
    g_hideRoot = 0;
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
    g_hideRoot = root;
    return n;
}

static void Report(void) {
    char line[96];

    if (!g_status) return;
    if (!g_on)
        snprintf(line, sizeof(line), "off");
    else if (!g_widgetGetS)
        snprintf(line, sizeof(line),
                 "on, no widget tree: update the ScriptHook");
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
        Hold(1);
        if (g_setBlur) g_setBlur(0);
        if (root && g_wantHide) {
            g_root = root;
            HideHead(root);
        }
    } else {
        g_on = 0;
        ShowHead();
        if (g_setBlur) g_setBlur(1);
        Hold(0);
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
    /* Only while we already own it, or adjusting a slider
     * would take the camera back during a screen. */
    if (g_on && g_held) PushCamera();
}

static void OnUp(uint32_t menu, uint32_t item, int value,
                 void *user) {
    (void)menu; (void)item; (void)user;
    g_up = (float)value;
    if (g_on && g_held) PushCamera();
}

/* 0 hands the camera over the instant iron sights come up,
 * which shows the eye flying into the body. */
static void OnSettle(uint32_t menu, uint32_t item, int value,
                     void *user) {
    (void)menu; (void)item; (void)user;
    g_settleMs = value;
}

/* The weapon prompt names the mode Alt switches TO, not
 * the one you are in. So a label reading OVER THE SHOULDER
 * means iron sights are up right now. */
/* Iron sights are the engine's own aim camera. Holding the
 * eye there rips the scope glass off the weapon and smears
 * the scenery through the temporal upscaler. */
/* The label holds a localisation key, not the drawn text:
 * [AIMMODE_PC_OTS] is what renders as OVER THE SHOULDER.
 * The literal is kept for a build that resolves inline. */
#define WANT_KEY     "AIMMODE_PC_OTS"
#define WANT_LABEL   "OVER THE SHOULDER"
#define WANT_SCENE   "HUD_WeaponItemDisplay"
#define WALK_DEPTH   10
#define WALK_BUDGET  600

static int Contains(const char *hay, const char *needle) {
    int i, j;

    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j]; j++) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (b >= 'a' && b <= 'z') b = (char)(b - 32);
            if (a != b) break;
        }
        if (!needle[j]) return 1;
    }
    return 0;
}

static int LabelSays(uint64_t w, int depth, int *budget) {
    char cls[32], txt[160];
    int n, i;

    if (!w || depth > WALK_DEPTH) return 0;
    if (--*budget < 0) return 0;
    g_nWidgets++;

    if (g_widgetClass(w, cls, sizeof(cls)) && Contains(cls, "Label")) {
        g_nLabels++;
        if (g_widgetGetS(w, SH_P_TEXT, txt, sizeof(txt))) {
            g_nText++;
            if (Contains(txt, WANT_KEY) || Contains(txt, WANT_LABEL))
                return 1;
        }
    }

    n = g_childCount(w);
    for (i = 0; i < n; i++)
        if (LabelSays(g_childAt(w, i), depth + 1, budget)) return 1;
    return 0;
}

/* The weapon prompt stays up after ADS ends, so the aim
 * mode alone would leave the camera released for good.
 * Our own poll: the hook only blocks the GAME's reads. */
static int Aiming(void) {
    return (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
}

/* Walks the game's own UI, so it needs a ScriptHook that
 * exposes the widget tree. Older ones keep the camera. */
static int IronSights(void) {
    int i, n, budget = WALK_BUDGET;

    if (!g_sceneCount || !g_sceneAt || !g_sceneRoot ||
        !g_childCount || !g_childAt || !g_widgetClass ||
        !g_widgetGetS)
        return 0;

    g_nWidgets = 0; g_nLabels = 0; g_nText = 0;
    n = g_sceneCount();
    g_nScenes = n;
    for (i = 0; i < n; i++) {
        uint64_t s = g_sceneAt(i), root;
        char name[64];

        /* Only the weapon display carries the prompt, and
         * walking all thirteen scenes every tick is work
         * for nothing. */
        if (g_sceneName && g_sceneName(s, name, sizeof(name)) &&
            !Contains(name, WANT_SCENE))
            continue;
        root = g_sceneRoot(s);
        if (!root) continue;
        if (LabelSays(root, 0, &budget)) { g_nSights = 1; return 1; }
    }
    g_nSights = 0;
    return 0;
}

/* Every widget of every game scene, written out, because
 * the label we want is on screen and the walk does not
 * see its text. Ground truth beats another guess. */
static FILE *g_dump;

static void DumpWidget(uint64_t w, int depth, int *budget) {
    char cls[48], txt[192], pad[24];
    int n, i, got, type;

    if (!w || depth > 16 || --*budget < 0) return;

    for (i = 0; i < depth && i < 20; i++) pad[i] = ' ';
    pad[i] = 0;

    cls[0] = 0;
    g_widgetClass(w, cls, sizeof(cls));
    type = g_widgetPropType ? g_widgetPropType(w, SH_P_TEXT) : -1;
    got = g_widgetGetS(w, SH_P_TEXT, txt, sizeof(txt));

    fprintf(g_dump, "%s%016llx %-28s texttype %d %s\n", pad,
            (unsigned long long)w, cls[0] ? cls : "?", type,
            got ? txt : "<no text>");

    n = g_childCount(w);
    for (i = 0; i < n; i++)
        DumpWidget(g_childAt(w, i), depth + 1, budget);
}

static void OnDump(uint32_t menu, uint32_t item, int value,
                   void *user) {
    char path[MAX_PATH], name[64];
    int i, n, budget = 20000;
    (void)menu; (void)item; (void)value; (void)user;

    if (!g_sceneCount) return;
    /* All logs live in <gamedir>\logs; fall back to the exe
     * folder on hooks old enough to lack ShLogPath. */
    if (!g_logPath ||
        !g_logPath("firstperson_ui.log", path, sizeof(path))) {
        GetModuleFileNameA(NULL, path, sizeof(path));
        { char *s = strrchr(path, '\\'); if (s) s[1] = 0; }
        strcat(path, "firstperson_ui.log");
    }

    g_dump = fopen(path, "w");
    if (!g_dump) return;
    n = g_sceneCount();
    fprintf(g_dump, "%d scenes drawn last frame\n\n", n);
    for (i = 0; i < n; i++) {
        uint64_t s = g_sceneAt(i), root = g_sceneRoot(s);
        name[0] = 0;
        if (g_sceneName) g_sceneName(s, name, sizeof(name));
        fprintf(g_dump, "scene %d %016llx root %016llx name %s\n",
                i, (unsigned long long)s, (unsigned long long)root,
                name[0] ? name : "?");
        DumpWidget(root, 1, &budget);
        fprintf(g_dump, "\n");
    }
    fclose(g_dump);
    g_dump = NULL;
    if (g_status) g_status(g_menu, "dumped firstperson_ui.log");
}

/* Paused counts as in game, but the player lookup falls
 * back to a heap scan while a menu is up. Nothing here is
 * urgent enough to pay for that, so the tick waits. */
static int Playing(void) {
    if (g_state) return g_state() == SH_STATE_INGAME;
    return g_inGame && g_inGame();
}

/* A new body means the old parts are gone, so the hold is
 * dropped and armed again on the new one.
 */
static DWORD WINAPI TickThread(LPVOID p) {
    int said = 0, settle = 0;
    (void)p;

    for (;;) {
        uint64_t root;

        Sleep(TICK_MS);
        /* Give the camera back on every screen, not just on
         * the toggle, or the drone never gets it. */
        if (!g_on || !Playing()) {
            Hold(0);
            continue;
        }
        /* The engine's aim camera owns iron sights, but
         * only while the player is actually aiming. */
        /* The wait is on the AIM, not on the mode: raising
         * the weapon zooms the eye into the body, while
         * Alt switches mode with no transition at all. */
        /* So an already settled aim hands over the moment
         * the mode changes. Taking it back is immediate. */
        if (Aiming()) {
            if (settle < g_settleMs) settle += TICK_MS;
        } else {
            settle = 0;
        }
        Hold(!(settle >= g_settleMs && IronSights()));
        if (!g_held) continue;

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

/* The plugin's own settings live in
 * scripts/firstperson/firstperson.ini, beside the .asi.
 * They are optional: the built-in defaults stand in. */
static int  IniInt(const char *path, const char *key, int def) {
    return GetPrivateProfileIntA("Settings", key, def, path);
}

static int  IniBool(const char *path, const char *key, int def) {
    char buf[16];
    if (!GetPrivateProfileStringA("Settings", key, "", buf,
                                  sizeof(buf), path))
        return def;
    if (!buf[0]) return def;
    if (!_stricmp(buf, "1") || !_stricmp(buf, "true") ||
        !_stricmp(buf, "yes") || !_stricmp(buf, "on")) return 1;
    if (!_stricmp(buf, "0") || !_stricmp(buf, "false") ||
        !_stricmp(buf, "no") || !_stricmp(buf, "off")) return 0;
    return def;
}

static float IniFloat(const char *path, const char *key, float def) {
    char buf[64];
    if (!GetPrivateProfileStringA("Settings", key, "", buf,
                                  sizeof(buf), path))
        return def;
    if (!buf[0]) return def;
    return (float)atof(buf);
}

static void LoadIni(HMODULE m) {
    typedef int (*PluginIni_t)(const char *, char *, int);
    PluginIni_t pluginIni = NULL;
    char path[MAX_PATH];

    *(FARPROC *)&pluginIni = GetProcAddress(m, "ShPluginIniPath");
    if (!pluginIni || !pluginIni("firstperson", path, sizeof(path)))
        return;
    g_wantHide = IniBool(path, "hide_head", g_wantHide);
    g_fwd      = IniFloat(path, "forward_cm", g_fwd);
    g_up       = IniFloat(path, "height_cm", g_up);
    g_settleMs = IniInt(path, "settle_ms", g_settleMs);
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
    *(FARPROC *)&g_logPath = GetProcAddress(m, "ShLogPath");
    *(FARPROC *)&g_inGame = GetProcAddress(m, "ShIsInGame");
    *(FARPROC *)&g_state = GetProcAddress(m, "ShGetGameState");
    *(FARPROC *)&g_getPlayer = GetProcAddress(m, "ShGetPlayer");
    *(FARPROC *)&g_fp = GetProcAddress(m, "ShCameraFirstPerson");
    *(FARPROC *)&g_release =
        GetProcAddress(m, "ShCameraReleaseFields");
    *(FARPROC *)&g_headNodes = GetProcAddress(m, "ShGetHeadNodes");
    *(FARPROC *)&g_setVisible = GetProcAddress(m, "ShSetVisible");
    /* Optional: an older dinput8 just keeps the blur. */
    *(FARPROC *)&g_setBlur = GetProcAddress(m, "ShSetCameraBlur");
    /* Optional: without the widget tree the camera is held
     * through iron sights, which is the old behaviour. */
    *(FARPROC *)&g_sceneCount = GetProcAddress(m, "ShGameSceneCount");
    *(FARPROC *)&g_sceneAt = GetProcAddress(m, "ShGameSceneAt");
    *(FARPROC *)&g_sceneRoot = GetProcAddress(m, "ShSceneRoot");
    *(FARPROC *)&g_childCount = GetProcAddress(m, "ShWidgetChildCount");
    *(FARPROC *)&g_childAt = GetProcAddress(m, "ShWidgetChildAt");
    *(FARPROC *)&g_widgetClass = GetProcAddress(m, "ShWidgetClass");
    *(FARPROC *)&g_widgetGetS = GetProcAddress(m, "ShWidgetGetS");
    *(FARPROC *)&g_sceneName = GetProcAddress(m, "ShGameSceneName");
    *(FARPROC *)&g_widgetPropType =
        GetProcAddress(m, "ShWidgetPropType");
    *(FARPROC *)&menuCreate = GetProcAddress(m, "ShMenuCreate");
    *(FARPROC *)&menuToggle = GetProcAddress(m, "ShMenuToggle");
    *(FARPROC *)&menuNumber = GetProcAddress(m, "ShMenuNumber");
    *(FARPROC *)&g_status = GetProcAddress(m, "ShMenuStatus");
    if (!g_inGame || !g_getPlayer || !g_fp || !g_release) return 1;
    if (!g_headNodes || !g_setVisible) return 1;
    if (!menuCreate || !menuToggle || !menuNumber || !g_status)
        return 1;

    LoadIni(m);
    g_menu = menuCreate("First person");
    menuToggle(g_menu, "Enabled", 0, OnToggle, NULL);
    menuToggle(g_menu, "Hide head", g_wantHide, OnHide, NULL);
    menuNumber(g_menu, "Forward cm", g_fwd, FWD_MIN, FWD_MAX,
               FWD_STEP, OnForward, NULL);
    menuNumber(g_menu, "Height cm", g_up, UP_MIN, UP_MAX,
               UP_STEP, OnUp, NULL);
    menuNumber(g_menu, "ADS settle ms", (float)g_settleMs,
               SETTLE_MIN, SETTLE_MAX, SETTLE_STEP, OnSettle, NULL);
    {
        MenuAction_t menuAction;
        *(FARPROC *)&menuAction = GetProcAddress(m, "ShMenuAction");
        if (menuAction)
            menuAction(g_menu, "Dump UI to log", OnDump, NULL);
    }
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
