/* The framework's own text, compiled in.
 *
 * Every string the framework shows goes through @ref lang, and these
 * tables are the bottom layer of that lookup: with no lang.ini
 * anywhere the UI still reads - delete <gamedir>\lang.ini and the menu
 * still switches between Chinese and English.
 *
 * Adding a language is two things: another table below, and one more
 * ShLangDeclare call in ShTextInitFramework. The picker picks the
 * language up by itself, because the languages declared here are what
 * [Settings] Languages falls back to (ResolveLanguage in
 * scripthook_config.c builds that list).
 *
 * Keys are stable IDs ('@' and a dotted path), so renaming a page or a row
 * never breaks a translation, and one screen never has to borrow another
 * screen's word: the loading stages' efficiency-mode dial says on / off
 * while the status line names the mode it resolves to. Every string this
 * file lists is keyed that way - the English literals are the values,
 * never the keys. The one place a literal is still a key is a plugin
 * shipped without source: its own English text is all it has.
 * Diagnostics treat the en-US table as the authoritative list of what
 * this build can say.
 *
 * The two tables are kept in the same order on purpose: a difference
 * between them is a missing translation, and it should be visible.
 *
 * The file is UTF-8 (the MSVC build passes /utf-8, gcc defaults to it).
 */
#include <windows.h>
#include "scripthook.h"

/* ---- English (the source of truth) ------------------------------ */

