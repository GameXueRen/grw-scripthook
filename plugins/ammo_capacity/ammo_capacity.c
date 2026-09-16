/* Ammo capacity, the framework's replacement for the third-party
 * AmmoCapacity.asi.
 *
 * The plugin is the surface only: it owns the setting, the menu row and the
 * text. The hook and the arithmetic live in the framework
 * (scripthook_ammocap.c), which is the rule for everything that writes
 * engine memory - plugins call, the framework writes. See
 * docs/ammocapacity-reverse.md for what the old plugin did and why the
 * capacity can only be reached this way: the game computes it in one
 * function and stores it nowhere.
 *
 * The six values are the old plugin's own table (.rdata 0x33D0 / 0x33E8):
 * 0.50, 0.75, 1.00, 1.25, 1.50, 2.00, as integer num/den pairs. Its
 * "1.00x  Vanilla" entry is the pass-through - at 1.00 nothing is hooked at
 * all, which is also the default here.
 *
 * Config: this plugin's own ini, plugins\ammo_capacity\ammo_capacity.ini
 *
 *     [AmmoCapacity]
 *     Multiplier=1.00
 *
 * The key keeps the old plugin's name and its decimal form, so an ini
 * carried over from it reads the same: the value is matched to the nearest
 * of the six, exactly as that plugin did.
 *
 * A change takes effect at the next refill - exactly what the old plugin's
 * status line said, and what the game does: the crate is what asks for the
 * capacity.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "scripthook.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ---- the six values, and the old plugin's rationals ------------------- */

static const int g_num[] = { 1, 3, 1, 5, 3, 2 };
static const int g_den[] = { 2, 4, 1, 4, 2, 1 };
#define NVALS ((int)ARRAY_LEN(g_num))
#define VANILLA 2                     /* 1.00x - the pass-through */

/* ShMenuList keeps the array, so it outlives the call by design; the
 * strings themselves are literals. No inner const, because that is the
 * shape the API takes (const char **) and the extra qualifier only earns
 * a C4090. */
static const char *g_labels[NVALS] = {
    "0.50x", "0.75x", "1.00x", "1.25x", "1.50x", "2.00x"
};

static volatile LONG g_index = VANILLA;
static uint32_t      g_menu;

/* ---- text -------------------------------------------------------------
 * Compiled in with stable IDs, so a lang.ini can reword any row without
 * this file changing. The three error lines are the old plugin's, word for
 * word, because they say exactly what went wrong.
 */
static const ShText kEn[] = {
    { "@ac.page",  "Ammo Capacity" },
    { "@ac.mult",  "Capacity Multiplier" },
    { "@ac.note",  "Changes apply after visiting an ammo crate." },
    { "@ac.err.build", "Unsupported game build" },
    { "@ac.err.hook",  "Hook validation failed" },
    { "@ac.err.save",  "INI save failed" }
};

static const ShText kZh[] = {
    { "@ac.page",  "弹药容量" },
    { "@ac.mult",  "弹药容量倍率" },
    { "@ac.note",  "修改后需到弹药箱补给后生效。" },
    { "@ac.err.build", "不支持的游戏版本" },
    { "@ac.err.hook",  "挂钩校验失败" },
    { "@ac.err.save",  "配置保存失败" }
};

static void AcText(void) {
    static int done;

    if (done) return;
    done = 1;
    ShLangDeclare("ammo_capacity", "en-US", kEn,
                  (int)(sizeof(kEn) / sizeof(kEn[0])));
    ShLangDeclare("ammo_capacity", "zh-CN", kZh,
                  (int)(sizeof(kZh) / sizeof(kZh[0])));
}

/* ---- logging ---------------------------------------------------------- */

static FILE *g_log;
static LONG  g_logBusy;

static void AcLog(const char *fmt, ...) {
    va_list ap;
    char line[400];
    SYSTEMTIME st;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (!g_log) return;
    while (InterlockedExchange(&g_logBusy, 1)) Sleep(1);
    if (g_log) {
        GetLocalTime(&st);
        fprintf(g_log, "%02u:%02u:%02u.%03u  %s\n", st.wHour, st.wMinute,
                st.wSecond, st.wMilliseconds, line);
        fflush(g_log);
    }
    InterlockedExchange(&g_logBusy, 0);
}

