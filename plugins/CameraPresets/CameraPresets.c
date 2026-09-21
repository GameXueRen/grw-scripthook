/* Third person camera presets: pick a chase distance and height, or
 * build a custom one with a sideways, over-the-shoulder offset.
 *
 * The offsets are the Wildlands Immersion Suite's own preset table -
 * Close, Medium, Far, and a fully custom row - driven through the
 * framework's ShCameraOrbitAdvanced, whose sideways axis is what an
 * over-the-shoulder camera is. Every change goes through the
 * framework's own camera calls, so this installs no hook of its own.
 *
 * The camera is a position owner like any other: taking a preset
 * releases a first person claim, and Default hands the camera back to
 * the engine entirely.
 *
 * The default blacklist applies (Ghost War and Mercenaries): while the
 * mode is not allowed the camera is handed back - a chosen shoulder
 * view in a PvP mode is an advantage this has no business taking.
 *
 * Text: kEn / kZh here, overridable by plugins\CameraPresets\lang.ini.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scripthook.h"
#include "log.h"

/* The Suite's own table, names and values: Close shoulder, Tactical
 * third person, Wide cinematic, Custom, SOCOM. In metres. */
typedef struct { const char *label; float back, up; } Preset;
static const Preset kPresets[] = {
    { "@cp.default", 0.0f,  0.0f  },   /* hands the camera back */
    { "@cp.close",   1.8f,  1.45f },
    { "@cp.tactical",3.5f,  1.75f },
    { "@cp.wide",    7.0f,  2.6f  },
    { "@cp.custom",  0.0f,  0.0f  },   /* the three rows below */
    { "@cp.socom",   3.2f,  2.2f  }
};
#define PRESET_N   ((int)(sizeof(kPresets) / sizeof(kPresets[0])))
#define PRESET_CUSTOM (PRESET_N - 1)
static const char *kPresetPtr[PRESET_N];

/* ---- state ----------------------------------------------------------- */

static char     g_ini[MAX_PATH];
static char     g_name[64];
static uint32_t g_menu;

/* The menu thread writes these; the worker thread reads them. */
static volatile LONG g_preset;             /* 0 = Default (engine camera) */
static volatile LONG g_backCm  = 400;      /* custom rows, centimetres */
static volatile LONG g_upCm    = 170;
static volatile LONG g_sideCm  = 0;

/* The worker thread alone writes these. */
static int g_claimed;                      /* a preset is holding the camera */
static int g_guarded;                      /* handed back for a blocked mode */

/* ---- settings -------------------------------------------------------- */

static void SaveInt(const char *key, LONG value) {
    char text[24];

    snprintf(text, sizeof(text), "%ld", (long)value);
    if (!WritePrivateProfileStringA("Settings", key, text, g_ini))
        Log("cp: could not write %s=%s to %s", key, text, g_ini);
}

static void LoadSettings(void) {
    LONG preset, back, up, side;

    preset = (LONG)GetPrivateProfileIntA("Settings", "preset", 0, g_ini);
    back   = (LONG)GetPrivateProfileIntA("Settings", "backCm", 400, g_ini);
    up     = (LONG)GetPrivateProfileIntA("Settings", "upCm", 170, g_ini);
    side   = (LONG)GetPrivateProfileIntA("Settings", "sideCm", 0, g_ini);
    if (preset < 0 || preset >= PRESET_N) preset = 0;
    if (back < 50) back = 50;
    if (back > 1500) back = 1500;
    if (up < -200) up = -200;
    if (up > 800) up = 800;
    if (side < -500) side = -500;
    if (side > 500) side = 500;

    InterlockedExchange(&g_preset, preset);
    InterlockedExchange(&g_backCm, back);
    InterlockedExchange(&g_upCm, up);
    InterlockedExchange(&g_sideCm, side);
}

/* ---- the camera ------------------------------------------------------ */