static const ShText kEnUS[] = {
    /* menu furniture */
    /* One line, kept as short as the Chinese it mirrors: the panel does not
     * wrap, so a line that runs past it is a line read in halves. */
    { "@menu.root.hint",
      "F4 menu, Enter confirm; Esc / Backspace back; "
      "\xE2\x86\x91 \xE2\x86\x93 select, \xE2\x86\x90 \xE2\x86\x92 adjust" },
    { "@menu.on",             "on" },
    { "@menu.off",            "off" },
    { "@menu.footer.pos",     "%d / %d" },

    /* the root menu's top-right credits. Framework text like the rest, so
     * a lang.ini can override either line; the build line formats the
     * version in (%s), which is SH_VERSION and not a number typed here.
     * The group is on the About page only: the corner is two short lines
     * and the second one is the build, which is what a report names. */
    { "@ui.credit.author",    "Original: Phiality \xC2\xB7 "
                              "modded by GameXueRen" },
    { "@ui.credit.build",     "Version: %s" },

    /* the settings tree */
    { "@settings.page",       "ScriptHook settings" },
    { "@settings.load",       "Load plugins (master switch)" },
    { "@settings.plugins",    "Plugin switches" },
    { "@settings.cpu",        "CPU scheduling" },
    { "@settings.language",   "Menu language" },
    { "@settings.hint",
      "Plugin switches and CPU scheduling take effect on the next start." },
    { "@settings.order",      "Menu order" },
    /* One line, and about the length of the Chinese it translates. The
     * English this replaces explained when a move is saved and what happens
     * to a plugin that is switched off - useful, but it ran to four lines
     * where the Chinese is one sentence, and a hint is read at a glance. */
    { "@settings.order.hint",
      "Menu order: \xE2\x86\x90 \xE2\x86\x92 moves the row up / down." },
    { "@settings.order.empty", "No plugin menu to order yet." },
    { "@settings.order.saved", "Order saved" },

    /* the About page: one fact per row, and the build's version and
     * source address come in as values (SH_VERSION, SH_REPO) so this page
     * cannot name a build other than the one drawing it */
    { "@about.page",          "About" },
    { "@about.version",       "Version: %s" },
    { "@about.api",           "API: %s" },
    { "@about.build.ok",      "Game build: %s (verified)" },
    { "@about.build.new",     "Game build: %s (not verified - see the log)" },
    { "@about.author",        "Original author: Phiality" },
    { "@about.modder",        "Modded by: GameXueRen" },
    { "@about.qq",            "QQ group: 299177445" },

    /* A plugin that asks for an API newer than this framework (SH_REQUIRES_API)
     * is refused and this is what says so on screen; loader.c holds the line
     * back until the overlay is drawing, or nobody would ever see it. */
    { "@toast.api.refused",
      "Plugin not loaded: %s needs API %d, this one is %d" },

    /* the Forge page. "(experimental)" is part of the label the root menu
     * shows: the feature serves mod bytes without touching the archives,
     * which is proven, but the shape of what it accepts is still moving -
     * so the page says so where a player reads it, not only in the docs. */
    { "@forge.page",          "Forge Mod Loader (experimental)" },
    { "@forge.hint",
      "Loads loose files from mods\\ over existing\n"
      ".forge entries without touching the archives.\n"
      "Lay them out as mods\\<archive>\\<file> or\n"
      "mods\\<mod name>\\<archive>\\<file>.\n"
      "'~' disables a mod folder, and\n"
      "'<n>_-_<name>.data' names the entry.\n"
      "Changes need a restart." },
    { "@forge.enabled",       "Enabled" },
    { "@forge.dryrun",        "Dry run" },
    { "@forge.status.off",    "Off ([forgemod] enabled=0)." },
    { "@forge.status.nomods", "On, but mods\\ has no mod files." },
    { "@forge.status.mods",
      "%d mod(s): %d applied, %d rejected or overridden." },

    /* lines and hints those pages carry */
    { "@settings.saved",      "Saved. Restart to apply." },
    { "@settings.restart",    "Plugin switch changes take effect on the next start." },
    { "@settings.mode",       "Play mode" },
    /* The bottom line of the switches page when no mode has been read yet,
     * which is the normal state in the front end. Three words on purpose:
     * the evidence it used to print is recorded line by line in
     * scripthook_playmode.log, and ShPlayModeEvidence still hands it to
     * anything that asks. */
    { "@settings.mode.none",  "no mode" },
    { "@settings.plugins.note",
      "No [plugins] line means the plugin is loaded. Switch it off here; "
      "deleting scripthook.ini resets every plugin to loaded." },
    { "@settings.plugins.empty", "No plugin folders in plugins\\ yet." },
    { "@menu.offnow",         "Off now" },
    { "@cpu.now",             "Now: %s - cores %s - priority %s" },
    { "@cpu.hint",
      "Cores and priority per start up stage; restart to apply." },
    { "@cpu.hint.ecore.na",   "E-cores off: not applicable on this CPU." },
    { "@cpu.hint.ecore.none", "E-cores off: this CPU has no E-cores." },
    { "@cpu.hint.ecore.failed", "E-cores off: detection failed." },
    { "@cpu.hint.eco.na",
      "Efficiency mode: not available on this system - the loading stages "
      "hold the low priority instead." },
    { "@cpu.hint.eco.failed",
      "Efficiency mode: the call failed - the loading stages hold the low "
      "priority instead." },
    { "@settings.thread.blacklist", "blacklist line thread failed" },
    { "@settings.thread.cpu",       "CPU line thread failed" },

    /* CPU page rows */
    { "@cpu.row.boot",        "Boot cores" },
    { "@cpu.row.window",      "Loading cores" },
    { "@cpu.row.eco",         "Efficiency mode while loading" },
    { "@cpu.row.play",        "Play cores" },
    { "@cpu.row.prio",        "Play priority" },
    { "@cpu.row.cores",       "Play max cores" },

    /* the stage scales (row options, and the CPU status line) */
    { "@cpu.opt.leave",       "Leave alone" },
    { "@cpu.opt.all",         "All cores" },
    { "@cpu.opt.nosmt",       "SMT off" },
    { "@cpu.opt.noecore",     "E-cores off" },
    { "@cpu.opt.nosmt_noecore", "SMT + E-cores off" },
    { "@cpu.opt.nocpu0",      "CPU 0 off" },
    { "@cpu.opt.nosmt_nocpu0", "SMT + CPU0 off" },
    { "@cpu.opt.noecore_nocpu0", "E-cores + CPU0 off" },
    { "@cpu.opt.nosmt_noecore_nocpu0", "SMT + E-cores + CPU0 off" },
    { "@cpu.prio.normal",     "Normal" },
    { "@cpu.prio.above",      "Above normal" },
    { "@cpu.prio.high",       "High" },
    { "@cpu.prio.eco",        "Efficiency mode" },
    { "@cpu.prio.low",        "Low" },

    /* the loading stages' efficiency-mode dial: the three places that row
     * offers, in the ini's own order (0 leave alone, 1 hold the mode, 2
     * drop it). The words are its own, not the priority scale's - the row
     * reads as a switch ("on" / "off"), while the status line names the
     * mode it resolves to. */
    { "@cpu.eco.on",          "on" },
    { "@cpu.eco.off",         "off" },

    /* the three start up stages, as the CPU status line names them */
    { "@cpu.stage.boot",      "boot" },
    { "@cpu.stage.window",    "window" },
    { "@cpu.stage.play",      "play" },

    /* Every language the game ships, named the way that language names
     * itself, so the picker reads with no lang.ini present (a
     * [LanguageNames] row in a file overrides these). This table is the
     * one the lookup falls back to as well - see the note above the
     * language tables below - so it carries all of them and not only the
     * two with a translation. */
    { "@lang.name.zh-CN",     "简体中文" },
    { "@lang.name.zh-TW",     "繁體中文" },
    { "@lang.name.en-US",     "English" },
    { "@lang.name.fr-FR",     "Français" },
    { "@lang.name.it-IT",     "Italiano" },
    { "@lang.name.de-DE",     "Deutsch" },
    { "@lang.name.es-ES",     "Español (España)" },
    { "@lang.name.es-MX",     "Español (Latinoamérica)" },
    { "@lang.name.pt-BR",     "Português (Brasil)" },
    { "@lang.name.ru-RU",     "Русский" },
    { "@lang.name.pl-PL",     "Polski" },
    { "@lang.name.cs-CZ",     "Čeština" },
    { "@lang.name.nl-NL",     "Nederlands" },
    { "@lang.name.ar-SA",     "العربية" },
    { "@lang.name.ko-KR",     "한국어" },
    { "@lang.name.ja-JP",     "日本語" }
};

