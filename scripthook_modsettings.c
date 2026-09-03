/* Mod settings: a built-in root-menu page that edits the main
 * scripthook.ini at runtime.
 *
 * The engine reads every key it exposes here only at startup:
 *   - [loader] load_plugins / cpu_ecore_off / cpu_ht_off / cpu_cores
 *   - [plugins]  one toggle per plugins\<name>\<name>.asi
 *   - [Settings] Language (menu language)
 * Each page carries a single hint line noting that changes need a
 * game restart to take effect, instead of marking every row.
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
    const char *key;      /* ini key, e.g. "load_plugins" */
    const char *label;    /* menu label == translation key */
    int  isNumber;        /* a number row (cpu_cores) not a toggle */
    float lo, hi, step;
    int  def;             /* fallback when the key is missing */
} Setting;

static const Setting g_loaderSettings[] = {
    { "loader", "load_plugins",
      "Load all plugins", 0, 0, 0, 0, 1 },
    { "loader", "cpu_ecore_off",
      "Hide E-cores", 0, 0, 0, 0, 0 },
    { "loader", "cpu_ht_off",
      "Drop SMT/HT", 0, 0, 0, 0, 0 },
    { "loader", "cpu_cores",
      "Logical cores kept", 1, 0, 256, 1, 8 },
};

/* ---- menu handles ---------------------------------------------- */

static uint32_t g_modMenu = 0;     /* the Mod settings page        */
static uint32_t g_loaderMenu = 0;  /* [loader] rows                */
static uint32_t g_pluginMenu = 0;  /* [plugins] rows               */
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

static void OnLanguage(uint32_t menu, uint32_t item, int value,
                       void *user) {
    const char *const *opts = (const char *const *)user;
    (void)item;
    if (!opts || value < 0 || value >= LANG_MAX) return;
    if (!opts[value] || !opts[value][0]) return;
    if (ShConfigSetStr("Settings", "Language", opts[value]))
        ReportSaved(menu);
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

static void BuildLoaderMenu(void) {
    size_t i;

    for (i = 0; i < sizeof(g_loaderSettings) / sizeof(g_loaderSettings[0]);
         i++) {
        const Setting *s = &g_loaderSettings[i];
        if (s->isNumber) {
            int cur = ShConfigGetInt(s->section, s->key, s->def);
            ShMenuNumber(g_loaderMenu, s->label, (float)cur,
                         s->lo, s->hi, s->step, OnNumber, (void *)s);
        } else {
            int cur = ShConfigGetBool(s->section, s->key, s->def);
            ShMenuToggle(g_loaderMenu, s->label, cur, OnLoaderBool,
                         (void *)s);
        }
    }
}

/* Every plugin switch is a [plugins] toggle the loader's next scan
 * honours. Rows show the plugin folder name only; the "restart
 * needed" note lives once on the menu hint line, not on every row. */
static void BuildPluginMenu(void) {
    int i;

    for (i = 0; i < g_nplugins; i++) {
        const char *name = g_plugins[i];
        int cur = ShConfigGetBool("plugins", name, 1);
        ShMenuToggle(g_pluginMenu, name, cur, OnPlugin, (void *)name);
    }
}

/* The language switch options come from [Settings] Languages, a
 * comma-separated list that must outlive the IT_LIST (the option
 * pointers are borrowed). "zh_cn,en" is the default when the key is
 * absent, so the list order is exactly the order the menu shows. */
static char        g_langNames[LANG_MAX][16];
static const char *g_langOpts[LANG_MAX];
static int         g_nLangs = 0;

static void LoadLanguages(void) {
    char raw[160];
    const char *p;
    int n = 0;

    g_nLangs = 0;
    if (ShConfigGetStr("Settings", "Languages", "zh_cn,en",
                       raw, sizeof(raw)) && raw[0]) {
        p = raw;
        while (*p && n < LANG_MAX) {
            const char *comma = strchr(p, ',');
            size_t len = comma ? (size_t)(comma - p) : strlen(p);
            size_t lead = 0;

            while (lead < len &&
                   (p[lead] == ' ' || p[lead] == '\t'))
                lead++;
            while (len > lead &&
                   (p[len - 1] == ' ' || p[len - 1] == '\t'))
                len--;
            if (len > lead) {
                len -= lead;
                if (len >= sizeof(g_langNames[n]))
                    len = sizeof(g_langNames[n]) - 1;
                memcpy(g_langNames[n], p + lead, len);
                g_langNames[n][len] = 0;
                g_langOpts[n] = g_langNames[n];
                n++;
            }
            if (!comma) break;
            p = comma + 1;
        }
    }
    if (n == 0) {   /* fallback: keep the documented default */
        strncpy(g_langNames[0], "zh_cn", sizeof(g_langNames[0]) - 1);
        g_langNames[0][sizeof(g_langNames[0]) - 1] = 0;
        g_langOpts[0] = g_langNames[0];
        n = 1;
    }
    g_nLangs = n;
}

static void BuildLanguageRow(uint32_t parent) {
    const char *cur = ShLangGet();
    int idx = 0, i;

    LoadLanguages();
    if (cur) {
        for (i = 0; i < g_nLangs; i++)
            if (!strcmp(cur, g_langOpts[i])) { idx = i; break; }
    }
    if (g_nLangs > 0)
        ShMenuList(parent, "Menu language",
                   g_langOpts, g_nLangs, idx, OnLanguage,
                   (void *)g_langOpts);
}

/* Register the whole tree. Called from the loader thread after the
 * config has been parsed, so the values and the scan see the real
 * ini. Safe to call once; the guard keeps rebuilds from stacking. */
void ShModSettingsStartup(void) {
    if (g_built) return;
    g_built = 1;

    /* Pin our row first: [MenuOrder] Mod settings = 0. Write it only
     * when it is not already pinned, so a normal launch does not
     * touch the ini file for nothing. */
    if (ShConfigGetInt("MenuOrder", "Mod settings", 1000) != 0)
        ShConfigSetInt("MenuOrder", "Mod settings", 0);

    g_modMenu = ShMenuCreate("Mod settings");
    if (!g_modMenu) return;

    g_loaderMenu = ShMenuSub(g_modMenu, "Startup & core");
    g_pluginMenu = ShMenuSub(g_modMenu, "Plugins");

    ScanPlugins();
    BuildLoaderMenu();
    BuildPluginMenu();
    BuildLanguageRow(g_modMenu);

    /* One hint per page is enough: loader and plugins rows only act
     * on the next launch, as does the language switch. The loader
     * page adds a second line: the core-count cap does nothing on its
     * own, it only trims when an E-core/SMT switch above is on.
     * The strings are pre-translated here (capture translates the
     * stored hint again, a no-op for already-localised text). */
    {
        char hint[192];
        snprintf(hint, sizeof(hint), "%s",
                 ShLang("These changes take effect after a game restart."));
        ShMenuHint(g_modMenu, hint);
        ShMenuHint(g_pluginMenu, hint);

        snprintf(hint, sizeof(hint), "%s\n%s",
                 ShLang("These changes take effect after a game restart."),
                 ShLang("The core limit applies only when an E-core "
                        "or SMT switch above is enabled."));
        ShMenuHint(g_loaderMenu, hint);
    }
}
