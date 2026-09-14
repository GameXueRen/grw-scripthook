/* ScriptHook settings: a built-in root-menu page that edits the main
 * scripthook.ini at runtime.
 *
 * The engine reads every key it exposes here only at startup:
 *   - [loader] load_plugins / cpu_boot / cpu_window / cpu_play / cpu_cores
 *     / cpu_prio_play / cpu_eco_boot
 *   - [plugins]  one toggle per plugins\<name>\<name>.asi
 *   - [Settings] Language (menu language)
 * Each page carries a single hint line noting that changes need a
 * game restart to take effect, instead of marking every row.
 *
 * The rows sit on this page and the two under it, in reading order: the
 * one switch about loading plugins at all comes first, on the page
 * itself, then "Plugin switches" (one row per plugins\<name>), then the
 * CPU scheduling page with the six dials that decide which set of
 * processors the game runs on in each stage of its start up, and at what
 * priority. The corefix status line travels with the CPU page, since it
 * is a statement about those dials on this machine and says nothing
 * about plugins.
 *
 * Values are written back with ShConfigSet*, which rewrites the
 * on-disk ini in place (comments and translation tables survive)
 * and refreshes the in-memory table. The loader already consumed
 * these keys at boot, so the change only affects the next launch.
 *
 * This module lives inside dinput8.dll and pins its root row to
 * [MenuOrder] weight 0, so it is always the first row and can never
 * be switched off by the [plugins] list it edits.
 */
#include <windows.h>
#include <string.h>
#include <stdio.h>

#define SH_BUILD 1
#include "scripthook.h"

/* ---- the exposed settings -------------------------------------- */

typedef struct {
    const char *section;  /* ini section, e.g. "loader" */
    const char *key;      /* ini key, e.g. "cpu_play" */
    const char *label;    /* menu label == translation key */
    int  isNumber;        /* a number row (cpu_cores) not a toggle */
    float lo, hi, step;
    int  def;             /* fallback when the key is missing */
    const char **opts;    /* a fixed-choice row when non-NULL */
    int  nopts;           /* how many of them this row offers */
} Setting;

/* The stage dials, in the ini's own order: what, if anything, is done to
 * the set of processors the game may run on while that stage is in
 * force. The first five values are shared by every stage; the play stage
 * also offers processor 0, whose combinations sit after them. The ini
 * stores the index into this list. */
static const char *g_stageOpts[] = {
    "Leave alone",              /* 0 */
    "All cores",                /* 1 */
    "SMT off",                  /* 2 */
    "E-cores off",              /* 3 */
    "SMT + E-cores off",        /* 4 */
    "CPU 0 off",                /* 5 */
    "SMT + CPU0 off",           /* 6 */
    "E-cores + CPU0 off",       /* 7 */
    "SMT + E-cores + CPU0 off"  /* 8 */
};
/* The logo and window stages stop before the processor-0 values: the
 * engine needs processor 0 while it is starting up. */
#define STAGE_NOPTS 5

/* The names a priority can be shown by, in corefix's own order - which is
 * the number the play row stores, so its first four entries are exactly the
 * choices that row offers. The last two are the states the loading stages'
 * one switch resolves to - the efficiency mode where the machine has it,
 * the low class where it does not - and they are never offered as a choice:
 * they only ever appear on the status line, which reports what is in force
 * rather than what was asked for. Realtime is offered nowhere: it can
 * starve the desktop and the audio threads. */
static const char *g_prioOpts[] = {
    "Leave alone",      /* 0 */
    "Normal",           /* 1 */
    "Above normal",     /* 2 */
    "High",             /* 3 */
    "Efficiency mode",  /* 4: what the loading switch resolves to */
    "Low"               /* 5: ... on a machine that cannot do that */
};
#define PRIO_NOPTS      6                   /* names: the status line's range */
#define PRIO_PLAY_NOPTS 4                   /* what the play row offers */

/* The one row that is about loading at all: whether the loader's next scan
 * brings any plugin up. It sits on the settings page itself, above "Plugin
 * switches", so the master switch and the rows it governs are read in that
 * order; the processor dials keep a page of their own, because a CPU dial
 * and a plugin switch have nothing to say to each other. */