static void ApplyPreset(int force) {
    int preset = (int)g_preset;
    float back, up, side = 0.0f;

    if (preset == 0) {
        if (g_claimed || force) {
            ShCameraReleaseFields(SH_CAM_POS);
            g_claimed = 0;
            Log("cp: camera handed back to the engine");
        }
        return;
    }
    if (preset == PRESET_CUSTOM) {
        back = (float)g_backCm / 100.0f;
        up   = (float)g_upCm / 100.0f;
        side = (float)g_sideCm / 100.0f;
    } else {
        back = kPresets[preset].back;
        up   = kPresets[preset].up;
    }
    if (!ShCameraOrbitAdvanced(back, side, up)) {
        Log("cp: preset %d refused (%08x, %s)", preset,
            ShLastError(), ShErrorString(ShLastError()));
        return;
    }
    g_claimed = 1;
}

/* The claim is re-asserted every couple of seconds - a world change can
 * reset the camera - and handed back the moment the mode stops allowing
 * this plugin. */
static void ApplyWatch(void) {
    static DWORD last;

    if ((long)(GetTickCount() - last) < 2000) return;
    last = GetTickCount();

    if (ShPluginAllowed()) {
        if (g_guarded) {
            g_guarded = 0;
            ApplyPreset(1);
            Log("cp: took the camera back (mode allowed again)");
        } else {
            ApplyPreset(0);
        }
    } else if (!g_guarded && g_claimed) {
        ShCameraReleaseFields(SH_CAM_POS);
        g_claimed = 0;
        g_guarded = 1;
        Log("cp: handed the camera back to the game (mode not allowed)");
    }
}

/* ---- the page -------------------------------------------------------- */

enum { ROW_PRESET = 1, ROW_BACK, ROW_UP, ROW_SIDE };

static void OnRow(uint32_t menu, uint32_t item, int value, void *user) {
    int which = (int)(intptr_t)user;

    (void)menu; (void)item;

    switch (which) {
    case ROW_PRESET:
        if (value < 0 || value >= PRESET_N) value = 0;
        InterlockedExchange(&g_preset, value);
        SaveInt("preset", value);
        Log("cp: preset %d", value);
        ApplyPreset(1);
        break;
    case ROW_BACK:
        if (value < 50) value = 50;
        if (value > 1500) value = 1500;
        InterlockedExchange(&g_backCm, value);
        SaveInt("backCm", value);
        if (g_preset == PRESET_CUSTOM) ApplyPreset(1);
        break;
    case ROW_UP:
        if (value < -200) value = -200;
        if (value > 800) value = 800;
        InterlockedExchange(&g_upCm, value);
        SaveInt("upCm", value);
        if (g_preset == PRESET_CUSTOM) ApplyPreset(1);
        break;
    case ROW_SIDE:
        if (value < -500) value = -500;
        if (value > 500) value = 500;
        InterlockedExchange(&g_sideCm, value);
        SaveInt("sideCm", value);
        if (g_preset == PRESET_CUSTOM) ApplyPreset(1);
        break;
    default:
        break;
    }
}

static void BuildMenu(void) {
    ShMenuHint(g_menu, "@cp.hint");
    ShMenuList(g_menu, "@cp.preset", kPresetPtr, PRESET_N, (int)g_preset,
               OnRow, (void *)(intptr_t)ROW_PRESET);
    ShMenuNumber(g_menu, "@cp.back", (float)g_backCm, 50.0f, 1500.0f,
                 10.0f, OnRow, (void *)(intptr_t)ROW_BACK);
    ShMenuNumber(g_menu, "@cp.up", (float)g_upCm, -200.0f, 800.0f,
                 10.0f, OnRow, (void *)(intptr_t)ROW_UP);
    ShMenuNumber(g_menu, "@cp.side", (float)g_sideCm, -500.0f, 500.0f,
                 10.0f, OnRow, (void *)(intptr_t)ROW_SIDE);
    ShMenuStatus(g_menu, "@cp.st.engine");
}

static void RefreshStatus(void) {
    int preset = (int)g_preset;

    if (!ShMenuIsShowing(g_menu)) return;
    if (preset == 0) { ShMenuStatus(g_menu, "@cp.st.engine"); return; }
    if (preset == PRESET_CUSTOM) {
        ShMenuStatusF(g_menu, "@cp.st.custom",
                      (int)g_backCm, (int)g_upCm, (int)g_sideCm);
    } else {
        ShMenuStatus(g_menu, kPresets[preset].label);
    }
}

