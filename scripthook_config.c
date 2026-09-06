/* Path helpers and the main scripthook.ini.
 *
 * The layout every path helper builds is the one the loader
 * uses too:
 *
 *   <gamedir>\scripthook.ini        main config
 *   <gamedir>\logs\                 every log file
 *   <gamedir>\plugins\<name>\       one folder per plugin
 *       <name>.asi
 *       <name>.ini                  the plugin's own config
 *
 * The main config is parsed once by the loader, before any
 * plugin loads. Plugins can query it as well, and keep their
 * own settings in plugins\<name>\<name>.ini (see
 * ShPluginIniPath).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

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

/** <gamedir>\plugins\ (with trailing backslash), created if
 *  missing. This is where .asi plugins live, one folder each. */
SH_API int ShPluginsDir(char *buf, int size) {
    if (!buf || size <= 0) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (snprintf(buf, size, "%splugins\\", GameDir()) < 0) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}

/* Compatibility alias: the folder was historically called
 * "scripts". The name lives on so third-party .asi plugins that
 * resolve ShScriptsDir by GetProcAddress keep working; it returns
 * the very same plugins\ directory. New code should call
 * ShPluginsDir. */
SH_API int ShScriptsDir(char *buf, int size) {
    return ShPluginsDir(buf, size);
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

/** plugins\<name>\<name>.ini, the config file that belongs
 *  beside the plugin of the same name. */
SH_API int ShPluginIniPath(const char *plugin, char *buf, int size) {
    if (!plugin || !buf || size <= 0) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    if (snprintf(buf, size, "%splugins\\%s\\%s.ini",
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

#define CONFIG_MAX  65536u
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
    "; 1 = hide the E-cores: on Intel 12th-gen+ hybrid CPUs the P-cores\n"
    "; are auto-detected (CPUID leaf 0x1A) and the game only sees/runs on\n"
    "; them, up to the lowest cpu_cores P-core threads. Fixes the hang.\n"
    "cpu_ecore_off=0\n"
    "; 1 = keep one logical thread per physical core (drop SMT/HT) on\n"
    "; ANY CPU. General fix when the game hangs even without E-cores,\n"
    "; only with Hyper-Threading on (Intel 12th-gen+, AMD).\n"
    "cpu_ht_off=0\n"
    "; Logical-processor count the game sees / is limited to (0..256).\n"
    "; 0 = keep every detected core (no trim); 1..256 = keep the lowest\n"
    "; cpu_cores of the detected set. Missing or out-of-range = 8.\n"
    "cpu_cores=8\n"
    "\n"
    "[plugins]\n"
    "; One line per plugin folder, 0 disables it.\n"
    "; firstperson=1\n"
    "\n"
    "[Settings]\n"
    "; Menu language: zh_cn = Chinese (default), en = English.\n"
    "Language=zh_cn\n"
    "; Languages the menu language switch offers (comma separated;\n"
    "; the order here is the order shown).\n"
    "Languages=zh_cn,en\n"
    "; Log level (reserved, not yet implemented): none/error/warn/info/debug\n"
    "; LogLevel=info\n"
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
    "Vehicles = 召唤载具\n"
    "; Display names of the selectable languages ([Settings] Languages).\n"
    "zh_cn = 简体中文\n"
    "en = English\n"
    "\n"
    "[zh_cn.Chaos]\n"
    "Enabled = 混沌开关\n"
    "Seconds between = 每次间隔\n"
    "Effect seconds = 特效时长\n"
    "Roll one now = 立即随机一个\n"
    "Clear active = 清除当前特效\n"
    "off, %d effects = 已关闭，%d 个效果\n"
    "running = 运行中\n"
    "\n"
    "[zh_cn.Field of view]\n"
    "Override = 覆盖游戏视野范围设置\n"
    "Vertical fov = 调整垂直视野范围（度）\n"
    "Back to the game default = 恢复游戏默认视野范围\n"
    "the camera is not ready = 游戏视野尚未就绪\n"
    "%.0f deg, game default %.0f = %.0f 度，游戏默认 %.0f\n"
    "off, game is %.0f deg = 已关闭，游戏当前 %.0f 度\n"
    "\n"
    "[zh_cn.First person]\n"
    "Enabled = 第一人称\n"
    "First-person view: hide head, adjust eye height and distance. = 第一人称视角，可隐藏头部，可调整视角前后高低\n"
    "Hide head = 隐藏头部\n"
    "Forward cm = 前后调整(cm)\n"
    "Height cm = 高低调整(cm)\n"
    "ADS settle ms = 开镜速度(ms)\n"
    "Dump UI to log = 转储UI信息到日志（调试用）\n"
    "\n"
    "[zh_cn.Free camera]\n"
    "Detached = 分离\n"
    "Speed = 速度\n"
    "Recentre on game camera = 对准游戏相机\n"
    "\n"
    "[zh_cn.Vehicles]\n"
    "Note: the first summon may take a moment. = 注意：首次召唤需稍等片刻，载具才会出现。\n"
    "spawning... = 召唤中…\n"
    "spawned, %d this session = 已生成，本次会话 %d 辆\n"
    "nothing appeared = 未出现（可能仍在生成）\n"
    "no player position = 无法获取玩家位置\n"
    "Off road bike, civilian = 越野摩托车（民用）\n"
    "Tommy bike, civilian = 摩托车（民用）\n"
    "Tommy bike, rebels = 摩托车（反抗军）\n"
    "Alpaca, static, FREEZES ON ENTRY = 羊驼摩托车（勿上车会卡住,仅展示）\n"
    "Tractor, civilian = 拖拉机（民用）\n"
    "4x4, Santa Blanca = 四驱吉普车（圣塔布兰卡）\n"
    "Buggy, Unidad = 越野车（联合军）\n"
    "DXI sedan, civilian = DXI轿车（民用）\n"
    "4x4, Unidad = 四驱吉普车（联合军）\n"
    "SUV, civilian = SUV（民用）\n"
    "Sumitzu car, civilian = 苏米特苏轿车（民用）\n"
    "Minibus, rebels = 小型公交车（反抗军）\n"
    "Minibus, civilian = 小型公交车 （民用）\n"
    "Sumitzu 200GT, civilian = 苏米特苏200GT轿车（民用）\n"
    "Sumitzu hatchback, civilian = 苏米特苏掀背轿车（民用）\n"
    "BLOCK pickup, civilian = BLOCK小货卡（民用）\n"
    "Sumitzu Carry Ace 250 van = 苏米特苏CarryAce250厢式货车（民用）\n"
    "Technical, rebels = 武装小货卡（反抗军）\n"
    "Paranero, Santa Blanca = 白色跑车（圣塔布兰卡）\n"
    "Paranero, Santa Blanca, default = 默认白色跑车（圣塔布兰卡）\n"
    "Landrock armed, Santa Blanca = 武装SUV（圣塔布兰卡）\n"
    "Minibus, civilian, white = 白色小型公交车（民用）\n"
    "Trophy truck, Santa Blanca = 越野卡车（圣塔布兰卡）\n"
    "4x4 armed, Unidad = 武装四驱吉普车（联合军）\n"
    "Chobolet sedan, Santa Blanca = 雪波特轿车（圣塔布兰卡）\n"
    "AMV, Unidad = AMV武装越野车（联合军）\n"
    "Mercedes style sedan, Santa Blanca = 奔驰风格轿车（圣塔布兰卡）\n"
    "Nakahawa pickup, civilian = 中河小货卡（民用）\n"
    "Monster truck, civilian = 怪兽卡车（民用）\n"
    "Monster, unused, FREEZES ON ENTRY = 怪兽卡车DLC同款（勿上车会卡住,仅展示）\n"
    "Decussine sedan, civilian = 德卡辛轿车（圣塔布兰卡）\n"
    "Decussine SUV, Santa Blanca = 德卡辛SUV（圣塔布兰卡）\n"
    "Landrock van, civilian = 陆岩面包车（民用）\n"
    "Decussine 90s, Santa Blanca = 德卡辛90年代款轿车（圣塔布兰卡）\n"
    "HELICOPTER = 直升机（联合军）\n"
    "Decussine SUV, civilian = 德卡辛2 SUV（圣塔布兰卡）\n"
    "Zeus pickup, Santa Blanca = 宙斯小货卡（圣塔布兰卡）\n"
    "MRAP, Unidad = SUV装甲车（联合军）\n"
    "Wooden boat, Last Rites = 小木船\n"
    "Fohd pickup, Unidad = 福特小货卡（联合军）\n"
    "Brubeck tow truck, Los Penitentes = 布鲁贝克拖车（最后的仪式）\n"
    "Brubeck tow truck, rebels = 布鲁贝克拖车（反抗军）\n"
    "Armoured ambulance, cut but driveable = 装甲救护车\n"
    "Dinghy, Santa Blanca = 橡皮艇（圣塔布兰卡）\n"
    "Scoossna 171, plane, Santa Blanca = 斯库斯纳171飞机（圣塔布兰卡）\n"
    "Convoy ambulance, Santa Blanca = 医疗救护车（圣塔布兰卡）\n"
    "Mama Cocha advert truck, Unidad = MamaCocha广告小货卡（联合军）\n"
    "Brubeck oil truck, Los Penitentes = 油车（最后的仪式）\n"
    "APC, Santa Blanca = 装甲运兵车（圣塔布兰卡）\n"
    "APC, Unidad = 装甲运兵车（联合军）\n"
    "GUNSHIP, Unidad = 军用直升机（联合军）\n"
    "Boxcar truck, Santa Blanca = 运输卡车（圣塔布兰卡）\n"
    "Barracks truck, Unidad = 军式卡车（联合军）\n"
    "Boxcar, Santa Blanca = 箱式运输卡车（圣塔布兰卡）\n"
    "Murder disposal truck, Santa Blanca = 运尸卡车（圣塔布兰卡）\n"
    "Rancho Luna advert truck, Santa Blanca = RanchoLuna食物运输车（圣塔布兰卡）\n"
    "Digger, civilian = 推土机（民用）\n"
    "KILLDOZER, armoured = 装甲推土机（民用）\n"
    "Gunboat, Unidad = 机枪快艇（联合军）\n"
    "Gunboat, Santa Blanca = 机枪快艇（圣塔布兰卡）\n"
    "Convoy comms truck, Santa Blanca = 通信运输车（圣塔布兰卡）\n"
    "Yacht, honks at itself, civilian = 游艇（民用）\n"
    "Classic airplane, civilian = 经典运输机（民用）\n"
    "Cossna 172, civilian = 塞斯纳172飞机（圣塔布兰卡）\n"
    "UH-60, Santa Blanca = 黑鹰直升机（圣塔布兰卡）\n";

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
        char *s, *e, *eq, *key;
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

        /* Find the '=' that separates key and value. The first
         * '=' in an unquoted language key may belong to the key
         * itself ("Equal (=)"), so split those on " = " instead.
         * A quoted key ("\"Equal (=)\"") reads verbatim up to its
         * closing quote, so no '=' inside it is ever mistaken for
         * the separator. Plain config rows keep the simple rule. */
        eq = NULL;
        if (IsLangSection(section)) {
            char *qs = s, q = 0;
            while (*qs == ' ' || *qs == '\t') qs++;
            if (*qs == '"' || *qs == '\'') {
                char *c;
                q = *qs;
                c = strchr(qs + 1, q);
                if (c) {
                    char *p = c + 1;
                    while (*p == ' ' || *p == '\t') p++;
                    if (*p == '=') {
                        /* Closing quote ends the key; s moves to
                         * the text inside the quotes and eq points
                         * at the real separator. */
                        s = qs + 1;
                        *c = 0;
                        eq = p;
                    }
                }
            }
            if (!eq) {
                /* Unquoted key: split on " = " (space-equals-space)
                 * so an '=' inside the key is not the separator. */
                char *sp = strstr(s, " = ");
                if (sp) eq = sp + 1;
                else    eq = strchr(s, '=');
            }
        } else {
            eq = strchr(s, '=');
        }
        if (!eq) continue;
        *eq = 0;

        /* key is whatever s points at now: the text after the
         * opening quote for a quoted key, the raw key otherwise.
         * Save it before s is reused for the value below. */
        key = s;

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

        /* eq points at '='; the key is key..eq-1, value is s.
         * Language sections never touch the main entry table, so
         * a large translation set cannot crowd the config out. */
        if (IsLangSection(section)) {
            AddLangEntry(section, key, s);
            continue;
        }
        i = strlen(key);
        if (i >= sizeof(g_entries[g_nentries].key))
            i = sizeof(g_entries[g_nentries].key) - 1;
        memcpy(g_entries[g_nentries].key, key, i);
        g_entries[g_nentries].key[i] = 0;

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
 * anything else (Settings, loader, plugins) is config. */
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

/* ---- write-back ---------------------------------------------- */

/* Refresh or add one in-memory entry so later reads see the
 * value that was just persisted. */
static void SetEntry(const char *section, const char *key,
                     const char *value) {
    CfgEntry *e;
    size_t i;

    for (i = 0; i < (size_t)g_nentries; i++) {
        if (!strcmp(g_entries[i].section, section) &&
            !strcmp(g_entries[i].key, key)) {
            strncpy(g_entries[i].value, value,
                    sizeof(g_entries[i].value) - 1);
            g_entries[i].value[sizeof(g_entries[i].value) - 1] = 0;
            return;
        }
    }
    if (g_nentries >= ENTRIES_MAX) return;
    e = &g_entries[g_nentries++];
    strncpy(e->section, section, sizeof(e->section) - 1);
    e->section[sizeof(e->section) - 1] = 0;
    strncpy(e->key, key, sizeof(e->key) - 1);
    e->key[sizeof(e->key) - 1] = 0;
    strncpy(e->value, value, sizeof(e->value) - 1);
    e->value[sizeof(e->value) - 1] = 0;
}

/* Path of the main scripthook.ini. */
static int IniPath(char *buf, int size) {
    if (snprintf(buf, size, "%sscripthook.ini", GameDir()) < 0)
        return 0;
    return 1;
}

/* One source line: [start, start+contentLen) excludes the line
 * ending. Keeps a pointer into the source buffer. */
typedef struct {
    const char *s;
    size_t n;      /* content length (no CR/LF) */
    size_t total;  /* content + its line ending */
} IniLine;

/* Append a printf-style line to the output buffer, growing it. */
static int OutPrint(char **out, size_t *len, size_t *cap,
                    const char *fmt, ...) {
    va_list ap;
    int need;
    size_t grow;

    va_start(ap, fmt);
    need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) return 0;
    if (*len + (size_t)need + 1 > *cap) {
        grow = (size_t)need + 256;
        {
            char *np = (char *)realloc(*out, *cap + grow);
            if (!np) return 0;
            *out = np;
            *cap += grow;
        }
    }
    va_start(ap, fmt);
    vsnprintf(*out + *len, *cap - *len, fmt, ap);
    va_end(ap);
    *len += (size_t)need;
    return 1;
}

/* Collect the content and total span of one source line. */
static void SplitLine(const char *text, const char *end,
                      const char **lineEnd, IniLine *li) {
    const char *nl = memchr(text, '\n', (size_t)(end - text));
    const char *e = nl ? nl : end;
    li->s = text;
    li->n = (size_t)(e - text);
    if (li->n > 0 && li->s[li->n - 1] == '\r') li->n--;
    li->total = (size_t)((nl ? nl + 1 : e) - text);
    *lineEnd = nl ? nl + 1 : end;
}

/* Leading whitespace of the key area. */
static const char *SkipWs(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

/* Replace the value of key=... inside [section] in the on-disk
 * scripthook.ini. Comments, blank lines and every other section
 * survive byte for byte; a missing key is appended at the end of
 * its section (the section itself is created if absent). The file
 * is treated as opaque bytes, so UTF-8 content is preserved.
 * Returns 1 when the file was rewritten. */
static int IniWriteValue(const char *section, const char *key,
                         const char *value) {
    char path[GAME_DIR_MAX], tmp[GAME_DIR_MAX];
    FILE *f;
    long len;
    char *whole;
    const char *end, *p;
    char curSec[48];
    int curSecSet = 0;
    int inSec = 0;
    int replaced = 0;
    int wrote = 0;
    char *out = NULL;
    size_t outLen = 0, outCap = 0;
    int ok = 0;

    if (!section || !key || !value) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    if (!IniPath(path, sizeof(path))) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }

    f = fopen(path, "rb");
    if (!f) {
        /* A missing main ini means config was never parsed; let
         * LoadConfig write the default first. */
        LoadConfig();
        f = fopen(path, "rb");
        if (!f) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || (unsigned long)len + 1u > CONFIG_MAX + 1u) {
        fclose(f);
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    whole = (char *)malloc((size_t)len + 1);
    if (!whole) {
        fclose(f);
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    if (len > 0 && fread(whole, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(whole);
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    fclose(f);
    whole[len] = 0;

    end = whole + len;
    p = whole;

    /* Single pass over the source lines:
     *   - matching key inside the target section is replaced;
     *   - a missing key is appended just before the header that ends
     *     the section (or at the end of the file when the section is
     *     the last thing in it). A never-seen section is created. */
    if (!out && outCap == 0) {
        /* Initial buffer: source plus room to grow. */
        outCap = (size_t)len + 512;
        out = (char *)malloc(outCap);
        if (!out) {
            free(whole);
            ShSetError(SH_ERR_BAD_ARG);
            return 0;
        }
    }

    while (p < end) {
        IniLine li;
        const char *next;
        const char *q;

        SplitLine(p, end, &next, &li);
        q = SkipWs(li.s, li.s + li.n);

        if (q < li.s + li.n && *q == '[') {
            /* A section header. If the section we are leaving was the
             * target one and its key was never placed, append it here,
             * at the very end of the target section. */
            if (inSec && !replaced) {
                OutPrint(&out, &outLen, &outCap, "%s=%s\n",
                         key, value);
                replaced = 1;
                wrote = 1;
            }
            curSecSet = 1;
            inSec = 0;
            {
                const char *close = memchr(q + 1, ']',
                                           (size_t)((li.s + li.n) - (q + 1)));
                if (close) {
                    size_t n = (size_t)(close - (q + 1));
                    if (n >= sizeof(curSec)) n = sizeof(curSec) - 1;
                    memcpy(curSec, q + 1, n);
                    curSec[n] = 0;
                    inSec = (strcmp(curSec, section) == 0);
                }
            }
        } else if (inSec && !replaced) {
            /* Inside the target section, the key is still missing. */
            if (q < li.s + li.n && *q != ';' && *q != '#') {
                const char *eq = memchr(q, '=',
                                        (size_t)((li.s + li.n) - q));
                if (eq) {
                    size_t klen = (size_t)(eq - q);
                    while (klen > 0 &&
                           (q[klen - 1] == ' ' || q[klen - 1] == '\t'))
                        klen--;
                    if (klen == strlen(key) && !strncmp(q, key, klen)) {
                        /* Replace this line: key=value + its ending. */
                        OutPrint(&out, &outLen, &outCap, "%s=%s",
                                 key, value);
                        if (li.s + li.n < p + li.total) {
                            const char *nlp = li.s + li.n;
                            size_t nlLen = (size_t)((p + li.total) - nlp);
                            if (outLen + nlLen + 1 > outCap) {
                                char *np = (char *)realloc(
                                    out, outCap + nlLen + 1);
                                if (!np) goto done;
                                out = np;
                                outCap += nlLen + 1;
                            }
                            memcpy(out + outLen, nlp, nlLen);
                            outLen += nlLen;
                        } else {
                            out[outLen++] = '\n';
                        }
                        replaced = 1;
                        wrote = 1;
                        p = next;
                        continue;
                    }
                }
            }
        }

        /* Verbatim copy of the line. */
        if (outLen + li.total + 1 > outCap) {
            char *np = (char *)realloc(out, outCap + li.total + 1);
            if (!np) goto done;
            out = np;
            outCap += li.total + 1;
        }
        memcpy(out + outLen, p, li.total);
        outLen += li.total;
        p = next;
    }

    if (!replaced) {
        /* The key was never placed: the target section was the last
         * thing in the file, or it never appeared at all. */
        if (outLen > 0 && out[outLen - 1] != '\n')
            OutPrint(&out, &outLen, &outCap, "\n");
        if (!curSecSet || !inSec) {
            /* A new section must be created. */
            if (outLen > 0)
                OutPrint(&out, &outLen, &outCap, "\n");
            OutPrint(&out, &outLen, &outCap, "[%s]\n", section);
        }
        OutPrint(&out, &outLen, &outCap, "%s=%s\n", key, value);
        wrote = 1;
    }

    free(whole);
    whole = NULL;

    /* Write back via a temp file, then move it into place. */
    if (!wrote) goto done;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) < 0) goto done;
    f = fopen(tmp, "wb");
    if (!f) goto done;
    if (outLen > 0 && fwrite(out, 1, outLen, f) != outLen) {
        fclose(f);
        remove(tmp);
        goto done;
    }
    if (fclose(f) != 0) {
        remove(tmp);
        goto done;
    }
    if (!MoveFileExA(tmp, path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(tmp);
        goto done;
    }
    ok = 1;

done:
    free(out);
    if (whole) free(whole);
    ShSetError(ok ? SH_OK : SH_ERR_BAD_ARG);
    return ok;
}

/** Write a string value back to scripthook.ini and refresh the
 *  in-memory table. */
SH_API int ShConfigSetStr(const char *section, const char *key,
                          const char *value) {
    if (!section || !key || !value) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    LoadConfig();
    if (!IniWriteValue(section, key, value)) return 0;
    SetEntry(section, key, value);
    return 1;
}

SH_API int ShConfigSetInt(const char *section, const char *key,
                          int value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    return ShConfigSetStr(section, key, buf);
}

SH_API int ShConfigSetBool(const char *section, const char *key,
                           int value) {
    return ShConfigSetStr(section, key, value ? "1" : "0");
}
