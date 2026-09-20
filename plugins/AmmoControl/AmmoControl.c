/* Ammo control: the magazine capacity multiplier, and a reload that happens by
 * itself when the magazine runs dry.
 *
 * Every change goes through the framework's own engine calls - ShSetAmmoScale
 * for the capacity, ShFakeKey for the reload - so this installs no hook of its
 * own, patches no code and writes no engine memory.
 *
 * The two halves:
 *
 *   1. CAPACITY. The old plugin's six steps, as integer pairs so the result is
 *      exactly reproducible (0.50 = 1/2 ... 2.00 = 2/1). A change takes effect
 *      at the next refill: an ammo crate is what puts the number to use. At
 *      1.00x the framework installs nothing at all - the game keeps its own
 *      numbers - which is what that row is for.
 *
 *   2. AUTO RELOAD. ShGetAmmoRounds returns the rounds left in the magazine of
 *      the player's own weapon - the object the engine hands its capacity
 *      function, 0x180 into it; see the framework header and
 *      docs/ammocapacity-reverse.md section 9 - and when that reaches the
 *      threshold this presses the reload key for the player. The key is a row
 *      rather than a constant because the game's binding is the player's own
 *      business: it has to be set to whatever the game reloads on.
 *
 * Reading the rounds needs the capacity hook to be installed, because that hook
 * is what sees the engine ask for a capacity, and that call is what says which
 * weapon is in hand. So switching auto reload ON asks for a scale and puts the
 * wanted one straight back, which leaves the hook in place while the game's own
 * numbers stay in force. With auto reload OFF and 1.00x selected, nothing is
 * installed and this plugin changes nothing at all.
 *
 * The default blacklist applies (Ghost War and Mercenaries): while the mode is
 * not allowed the scale is handed back to the game and no key is pressed - a
 * scaled magazine in a PvP mode is an advantage this has no business taking.
 *
 * Text: kEn / kZh here, overridable by plugins\AmmoControl\lang.ini.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scripthook.h"
#include "log.h"

/* ---- the capacity steps ---------------------------------------------- */

/* 0.20x to 2.00x in 0.20x steps, 1.00x being the game's own numbers. Integer
 * pairs, so the result is exactly reproducible (0.20 = 1/5 ... 2.00 = 2/1),
 * and 1.00x is the pair 1/1 rather than 5/5: that is what makes its row
 * install no hook at all - see ShSetAmmoScale. */
typedef struct { const char *label; int num, den; } Step;

static const Step kSteps[] = {
    { "0.20x", 1, 5 }, { "0.40x", 2, 5 }, { "0.60x", 3, 5 },
    { "0.80x", 4, 5 }, { "1.00x", 1, 1 }, { "1.20x", 6, 5 },
    { "1.40x", 7, 5 }, { "1.60x", 8, 5 }, { "1.80x", 9, 5 },
    { "2.00x", 2, 1 }
};
#define STEP_N    ((int)(sizeof(kSteps) / sizeof(kSteps[0])))
#define STEP_ONE  4                 /* the 1.00x row: installs nothing */

static const char *kStepPtr[STEP_N];

/* ---- the rows that are choices -------------------------------------- */

/* Reload thresholds: 0 to 5 rounds left. */
static char        kThreshOpts[6][2];
static const char *kThreshPtr[6];
#define THRESH_N   6
#define THRESH_ONE 1

/* The reload key: the letters, then the digits. A row rather than a constant,
 * because this has to match whatever the game is bound to reload on. */
static char        kKeyOpts[36][3];
static const char *kKeyPtr[36];
#define KEY_N       36
#define KEY_VK(i)   ((i) < 26 ? ('A' + (i)) : ('0' + (i) - 26))
#define KEY_DEFAULT 17              /* R, the game's own default */

/* ---- state ----------------------------------------------------------- */

static char     g_ini[MAX_PATH];
static char     g_name[64];
static uint32_t g_menu;

/* The menu thread writes these; the worker thread reads them. */
static volatile LONG g_step = STEP_ONE;
static volatile LONG g_auto;
static volatile LONG g_thresh = THRESH_ONE;
static volatile LONG g_key = KEY_DEFAULT;

/* The worker thread alone writes these; the status line reads them. */
static volatile LONG g_rounds = -1;     /* -1 = not readable yet */
static int           g_hooked;          /* the capacity hook is installed */
static int           g_applied = -1;    /* the step actually written */
static int           g_waiting;         /* asked for a reload, waiting for it */
static DWORD         g_pressedAt;

