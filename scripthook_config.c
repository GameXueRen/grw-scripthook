/* Path helpers and the main scripthook.ini.
 *
 * The layout every path helper builds is the one the loader
 * uses too:
 *
 *   <gamedir>\scripthook.ini        main config
 *   <gamedir>\logs\                 every log file
 *   <gamedir>\scripts\<name>\       one folder per plugin
 *       <name>.asi
 *       <name>.ini                  the plugin's own config
 *
 * The main config is parsed once by the loader, before any
 * plugin loads. Plugins can query it as well, and keep their
 * own settings in scripts\<name>\<name>.ini (see
 * ShPluginIniPath).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SH_BUILD 1
#include "scripthook.h"

extern void ShSetError(int err);

/* ---- paths -------------------------------------------------- */

#define GAME_DIR_MAX MAX_PATH

static char g_gameDir[GAME_DIR_MAX];
static int  g_dirInit = 0;

/* The folder holding GRW.exe, with a trailing backslash.
 * Anchored to the module file: the game is free to change
 * the working directory. */
static const char *GameDir(void) {
    char *slash;

    if (g_dirInit) return g_gameDir;
    g_dirInit = 1;
    if (!GetModuleFileNameA(NULL, g_gameDir, GAME_DIR_MAX)) {
        g_gameDir[0] = 0;
        return g_gameDir;
    }
    slash = strrchr(g_gameDir, '\\');
    if (slash) slash[1] = 0;
    else g_gameDir[0] = 0;
    return g_gameDir;
}

/** The folder containing GRW.exe, no trailing backslash. */
SH_API int ShGameDir(char *buf, int size) {
    const char *d = GameDir();
    size_t len;

    if (!buf || size <= 0) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    len = strlen(d);
    if (len && d[len - 1] == '\\') len--;
    if (len >= (size_t)size) len = (size_t)size - 1;
    memcpy(buf, d, len);
    buf[len] = 0;
    ShSetError(SH_OK);
    return 1;
}

/** <gamedir>\scripts\, created if missing. */
SH_API int ShScriptsDir(char *buf, int size) {
    if (!buf || size <= 0) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (snprintf(buf, size, "%sscripts\\", GameDir()) < 0) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}

/** <gamedir>\logs\<name>, created if missing. The name may
 *  contain a subfolder ("ui/firstperson.log"). */
SH_API int ShLogPath(const char *name, char *buf, int size) {
    char logs[GAME_DIR_MAX];

    if (!name || !buf || size <= 0) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    if (snprintf(logs, sizeof(logs), "%slogs", GameDir()) < 0) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    CreateDirectoryA(logs, NULL);
    if (snprintf(buf, size, "%s\\%s", logs, name) < 0) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}

/** scripts\<name>\<name>.ini, the config file that belongs
 *  beside the plugin of the same name. */
SH_API int ShPluginIniPath(const char *plugin, char *buf, int size) {
    if (!plugin || !buf || size <= 0) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    if (snprintf(buf, size, "%sscripts\\%s\\%s.ini",
                 GameDir(), plugin, plugin) < 0) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}

/* ---- main config --------------------------------------------- */

#define CONFIG_MAX  16384u
#define ENTRIES_MAX 256

typedef struct {
    char section[48];
    char key[64];
    char value[128];
} CfgEntry;

static char    g_config[CONFIG_MAX];
static CfgEntry g_entries[ENTRIES_MAX];
static int     g_nentries = 0;
static int     g_configReady = 0;

/* The default written on first launch, so the schema is
 * visible without hunting for it. */
static const char *DEFAULT_CONFIG =
    "; GRW ScriptHook main config\n"
    "; The master switch for every big feature.\n"
    "\n"
    "[loader]\n"
    "; 0 refuses to load any plugin this launch.\n"
    "load_plugins=1\n"
    "\n"
    "[plugins]\n"
    "; One line per plugin folder, 0 disables it.\n"
    "; firstperson=1\n"
    "\n"
    "[logging]\n"
    "; future: log level (none, error, warn, info, debug)\n"
    "; level=info\n";

static void WriteDefaultConfig(const char *path) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(DEFAULT_CONFIG, f);
        fclose(f);
    }
}

/* A deliberately small INI parser: sections, key=value,
 * # and ; comments, quoted values, trailing comments are
 * stripped only when separated by whitespace. */