/* ---- Chinese ---------------------------------------------------- */

static const ShText kZhCN[] = {
    { "@menu.root.hint",
      "F4 菜单，Enter 确认；Esc / Backspace 返回；↑ ↓ 上下选择，← → 左右调整" },
    { "@menu.on",             "开" },
    { "@menu.off",            "关" },
    { "@menu.footer.pos",     "第 %d / %d 行" },

    { "@ui.credit.author",    "原作者：Phiality · 魔改：GameXueRen" },
    { "@ui.credit.build",     "版本：%s" },

    { "@settings.page",       "ScriptHook 设置" },
    { "@settings.load",       "加载插件（总开关）" },
    { "@settings.plugins",    "各插件开关" },
    { "@settings.cpu",        "CPU 调度" },
    { "@settings.language",   "菜单语言" },
    { "@settings.hint",
      "插件开关、CPU调度改变需下次启动生效。" },
    { "@settings.order",      "菜单排序" },
    { "@settings.order.hint",
      "改变菜单显示顺序。← → 把当前行上移 / 下移。" },
    { "@settings.order.empty", "还没有可排序的插件菜单。" },
    { "@settings.order.saved", "菜单显示顺序已保存" },

    { "@about.page",          "关于" },
    { "@about.version",       "版本：%s" },
    { "@about.api",           "API版本：%s" },
    { "@about.build.ok",      "游戏构建：%s（已验证）" },
    { "@about.build.new",     "游戏构建：%s（未验证 —— 见日志）" },
    { "@about.author",        "原作者：Phiality" },
    { "@about.modder",        "魔改版：GameXueRen" },
    { "@about.qq",            "QQ群：299177445" },

    { "@toast.api.refused",
      "插件未加载：%s 需要 API %d，当前框架是 %d" },

    { "@forge.page",          "Forge资源侧载（实验功能）" },
    { "@forge.hint",
      "把 mods\\ 下的文件覆盖到已有 .forge 游戏文件上。此项开关改变需下次启动生效。" },
    { "@forge.enabled",       "启用" },
    { "@forge.dryrun",        "仅验证能否覆盖成功，不实际应用" },
    { "@forge.status.off",    "已关闭（[forgemod] enabled=0）。" },
    { "@forge.status.nomods", "已开启，但 mods\\ 下没有 mod 文件。" },
    { "@forge.status.mods",
      "%d 个 mod：%d 已生效，%d 被拒绝或覆盖" },

    { "@settings.saved",      "已保存。重启后生效。" },
    { "@settings.restart",    "插件开关改变需下次启动生效。" },
    { "@settings.mode",       "游玩模式" },
    { "@settings.mode.none",  "no mode" },
    { "@settings.plugins.note",
      "没有 [plugins] 行即为加载。在这里关闭；删除 scripthook.ini 会把所有"
      "插件重置为加载。" },
    { "@settings.plugins.empty", "plugins\\ 里还没有插件。" },
    { "@menu.offnow",         "当前不可用" },
    { "@cpu.now",             "当前：%s - 核心 %s - 优先级 %s" },
    { "@cpu.hint",
      "设定每个启动阶段的处理器集合与优先级，改变需下次启动生效。" },
    { "@cpu.hint.ecore.na",   "关闭能效核：本 CPU 不适用。" },
    { "@cpu.hint.ecore.none", "关闭能效核：本 CPU 没有能效核。" },
    { "@cpu.hint.ecore.failed", "关闭能效核：检测失败。" },
    { "@cpu.hint.eco.na",
      "效率模式：本系统不支持。加载阶段将改用低优先级。" },
    { "@cpu.hint.eco.failed",
      "效率模式：调用失败。加载阶段将改用低优先级。" },
    { "@settings.thread.blacklist", "黑名单状态行线程创建失败" },
    { "@settings.thread.cpu",       "CPU 状态行线程创建失败" },

    { "@cpu.row.boot",        "1-启动Logo窗口加载阶段" },
    { "@cpu.row.window",      "2-游戏主窗口加载阶段" },
    { "@cpu.row.eco",         "加载阶段使用效率模式" },
    { "@cpu.row.play",        "3-游戏中..." },
    { "@cpu.row.prio",        "游戏中的CPU优先级" },
    { "@cpu.row.cores",       "游戏中的CPU核心数（0为不限制）" },

    { "@cpu.opt.leave",       "不干预" },
    { "@cpu.opt.all",         "全部核心" },
    { "@cpu.opt.nosmt",       "关闭超线程" },
    { "@cpu.opt.noecore",     "关闭能效核" },
    { "@cpu.opt.nosmt_noecore", "关闭超线程+能效核" },
    { "@cpu.opt.nocpu0",      "关闭 CPU 0" },
    { "@cpu.opt.nosmt_nocpu0", "关闭超线程+CPU 0" },
    { "@cpu.opt.noecore_nocpu0", "关闭能效核+CPU 0" },
    { "@cpu.opt.nosmt_noecore_nocpu0", "关闭超线程+能效核+CPU 0" },
    { "@cpu.prio.normal",     "正常" },
    { "@cpu.prio.above",      "高于正常" },
    { "@cpu.prio.high",       "高" },
    { "@cpu.prio.eco",        "效率模式" },
    { "@cpu.prio.low",        "低" },

    { "@cpu.eco.on",          "开" },
    { "@cpu.eco.off",         "关" },

    { "@cpu.stage.boot",      "启动Logo窗口" },
    { "@cpu.stage.window",    "主窗口加载" },
    { "@cpu.stage.play",      "游戏中" },

    { "@lang.name.zh-CN",     "简体中文" },
    { "@lang.name.en-US",     "English" }
};