/* ---- settings -------------------------------------------------------- */

static void SaveInt(const char *key, LONG value) {
    char text[24];

    snprintf(text, sizeof(text), "%ld", (long)value);
    if (!WritePrivateProfileStringA("Settings", key, text, g_ini))
        Log("ac: could not write %s=%s to %s", key, text, g_ini);
}

static void LoadSettings(void) {
    LONG step, auto_, thresh, key;

    step   = (LONG)GetPrivateProfileIntA("Settings", "multiplier", STEP_ONE, g_ini);
    auto_  = (LONG)GetPrivateProfileIntA("Settings", "autoreload", 0, g_ini);
    thresh = (LONG)GetPrivateProfileIntA("Settings", "threshold", THRESH_ONE, g_ini);
    key    = (LONG)GetPrivateProfileIntA("Settings", "key", KEY_DEFAULT, g_ini);

    if (step < 0 || step >= STEP_N) step = STEP_ONE;
    if (thresh < 0 || thresh >= THRESH_N) thresh = THRESH_ONE;
    if (key < 0 || key >= KEY_N) key = KEY_DEFAULT;

    InterlockedExchange(&g_step, step);
    InterlockedExchange(&g_auto, auto_ ? 1 : 0);
    InterlockedExchange(&g_thresh, thresh);
    InterlockedExchange(&g_key, key);
}

/* ---- the capacity ---------------------------------------------------- */

/* Write the wanted scale. force makes it write even when it already did - the
 * call after EnsureHook, where 1.00x has to be written back so the game's own
 * number is what the engine computes. */
static void ApplyScale(int force) {
    int step = (int)g_step;
    int n = kSteps[step].num, d = kSteps[step].den;
    int ok;

    if (!force && step == g_applied) return;
    if (!force && !g_hooked && step == STEP_ONE) return;
    ok = ShSetAmmoScale(n, d);
    if (!ok) {
        Log("ac: scale %s refused (%08x, %s)", kSteps[step].label,
            ShLastError(), ShErrorString(ShLastError()));
        return;
    }
    g_applied = step;
    if (step != STEP_ONE) g_hooked = 1;
    Log("ac: scale %s", kSteps[step].label);
}

/* The rounds are only readable once the hook is in: it is what sees the engine
 * ask for a capacity, and that call is what hands over the weapon object. Ask
 * for 2/1 to install it, then write the scale that was actually wanted. */
static void EnsureHook(void) {
    if (!g_hooked) {
        if (!ShSetAmmoScale(2, 1)) {
            Log("ac: could not install the capacity hook (%08x, %s)",
                ShLastError(), ShErrorString(ShLastError()));
            return;
        }
        g_hooked = 1;
        Log("ac: capacity hook installed (it is what makes the rounds readable)");
        ApplyScale(1);
        return;
    }
    ApplyScale(1);
}

/* The wanted scale is re-asserted every couple of seconds, and handed back the
 * moment the mode stops allowing this plugin: the framework blacklists by mode,
 * and the answer can change under a long session. */
static void ApplyWatch(void) {
    static DWORD last;

    if ((long)(GetTickCount() - last) < 2000) return;
    last = GetTickCount();

    if (ShPluginAllowed()) {
        ApplyScale(0);
    } else if (g_hooked) {
        ShSetAmmoScale(1, 1);
        g_hooked = 0;
        g_applied = STEP_ONE;
        Log("ac: handed the magazine back to the game (mode not allowed)");
    }
}

/* ---- the reload ------------------------------------------------------ */

/* Press once when the magazine is down to the threshold, then wait for the
 * magazine to come back. A weapon with no reserve left never comes back, so
 * after that wait the next attempt is held off - rather than clicking away on
 * every poll.
 *
 * 3500 ms rather than 2500: a normal reload lands about 2.8 s after the press
 * in the sessions of 2026-09-21, so the shorter wait reported every reload as
 * "no reserve left" and held the next attempt back for nothing
 * (logs\AmmoControl.log 00:58:05 pressed -> 00:58:08 back to 50). */
#define RELOAD_WAIT_MS 3500
#define RELOAD_BACKOFF 6000

