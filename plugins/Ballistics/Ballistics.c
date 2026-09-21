/* Ballistics control: global projectile velocity scaling.
 *
 * Ported from the Wildlands Immersion Suite's Ballistics Control. Every
 * change goes through the framework's own engine calls -
 * ShSetProjectileVelocityMultiplier for the number, ShBallisticsHookInstall
 * for the trajectory patch - so this installs no hook of its own, patches
 * no code and writes no engine memory.
 *
 * The halves:
 *
 *   1. THE PATCH. The framework scales the muzzle velocity the engine
 *      stores into each new round (and the tracer's own speed ceiling) at
 *      TrailFX creation, so one patch covers both the visible tracer and
 *      the authoritative round. It is asked for once, the first time the
 *      scaling is switched on.
 *
 *   2. THE NUMBER. 10% to 300%, 100% being the game's own. Off writes
 *      1.00x back, which leaves the patch in place doing nothing - the
 *      game's own numbers are what the engine computes.
 *
 * The default blacklist applies (Ghost War and Mercenaries): while the
 * mode is not allowed the scale is handed back to the game - a faster
 * round in a PvP mode is an advantage this has no business taking.
 *
 * Text: kEn / kZh here, overridable by plugins\Ballistics\lang.ini.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scripthook.h"
#include "log.h"

/* ---- state ----------------------------------------------------------- */

static char     g_ini[MAX_PATH];
static char     g_name[64];
static uint32_t g_menu;

/* The menu thread writes these; the worker thread reads them. The
 * built-in start is the game's own numbers: nothing is scaled until
 * the switch is flipped (and then written to the ini). */
static volatile LONG g_enabled;          /* 1 = scale new shots */
static volatile LONG g_percent = 100;    /* 10..300, 100 = vanilla */
static volatile LONG g_accuracy;         /* 1 = zero spread (player only) */
static volatile LONG g_dropPercent = 100;/* 0..500, 100 = vanilla drop */

/* The worker thread alone writes these; the status line reads them. */
static int  g_hooked;                    /* the trajectory patch is in */
static int  g_applied = -1;              /* the percent actually written */
static int  g_guarded;                   /* handed back for a blocked mode */

/* ---- settings -------------------------------------------------------- */

static void SaveInt(const char *key, LONG value) {
    char text[24];

    snprintf(text, sizeof(text), "%ld", (long)value);
    if (!WritePrivateProfileStringA("Settings", key, text, g_ini))
        Log("bt: could not write %s=%s to %s", key, text, g_ini);
}

static void LoadSettings(void) {
    LONG enabled, percent, drop;

    enabled = (LONG)GetPrivateProfileIntA("Settings", "enabled", 0, g_ini);
    percent = (LONG)GetPrivateProfileIntA("Settings", "percent", 100, g_ini);
    g_accuracy = (LONG)GetPrivateProfileIntA("Settings", "accuracy", 0, g_ini) ? 1 : 0;
    drop = (LONG)GetPrivateProfileIntA("Settings", "drop", 100, g_ini);
    if (drop < 0) drop = 0;
    if (drop > 500) drop = 500;
    if (percent < 10) percent = 10;
    if (percent > 300) percent = 300;

    InterlockedExchange(&g_enabled, enabled ? 1 : 0);
    InterlockedExchange(&g_percent, percent);
}

/* ---- the scaling ----------------------------------------------------- */

/* Write the wanted multiplier. force writes even when it already did -
 * the call after the patch goes in, and the call that takes the number
 * back after a blocked mode. */
static void ApplyDrop(void) {
    ShSetProjectileDropMultiplier((float)g_dropPercent / 100.0f);
}

static void Apply(int force) {
    int percent = g_enabled ? (int)g_percent : 100;

    if (!force && percent == g_applied) return;
    if (!ShSetProjectileVelocityMultiplier(percent / 100.0f)) {
        Log("bt: %d%% refused (%08x, %s)", percent,
            ShLastError(), ShErrorString(ShLastError()));
        return;
    }
    g_applied = percent;
}

/* The patch is what makes the number mean anything, so switching the
 * scaling on asks for it first. */
static void EnsureHook(void) {
    if (g_hooked) return;
    if (!ShBallisticsHookInstall()) {
        Log("bt: could not install the trajectory patch (%08x, %s)",
            ShLastError(), ShErrorString(ShLastError()));
        return;
    }
    g_hooked = 1;
    Log("bt: trajectory patch installed");
    Apply(1);
}