/* The rest of the languages the game ships, each carrying its own name and
 * nothing else. Declaring one is what puts it in the picker and in the
 * first-run auto-pick (scripthook_config.c: a language exists only if some
 * module declares text for it); every other key misses here and is answered
 * by the English table above, which is the lookup's own second step. So the
 * menu still reads as a menu, and a translation lands by adding rows to the
 * table - or, without the source, to a lang.ini the player writes.
 */
static const ShText kNameFrFR[] = { { "@lang.name.fr-FR", "Français" } };
static const ShText kNameItIT[] = { { "@lang.name.it-IT", "Italiano" } };
static const ShText kNameDeDE[] = { { "@lang.name.de-DE", "Deutsch" } };
static const ShText kNameEsES[] = { { "@lang.name.es-ES", "Español (España)" } };
static const ShText kNameEsMX[] = { { "@lang.name.es-MX",
                                      "Español (Latinoamérica)" } };
static const ShText kNamePtBR[] = { { "@lang.name.pt-BR",
                                      "Português (Brasil)" } };
static const ShText kNameRuRU[] = { { "@lang.name.ru-RU", "Русский" } };
static const ShText kNamePlPL[] = { { "@lang.name.pl-PL", "Polski" } };
static const ShText kNameCsCZ[] = { { "@lang.name.cs-CZ", "Čeština" } };
static const ShText kNameNlNL[] = { { "@lang.name.nl-NL", "Nederlands" } };
static const ShText kNameArSA[] = { { "@lang.name.ar-SA", "العربية" } };
static const ShText kNameKoKR[] = { { "@lang.name.ko-KR", "한국어" } };
static const ShText kNameJaJP[] = { { "@lang.name.ja-JP", "日本語" } };
static const ShText kNameZhTW[] = { { "@lang.name.zh-TW", "繁體中文" } };

