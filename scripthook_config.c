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
SH_API const char *ShLangForOwned(const char *owner, const char *scope,
                                  const char *text);

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
    "; --- CPU scheduling, by stage of the game's start up ---\n"
    "; One dial per stage. Values:\n"
    ";   0 leave alone     set nothing at all - the system schedules it,\n"
    ";                     its own trimming included\n"
    ";   1 all cores       force every processor the machine has; this is\n"
    ";                     what undoes a trim the system did by itself\n"
    ";   2 SMT off         one thread per physical core\n"
    ";   3 E-cores off     P-cores only (Intel 12th-gen+ hybrid; on any\n"
    ";                     other CPU it does not apply and nothing changes)\n"
    ";   4 SMT + E-cores off\n"
    "; The play dial adds: 5 CPU 0 off, 6 SMT + CPU0 off,\n"
    "; 7 E-cores + CPU0 off, 8 SMT + E-cores + CPU0 off.\n"
    "; The logo screen: SMT off here is what makes it pass at once.\n"
    "cpu_boot=0\n"
    "; The game window (loading, main menu, lobby): E-cores off fixes the\n"
    "; endless loading on some machines, all cores on others.\n"
    "cpu_window=0\n"
    "; In play: SMT off and CPU 0 off are the ones that help the odd\n"
    "; stutter.\n"
    "cpu_play=0\n"
    "; The priority class each stage holds: 0 low, 1 below normal,\n"
    "; 2 leave alone, 3 normal, 4 above normal, 5 high. 2 is the default.\n"
    "; The two start-up stages offer the lower half as well - a loading\n"
    "; screen may as well yield the machine to whatever else wants it -\n"
    "; while the play stage starts at leave alone, since a game being\n"
    "; played at low priority is just a stutter. Realtime is offered\n"
    "; nowhere: it can starve the desktop and the audio threads. The\n"
    "; engine sets a class of its own during start up, so a stage that\n"
    "; holds one keeps it held.\n"
    "cpu_prio_boot=2\n"
    "cpu_prio_window=2\n"
    "cpu_prio_play=2\n"
    "; A ceiling on the PLAY stage alone (0..64, 0 = none). The start-up\n"
    "; stages are never capped: trimming the set while the game is still\n"
    "; starting is a good way to make it not start.\n"
    "cpu_cores=0\n"
    "; 1 = render-thread heartbeat every 5s logging process memory and\n"
    "; UI/scene object counts (\"leak:\" lines) for leak hunting.\n"
    "; Default 0: off - one branch per frame, nothing else runs.\n"
    "leak_probe=0\n"
    "\n"
    "[plugins]\n"
    "; One line per plugin folder, 0 disables it.\n"
    "; firstperson=1\n"
    "; GhostNoWipe=1 - the wipe of a Ghost Mode save when a run ends.\n"
    "; Two records mark the slot as gone: the game renames N.save to\n"
    "; N.save.delete and writes a .delete beside it, and before that it\n"
    "; has already written the run's end into N.save's own content. Both\n"
    "; are held - the marked write is refused and the save put back from\n"
    "; a clean copy, the rename is dropped, and .delete writes go to the\n"
    "; plugin's own temp folder. No trace of the wipe is left on disk and\n"
    "; the save keeps the content it had before the death.\n"
    "; The list still shows the slot as gone for the rest of the session\n"
    "; - that is in the process - so a status line goes up saying the save\n"
    "; was kept, and comes down once you are in a game again. The slot is\n"
    "; listed again on the next launch. Deleting a slot by hand is caught\n"
    "; too - turn the plugin off to remove one. Single player only; delete\n"
    "; the plugin folder once the game itself is patched.\n"
    "; LastRites_dlcfix=1 - the crash on entering the \"Last Rite\"\n"
    "; DLC. One switch, off by default, in the mod menu; the single id\n"
    "; it answers for is built in and cannot be pointed elsewhere.\n"
    "; Single player only, and delete the plugin folder once the game\n"
    "; itself is patched.\n"
    "; GhostRevive=1 - experimental, answered, switched off in its own ini.\n"
    "; Whether a Ghost Mode death can be sent down the reviving path: it\n"
    "; cannot. The branch turns on whether the AI squad is aboard - with\n"
    "; one the death goes 7 -> 5 -> 4 and play carries on, without one it\n"
    "; bounces 7 -> 4 -> 7 at zero health and ends at the menu with the\n"
    "; slot gone - and nothing outside the engine reaches that decision:\n"
    "; ShTriggerGameOver is accepted and ignored, and refilling the health\n"
    "; changes nothing (the game kills the player again 56 ms later).\n"
    "; GhostNoWipe is what solves it, by keeping the file. This line stays\n"
    "; 1 so the menu is there, but the plugin's own enabled=0 means it\n"
    "; registers nothing until asked. Single player only.\n"
    "\n"
    "[Settings]\n"
    "; Menu language: zh_cn = Chinese (default), en = English.\n"
    "Language=zh_cn\n"
    "; Languages the menu language switch offers (comma separated;\n"
    "; the order here is the order shown).\n"
    "Languages=zh_cn,en\n"
    "; Mod menu / chat box UI scale, driven by the game resolution:\n"
    ";   MenuScale = 0 auto (height/1080, so 4K -> 2.0); >0 fixed rate\n"
    ";   (still clamped by Min/Max).  MenuScaleMin/Max bound the final\n"
    ";   scale in both modes.  Restart to apply.  1080p baseline = 1.0.\n"
    "MenuScale=0\n"
    "MenuScaleMin=0.75\n"
    "MenuScaleMax=3.0\n"
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
    "; A plugin can also carry its translations in its OWN ini\n"
    "; (plugins\\<name>\\<name>.ini, same [lang] sections): those win\n"
    "; over this file, which stays the shared fallback - so a plugin\n"
    "; travels with its translations.\n"
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
    "; The Mod settings page and its [loader] rows.\n"
    "Mod settings = 模组功能设置\n"
    "Startup & core = 加载设置\n"
    "Plugins = 插件开关\n"
    "Load all plugins = 启用插件（总开关）\n"
    "Boot cores = Logo 阶段\n"
    "Loading cores = 窗口加载阶段\n"
    "Play cores = 游玩阶段\n"
    "Leave alone = 不干涉\n"
    "All cores = 全部核心(强制)\n"
    "SMT off = 禁超线程\n"
    "E-cores off = 禁小核\n"
    "SMT + E-cores off = 禁超线程+小核\n"
    "CPU 0 off = 禁CPU0\n"
    "SMT + CPU0 off = 禁超线程+CPU0\n"
    "E-cores + CPU0 off = 禁小核+CPU0\n"
    "SMT + E-cores + CPU0 off = 禁超线程+小核+CPU0\n"
    "Boot priority = Logo 阶段优先级\n"
    "Loading priority = 窗口加载优先级\n"
    "Play priority = 游玩阶段优先级\n"
    "Low = 低\n"
    "Below normal = 低于正常\n"
    "Normal = 正常\n"
    "Above normal = 高于正常\n"
    "High = 高\n"
    "Play max cores = 游玩时最大核心数（0=不限制）\n"
    "boot = Logo\n"
    "window = 窗口加载\n"
    "play = 游玩\n"
    "These changes take effect after a game restart. = 这些改动将在重启游戏后生效。\n"
    "E-cores off: not applicable on this CPU. = 禁小核：本机 CPU 不适用（非 Intel 大小核）\n"
    "E-cores off: this CPU has no E-cores. = 禁小核：本机 Intel CPU 无小核\n"
    "E-cores off: detection failed. = 禁小核：检测失败\n"
    "Machine: %u processors, the game started on %u. = 本机 %u 个处理器，游戏启动时可用 %u 个。\n"
    "Now: %s stage, left alone. = 当前：%s阶段，不干涉\n"
    "Now: %s stage, %u processors. = 当前：%s阶段，%u 个处理器\n"
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