/* Super accuracy (player-only zero spread) rides the same page: the
 * hooks install once, the Active flag pauses the effect without
 * uninstalling, which is what the mode blacklist needs. */
static int g_accInstalled;

static void ApplyAccuracy(void) {
    if (g_accuracy && !g_accInstalled) {
        if (!ShSetSuperAccuracy(1)) {
            g_accuracy = 0;
            Log("bt: super accuracy refused (%08x, %s)",
                ShLastError(), ShErrorString(ShLastError()));
            ShMenuSetValue(g_menu, "@bt.accuracy", 0);
            return;
        }
        g_accInstalled = 1;
        Log("bt: super accuracy installed");
    }
    if (g_accInstalled) ShSetSuperAccuracyActive(1);
}

/* The wanted scale is re-asserted every couple of seconds, and handed
 * back the moment the mode stops allowing this plugin: the framework
 * blacklists by mode, and the answer can change under a long session. */
static void ApplyWatch(void) {
    static DWORD last;

    if ((long)(GetTickCount() - last) < 2000) return;
    last = GetTickCount();

    if (ShPluginAllowed()) {
        if (g_guarded) {
            g_guarded = 0;
            Apply(1);
            ApplyDrop();
            if (g_accInstalled) ShSetSuperAccuracyActive(1);
            Log("bt: took the round speed back (mode allowed again)");
        } else {
            Apply(0);
        }
    } else if (!g_guarded && (g_hooked || g_accInstalled)) {
        if (g_hooked) ShSetProjectileVelocityMultiplier(1.0f);
        if (g_accInstalled) ShSetSuperAccuracyActive(0);
        ShSetProjectileDropMultiplier(1.0f);
        g_guarded = 1;
        g_applied = 100;
        Log("bt: handed the round speed and the spread back to the game (mode not allowed)");
    }
}

/* ---- the page -------------------------------------------------------- */

enum { ROW_ENABLE = 1, ROW_PERCENT, ROW_DROP, ROW_ACCURACY, ROW_RESET };

static void OnRow(uint32_t menu, uint32_t item, int value, void *user) {
    int which = (int)(intptr_t)user;

    (void)menu; (void)item;

    switch (which) {
    case ROW_ENABLE:
        InterlockedExchange(&g_enabled, value ? 1 : 0);
        SaveInt("enabled", value ? 1 : 0);
        Log("bt: velocity scaling %s", value ? "on" : "off");
        if (value) EnsureHook();
        Apply(1);
        break;
    case ROW_PERCENT:
        if (value < 10) value = 10;
        if (value > 300) value = 300;
        InterlockedExchange(&g_percent, value);
        SaveInt("percent", value);
        Log("bt: velocity %d%%", value);
        if (g_enabled) EnsureHook();
        Apply(1);
        break;
    case ROW_DROP:
        if (value < 0) value = 0;
        if (value > 500) value = 500;
        InterlockedExchange(&g_dropPercent, value);
        SaveInt("drop", value);
        Log("bt: drop %d%%", value);
        ApplyDrop();
        break;
    case ROW_ACCURACY:
        InterlockedExchange(&g_accuracy, value ? 1 : 0);
        SaveInt("accuracy", value ? 1 : 0);
        Log("bt: super accuracy %s", value ? "on" : "off");
        if (value) ApplyAccuracy();
        else if (g_accInstalled) ShSetSuperAccuracyActive(0);
        break;
    case ROW_RESET:
        InterlockedExchange(&g_enabled, 0);
        InterlockedExchange(&g_percent, 100);
        InterlockedExchange(&g_accuracy, 0);
        InterlockedExchange(&g_dropPercent, 100);
        SaveInt("enabled", 0);
        SaveInt("percent", 100);
        SaveInt("accuracy", 0);
        SaveInt("drop", 100);
        ShMenuSetValue(g_menu, "@bt.enable", 0);
        ShMenuSetValue(g_menu, "@bt.percent", 100);
        ShMenuSetValue(g_menu, "@bt.accuracy", 0);
        ShMenuSetValue(g_menu, "@bt.drop", 100);
        ApplyDrop();
        if (g_accInstalled) ShSetSuperAccuracyActive(0);
        Log("bt: reset to vanilla");
        Apply(1);
        break;
    default:
        break;
    }
}

