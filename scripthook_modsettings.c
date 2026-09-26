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
 * The rows sit on this page and the three under it, in reading order:
 * the one switch about loading plugins at all comes first, on the page
 * itself, then "Plugin switches" (one row per plugins\<name>), then the
 * CPU scheduling page with the six dials that decide which set of
 * processors the game runs on in each stage of its start up, and at what
 * priority, and then "Menu order", which is where the root menu's own
 * order is decided. The corefix status line travels with the CPU page,
 * since it is a statement about those dials on this machine and says
 * nothing about plugins.
 *
 * Values are written back with ShConfigSet*, which rewrites the
 * on-disk ini in place (comments and translation tables survive)
 * and refreshes the in-memory table. The loader already consumed
 * these keys at boot, so the change only affects the next launch.
 *
 * This module lives inside dinput8.dll; its root row defaults to
 * [MenuOrder] weight 0, which puts it first out of the box. It is a row
 * like any other on the mod menu's ordering page, though, and the
 * [plugins] list it edits never applies to it: the loader gates plugin
 * folders only.
 */
#include <windows.h>
#include <string.h>
#include <stdio.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "scripthook_tick.h"

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
    "@cpu.opt.leave",                  /* 0 */
    "@cpu.opt.all",                    /* 1 */
    "@cpu.opt.nosmt",                  /* 2 */
    "@cpu.opt.noecore",                /* 3 */
    "@cpu.opt.nosmt_noecore",          /* 4 */
    "@cpu.opt.nocpu0",                 /* 5 */
    "@cpu.opt.nosmt_nocpu0",           /* 6 */
    "@cpu.opt.noecore_nocpu0",         /* 7 */
    "@cpu.opt.nosmt_noecore_nocpu0"    /* 8 */
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
    "@cpu.opt.leave",      /* 0 */
    "@cpu.prio.normal",    /* 1 */
    "@cpu.prio.above",     /* 2 */
    "@cpu.prio.high",      /* 3 */
    "@cpu.prio.eco",       /* 4: what the loading switch resolves to */
    "@cpu.prio.low"        /* 5: ... on a machine that cannot do that */
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

/* The loading stages' efficiency-mode dial. The order is the ini's, so an
 * option's index IS the value stored: 0 leave alone, 1 hold the mode, 2
 * drop it. 0 and 1 keep the meaning the old on/off switch had, so an
 * existing cpu_eco_boot=0 or =1 reads the same as before.
 *
 * The words are the row's own ("on" / "off"), not the priority scale's:
 * this row is a switch, while the status line names the mode it resolves
 * to. One key serving both made the row read "efficiency mode" instead of
 * "on" - the split is what keeps each place saying what it means. */
static const char *g_ecoOpts[] = {
    "@cpu.opt.leave", "@cpu.eco.on", "@cpu.eco.off"
};

/* One pair of rows per stage of the game's start up: which set of
 * processors it runs on, and which priority (or the efficiency mode) it
 * holds. Same rows, same ini keys and same order as before - only the page
 * is new. Each scale is written in its own order, so an option's index IS
 * the value the ini stores and no row needs a mapping. */
static const Setting g_cpuSettings[] = {
    { "loader", "cpu_boot",
      "@cpu.row.boot", 1, 0, STAGE_NOPTS - 1, 1, 0, g_stageOpts, STAGE_NOPTS },
    { "loader", "cpu_window",
      "@cpu.row.window", 1, 0, STAGE_NOPTS - 1, 1, 0, g_stageOpts, STAGE_NOPTS },
    /* One dial for the two loading stages between them: hold the efficiency
     * mode while they last, drop it for them, or leave the class alone
     * (the default). */
    { "loader", "cpu_eco_boot",
      "@cpu.row.eco", 0, 0, 2, 1, 0, g_ecoOpts, 3 },
    { "loader", "cpu_play",
      "@cpu.row.play", 1, 0, 8, 1, 0, g_stageOpts, 9 },
    { "loader", "cpu_prio_play",
      "@cpu.row.prio", 1, 0, PRIO_PLAY_NOPTS - 1, 1, 0, g_prioOpts,
      PRIO_PLAY_NOPTS },
    /* A ceiling on the play stage alone (0 = none): trimming the set
     * while the game is still starting is a good way to make it not
     * start, and the stutter it is for is a play-time thing. */
    { "loader", "cpu_cores",
      "@cpu.row.cores", 1, 0, 64, 1, 0, NULL, 0 },
};