/* Declared in scripthook_config.c, called from ResolveLanguage before
 * the language list is built: a language exists only if some module
 * declares text for it. */
void ShTextInitFramework(void) {
    static int done;

    if (done) return;
    done = 1;
    ShLangDeclare(NULL, "en-US", kEnUS,
                  (int)(sizeof(kEnUS) / sizeof(kEnUS[0])));
    ShLangDeclare(NULL, "zh-CN", kZhCN,
                  (int)(sizeof(kZhCN) / sizeof(kZhCN[0])));
    /* The rest, in the order the game's own list keeps. That order is what
     * the picker shows first and nothing else: the first-run pick no longer
     * turns on it, it prefers the same region and then the same Chinese
     * script (scripthook_config.c, LangPickOffered). */
    ShLangDeclare(NULL, "fr-FR", kNameFrFR, 1);
    ShLangDeclare(NULL, "it-IT", kNameItIT, 1);
    ShLangDeclare(NULL, "de-DE", kNameDeDE, 1);
    ShLangDeclare(NULL, "es-ES", kNameEsES, 1);
    ShLangDeclare(NULL, "ar-SA", kNameArSA, 1);
    ShLangDeclare(NULL, "zh-TW", kNameZhTW, 1);
    ShLangDeclare(NULL, "ko-KR", kNameKoKR, 1);
    ShLangDeclare(NULL, "ja-JP", kNameJaJP, 1);
    ShLangDeclare(NULL, "nl-NL", kNameNlNL, 1);
    ShLangDeclare(NULL, "pl-PL", kNamePlPL, 1);
    ShLangDeclare(NULL, "pt-BR", kNamePtBR, 1);
    ShLangDeclare(NULL, "ru-RU", kNameRuRU, 1);
    ShLangDeclare(NULL, "cs-CZ", kNameCsCZ, 1);
    ShLangDeclare(NULL, "es-MX", kNameEsMX, 1);
}