static void BuildMenu(void) {
    ShMenuHint(g_menu, "@bt.hint");
    ShMenuToggle(g_menu, "@bt.enable", (int)g_enabled, OnRow,
                 (void *)(intptr_t)ROW_ENABLE);
    ShMenuNumber(g_menu, "@bt.percent", (float)g_percent, 10.0f, 300.0f,
                 10.0f, OnRow, (void *)(intptr_t)ROW_PERCENT);
    ShMenuNumber(g_menu, "@bt.drop", (float)g_dropPercent, 0.0f, 500.0f,
                 10.0f, OnRow, (void *)(intptr_t)ROW_DROP);
    ShMenuToggle(g_menu, "@bt.accuracy", (int)g_accuracy, OnRow,
                 (void *)(intptr_t)ROW_ACCURACY);
    ShMenuAction(g_menu, "@bt.reset", OnRow, (void *)(intptr_t)ROW_RESET);
    ShMenuStatus(g_menu, "@bt.st.off");
}

static void RefreshStatus(void) {
    if (!ShMenuIsShowing(g_menu)) return;
    if (!g_hooked) { ShMenuStatus(g_menu, "@bt.st.off"); return; }
    ShMenuStatusF(g_menu, "@bt.status",
                  (int)ShGetProjectileTrailHookCount(),
                  (int)(ShGetProjectileVelocityMultiplier() * 100.0f + 0.5f),
                  g_accuracy && g_accInstalled,
                  (int)g_dropPercent);
}

/* ---- the plugin's own name ------------------------------------------ */

/* From the module path, like every other plugin here: the ini, the log
 * and the text owner all follow the file name. */
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
    { "@bt.page",    "Ballistics control" },
    { "@bt.hint",    "Faster or slower bullets for every weapon" },
    { "@bt.enable",  "Enable global bullet velocity" },
    { "@bt.percent", "Global bullet velocity %" },
    { "@bt.reset",   "Reset ballistics to vanilla" },
    { "@bt.accuracy", "Super accuracy (player only)" },
    { "@bt.drop",     "Bullet drop % (player only)" },
    { "@bt.status",  "tracers: %d  velocity: %d%%  accuracy: %d  drop: %d%%" },
    { "@bt.st.off",  "velocity scaling off - the game's own numbers are in force" }
};

static const ShText kZh[] = {
    { "@bt.page",    "弹道控制" },
    { "@bt.hint",    "让所有武器的子弹更快或更慢" },
    { "@bt.enable",  "启用全局子弹速度" },
    { "@bt.percent", "全局子弹速度 %" },
    { "@bt.reset",   "恢复原始弹道" },
    { "@bt.accuracy", "超级精度（仅玩家）" },
    { "@bt.drop",     "子弹下坠 %（仅玩家）" },
    { "@bt.status",  "曳光： %d  速度： %d%%  精度： %d  下坠： %d%%" },
    { "@bt.st.off",  "子弹速度缩放已关闭 —— 使用游戏原始数值" }
};

/* ---- start up -------------------------------------------------------- */

static DWORD WINAPI PluginThread(LPVOID param) {
    char logFile[80];

    NameFromModule((HINSTANCE)param);
    if (!g_name[0]) strcpy(g_name, "Ballistics");

    snprintf(logFile, sizeof(logFile), "%s.log", g_name);
    LogInitAlways(logFile);

    while (!GetModuleHandleA("dinput8.dll")) Sleep(500);

    if (!ShPluginIniPath(g_name, g_ini, sizeof(g_ini)))
        g_ini[0] = 0;
    LoadSettings();

    ShLangDeclare(g_name, "en-US", kEn, (int)(sizeof(kEn) / sizeof(kEn[0])));
    ShLangDeclare(g_name, "zh-CN", kZh, (int)(sizeof(kZh) / sizeof(kZh[0])));

    g_menu = ShMenuCreate("@bt.page");
    if (!g_menu) {
        Log("bt: no menu page (%08x) - nothing to drive", ShLastError());
        return 0;
    }
    BuildMenu();

    Log("bt: page up, scaling %s at %d%%",
        g_enabled ? "on" : "off", (int)g_percent);

    if (g_enabled) EnsureHook();        /* the ini's own value */
    ApplyAccuracy();                    /* installs if the ini asks */
    ApplyDrop();                        /* stored; bites once the patch is in */
    Apply(0);

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