static void ParseConfig(const char *text) {
    const char *p = text;
    char section[48] = "";

    while (*p) {
        char line[512];
        char *s, *e, *eq;
        size_t i = 0;

        while (*p && *p != '\n' && *p != '\r' && i < sizeof(line) - 1)
            line[i++] = *p++;
        line[i] = 0;
        if (*p == '\r') p++;
        if (*p == '\n') p++;

        s = line;
        while (*s == ' ' || *s == '\t') s++;
        e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        *e = 0;

        if (!*s || *s == ';' || *s == '#') continue;

        if (*s == '[') {
            char *c = strchr(s, ']');
            if (!c) continue;
            *c = 0;
            i = strlen(s + 1);
            if (i >= sizeof(section)) i = sizeof(section) - 1;
            memcpy(section, s + 1, i);
            section[i] = 0;
            continue;
        }

        eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;

        /* trim the key */
        e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        *e = 0;

        /* trim the value, drop quotes and a trailing comment */
        s = eq + 1;
        while (*s == ' ' || *s == '\t') s++;
        e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        *e = 0;
        if (*s == '"' || *s == '\'') {
            size_t l = strlen(s);
            if (l > 1 && s[l - 1] == *s) s[l - 1] = 0;
            s++;
        }

        if (!*s) continue;
        if (g_nentries >= ENTRIES_MAX) continue;

        i = strlen(section);
        if (i >= sizeof(g_entries[g_nentries].section))
            i = sizeof(g_entries[g_nentries].section) - 1;
        memcpy(g_entries[g_nentries].section, section, i);
        g_entries[g_nentries].section[i] = 0;

        /* eq now points at '=', so the key is line..eq-1 */
        {
            char *k = line;
            while (*k == ' ' || *k == '\t') k++;
            i = strlen(k);
            if (i >= sizeof(g_entries[g_nentries].key))
                i = sizeof(g_entries[g_nentries].key) - 1;
            memcpy(g_entries[g_nentries].key, k, i);
            g_entries[g_nentries].key[i] = 0;
        }

        i = strlen(s);
        if (i >= sizeof(g_entries[g_nentries].value))
            i = sizeof(g_entries[g_nentries].value) - 1;
        memcpy(g_entries[g_nentries].value, s, i);
        g_entries[g_nentries].value[i] = 0;

        g_nentries++;
    }
}

static void LoadConfig(void) {
    char path[GAME_DIR_MAX];
    FILE *f;
    size_t n;

    if (g_configReady) return;
    g_configReady = 1;

    if (snprintf(path, sizeof(path), "%sscripthook.ini",
                 GameDir()) < 0)
        return;
    f = fopen(path, "rb");
    if (!f) {
        /* First launch: write the default so the schema is
         * visible, then parse it. */
        WriteDefaultConfig(path);
        f = fopen(path, "rb");
    }
    if (!f) return;
    n = fread(g_config, 1, sizeof(g_config) - 1, f);
    fclose(f);
    g_config[n] = 0;
    ParseConfig(g_config);
}

static const char *FindEntry(const char *section, const char *key) {
    int i;
    for (i = 0; i < g_nentries; i++)
        if (!strcmp(g_entries[i].section, section) &&
            !strcmp(g_entries[i].key, key))
            return g_entries[i].value;
    return NULL;
}

/** Parse scripthook.ini now. The loader calls this before
 *  plugins load; harmless to call more than once. */
SH_API void ShConfigInit(void) {
    LoadConfig();
}

/** Integer setting from the main config; def when missing. */
SH_API int ShConfigGetInt(const char *section, const char *key,
                          int def) {
    const char *v;
    char *end;
    long r;

    if (!section || !key) { ShSetError(SH_ERR_BAD_ARG); return def; }
    LoadConfig();
    v = FindEntry(section, key);
    if (!v) { ShSetError(SH_OK); return def; }
    r = strtol(v, &end, 0);
    if (end == v) { ShSetError(SH_OK); return def; }
    ShSetError(SH_OK);
    return (int)r;
}

/** Boolean setting: 1/0, true/false, yes/no, on/off. */
SH_API int ShConfigGetBool(const char *section, const char *key,
                           int def) {
    const char *v;

    if (!section || !key) { ShSetError(SH_ERR_BAD_ARG); return def; }
    LoadConfig();
    v = FindEntry(section, key);
    if (!v) { ShSetError(SH_OK); return def; }
    if (!_stricmp(v, "1") || !_stricmp(v, "true") ||
        !_stricmp(v, "yes") || !_stricmp(v, "on"))
        return 1;
    if (!_stricmp(v, "0") || !_stricmp(v, "false") ||
        !_stricmp(v, "no") || !_stricmp(v, "off"))
        return 0;
    ShSetError(SH_OK);
    return def;
}

/** String setting; returns 1 and copies the value (or def).
 *  ShErrorString explains a bad argument. */
SH_API int ShConfigGetStr(const char *section, const char *key,
                          const char *def, char *out, int size) {
    const char *v;
    size_t n;

    if (!out || size <= 0) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (!section || !key) {
        ShSetError(SH_ERR_BAD_ARG);
        out[0] = 0;
        return 0;
    }
    LoadConfig();
    v = FindEntry(section, key);
    if (!v) v = def ? def : "";
    n = strlen(v);
    if (n >= (size_t)size) n = (size_t)size - 1;
    memcpy(out, v, n);
    out[n] = 0;
    ShSetError(SH_OK);
    return 1;
}