/* ---- the plugin's own name ------------------------------------------ */

static void NameFromModule(HINSTANCE inst) {
    char mod[MAX_PATH];
    char *base, *dot;
    size_t n;

    g_name[0] = 0;
    if (!inst || !GetModuleFileNameA(inst, mod, sizeof(mod))) return;
    base = strrchr(mod, '\\');
    base = base ? base + 1 : mod;
    dot = strrchr(base, '.');
    n = dot ? (size_t)(dot - base) : strlen(base);
    if (n == 0 || n >= sizeof(g_name)) { g_name[0] = 0; return; }
    memcpy(g_name, base, n);
    g_name[n] = 0;
}

/* ---- text ------------------------------------------------------------ */

static const ShText kEn[] = {
    { "@cp.page",      "Third person camera" },
    { "@cp.hint",      "Presets for the chase camera, or a custom one with a shoulder offset" },
    { "@cp.preset",    "Camera preset" },
    { "@cp.default",   "Default (the game's own)" },
    { "@cp.close",     "Close shoulder" },
    { "@cp.tactical",  "Tactical third person" },
    { "@cp.wide",      "Wide cinematic" },
    { "@cp.custom",    "Custom" },
    { "@cp.socom",     "SOCOM" },
    { "@cp.back",      "Custom distance (cm)" },
    { "@cp.up",        "Custom height (cm)" },
    { "@cp.side",      "Custom shoulder offset (cm)" },
    { "@cp.st.engine", "the engine's own camera is in force" },
    { "@cp.st.custom", "custom: %d cm back  %d cm up  %d cm aside" }
};

static const ShText kZh[] = {
    { "@cp.page",      "第三人称相机" },
    { "@cp.hint",      "预设追尾相机，或自定义带肩部偏移的相机" },
    { "@cp.preset",    "相机预设" },
    { "@cp.default",   "默认（游戏自带）" },
    { "@cp.close",     "近距肩视" },
    { "@cp.tactical",  "战术第三人称" },
    { "@cp.wide",      "宽景电影" },
    { "@cp.custom",    "自定义" },
    { "@cp.socom",     "SOCOM" },
    { "@cp.back",      "自定义距离（厘米）" },
    { "@cp.up",        "自定义高度（厘米）" },
    { "@cp.side",      "自定义肩部偏移（厘米）" },
    { "@cp.st.engine", "当前使用游戏自带相机" },
    { "@cp.st.custom", "自定义： 后 %d 厘米  高 %d 厘米  侧 %d 厘米" }
};

/* ---- start up -------------------------------------------------------- */

static DWORD WINAPI PluginThread(LPVOID param) {
    char logFile[80];
    int i;

    NameFromModule((HINSTANCE)param);
    if (!g_name[0]) strcpy(g_name, "CameraPresets");

    snprintf(logFile, sizeof(logFile), "%s.log", g_name);
    LogInitAlways(logFile);

    while (!GetModuleHandleA("dinput8.dll")) Sleep(500);

    if (!ShPluginIniPath(g_name, g_ini, sizeof(g_ini)))
        g_ini[0] = 0;
    LoadSettings();

    for (i = 0; i < PRESET_N; i++) kPresetPtr[i] = kPresets[i].label;

    ShLangDeclare(g_name, "en-US", kEn, (int)(sizeof(kEn) / sizeof(kEn[0])));
    ShLangDeclare(g_name, "zh-CN", kZh, (int)(sizeof(kZh) / sizeof(kZh[0])));

    g_menu = ShMenuCreate("@cp.page");
    if (!g_menu) {
        Log("cp: no menu page (%08x) - nothing to drive", ShLastError());
        return 0;
    }
    BuildMenu();

    Log("cp: page up, preset %d, custom %d/%d/%d cm",
        (int)g_preset, (int)g_backCm, (int)g_upCm, (int)g_sideCm);

    ApplyPreset(0);

    for (;;) {
        Sleep(250);
        RefreshStatus();
        ApplyWatch();
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    HANDLE h;

    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    DisableThreadLibraryCalls(inst);
    h = CreateThread(NULL, 0, PluginThread, (LPVOID)inst, 0, NULL);
    if (h) CloseHandle(h);
    return TRUE;
}