static void AutoReload(void) {
    DWORD now = GetTickCount();
    int rounds = (int)g_rounds;

    if (!g_auto || rounds < 0) return;
    if (!ShPluginAllowed()) return;
    if (ShGetGameState() != SH_STATE_INGAME) return;
    if (rounds > (int)g_thresh) { g_waiting = 0; return; }

    if (g_waiting) {
        if ((long)(now - g_pressedAt) < RELOAD_WAIT_MS) return;
        g_waiting = 0;
        g_pressedAt = now;
        Log("ac: the reload at %d did not come back - no reserve left?", rounds);
        return;
    }
    if ((long)(now - g_pressedAt) < RELOAD_BACKOFF) return;

    ShFakeKey(KEY_VK((int)g_key), 1);
    g_pressedAt = now;
    g_waiting = 1;
    Log("ac: magazine at %d (threshold %d) - pressed '%c'", rounds,
        (int)g_thresh, (char)KEY_VK((int)g_key));
}

/* ---- the page -------------------------------------------------------- */

enum { ROW_MULT = 1, ROW_AUTO, ROW_THRESH, ROW_KEY };

static void OnRow(uint32_t menu, uint32_t item, int value, void *user) {
    int which = (int)(intptr_t)user;

    (void)menu; (void)item;

    switch (which) {
    case ROW_MULT:
        InterlockedExchange(&g_step, value);
        SaveInt("multiplier", value);
        Log("ac: multiplier %s", kSteps[value].label);
        ApplyScale(1);
        break;
    case ROW_AUTO:
        InterlockedExchange(&g_auto, value ? 1 : 0);
        SaveInt("autoreload", value ? 1 : 0);
        Log("ac: auto reload %s", value ? "on" : "off");
        if (value) EnsureHook();
        break;
    case ROW_THRESH:
        InterlockedExchange(&g_thresh, value);
        SaveInt("threshold", value);
        Log("ac: reload when down to %d", value);
        break;
    case ROW_KEY:
        InterlockedExchange(&g_key, value);
        SaveInt("key", value);
        Log("ac: reload key '%c'", (char)KEY_VK(value));
        break;
    default:
        break;
    }
}

static void BuildMenu(void) {
    ShMenuHint(g_menu, "@ac.hint");
    ShMenuList(g_menu, "@ac.mult", kStepPtr, STEP_N, (int)g_step, OnRow,
               (void *)(intptr_t)ROW_MULT);
    ShMenuToggle(g_menu, "@ac.auto", (int)g_auto, OnRow,
                 (void *)(intptr_t)ROW_AUTO);
    ShMenuList(g_menu, "@ac.thresh", kThreshPtr, THRESH_N, (int)g_thresh, OnRow,
               (void *)(intptr_t)ROW_THRESH);
    ShMenuList(g_menu, "@ac.key", kKeyPtr, KEY_N, (int)g_key, OnRow,
               (void *)(intptr_t)ROW_KEY);
    ShMenuStatus(g_menu, "@ac.st.norounds");
}

static void RefreshStatus(void) {
    int rounds = (int)g_rounds;

    if (!ShMenuIsShowing(g_menu)) return;
    if (rounds < 0) { ShMenuStatus(g_menu, "@ac.st.norounds"); return; }
    ShMenuStatusF(g_menu, "@ac.status", rounds, (int)g_thresh,
                  ShLangText(g_name, g_auto ? "@ac.on" : "@ac.off"),
                  kSteps[(int)g_step].label);
}

/* ---- the plugin's own name ------------------------------------------ */

/* From the module path, like every other plugin here: the ini, the log and the
 * text owner all follow the file name. */
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
    { "@ac.page",   "Ammo control" },
    { "@ac.hint",   "Raises or lowers all weapons' ammo cap" },
    { "@ac.mult",   "Capacity multiplier (applies on refill)" },
    { "@ac.auto",   "Auto reload" },
    { "@ac.thresh", "Reload when down to" },
    { "@ac.key",    "Reload key (same as in-game)" },
    { "@ac.on",     "on" },
    { "@ac.off",    "off" },
    { "@ac.status", "magazine %d  reload at %d  auto %s  cap %s" },
    { "@ac.st.norounds", "no rounds yet - switch a weapon or take an ammo crate" }
};