static const Setting g_loaderSettings[] = {
    { "loader", "load_plugins",
      "@settings.load", 0, 0, 0, 0, 1, NULL, 0 },
};

/* One pair of rows per stage of the game's start up: which set of
 * processors it runs on, and which priority (or the efficiency mode) it
 * holds. Same rows, same ini keys and same order as before - only the page
 * is new. Each scale is written in its own order, so an option's index IS
 * the value the ini stores and no row needs a mapping. */
static const Setting g_cpuSettings[] = {
    { "loader", "cpu_boot",
      "Boot cores", 1, 0, STAGE_NOPTS - 1, 1, 0, g_stageOpts, STAGE_NOPTS },
    { "loader", "cpu_window",
      "Loading cores", 1, 0, STAGE_NOPTS - 1, 1, 0, g_stageOpts, STAGE_NOPTS },
    /* One switch for the two loading stages between them: on = efficiency
     * mode while they last, off (the default) = the class is left alone. */
    { "loader", "cpu_eco_boot",
      "Efficiency mode while loading", 0, 0, 0, 0, 0, NULL, 0 },
    { "loader", "cpu_play",
      "Play cores", 1, 0, 8, 1, 0, g_stageOpts, 9 },
    { "loader", "cpu_prio_play",
      "Play priority", 1, 0, PRIO_PLAY_NOPTS - 1, 1, 0, g_prioOpts,
      PRIO_PLAY_NOPTS },
    /* A ceiling on the play stage alone (0 = none): trimming the set
     * while the game is still starting is a good way to make it not
     * start, and the stutter it is for is a play-time thing. */
    { "loader", "cpu_cores",
      "Play max cores", 1, 0, 64, 1, 0, NULL, 0 },
};

/* ---- menu handles ---------------------------------------------- */

static uint32_t g_modMenu = 0;     /* the ScriptHook settings page */
static uint32_t g_pluginMenu = 0;  /* [plugins] rows               */
static uint32_t g_cpuMenu = 0;     /* [loader] CPU scheduling rows */
static volatile int g_built = 0;

/* ---- plugin scan buffer ---------------------------------------- */

#define PLUGIN_MAX 64
#define NAME_MAX   64
#define LANG_MAX   8

static char g_plugins[PLUGIN_MAX][NAME_MAX];
static int  g_nplugins = 0;

/* ---- callbacks -------------------------------------------------- */

static void ReportSaved(uint32_t menu) {
    ShMenuStatus(menu, "Saved. Restart to apply.");
}

static void OnLoaderBool(uint32_t menu, uint32_t item, int value,
                         void *user) {
    const Setting *s = (const Setting *)user;
    (void)item;
    if (!s) return;
    if (ShConfigSetBool(s->section, s->key, value))
        ReportSaved(menu);
}

static void OnNumber(uint32_t menu, uint32_t item, int value,
                     void *user) {
    const Setting *s = (const Setting *)user;
    (void)item;
    if (!s) return;
    /* A list row hands back the option's index, and for every row here
     * that index IS the value the ini stores - each scale is written in
     * its own order for exactly that reason. */
    if (ShConfigSetInt(s->section, s->key, value))
        ReportSaved(menu);
}

/* Plugin toggles carry the plugin folder name in user. */
static void OnPlugin(uint32_t menu, uint32_t item, int value,
                     void *user) {
    const char *name = (const char *)user;
    (void)item;
    if (!name || !name[0]) return;
    if (ShConfigSetBool("plugins", name, value))
        ReportSaved(menu);
}

static void RefreshOwnText(void);

/* The list shows a language's label; the value stored is its code, so
 * the callback is handed the code array and indexes it by the option
 * the player picked. The switch is immediate: the ini gets the code for
 * the next launch, and the text layer is told right now - the menu
 * itself follows on the next frame, because it translates as it
 * captures. */
static void OnLanguage(uint32_t menu, uint32_t item, int value,
                       void *user) {
    char (*codes)[16] = (char (*)[16])user;
    (void)item;
    if (!codes || value < 0 || value >= LANG_MAX) return;
    if (!codes[value][0]) return;
    if (ShConfigSetStr("Settings", "Language", codes[value]))
        ReportSaved(menu);
    if (ShLangSet(codes[value]))
        RefreshOwnText();
}

