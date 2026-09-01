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

/* Forward declarations: the parser, the language tables and the
 * lookup helpers are defined below in an order that is easy to
 * read, so the cross calls get a prototype up front. */
static void LoadConfig(void);
static void ResolveLanguage(void);
static void PeekLanguage(const char *text);
static int  IsLangSection(const char *sec);
static void AddLangEntry(const char *section, const char *key,
                         const char *value);

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
    "; level=info\n"
    "\n"
    "[Settings]\n"
    "; Menu language: zh_cn = Chinese (default), en = English.\n"
    "Language=zh_cn\n"
    "\n"
    "; ------------------------------------------------------------\n"
    "; Translation tables. One section per language (the [Settings]\n"
    "; Language value picks which one), plus optional per-menu\n"
    "; subsections [lang.<menu title>] to give the same English word\n"
    "; different translations in different menus. Keys are the menu\n"
    "; labels verbatim; values are the localized text. Save this file\n"
    "; as UTF-8. A missing key falls back to English.\n"
    "; NOTE: <menu title> is the menu's display title (the string the\n"
    "; plugin passed to ShMenuCreate), NOT the plugin folder name.\n"
    "; Deeper menus use a dotted path: [zh_cn.A.B] then falls back to\n"
    "; [zh_cn.A] then [zh_cn].\n"
    "; ------------------------------------------------------------\n"
    "\n"
    "[zh_cn]\n"
    "SCRIPTHOOK = 模组菜单\n"
    "; Control hints shown under every menu title.\n"
    "F4 toggle menu, Enter select, ESC back = F4 打开/关闭 菜单，回车选择，ESC 返回\n"
    "\xE2\x86\x91 \xE2\x86\x93 or W/S select, \xE2\x86\x90 \xE2\x86\x92 or A/D adjust = \xE2\x86\x91 \xE2\x86\x93 或 W/S 上下选择，\xE2\x86\x90 \xE2\x86\x92 或 A/D 左右调整数值\n"
    "on = 开\n"
    "off = 关\n"
    "; The root menu's rows and every submenu title also read from\n"
    "; the global table, so they live here too.\n"
    "Chaos = 混沌模式\n"
    "Field of view = 视野\n"
    "First person = 第一人称\n"
    "Free camera = 自由视角\n"
    "Vehicles = 载具\n"
    "\n"
    "[zh_cn.Chaos]\n"
    "Enabled = 混沌开关\n"
    "Seconds between = 每次间隔\n"
    "Effect seconds = 特效时长\n"
    "Roll one now = 立即随机一个\n"
    "Clear active = 清除当前特效\n"
    "\n"
    "[zh_cn.Field of view]\n"
    "Override = 覆盖\n"
    "Vertical fov = 垂直视野\n"
    "Back to the game default = 恢复游戏默认\n"
    "\n"
    "[zh_cn.First person]\n"
    "Enabled = 第一人称\n"
    "First-person view: hide head, adjust eye height and distance. = 第一人称视角，可隐藏头部，可调整视角前后高低\n"
    "Hide head = 隐藏头部\n"
    "Forward cm = 前移(厘米)\n"
    "Height cm = 高度(厘米)\n"
    "ADS settle ms = ADS稳定(毫秒)\n"
    "Dump UI to log = 转储UI到日志\n"
    "\n"
    "[zh_cn.Free camera]\n"
    "Detached = 分离\n"
    "Speed = 速度\n"
    "Recentre on game camera = 对准游戏相机\n"
    "\n"
    "[zh_cn.Vehicles]\n"
    "; Vehicle names are dynamic data, nothing to translate here.\n";

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

    PeekLanguage(text);
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

        /* eq points at '='; the key is line..eq-1, value is s.
         * Language sections never touch the main entry table, so
         * a large translation set cannot crowd the config out. */
        {
            char *k = line;
            while (*k == ' ' || *k == '\t') k++;
            if (IsLangSection(section)) {
                AddLangEntry(section, k, s);
                continue;
            }
            i = strlen(k);
            if (i >= sizeof(g_entries[g_nentries].key))
                i = sizeof(g_entries[g_nentries].key) - 1;
            memcpy(g_entries[g_nentries].key, k, i);
            g_entries[g_nentries].key[i] = 0;
        }

        if (g_nentries >= ENTRIES_MAX) continue;

        i = strlen(section);
        if (i >= sizeof(g_entries[g_nentries].section))
            i = sizeof(g_entries[g_nentries].section) - 1;
        memcpy(g_entries[g_nentries].section, section, i);
        g_entries[g_nentries].section[i] = 0;

        i = strlen(s);
        if (i >= sizeof(g_entries[g_nentries].value))
            i = sizeof(g_entries[g_nentries].value) - 1;
        memcpy(g_entries[g_nentries].value, s, i);
        g_entries[g_nentries].value[i] = 0;

        g_nentries++;
    }
}

/* ---- localization --------------------------------------------- */

/* No language whitelist: whatever [Settings] Language names is the
 * section prefix. Language=zh reads [zh], [zh.xx], [zh.xx.xx]; a
 * Language=cn reads [cn], [cn.xx]... A section "[<lang>]" or
 * "[<lang>.<scope>]" belongs to the table for that language;
 * anything else (Settings, loader, plugins, logging) is config. */
#define LANGS_MAX       512

typedef struct {
    char lang[16];
    char scope[48];
    char key[128];
    char value[256];
} LangEntry;

static LangEntry g_langs[LANGS_MAX];
static int  g_nlangs = 0;
static char g_langName[16] = "";

static int IsLangSection(const char *sec) {
    size_t n;
    if (!sec || !sec[0] || !g_langName[0]) return 0;
    n = strlen(g_langName);
    if (strncmp(sec, g_langName, n)) return 0;
    return sec[n] == 0 || sec[n] == '.';
}