static const ShText kZh[] = {
    { "@ac.page",   "弹药控制" },
    { "@ac.hint",   "增加或减少所有武器可携带的弹药数量上限" },
    { "@ac.mult",   "弹药上限倍率（修改后需到弹药箱补给生效）" },
    { "@ac.auto",   "自动换弹" },
    { "@ac.thresh", "剩余多少时换弹" },
    { "@ac.key",    "换弹键（需与游戏内设置的按键一致）" },
    { "@ac.on",     "开" },
    { "@ac.off",    "关" },
    { "@ac.status", "弹匣 %d  剩 %d 换弹  自动 %s  上限 %s" },
    { "@ac.st.norounds", "还没有弹匣数 —— 换一次枪或进一次弹药箱" }
};

/* ---- start up -------------------------------------------------------- */

static DWORD WINAPI PluginThread(LPVOID param) {
    char logFile[80];
    int i;

    NameFromModule((HINSTANCE)param);
    if (!g_name[0]) strcpy(g_name, "AmmoControl");

    snprintf(logFile, sizeof(logFile), "%s.log", g_name);
    LogInitAlways(logFile);

    while (!GetModuleHandleA("dinput8.dll")) Sleep(500);

    if (!ShPluginIniPath(g_name, g_ini, sizeof(g_ini)))
        g_ini[0] = 0;
    LoadSettings();

    for (i = 0; i < STEP_N; i++) kStepPtr[i] = kSteps[i].label;
    for (i = 0; i < THRESH_N; i++) {
        snprintf(kThreshOpts[i], sizeof(kThreshOpts[i]), "%d", i);
        kThreshPtr[i] = kThreshOpts[i];
    }
    for (i = 0; i < KEY_N; i++) {
        kKeyOpts[i][0] = (char)KEY_VK(i);
        kKeyOpts[i][1] = 0;
        kKeyPtr[i] = kKeyOpts[i];
    }

    ShLangDeclare(g_name, "en-US", kEn, (int)(sizeof(kEn) / sizeof(kEn[0])));
    ShLangDeclare(g_name, "zh-CN", kZh, (int)(sizeof(kZh) / sizeof(kZh[0])));

    g_menu = ShMenuCreate("@ac.page");
    if (!g_menu) {
        Log("ac: no menu page (%08x) - nothing to drive", ShLastError());
        return 0;
    }
    BuildMenu();

    Log("ac: page up, multiplier %s, auto reload %s (at %d, key '%c')",
        kSteps[(int)g_step].label, g_auto ? "on" : "off", (int)g_thresh,
        (char)KEY_VK((int)g_key));

    ApplyScale(0);                      /* the ini's own value, if not 1.00x */
    if (g_auto) EnsureHook();           /* the rounds need the hook in */

    for (;;) {
        int rounds;

        Sleep(250);
        RefreshStatus();
        ApplyWatch();

        if (ShGetGameState() != SH_STATE_INGAME &&
            ShGetGameState() != SH_STATE_PAUSED)
            continue;

        /* Reading the rounds walks every weapon the engine has been asking
         * about (it is what ShGetAmmoRounds weighs up), and its only two
         * consumers are auto reload and the status line. With auto reload off
         * there is nothing to do while this page is not the page on screen:
         * ShMenuIsShowing asks about EXACTLY this page, so the scan is skipped
         * with the menu closed, on any other plugin's page, and inside any
         * submenu of someone else's. The loop reads again the moment the page
         * comes up, so the line follows within one tick - the framework shows
         * what was written last, which for that first tick is the old value. */
        if (g_auto || ShMenuIsShowing(g_menu)) {
            if (ShGetAmmoRounds(&rounds)) {
                if (rounds != (int)g_rounds) {
                    int was = (int)g_rounds;

                    if (was < 0)
                        Log("ac: rounds readable now (%d)", rounds);
                    /* A shot's step is one; anything bigger is a reload or
                     * another weapon, and that is what a switch has to show. */
                    else if (rounds > was || was - rounds > 5)
                        Log("ac: magazine %d -> %d%s", was, rounds,
                            rounds > was ? " (reload, or another weapon)" : "");
                    InterlockedExchange(&g_rounds, rounds);
                }
            } else if ((int)g_rounds >= 0 &&
                       ShLastError() == SH_ERR_NO_CANDIDATE) {
                /* A load or a respawn puts it out of reach until the engine
                 * asks for a capacity again; say so once, not every poll. */
                InterlockedExchange(&g_rounds, -1);
                Log("ac: rounds not readable any more (no capacity asked yet)");
            }
        }

        AutoReload();
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