/* ---- plugin list ------------------------------------------------ */

/* Scan plugins\ for <folder>\<folder>.asi, mirroring the loader's
 * own scan, and keep the names sorted. The names are borrowed by the
 * toggle rows (and used as ini keys), so they live in a static
 * buffer for the whole session. */
static void ScanPlugins(void) {
    char pluginsDir[MAX_PATH], asi[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int i;

    g_nplugins = 0;
    if (!ShPluginsDir(pluginsDir, sizeof(pluginsDir))) return;
    {
        size_t n = strlen(pluginsDir);
        if (n > 0 && pluginsDir[n - 1] == '\\') pluginsDir[n - 1] = 0;
    }
    snprintf(asi, sizeof(asi), "%s\\*", pluginsDir);

    h = FindFirstFileA(asi, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const char *name = fd.cFileName;

        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (name[0] == '.') continue;

        snprintf(asi, sizeof(asi), "%s\\%s\\%s.asi",
                 pluginsDir, name, name);
        if (GetFileAttributesA(asi) == INVALID_FILE_ATTRIBUTES)
            continue;
        if (g_nplugins >= PLUGIN_MAX) break;

        strncpy(g_plugins[g_nplugins], name, NAME_MAX - 1);
        g_plugins[g_nplugins][NAME_MAX - 1] = 0;
        g_nplugins++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    /* Keep the menu stable across launches: sort by name. */
    for (i = 1; i < g_nplugins; i++) {
        char tmp[NAME_MAX];
        int j = i;
        while (j > 0 && strcmp(g_plugins[j - 1], g_plugins[j]) > 0) {
            memcpy(tmp, g_plugins[j - 1], sizeof(tmp));
            memcpy(g_plugins[j - 1], g_plugins[j], sizeof(tmp));
            memcpy(g_plugins[j], tmp, sizeof(tmp));
            j--;
        }
    }
}

/* ---- menu construction ------------------------------------------ */

/* One row per Setting, on whichever page the table belongs to. The three
 * row shapes are the same for both tables, which is why this is a
 * function rather than the same loop written out twice. */
static void BuildSettings(uint32_t menu, const Setting *rows, int n) {
    int i;

    for (i = 0; i < n; i++) {
        const Setting *s = &rows[i];
        if (s->opts) {
            /* A fixed-choice row: the ini holds the option's value, which
             * is its index in the list, and the callback is handed that
             * index again. A value the row does not offer - a hand-edited
             * ini, or one written by a build with more dials - reads as
             * the first entry rather than pointing past the list. */
            int cur = ShConfigGetInt(s->section, s->key, s->def);
            int idx = 0, k;

            for (k = 0; k < s->nopts; k++) {
                if (k == cur) { idx = k; break; }
            }
            ShMenuList(menu, s->label, s->opts, s->nopts, idx,
                       OnNumber, (void *)s);
        } else if (s->isNumber) {
            int cur = ShConfigGetInt(s->section, s->key, s->def);
            ShMenuNumber(menu, s->label, (float)cur,
                         s->lo, s->hi, s->step, OnNumber, (void *)s);
        } else {
            int cur = ShConfigGetBool(s->section, s->key, s->def);
            ShMenuToggle(menu, s->label, cur, OnLoaderBool, (void *)s);
        }
    }
}

/* Every plugin switch is a [plugins] toggle the loader's next scan
 * honours. Rows show the plugin folder name only; the "restart
 * needed" note lives once on the menu hint line, not on every row.
 * A folder with no line of its own reads as off, which is the same
 * rule the loader applies: by the time this page is built the loader
 * has usually written the line itself, so this default only covers a
 * folder that appeared after that scan. */
static void BuildPluginMenu(void) {
    int i;

    for (i = 0; i < g_nplugins; i++) {
        const char *name = g_plugins[i];
        int cur = ShConfigGetBool("plugins", name, 0);
        ShMenuToggle(g_pluginMenu, name, cur, OnPlugin, (void *)name);
    }
}

/* The language switch. The options are the codes [Settings] Languages
 * lists; with no such key they are the languages this build ships text
 * for (ShLangBuiltin), so adding a language to the compile-time tables
 * is enough to offer it. The option text is the code's label
 * ([LanguageNames]); the code itself is what gets stored. Both arrays
 * outlive the IT_LIST, whose option pointers are borrowed. */
static char        g_langCodes[LANG_MAX][16];
static char        g_langLabels[LANG_MAX][48];
static const char *g_langOpts[LANG_MAX];
static int         g_nLangs = 0;

static void AddLang(const char *code) {
    size_t len;

    if (g_nLangs >= LANG_MAX || !code) return;
    while (*code == ' ' || *code == '\t') code++;
    len = strlen(code);
    while (len > 0 && (code[len - 1] == ' ' || code[len - 1] == '\t'))
        len--;
    if (len == 0) return;
    if (len >= sizeof(g_langCodes[0])) len = sizeof(g_langCodes[0]) - 1;
    memcpy(g_langCodes[g_nLangs], code, len);
    g_langCodes[g_nLangs][len] = 0;
    snprintf(g_langLabels[g_nLangs], sizeof(g_langLabels[0]), "%s",
             ShLangLabel(g_langCodes[g_nLangs]));
    g_langOpts[g_nLangs] = g_langLabels[g_nLangs];
    g_nLangs++;
}

static void LoadLanguages(void) {
    char raw[160];
    const char *p;
    int i;

    g_nLangs = 0;
    if (ShConfigGetStr("Settings", "Languages", "", raw, sizeof(raw)) &&
        raw[0]) {
        p = raw;
        while (*p && g_nLangs < LANG_MAX) {
            const char *comma = strchr(p, ',');
            size_t len = comma ? (size_t)(comma - p) : strlen(p);
            char code[32];

            if (len >= sizeof(code)) len = sizeof(code) - 1;
            memcpy(code, p, len);
            code[len] = 0;
            AddLang(code);
            if (!comma) break;
            p = comma + 1;
        }
    }
    if (g_nLangs == 0) {
        char one[48];

        for (i = 0; ShLangBuiltin(i, one, sizeof(one)) &&
                    g_nLangs < LANG_MAX; i++) {
            char *tab = strchr(one, '\t');
            if (tab) *tab = 0;
            AddLang(one);
        }
    }
    if (g_nLangs == 0)              /* nothing anywhere: stay readable */
        AddLang("zh-CN");
}

static void BuildLanguageRow(uint32_t parent) {
    const char *cur = ShLangGet();
    int idx = 0, i;

    LoadLanguages();
    if (cur) {
        for (i = 0; i < g_nLangs; i++)
            if (ShLangMatch(cur, g_langCodes[i])) { idx = i; break; }
    }
    if (g_nLangs > 0)
        ShMenuList(parent, "@settings.language",
                   g_langOpts, g_nLangs, idx, OnLanguage,
                   (void *)g_langCodes);
}

/* ---- the mode blacklist line ------------------------------------------
 * The Plugins page lists every plugin with a switch that only takes effect
 * on the next launch. The mode blacklist is the other reason a plugin can
 * be off - a temporary one that follows the play mode rather than the ini -
 * so it is shown on the same page, as a second line under the hint it
 * already carries. A slow thread refreshes it: the answer follows the
 * mode, and nothing tells a menu when that changes.
 */
static char g_blLast[96];

static void SetPluginHint(void) {
    char notice[96], text[384];
    int n;

    ShPluginBlacklistNotice(notice, sizeof(notice));
    n = snprintf(text, sizeof(text), "%s\n%s",
                 ShLang("These changes take effect after a game restart."),
                 ShLang("No [plugins] line means off. Switch it on here; "
                        "deleting scripthook.ini resets every plugin to off."));
    if (notice[0] && n > 0 && (size_t)n + 2 < sizeof(text))
        snprintf(text + n, sizeof(text) - (size_t)n, "\n%s", notice);
    ShMenuHint(g_pluginMenu, text);
}

static DWORD WINAPI BlHintThread(LPVOID p) {
    char notice[96];

    (void)p;
    for (;;) {
        Sleep(1000);
        if (!g_pluginMenu) continue;
        notice[0] = 0;
        ShPluginBlacklistNotice(notice, sizeof(notice));
        if (!strcmp(notice, g_blLast)) continue;
        snprintf(g_blLast, sizeof(g_blLast), "%s", notice);
        SetPluginHint();
    }
    return 0;
}

/* ---- the CPU page's live line ------------------------------------------
 * The rows on that page say what will be done from the next launch on; the
 * line under them says what the game is running with RIGHT NOW - which
 * stage it is in, which processor dial is in force for that stage, and at
 * what priority. Those come from the corefix module, which read the dials
 * when the game started, so the line describes the process as it is rather
 * than the ini as it is being edited. It follows the stage, and nothing
 * tells a menu when that changes, so it is polled.
 */
static char g_cpuLast[128];

/* The stage as the translation table spells it: "boot" / "window" / "play"
 * are keys there, and read as Logo / 窗口加载 / 游玩. */
static const char *StageKey(int stage) {
    return stage == SH_STAGE_BOOT ? "boot"
         : stage == SH_STAGE_WINDOW ? "window"
         : "play";
}

static void SetCpuLine(void) {
    ShCpuStatus cf;
    const char *dial, *prio;
    char text[160];
    int st, d, p;

    if (!g_cpuMenu) return;

    /* The dials ran on the attach path, long before this module was
     * called, so the status is filled by now and the fields are settled.
     * Clamped all the same: an index from a build with more values than
     * this one knows must not read past the tables above. */
    ShCpuGetStatus(&cf);
    st = cf.stage >= 0 && cf.stage <= 2 ? cf.stage : 0;
    d  = cf.dial[st] >= 0 && cf.dial[st] <= 8 ? cf.dial[st] : 0;
    p  = cf.prio[st] >= 0 && cf.prio[st] < PRIO_NOPTS ? cf.prio[st] : 0;

    /* Every entry of those two scales is already a translation key - the
     * same words the rows above use. */
    dial = g_stageOpts[d];
    prio = g_prioOpts[p];

    snprintf(text, sizeof(text),
             ShLang("Now: %s - cores %s - priority %s"),
             ShLang(StageKey(st)), ShLang(dial), ShLang(prio));
    if (!strcmp(text, g_cpuLast)) return;
    snprintf(g_cpuLast, sizeof(g_cpuLast), "%s", text);
    ShMenuStatus(g_cpuMenu, text);
}

/* One hint per page: these rows only act on the next launch. On the CPU
 * page the note is followed by the two facts about this machine that
 * decide whether a dial can apply at all: a dial that cannot (E-cores
 * off on a CPU without E-cores) otherwise reads as one that was
 * ignored, and "the system already trimmed this process" is the one
 * thing that decides whether an "All cores" dial is worth setting. What
 * the dials are doing right now is the line at the bottom of that page,
 * not this one.
 *
 * The text is resolved here rather than at capture time, so a language
 * switch has to rebuild it - see RefreshOwnText. */
static void BuildHints(void) {
    char   hint[384];
    char   line[160];
    size_t used;
    ShCpuStatus cf;

    if (!g_modMenu) return;

    /* The settings page: the rows that need a restart are the plugin and
     * CPU ones, while the language row above applies at once - the note
     * says both rather than sending the player to a restart it does not
     * need. */
    ShMenuHint(g_modMenu, ShLang("@settings.hint"));
    /* The Plugins page shows the same note plus the mode blacklist line,
     * which the thread started below keeps up to date. */
    SetPluginHint();

    /* The CPU page: one sentence - what the page does, and that it acts
     * from the next launch on. The one thing worth a second line is the
     * E-core caveat, and only on a machine where that dial cannot apply
     * at all; anywhere else it would explain nothing. What the dials are
     * doing right now is the line at the bottom of the page. */
    used = (size_t)snprintf(hint, sizeof(hint), "%s",
                            ShLang("Processor set and priority for each "
                                   "start up stage - changes need a "
                                   "restart."));
    if (ShCpuGetStatus(&cf)) {
        /* Two dials can be picked and then do nothing at all, and each of
         * them needs a line of its own - the row alone would read as "set
         * and quietly ignored": "E-cores off" on a CPU with no E-cores,
         * and the efficiency mode on anything but Windows 11, where the
         * call exists but the level it names does not. */
        line[0] = 0;
        if (cf.ecoreState == SH_CF_NA_NOT_INTEL)
            snprintf(line, sizeof(line), "%s",
                     ShLang("E-cores off: not applicable on this CPU."));
        else if (cf.ecoreState == SH_CF_NA_NO_ECORE)
            snprintf(line, sizeof(line), "%s",
                     ShLang("E-cores off: this CPU has no E-cores."));
        else if (cf.ecoreState == SH_CF_FAILED)
            snprintf(line, sizeof(line), "%s",
                     ShLang("E-cores off: detection failed."));
        if (line[0] && used + 2 < sizeof(hint)) {
            snprintf(hint + used, sizeof(hint) - used, "\n%s", line);
            used = strlen(hint);
            line[0] = 0;
        }
        if (cf.ecoBoot) {
            if (cf.eco == SH_ECO_NA)
                snprintf(line, sizeof(line), "%s",
                         ShLang("Efficiency mode: not available on this "
                                "system - the loading stages hold the low "
                                "priority instead."));
            else if (cf.eco == SH_ECO_FAILED)
                snprintf(line, sizeof(line), "%s",
                         ShLang("Efficiency mode: the call failed - the "
                                "loading stages hold the low priority "
                                "instead."));
        }
        if (line[0] && used + 2 < sizeof(hint))
            snprintf(hint + used, sizeof(hint) - used, "\n%s", line);
    }
    ShMenuHint(g_cpuMenu, hint);
}

/* Text this module composed itself does not follow a language switch:
 * menu rows do (they are translated as they are captured), but a hint
 * and a status line are strings we handed over. Called right after a
 * switch, and from the poll thread, so a switch made anywhere else lands
 * within a tick. */
static char g_langSeen[16];

static void RefreshOwnText(void) {
    const char *cur = ShLangGet();

    if (cur) snprintf(g_langSeen, sizeof(g_langSeen), "%s", cur);
    BuildHints();
    SetCpuLine();
}

static DWORD WINAPI CpuLineThread(LPVOID p) {
    (void)p;
    for (;;) {
        const char *cur;

        Sleep(1000);
        cur = ShLangGet();
        if (cur && strcmp(cur, g_langSeen)) {
            snprintf(g_langSeen, sizeof(g_langSeen), "%s", cur);
            BuildHints();
        }
        SetCpuLine();
    }
    return 0;
}

/* Register the whole tree. Called from the loader thread after the
 * config has been parsed, so the values and the scan see the real
 * ini. Safe to call once; the guard keeps rebuilds from stacking. */
void ShModSettingsStartup(void) {
    if (g_built) return;
    g_built = 1;

    /* Pin our row first: [MenuOrder] ScriptHook settings = 0. Write it only
     * when it is not already pinned, so a normal launch does not
     * touch the ini file for nothing. */
    if (ShConfigGetInt("MenuOrder", "@settings.page", 1000) != 0)
        ShConfigSetInt("MenuOrder", "@settings.page", 0);

    g_modMenu = ShMenuCreate("@settings.page");
    if (!g_modMenu) return;

    /* Rows sort by the order they are made in: the plugin master switch
     * first, on this page, then the sub pages - the plugin list, then the
     * CPU dials on a page of their own - and the language row last. */
    BuildSettings(g_modMenu, g_loaderSettings,
                  (int)(sizeof(g_loaderSettings) / sizeof(g_loaderSettings[0])));
    g_pluginMenu = ShMenuSub(g_modMenu, "@settings.plugins");
    g_cpuMenu    = ShMenuSub(g_modMenu, "@settings.cpu");

    ScanPlugins();
    BuildSettings(g_cpuMenu, g_cpuSettings,
                  (int)(sizeof(g_cpuSettings) / sizeof(g_cpuSettings[0])));
    BuildPluginMenu();
    BuildLanguageRow(g_modMenu);

    /* The hints, then the live line. The note the pages carry is text we
     * composed, so the language it is in is remembered here: the poll
     * thread below rebuilds it when that changes. */
    BuildHints();
    snprintf(g_langSeen, sizeof(g_langSeen), "%s", ShLangGet());
    SetCpuLine();
    if (!CreateThread(NULL, 0, BlHintThread, NULL, 0, NULL))
        ShMenuStatus(g_pluginMenu, "blacklist line thread failed");
    if (!CreateThread(NULL, 0, CpuLineThread, NULL, 0, NULL))
        ShMenuStatus(g_cpuMenu, "CPU line thread failed");
}