/* Advance over one physical line, NUL-terminating it in place.
 * Returns 0 at end of text. */
static int NextLine(const char **p, char *line, size_t cap) {
    size_t i = 0;
    const char *q = *p;

    if (!*q) return 0;
    while (*q && *q != '\n' && *q != '\r' && i < cap - 1)
        line[i++] = *q++;
    line[i] = 0;
    if (*q == '\r') q++;
    if (*q == '\n') q++;
    *p = q;
    return 1;
}

/* Parse one line in place. [section] lines update section and
 * return 0; a key=value row fills *keyOut/*valueOut and returns
 * 1. Language-section rows use the quoted-key and " = " rules so
 * a key containing '=' still translates. */
static int ParseIniLine(char *line, char *section, size_t secCap,
                        char **keyOut, char **valueOut) {
    char *s = line, *e, *eq;
    size_t i = 0;

    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    *e = 0;

    if (!*s || *s == ';' || *s == '#') return 0;

    if (*s == '[') {
        char *c = strchr(s, ']');
        if (!c) return 0;
        *c = 0;
        i = strlen(s + 1);
        if (i >= secCap) i = secCap - 1;
        memcpy(section, s + 1, i);
        section[i] = 0;
        return 0;
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
    if (!eq) return 0;
    *eq = 0;

    /* key is whatever s points at now: the text after the
     * opening quote for a quoted key, the raw key otherwise.
     * Save it before s is reused for the value below. */
    *keyOut = s;

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
    *valueOut = s;
    return 1;
}

static void ParseConfig(const char *text) {
    char section[48] = "";

    PeekLanguage(text);
    while (*text) {
        char line[512];
        char *key, *val;
        size_t i;

        if (!NextLine(&text, line, sizeof(line))) break;
        if (!ParseIniLine(line, section, sizeof(section), &key, &val))
            continue;

        if (!*val) continue;

        /* Language sections never touch the main entry table, so
         * a large translation set cannot crowd the config out. */
        if (IsLangSection(section)) {
            AddLangEntry(section, key, val);
            continue;
        }
        if (g_nentries >= ENTRIES_MAX) continue;

        i = strlen(key);
        if (i >= sizeof(g_entries[g_nentries].key))
            i = sizeof(g_entries[g_nentries].key) - 1;
        memcpy(g_entries[g_nentries].key, key, i);
        g_entries[g_nentries].key[i] = 0;

        i = strlen(section);
        if (i >= sizeof(g_entries[g_nentries].section))
            i = sizeof(g_entries[g_nentries].section) - 1;
        memcpy(g_entries[g_nentries].section, section, i);
        g_entries[g_nentries].section[i] = 0;

        i = strlen(val);
        if (i >= sizeof(g_entries[g_nentries].value))
            i = sizeof(g_entries[g_nentries].value) - 1;
        memcpy(g_entries[g_nentries].value, val, i);
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
 *  -> [lang] -> the same for en -> the original text.
 *  With an owner (a plugin folder name), the plugin's own
 *  plugins\<owner>\<owner>.ini is consulted first at every step of
 *  that order, and scripthook.ini remains the fallback - so a
 *  shared plugin carries its translations in its own file. */
SH_API const char *ShLangFor(const char *scope, const char *text) {
    return ShLangForOwned(NULL, scope, text);
}

/* ---- plugin-owned translation tables ------------------------------ */

/* [<lang>...] sections from plugins\<owner>\<owner>.ini, so a
 * shared plugin brings its own translations.  The main ini stays
 * the fallback: lookup order is plugin(lang) -> main(lang) ->
 * plugin(en) -> main(en) -> the original text. */
#define PLANGS_MAX      512
#define PLOAD_MAX       32

typedef struct {
    char owner[48];
    char lang[16];
    char scope[48];
    char key[128];
    char value[256];
} PlangEntry;

static PlangEntry g_plangs[PLANGS_MAX];
static int g_nplangs = 0;

/* one attempted load per plugin, including "no file" */
static struct {
    char owner[48];
    int  done;
} g_pload[PLOAD_MAX];
static int g_npload = 0;
static CRITICAL_SECTION g_plangLock;
static volatile LONG g_plangLockReady = 0;

static void AddPlangEntry(const char *owner, const char *section,
                          const char *key, const char *value) {
    char lang[16], scope[48];
    const char *dot;
    size_t n;
    PlangEntry *e;

    if (g_nplangs >= PLANGS_MAX) return;
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
    e = &g_plangs[g_nplangs++];
    strncpy(e->owner, owner, sizeof(e->owner) - 1);
    e->owner[sizeof(e->owner) - 1] = 0;
    strncpy(e->lang, lang, sizeof(e->lang) - 1);
    e->lang[sizeof(e->lang) - 1] = 0;
    strncpy(e->scope, scope, sizeof(e->scope) - 1);
    e->scope[sizeof(e->scope) - 1] = 0;
    strncpy(e->key, key, sizeof(e->key) - 1);
    e->key[sizeof(e->key) - 1] = 0;
    strncpy(e->value, value, sizeof(e->value) - 1);
    e->value[sizeof(e->value) - 1] = 0;
}

/* Only [<lang>] / [<lang>.<scope>] sections feed the translation
 * table; anything else in a plugin ini belongs to the plugin's own
 * GetPrivateProfile config and is ignored here. */
static void ParsePluginLangs(const char *owner, const char *text) {
    char section[48] = "";

    while (*text) {
        char line[512];
        char *key, *val;

        if (!NextLine(&text, line, sizeof(line))) break;
        if (!ParseIniLine(line, section, sizeof(section), &key, &val))
            continue;
        if (!*val) continue;
        if (IsLangSection(section))
            AddPlangEntry(owner, section, key, val);
    }
}

/* Lazy per-plugin load.  Runs once per owner (a missing file
 * counts as done), guarded by a lock because ShMenuStatusF can
 * trigger lookups from plugin threads while the menu thread is
 * capturing. */
static void PluginLangsLoad(const char *owner) {
    char path[GAME_DIR_MAX];
    static char text[CONFIG_MAX];
    FILE *f;
    size_t n;
    int i, slot = -1;

    if (!owner || !owner[0]) return;
    if (!g_plangLockReady) {
        /* first-use init; concurrent doubles are harmless */
        if (InterlockedCompareExchange(&g_plangLockReady, 2, 0) == 0) {
            InitializeCriticalSection(&g_plangLock);
            InterlockedExchange(&g_plangLockReady, 1);
        }
    }
    if (g_plangLockReady != 1) return;
    EnterCriticalSection(&g_plangLock);
    for (i = 0; i < g_npload; i++)
        if (!strcmp(g_pload[i].owner, owner)) { slot = i; break; }
    if (slot >= 0 && g_pload[slot].done) {
        LeaveCriticalSection(&g_plangLock);
        return;
    }
    if (slot < 0 && g_npload < PLOAD_MAX) {
        slot = g_npload++;
        strncpy(g_pload[slot].owner, owner,
                sizeof(g_pload[slot].owner) - 1);
        g_pload[slot].owner[sizeof(g_pload[slot].owner) - 1] = 0;
    }
    if (slot >= 0) g_pload[slot].done = 1;
    if (slot >= 0 &&
        ShPluginIniPath(owner, path, sizeof(path)) &&
        (f = fopen(path, "rb")) != NULL) {
        n = fread(text, 1, sizeof(text) - 1, f);
        fclose(f);
        text[n] = 0;
        ParsePluginLangs(owner, text);
    }
    LeaveCriticalSection(&g_plangLock);
}

static const char *PlangFind(const char *owner, const char *lang,
                             const char *scope, const char *key) {
    int i;
    for (i = 0; i < g_nplangs; i++) {
        if (strcmp(g_plangs[i].owner, owner)) continue;
        if (strcmp(g_plangs[i].lang, lang)) continue;
        if (g_plangs[i].scope[0] &&
            strcmp(g_plangs[i].scope, scope))
            continue;
        if (!strcmp(g_plangs[i].key, key))
            return g_plangs[i].value;
    }
    return NULL;
}

/* One scope-fallback walk for the given table set.  An owner walks
 * the plugin table first and the main table second, so a key that
 * the plugin ini does not carry still finds the shared translation. */
static const char *TableChainFind(const char *owner, const char *lang,
                                  const char *scope, const char *text) {
    char buf[64];
    const char *v;
    const char *p = scope ? scope : "";
    int pass;

    for (pass = 0; pass < 2; pass++) {
        if (pass == 1 || !owner || !owner[0]) {
            for (;;) {
                v = LangFind(lang, p, text);
                if (v) return v;
                {
                    const char *dot = strrchr(p, '.');
                    size_t n;
                    if (!dot) break;
                    n = (size_t)(dot - (scope ? scope : ""));
                    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
                    memcpy(buf, scope ? scope : "", n);
                    buf[n] = 0;
                    p = buf;
                }
            }
            v = LangFind(lang, "", text);
            if (v) return v;
            /* no owner: the main table is the only table */
            if (!owner || !owner[0]) break;
        } else {
            for (;;) {
                v = PlangFind(owner, lang, p, text);
                if (v) return v;
                {
                    const char *dot = strrchr(p, '.');
                    size_t n;
                    if (!dot) break;
                    n = (size_t)(dot - (scope ? scope : ""));
                    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
                    memcpy(buf, scope ? scope : "", n);
                    buf[n] = 0;
                    p = buf;
                }
            }
            v = PlangFind(owner, lang, "", text);
            if (v) return v;
        }
    }
    return NULL;
}

/** Translate like ShLangFor, but honour an owning plugin: its own
 *  ini wins over scripthook.ini at every step.  owner NULL or ""
 *  behaves exactly like ShLangFor. */
SH_API const char *ShLangForOwned(const char *owner, const char *scope,
                                  const char *text) {
    const char *v;

    if (!text) return "";
    LoadConfig();
    if (owner && owner[0]) PluginLangsLoad(owner);
    if (g_langName[0]) {
        v = TableChainFind(owner, g_langName, scope, text);
        if (v) return v;
    }
    v = TableChainFind(owner, "en", scope, text);
    return v ? v : text;
}

SH_API const char *ShLangGet(void) {
    LoadConfig();
    return g_langName;
}

/* One lock guards parse-once publication and read-back: without it,
 * a parse in flight published half-filled tables to concurrent
 * readers, and two writers raced the same .tmp file. */
static CRITICAL_SECTION g_cfgLock;
static volatile LONG g_cfgLockReady = 0;

static void EnsureConfigLock(void) {
    for (;;) {
        LONG s = InterlockedCompareExchange(&g_cfgLockReady, 0, 0);
        if (s == 1) return;
        if (s == 2) { Sleep(0); continue; }
        if (InterlockedCompareExchange(&g_cfgLockReady, 2, 0)) continue;
        InitializeCriticalSection(&g_cfgLock);
        InterlockedExchange(&g_cfgLockReady, 1);
        return;
    }
}

static void LoadConfig(void) {
    char path[GAME_DIR_MAX];
    FILE *f;
    size_t n;

    EnsureConfigLock();
    EnterCriticalSection(&g_cfgLock);
    if (g_configReady) {
        LeaveCriticalSection(&g_cfgLock);
        return;
    }
    /* Inside the lock: readers block on the same CS until the
     * parse finishes, so the tables are never seen half-built. */

    if (snprintf(path, sizeof(path), "%sscripthook.ini",
                 GameDir()) < 0) {
        LeaveCriticalSection(&g_cfgLock);
        return;
    }
    f = fopen(path, "rb");
    if (!f) {
        /* First launch: write the default so the schema is
         * visible, then parse it. */
        WriteDefaultConfig(path);
        f = fopen(path, "rb");
    }
    if (f) {
        n = fread(g_config, 1, sizeof(g_config) - 1, f);
        fclose(f);
        g_config[n] = 0;
        ParseConfig(g_config);
        ResolveLanguage();
    }
    LeaveCriticalSection(&g_cfgLock);
}

static const char *FindEntry(const char *section, const char *key) {
    int i;
    const char *hit = NULL;
    /* SetEntry rewrites entries field-by-field under this lock; a
     * reader without it could strcmp a half-written key. */
    EnsureConfigLock();
    EnterCriticalSection(&g_cfgLock);
    for (i = 0; i < g_nentries; i++)
        if (!strcmp(g_entries[i].section, section) &&
            !strcmp(g_entries[i].key, key)) {
            hit = g_entries[i].value;
            break;
        }
    LeaveCriticalSection(&g_cfgLock);
    return hit;
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
    /* One lock around file rewrite and in-memory update: two
     * threads writing different keys used to race on the same
     * .tmp and the last writer wiped the first one's key. */
    EnsureConfigLock();
    LoadConfig();
    EnterCriticalSection(&g_cfgLock);
    if (!IniWriteValue(section, key, value)) {
        LeaveCriticalSection(&g_cfgLock);
        return 0;
    }
    SetEntry(section, key, value);
    LeaveCriticalSection(&g_cfgLock);
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