/* One quick pass for the [Settings] Language value before the
 * tables are built, so IsLangSection knows the prefix to collect. */
static void PeekLanguage(const char *text) {
    const char *p = text;
    int inSettings = 0;

    while (*p) {
        char line[256];
        char *s;
        size_t i = 0;

        while (*p && *p != '\n' && *p != '\r' && i < sizeof(line) - 1)
            line[i++] = *p++;
        line[i] = 0;
        if (*p == '\r') p++;
        if (*p == '\n') p++;

        s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == ';' || *s == '#') continue;

        if (*s == '[') {
            inSettings = !_strnicmp(s, "[Settings]", 10) ? 1 : 0;
            continue;
        }
        if (inSettings && !_strnicmp(s, "Language=", 9)) {
            char *v = s + 9;
            size_t l;
            while (*v == ' ' || *v == '\t') v++;
            l = strlen(v);
            while (l > 0 && (v[l - 1] == ' ' || v[l - 1] == '\t'))
                l--;
            if (l > 1 && (v[0] == '"' || v[0] == '\'') &&
                v[l - 1] == v[0]) {
                v++;
                l -= 2;
            }
            if (l >= sizeof(g_langName)) l = sizeof(g_langName) - 1;
            memcpy(g_langName, v, l);
            g_langName[l] = 0;
            return;
        }
    }
}

static void AddLangEntry(const char *section, const char *key,
                         const char *value) {
    char lang[16], scope[48];
    const char *dot;
    size_t n;
    LangEntry *e;

    if (g_nlangs >= LANGS_MAX) return;
    dot = strchr(section, '.');
    if (dot) {
        n = (size_t)(dot - section);
        if (n >= sizeof(lang)) n = sizeof(lang) - 1;
        memcpy(lang, section, n);
        lang[n] = 0;
        strncpy(scope, dot + 1, sizeof(scope) - 1);
        scope[sizeof(scope) - 1] = 0;
    } else {
        strncpy(lang, section, sizeof(lang) - 1);
        lang[sizeof(lang) - 1] = 0;
        scope[0] = 0;
    }
    e = &g_langs[g_nlangs++];
    strncpy(e->lang, lang, sizeof(e->lang) - 1);
    e->lang[sizeof(e->lang) - 1] = 0;
    strncpy(e->scope, scope, sizeof(e->scope) - 1);
    e->scope[sizeof(e->scope) - 1] = 0;
    strncpy(e->key, key, sizeof(e->key) - 1);
    e->key[sizeof(e->key) - 1] = 0;
    strncpy(e->value, value, sizeof(e->value) - 1);
    e->value[sizeof(e->value) - 1] = 0;
}

/* Direct hit on one entry. */
static const char *LangFind(const char *lang, const char *scope,
                            const char *key) {
    int i;
    for (i = 0; i < g_nlangs; i++) {
        if (strcmp(g_langs[i].lang, lang)) continue;
        if (g_langs[i].scope[0] &&
            strcmp(g_langs[i].scope, scope))
            continue;
        if (!strcmp(g_langs[i].key, key))
            return g_langs[i].value;
    }
    return NULL;
}

/* The [Settings] Language key is the table prefix, already peeked
 * during parsing. An empty or missing value falls back to the
 * documented default so a fresh config still localizes. There is no
 * whitelist: Language=zh reads the [zh] tables, Language=cn the
 * [cn] ones, whatever the string is. */
static void ResolveLanguage(void) {
    if (!g_langName[0]) {
        strncpy(g_langName, "zh_cn", sizeof(g_langName) - 1);
        g_langName[sizeof(g_langName) - 1] = 0;
    }
}

/** Translate without a scope: the [lang] table, then English. */
SH_API const char *ShLang(const char *text) {
    const char *v;

    if (!text) return "";
    LoadConfig();
    if (g_langName[0]) {
        v = LangFind(g_langName, "", text);
        if (v) return v;
    }
    v = LangFind("en", "", text);
    return v ? v : text;
}

/** Translate within a menu's scope, falling through the scoped
 *  and global tables of the active language, then English.
 *  The scope is a dotted title path ("First person.Custom.Height"),
 *  so deeper menus try the full path, then each shorter prefix,
 *  then the global table: [lang.A.B.C] -> [lang.A.B] -> [lang.A]
 *  -> [lang] -> the same for en -> the original text. */
SH_API const char *ShLangFor(const char *scope, const char *text) {
    const char *v;
    char buf[64];

    if (!text) return "";
    if (!scope) scope = "";
    LoadConfig();
    if (g_langName[0]) {
        const char *p = scope;
        for (;;) {
            v = LangFind(g_langName, p, text);
            if (v) return v;
            {
                const char *dot = strrchr(p, '.');
                size_t n;
                if (!dot) break;
                n = (size_t)(dot - scope);
                if (n >= sizeof(buf)) n = sizeof(buf) - 1;
                memcpy(buf, scope, n);
                buf[n] = 0;
                p = buf;
            }
        }
        v = LangFind(g_langName, "", text);
        if (v) return v;
    }
    {
        const char *p = scope;
        for (;;) {
            v = LangFind("en", p, text);
            if (v) return v;
            {
                const char *dot = strrchr(p, '.');
                size_t n;
                if (!dot) break;
                n = (size_t)(dot - scope);
                if (n >= sizeof(buf)) n = sizeof(buf) - 1;
                memcpy(buf, scope, n);
                buf[n] = 0;
                p = buf;
            }
        }
        v = LangFind("en", "", text);
        if (v) return v;
    }
    return text;
}

SH_API const char *ShLangGet(void) {
    LoadConfig();
    return g_langName;
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
    ResolveLanguage();
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