/* ---- ini -------------------------------------------------------------- */

static HINSTANCE g_inst;
static char      g_iniPath[MAX_PATH];

/* plugins\ammo_capacity\ammo_capacity.ini, from our own module path. */
static void ResolveIniPath(void) {
    char mod[MAX_PATH];
    const char *dot;
    size_t n;

    g_iniPath[0] = 0;
    if (!g_inst || !GetModuleFileNameA(g_inst, mod, sizeof(mod))) return;
    dot = strrchr(mod, '.');
    n = dot ? (size_t)(dot - mod) : strlen(mod);
    if (n >= sizeof(g_iniPath)) n = sizeof(g_iniPath) - 1;
    memcpy(g_iniPath, mod, n);
    g_iniPath[n] = 0;
    strncat(g_iniPath, ".ini", sizeof(g_iniPath) - n - 1);
}

/* "2.00" -> index 5. The nearest of the six wins, so a value that came from
 * the old plugin's free-form ini lands where it did there. */
static int IndexOfValue(double v) {
    int best = VANILLA, i;
    double bestd = 1e9;

    if (v <= 0.0) return VANILLA;
    for (i = 0; i < NVALS; i++) {
        double d = (double)g_num[i] / (double)g_den[i] - v;

        if (d < 0) d = -d;
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

static void LoadIni(void) {
    char buf[32];

    if (!g_iniPath[0]) return;
    buf[0] = 0;
    GetPrivateProfileStringA("AmmoCapacity", "Multiplier", "1.00", buf,
                             sizeof(buf), g_iniPath);
    InterlockedExchange(&g_index, (LONG)IndexOfValue(atof(buf)));
    AcLog("ini: Multiplier=%s -> index %ld (%s)", buf,
          (long)InterlockedCompareExchange(&g_index, 0, 0),
          g_labels[InterlockedCompareExchange(&g_index, 0, 0)]);
}

static int SaveIni(void) {
    char buf[32];
    LONG i = InterlockedCompareExchange(&g_index, 0, 0);

    if (!g_iniPath[0]) return 0;
    if (i < 0 || i >= NVALS) i = VANILLA;
    snprintf(buf, sizeof(buf), "%d.%02d",
             g_num[i] / g_den[i], (g_num[i] % g_den[i]) * 100 / g_den[i]);
    /* Reported, so @ac.err.save is reachable: it was the old plugin's line
     * for exactly this failure. */
    return WritePrivateProfileStringA("AmmoCapacity", "Multiplier", buf,
                                      g_iniPath) ? 1 : 0;
}

/* ---- the scale, through the framework --------------------------------- */

/* Returns 0 when the framework refused - it already logged why and set
 * ShLastError, and the menu shows the matching line. */
static int Apply(int index) {
    int ok;

    if (index < 0 || index >= NVALS) index = VANILLA;
    ok = ShSetAmmoScale(g_num[index], g_den[index]);
    if (!ok) {
        int err = ShLastError();
        const char *line = (err == SH_ERR_HOOK_FAILED) ? "@ac.err.hook"
                                                       : "@ac.err.build";

        AcLog("scale refused: index %d, order %d -> %s", index, err, line);
        /* BuildMenu may have failed and left this at 0; a status line
         * against an unknown page is a no-op the framework refuses, so it
         * is only worth calling when there is a page. */
        if (g_menu) ShMenuStatus(g_menu, line);
        return 0;
    }
    AcLog("scale index %d = %d/%d (active=%d)", index, g_num[index],
          g_den[index], ShAmmoScaleActive());
    return 1;
}

/* ---- menu ------------------------------------------------------------- */

static void OnPick(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    InterlockedExchange(&g_index, (LONG)value);
    AcLog("menu: index %d (%s)", value,
          (value >= 0 && value < NVALS) ? g_labels[value] : "?");
    if (!Apply(value)) return;
    if (!SaveIni()) {
        AcLog("SaveIni failed for index %d", value);
        ShMenuStatus(g_menu, "@ac.err.save");
        return;
    }
    /* The status line carries the plain statement the old plugin used, so
     * the one thing a player has to know - when it lands - is always on
     * screen. */
    ShMenuStatus(g_menu, "@ac.note");
}

static void BuildMenu(void) {
    LONG idx = InterlockedCompareExchange(&g_index, 0, 0);

    AcText();
    g_menu = ShMenuCreate("@ac.page");
    if (!g_menu) { AcLog("ShMenuCreate failed"); return; }
    if (idx < 0 || idx >= NVALS) idx = VANILLA;
    ShMenuList(g_menu, "@ac.mult", g_labels, NVALS, (int)idx, OnPick, NULL);
    ShMenuHint(g_menu, "@ac.note");
    ShMenuStatus(g_menu, "@ac.note");
    AcLog("menu created (index %ld)", (long)idx);
}

/* ---- startup ---------------------------------------------------------- */

static void OpenLog(void) {
    char path[MAX_PATH];
    char *slash;

    if (!GetModuleFileNameA(NULL, path, MAX_PATH)) return;
    slash = strrchr(path, '\\');
    if (!slash) return;
    slash[1] = 0;
    if (strlen(path) + 24 >= sizeof(path)) return;
    strcat(path, "logs");
    CreateDirectoryA(path, NULL);
    strcat(path, "\\ammo_capacity.log");
    /* "w", not "a": the framework's own logs are per session, and a
     * diagnostic that only ever grows is a file that grows on the player's
     * disk forever. */
    g_log = fopen(path, "w");
}

/* Blocked, or unblocked, mid-session: this plugin has no tick thread of
 * its own, so this callback is where the change lands. Vanilla is 1/1,
 * which makes the framework leave the engine's own capacity alone - the
 * honest state for a mode this plugin is not allowed to touch. */
static void OnBlocked(int allowed, int blocked, void *user) {
    (void)blocked; (void)user;

    if (!allowed) {
        ShSetAmmoScale(1, 1);
        AcLog("blocked in this mode: the capacity is the game's own again");
    } else {
        Apply((int)InterlockedCompareExchange(&g_index, 0, 0));
        AcLog("allowed again: the configured multiplier is back");
    }
}

static DWORD WINAPI InitThread(LPVOID p) {
    LONG idx;

    (void)p;
    OpenLog();
    AcLog("--- ammo_capacity plugin ---");
    ResolveIniPath();
    LoadIni();

    /* The one mode rule this plugin has: a capacity multiplier is not
     * something a PvP match wants. Declared before the page is built, so a
     * session that starts in a blocked mode never has the page up even for
     * the moment between the two calls - the framework hides it by polling,
     * and this removes the window. */
    if (!ShPluginBlacklist(SH_MODE_BLACKLIST_GHOST_WAR |
                           SH_MODE_BLACKLIST_MERCENARIES))
        AcLog("blacklist: declaration refused (error %d)", ShLastError());
    if (!ShPluginOnBlocked(OnBlocked, NULL))
        AcLog("blacklist: ShPluginOnBlocked refused (error %d)",
              ShLastError());

    BuildMenu();

    /* Vanilla costs nothing: the framework installs its hook on the first
     * call that asks for something else, so a session left at 1.00x runs
     * with no hook at all. */
    idx = InterlockedCompareExchange(&g_index, 0, 0);
    AcLog("ready: index %ld (%s), framework scale %s", (long)idx,
          (idx >= 0 && idx < NVALS) ? g_labels[idx] : "?",
          ShAmmoScaleActive() ? "active" : "not needed yet");
    if (!ShPluginAllowed()) {
        /* A session that starts in a blocked mode: the page is hidden, so
         * the engine keeps its own capacity. */
        AcLog("blocked in this session: the multiplier is not applied");
        return 0;
    }
    if (!Apply((int)idx)) return 0;
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = inst;
        DisableThreadLibraryCalls(inst);
        {
            HANDLE h = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);

            if (h) CloseHandle(h);   /* never waited on */
        }
    }
    return TRUE;
}