/* ---- menu handles ---------------------------------------------- */

static uint32_t g_modMenu = 0;     /* the ScriptHook settings page */
static uint32_t g_pluginMenu = 0;  /* [plugins] rows               */
static uint32_t g_cpuMenu = 0;     /* [loader] CPU scheduling rows */
static volatile int g_built = 0;

/* ---- plugin scan buffer ---------------------------------------- */

#define PLUGIN_MAX 64
#define NAME_MAX   64
/* The languages the settings page can offer. The game ships sixteen and
 * the framework declares a name row for each, so this is that count - a
 * smaller number silently cuts the tail of the picker off. */
#define LANG_MAX   16

static char g_plugins[PLUGIN_MAX][NAME_MAX];
static int  g_nplugins = 0;

/* ---- callbacks -------------------------------------------------- */

static void ReportSaved(uint32_t menu) {
    ShMenuStatus(menu, "@settings.saved");
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
 * honours. A row reads as
 *
 *     firstperson(第一人称)                 [关]
 *
 * because the two halves answer different questions: the folder is the
 * file the player has to find, edit or delete (plugins\firstperson\), and
 * the page name is what that plugin does, in this language. The state is
 * the value column, drawn by the row's own kind - the same "[on] / [off]"
 * every toggle in the menu uses.
 *
 * The page-name half is resolved HERE rather than left as a key: this page
 * belongs to the framework, so the capture would look the key up under ""
 * and an ID would come out readable-mangled instead of translated.
 *
 * The config key and the callback stay the bare folder name
 * ([plugins] firstperson=1), and a plugin with no page in the root right
 * now - switched off, or hidden by the mode blacklist - has no page name
 * to show, so it reads as its folder alone. The "restart needed" note
 * lives once on the menu hint line, not on every row. A folder with no
 * line of its own reads as off, which is the same rule the loader
 * applies: by the time this page is built the loader has usually written
 * the line itself, so this default only covers a folder that appeared
 * after that scan. */
static void BuildPluginMenu(void) {
    ShMenuOrderRow rows[64];
    int i, j, n;

    /* Dropped and rebuilt rather than added to: the first pass runs before
     * the loader's worker thread has brought a single plugin page up, so
     * every row comes out as its folder and nothing else. RefreshPluginMenu
     * names them again once the pages exist. ShMenuClear keeps the title,
     * the hint and the status, so the blacklist line the poll thread writes
     * survives the rebuild - the order page leans on the same thing. */
    if (!g_pluginMenu) return;
    ShMenuClear(g_pluginMenu);
    n = ShMenuRootOrderRows(rows, 64);

    if (n > (int)(sizeof(rows) / sizeof(rows[0])))
        n = (int)(sizeof(rows) / sizeof(rows[0]));
    for (i = 0; i < g_nplugins; i++) {
        const char *name = g_plugins[i];
        char buf[224];
        int cur = ShConfigGetBool("plugins", name, 0);

        /* "<folder>(<page name>)". The page name is dropped rather than
         * faked when the plugin has no page in the root right now:
         * switched off, or hidden by the mode blacklist. */
        snprintf(buf, sizeof(buf), "%s", name);
        for (j = 0; j < n; j++) {
            if (_stricmp(rows[j].owner, name)) continue;
            snprintf(buf, sizeof(buf), "%s(%s)", name,
                     ShLangText(rows[j].owner, rows[j].key));
            break;
        }
        ShMenuToggle(g_pluginMenu, buf, cur, OnPlugin, (void *)name);
    }
}

/* Keep the switches page named after the plugins.
 *
 * Two moments need a rebuild. The first is the loader finishing: this
 * module builds the page at start up, while the plugins are still being
 * brought up on the loader's worker thread, so on that pass the root holds
 * no plugin page yet and a row can only read as its folder - the page name
 * has nothing to come from. Counting the plugin pages the root holds, and
 * rebuilding when that number moves, catches exactly that.
 *
 * The second is a visit: every time the player opens the page it is built
 * again, which is what re-reads the names after a language switch. A row
 * label here is finished text rather than a key - the page belongs to the
 * framework, so the capture would look an ID up under "" - and finished
 * text has to be remade to change language.
 *
 * Called from the plugin-line thread, once a second. */
static int g_pluginShown = 0;           /* the page was on screen    */
static int g_pluginPages = -1;          /* plugin pages at the last build */
static volatile LONG g_pluginBusy = 0;

static void RefreshPluginMenu(void) {
    ShMenuOrderRow rows[64];
    int i, n, pages = 0, showing;

    if (!g_pluginMenu) return;
    showing = ShMenuIsShowing(g_pluginMenu);
    if (!showing) g_pluginShown = 0;

    n = ShMenuRootOrderRows(rows, 64);
    if (n > (int)(sizeof(rows) / sizeof(rows[0])))
        n = (int)(sizeof(rows) / sizeof(rows[0]));
    for (i = 0; i < n; i++)
        if (rows[i].owner[0]) pages++;      /* a plugin's own page */

    if (!showing && pages == g_pluginPages) return;
    if (showing && g_pluginShown && pages == g_pluginPages) return;
    if (InterlockedCompareExchange(&g_pluginBusy, 1, 0)) return;
    g_pluginShown = showing;
    g_pluginPages = pages;
    BuildPluginMenu();
    InterlockedExchange(&g_pluginBusy, 0);
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

/* ---- the root order page ----------------------------------------------
 * The root menu draws its rows in [MenuOrder] weight order. This page
 * edits those weights: one row per page in the root, the row's number is
 * its place among them, and left/right moves it one place. Every move
 * renumbers the whole set 0, 10, 20 ... and writes it at once, and the
 * root re-sorts on its next capture, so the page and the root cannot
 * drift apart.
 *
 * Nothing is held back: this page and the Forge page are rows here like
 * any other, and only their default weights (0 and 10) put them first
 * out of the box. Pages that are not in the root right now are not
 * listed and keep their weight - a plugin switched off, or one the play
 * mode has taken away, comes back to the place it had. The list is taken
 * again every time the page is opened, because the mode can have changed
 * since the last look.
 */
static uint32_t g_orderMenu = 0;
static uint32_t g_aboutMenu = 0;    /* the About page */
#define ORDER_MAX 64

typedef struct {
    char key[96];        /* the page key [MenuOrder] is keyed by */
    char owner[48];      /* the plugin that owns the page       */
    char name[192];      /* its label, in the current language  */
} OrderRow;

static OrderRow g_order[ORDER_MAX];
static int  g_nOrder;
static char g_orderSel[192];           /* label of the row the cursor was on */
static int  g_orderShown = 0;          /* the page was on screen    */
static volatile int g_orderStale = 0;  /* list or names need a rebuild */
static volatile LONG g_orderBusy = 0;

static void BuildOrderMenu(void);
static void OrderWrite(void);

/* A row carries its own index, so the callback knows which page moved.
 * On a step the value is the place to move to; on release the menu fires
 * once more with the value unchanged (so what is on screen and what the
 * plugin holds agree), and that one has nothing to do. */
static void OnOrderMove(uint32_t menu, uint32_t item, int value,
                        void *user) {
    int from = (int)(INT_PTR)user;
    OrderRow moved;
    int i, to;

    (void)menu; (void)item;
    if (from < 0 || from >= g_nOrder) return;
    if (value < 1 || value > g_nOrder) return;   /* the rows show 1..n */
    if (value == from + 1) return;               /* the release fire, a no-op */
    if (InterlockedCompareExchange(&g_orderBusy, 1, 0)) return;

    to = value - 1;
    moved = g_order[from];
    if (to > from)
        for (i = from; i < to; i++) g_order[i] = g_order[i + 1];
    else
        for (i = from; i > to; i--) g_order[i] = g_order[i - 1];
    g_order[to] = moved;
    /* Remember the moved page by the label it is drawn with: the page is
     * rebuilt below, and the cursor has to land back on that same page. */
    snprintf(g_orderSel, sizeof(g_orderSel), "%s", g_order[to].name);

    OrderWrite();
    ShMenuOrderDirty();          /* the root re-sorts on its next capture */
    BuildOrderMenu();
    ShMenuSelectRow(g_orderMenu, g_orderSel);
    /* These take a KEY, not translated text: the capture looks it up
     * with the page's owner. Handing over ShLang(...) here translated
     * twice - the second lookup missed, fell back to the readable form
     * of the Chinese string and cut it. */
    ShMenuStatus(g_orderMenu, "@settings.order.saved");
    InterlockedExchange(&g_orderBusy, 0);
}

/* Take the order from the menu model: the pages the root holds now, in
 * the order it draws them. */
static void OrderReload(void) {
    ShMenuOrderRow rows[ORDER_MAX];
    int i, n = ShMenuRootOrderRows(rows, ORDER_MAX);

    if (n > ORDER_MAX) n = ORDER_MAX;
    for (i = 0; i < n; i++) {
        snprintf(g_order[i].key, sizeof(g_order[i].key), "%s", rows[i].key);
        snprintf(g_order[i].owner, sizeof(g_order[i].owner), "%s",
                 rows[i].owner);
        /* The page's own label: a plugin's text is keyed by its owner,
         * which is the lookup that makes the row read in this language. */
        snprintf(g_order[i].name, sizeof(g_order[i].name), "%s",
                 ShLangText(rows[i].owner, rows[i].key));
    }
    g_nOrder = n;
}

/* Write the weights back: 0, 10, 20 ... for the pages on this page, in
 * the order they are in now. A page that is not in the root keeps what
 * it had, so a weight written here can equal a kept one - the sort is
 * stable, so the two hold their relative order until one of them moves. */
static void OrderWrite(void) {
    int i;

    for (i = 0; i < g_nOrder; i++)
        ShConfigSetInt("MenuOrder", g_order[i].key, i * 10);
}

/* Draw the page from g_order. Rebuilt after every move, so what is on
 * screen is the order that was just written. */
static void BuildOrderMenu(void) {
    int i;

    if (!g_orderMenu) return;
    ShMenuClear(g_orderMenu);
    for (i = 0; i < g_nOrder; i++)
        ShMenuNumber(g_orderMenu, g_order[i].name, (float)(i + 1), 1.0f,
                     (float)(g_nOrder > 1 ? g_nOrder : 1), 1.0f,
                     OnOrderMove, (void *)(INT_PTR)i);
    if (g_nOrder == 0)
        ShMenuStatus(g_orderMenu, "@settings.order.empty");
}

/* Once a second, on the thread that already watches the CPU line: the
 * list needs taking again when the page is opened (the play mode can have
 * changed which pages are in the root) and when the language changed. */
static void OrderTick(void) {
    int showing = g_orderMenu ? ShMenuIsShowing(g_orderMenu) : 0;

    if (!showing) {
        g_orderShown = 0;
        return;
    }
    if (!g_orderShown || g_orderStale) {
        if (InterlockedCompareExchange(&g_orderBusy, 1, 0)) return;
        g_orderShown = 1;
        g_orderStale = 0;
        OrderReload();
        BuildOrderMenu();
        if (g_orderSel[0])
            ShMenuSelectRow(g_orderMenu, g_orderSel);
        InterlockedExchange(&g_orderBusy, 0);
    }
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

/* The keys that go back a page: both (the default, and what the menu did
 * before this row existed), Esc alone, Backspace alone. The words are the key
 * names themselves - they are what is printed on the keyboard, so no language
 * has anything to translate.
 *
 * The menu is told the moment it changes, because this is the one setting whose
 * effect IS the menu: the key a press leaves a page on, the key the menu takes
 * from the game while it is up, and the root's hint line that names it, all
 * follow this value. The key left out is the player's and the game keeps
 * receiving it. The row's own value comes from the same ini key the menu loads
 * at start up, so the two cannot disagree. */
static const char *g_backOpts[] = { "Esc / Backspace", "Esc", "Backspace" };
#define BACK_OPTS 3

static void OnBackKey(uint32_t menu, uint32_t item, int value, void *user) {
    (void)item;
    (void)user;
    if (ShConfigSetInt("Settings", "backkey", value)) {
        ShMenuSetBackKeys(value);
        ReportSaved(menu);
    }
}

static void BuildBackKeyRow(uint32_t parent) {
    int idx = (int)ShConfigGetInt("Settings", "backkey", 0);

    if (idx < 0 || idx >= BACK_OPTS) idx = 0;
    ShMenuList(parent, "@settings.backkey", g_backOpts, BACK_OPTS, idx,
               OnBackKey, NULL);
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
    size_t used;
    int n;

    /* One line, by decision of 2026-09-17: the switches page says when its
     * changes land and nothing else. The [plugins] explanation used to be
     * a second line here; it is the row label and the page's own rows that
     * carry what a switch does, and the note is still in the table for any
     * page that wants it. The blacklist notice stays - that one is not an
     * explanation, it is which plugins the mode in play has taken away. */
    ShPluginBlacklistNotice(notice, sizeof(notice));
    n = snprintf(text, sizeof(text), "%s", ShLang("@settings.restart"));
    used = n > 0 ? (size_t)n : 0;
    /* One exception to that single line: a list with nothing in it. The rows
     * are the plugins the loader found, so with plugins\ empty the page is
     * blank - and a blank page reads as a menu that stopped working, which is
     * what it looked like on 2026-09-23. Said here rather than as a row,
     * because a row in that list would read as a plugin. */
    if (g_nplugins == 0 && used + 2 < sizeof(text)) {
        n = snprintf(text + used, sizeof(text) - used, "\n%s",
                     ShLang("@settings.plugins.empty"));
        if (n > 0) used += (size_t)n;
    }
    if (notice[0] && used + 2 < sizeof(text))
        snprintf(text + used, sizeof(text) - used, "\n%s", notice);
    ShMenuHint(g_pluginMenu, text);
}

static char g_modeLast[160];

/* The bottom line of that page: which play mode the framework has. The
 * notice above it says which plugins the mode took away, and this says
 * what the mode is - the two questions a player (or a bug report) asks in
 * that order. When no mode has been read the line says that in three
 * words. It used to print what had been read instead - the object number,
 * or that there is none yet - which is a sentence at the bottom of a
 * page, and it is the normal state in the front end: the state most
 * people would ever see. Nothing is lost by the change: the same evidence
 * is what scripthook_playmode.log records line by line, and
 * ShPlayModeEvidence still hands it to anything that asks. */
static void SetModeLine(void) {
    char line[160];
    int mode = ShSelectedPlayMode();

    if (mode != SH_PLAYMODE_NONE)
        snprintf(line, sizeof(line), "%s: %s", ShLang("@settings.mode"),
                 ShLang(ShPlayModeName(mode)));
    else
        snprintf(line, sizeof(line), "%s: %s", ShLang("@settings.mode"),
                 ShLang("@settings.mode.none"));
    if (!strcmp(line, g_modeLast)) return;
    snprintf(g_modeLast, sizeof(g_modeLast), "%s", line);
    ShMenuStatus(g_pluginMenu, line);
}

static DWORD WINAPI BlHintThread(LPVOID p) {
    char notice[96];

    (void)p;
    for (;;) {
        Sleep(1000);
        if (!g_pluginMenu) continue;
        RefreshPluginMenu();
        notice[0] = 0;
        ShPluginBlacklistNotice(notice, sizeof(notice));
        if (!strcmp(notice, g_blLast)) {
            SetModeLine();          /* the mode can change on its own */
            continue;
        }
        snprintf(g_blLast, sizeof(g_blLast), "%s", notice);
        SetPluginHint();
        SetModeLine();
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
    return stage == SH_STAGE_BOOT ? "@cpu.stage.boot"
         : stage == SH_STAGE_WINDOW ? "@cpu.stage.window"
         : "@cpu.stage.play";
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

    /* The English literal stays here on purpose: ShTextFormat checks a
     * translation against it. Only the template is keyed. */
    ShTextFormat(text, sizeof(text),
                 "Now: %s - cores %s - priority %s",
                 ShLang("@cpu.now"),
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
    ShMenuHint(g_modMenu, "@settings.hint");
    /* The Plugins page shows the same note plus the mode blacklist line,
     * which the thread started below keeps up to date. */
    SetPluginHint();
    /* The order page: one sentence, because the rows carry the rest -
     * the number is the place, left and right move it. */
    ShMenuHint(g_orderMenu, "@settings.order.hint");

    /* The CPU page: one sentence - what the page does, and that it acts
     * from the next launch on. The one thing worth a second line is the
     * E-core caveat, and only on a machine where that dial cannot apply
     * at all; anywhere else it would explain nothing. What the dials are
     * doing right now is the line at the bottom of the page. */
    used = (size_t)snprintf(hint, sizeof(hint), "%s",
                            ShLang("@cpu.hint"));
    if (ShCpuGetStatus(&cf)) {
        /* Two dials can be picked and then do nothing at all, and each of
         * them needs a line of its own - the row alone would read as "set
         * and quietly ignored": "E-cores off" on a CPU with no E-cores,
         * and the efficiency mode on anything but Windows 11, where the
         * call exists but the level it names does not. */
        line[0] = 0;
        if (cf.ecoreState == SH_CF_NA_NOT_INTEL)
            snprintf(line, sizeof(line), "%s",
                     ShLang("@cpu.hint.ecore.na"));
        else if (cf.ecoreState == SH_CF_NA_NO_ECORE)
            snprintf(line, sizeof(line), "%s",
                     ShLang("@cpu.hint.ecore.none"));
        else if (cf.ecoreState == SH_CF_FAILED)
            snprintf(line, sizeof(line), "%s",
                     ShLang("@cpu.hint.ecore.failed"));
        if (line[0] && used + 2 < sizeof(hint)) {
            snprintf(hint + used, sizeof(hint) - used, "\n%s", line);
            used = strlen(hint);
            line[0] = 0;
        }
        if (cf.ecoBoot) {
            if (cf.eco == SH_ECO_NA)
                snprintf(line, sizeof(line), "%s",
                         ShLang("@cpu.hint.eco.na"));
            else if (cf.eco == SH_ECO_FAILED)
                snprintf(line, sizeof(line), "%s",
                         ShLang("@cpu.hint.eco.failed"));
        }
        if (line[0] && used + 2 < sizeof(hint))
            snprintf(hint + used, sizeof(hint) - used, "\n%s", line);
    }
    ShMenuHint(g_cpuMenu, hint);
}

/* ---- the About page ---------------------------------------------- */

/* What this build is, one fact a row: which version, who wrote the
 * original, who modded it, where to ask, and the address of the source
 * itself. The version is formatted in through the template check every
 * other formatted line goes through, so a translation cannot put a %s
 * where nothing is passed. The rows do nothing when they are chosen:
 * the page is here to be read. */

static void OnAboutInfo(uint32_t menu, uint32_t item, int value,
                        void *user) {
    (void)menu; (void)item; (void)value; (void)user;
}

/* One row. `value` is NULL for a line that carries no value of its own. */
static void AboutRow(uint32_t menu, const char *key, const char *value) {
    char line[256];
    const char *tr = ShLang(key);
    const char *en = ShTextEnUS(NULL, key);

    if (value)
        ShTextFormat(line, sizeof(line), en ? en : tr, tr, value);
    else
        snprintf(line, sizeof(line), "%s", tr);
    ShMenuAction(menu, line, OnAboutInfo, NULL);
}

/* The game build's own name, from the loader (loader.c): two numbers read
 * out of the PE header, and whether they are a build this framework has run
 * on. -1 when there was nothing to read, and then the row is not made: the
 * About page is a list of facts, and "no idea" is not one of them. */
extern int ShGameBuildText(char *buf, int cap);

static void BuildAboutMenu(void) {
    char build[32];
    int known;

    if (!g_aboutMenu) return;
    ShMenuClear(g_aboutMenu);
    AboutRow(g_aboutMenu, "@about.version", SH_VERSION);
    /* Under the version: the API's own version, which is the number a plugin is
     * refused for (SH_REQUIRES_API). A player told that a plugin needs a newer
     * ScriptHook reads here what this one offers. */
    {
        char api[16];

        snprintf(api, sizeof(api), "%d", SH_API_VERSION);
        AboutRow(g_aboutMenu, "@about.api", api);
    }
    /* Then the other half of "which build is this": the game's. */
    known = ShGameBuildText(build, (int)sizeof(build));
    if (known >= 0)
        AboutRow(g_aboutMenu,
                 known ? "@about.build.ok" : "@about.build.new", build);
    AboutRow(g_aboutMenu, "@about.author", NULL);
    AboutRow(g_aboutMenu, "@about.modder", NULL);
    AboutRow(g_aboutMenu, "@about.qq", NULL);
    /* The address itself is the row's text, not a label in front of one:
     * there is nothing to translate, so this one row is not keyed - it is
     * SH_REPO, the same string the header defines. */
    ShMenuAction(g_aboutMenu, SH_REPO, OnAboutInfo, NULL);
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
    BuildAboutMenu();
    /* The order page's row labels are the pages' own titles, resolved
     * when the list was taken, so they need reading again in the new
     * language - OrderTick does that on its next pass. */
    g_orderStale = 1;
}

static DWORD WINAPI CpuLineThread(LPVOID p) {
    (void)p;
    for (;;) {
        const char *cur;

        Sleep(1000);
        ShTickPing(SH_TICK_CPULINE);
        cur = ShLangGet();
        if (cur && strcmp(cur, g_langSeen)) {
            snprintf(g_langSeen, sizeof(g_langSeen), "%s", cur);
            BuildHints();
        }
        SetCpuLine();
        OrderTick();
    }
    return 0;
}

/* Register the whole tree. Called from the loader thread after the
 * config has been parsed, so the values and the scan see the real
 * ini. Safe to call once; the guard keeps rebuilds from stacking. */
void ShModSettingsStartup(void) {
    if (g_built) return;
    g_built = 1;

    /* Our default place: [MenuOrder] @settings.page = 0. Written only
     * when the key is missing, so a normal launch does not touch the ini
     * file for nothing - and so a move made on the ordering page sticks
     * instead of being undone here at the next launch. */
    if (ShConfigGetInt("MenuOrder", "@settings.page", -1) < 0)
        ShConfigSetInt("MenuOrder", "@settings.page", 0);

    g_modMenu = ShMenuCreate("@settings.page");
    if (!g_modMenu) return;

    /* Rows sort by the order they are made in: the plugin master switch
     * first, on this page, then the sub pages - the plugin list, then the
     * CPU dials on a page of their own - and About last of all, which is
     * made further down, after the language row. */
    BuildSettings(g_modMenu, g_loaderSettings,
                  (int)(sizeof(g_loaderSettings) / sizeof(g_loaderSettings[0])));
    g_pluginMenu = ShMenuSub(g_modMenu, "@settings.plugins");
    g_cpuMenu    = ShMenuSub(g_modMenu, "@settings.cpu");
    g_orderMenu  = ShMenuSub(g_modMenu, "@settings.order");
    /* Directly under the menu order row: both are about the menu as the player
     * uses it, and the hint line the root shows names the key this picks. */
    BuildBackKeyRow(g_modMenu);

    ScanPlugins();
    BuildSettings(g_cpuMenu, g_cpuSettings,
                  (int)(sizeof(g_cpuSettings) / sizeof(g_cpuSettings[0])));
    BuildPluginMenu();
    OrderReload();
    BuildOrderMenu();
    BuildLanguageRow(g_modMenu);
    /* Made after the language row on purpose: a row keeps the place it was
     * made in, and About belongs under everything else on the page. */
    g_aboutMenu = ShMenuSub(g_modMenu, "@about.page");
    BuildAboutMenu();

    /* The hints, then the live line. The note the pages carry is text we
     * composed, so the language it is in is remembered here: the poll
     * thread below rebuilds it when that changes. */
    BuildHints();
    snprintf(g_langSeen, sizeof(g_langSeen), "%s", ShLangGet());
    SetCpuLine();
    {
        HANDLE h = CreateThread(NULL, 0, BlHintThread, NULL, 0, NULL);

        if (!h) ShMenuStatus(g_pluginMenu, "@settings.thread.blacklist");
        else    CloseHandle(h);   /* never waited on */
    }
    {
        HANDLE h = CreateThread(NULL, 0, CpuLineThread, NULL, 0, NULL);

        if (!h) ShMenuStatus(g_cpuMenu, "@settings.thread.cpu");
        else    CloseHandle(h);
    }
}
