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
    { "@menu.root.hint",
      "F4 toggle menu, Enter select, ESC back\n"
      "\xE2\x86\x91 \xE2\x86\x93 or W/S select, "
      "\xE2\x86\x90 \xE2\x86\x92 or A/D adjust" },
    { "@menu.on",             "on" },
    { "@menu.off",            "off" },
    { "@menu.footer.pos",     "%d / %d" },

    /* the settings tree */
    { "@settings.page",       "ScriptHook settings" },
    { "@settings.load",       "Load plugins (master switch)" },
    { "@settings.plugins",    "Plugin switches" },
    { "@settings.cpu",        "CPU scheduling" },
    { "@settings.language",   "Menu language" },
    { "@settings.hint",
      "Plugin switches take effect on the next launch; the language "
      "applies at once." },
    { "@settings.order",      "Menu order" },
    { "@settings.order.hint",
      "Left / right moves the highlighted page one place.\n"
      "Every move is saved at once and the root menu\n"
      "follows it; a plugin switched off keeps the place\n"
      "it will come back to." },
    { "@settings.order.empty", "No plugin menu to order yet." },
    { "@settings.order.saved", "Order saved" },

    /* the Forge page */
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
    { "@settings.restart",    "These changes take effect after a game restart." },
    { "@settings.plugins.note",
      "No [plugins] line means off. Switch it on here; deleting "
      "scripthook.ini resets every plugin to off." },
    { "@menu.offnow",         "Off now" },
    { "@cpu.now",             "Now: %s - cores %s - priority %s" },
    { "@cpu.hint",
      "Processor set and priority for each start up stage - changes need "
      "a restart." },
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

    /* the translation report */
    { "@settings.diag",       "Translation report" },
    { "@settings.diag.hint",
      "File text this build still wants.\n"
      "Still in English - the row's value is\n"
      "English text; translate it in place.\n"
      "No text at all: keys only the build has.\n"
      "Export writes them all to\n"
      "lang\\<code>.missing.ini, ready to fill in.\n"
      "A plugin without source is translated this\n"
      "way too: its English literal is the key." },
    { "@settings.diag.export", "Write lang\\<code>.missing.ini" },
    { "@diag.english",        "Still in English:" },
    { "@diag.missing",        "No text at all:" },
    { "@diag.orphan",         "Not declared (\"@\" keys):" },
    { "@diag.dup",            "Repeated rows:" },
    { "@diag.dropped",        "Dropped (table full):" },
    { "@diag.framework",      "(framework)" },
    { "@diag.exported",       "%d row(s) written to %s" },
    { "@diag.exportfail",     "Export failed - see logs\\scripthook_text.log" },

    /* the languages this build ships, named in their own language, so
     * the picker reads with no lang.ini present (a [LanguageNames] row
     * in a file overrides these) */
    { "@lang.name.zh-CN",     "简体中文" },
    { "@lang.name.en-US",     "English" }
};

/* ---- Chinese ---------------------------------------------------- */

static const ShText kZhCN[] = {
    { "@menu.root.hint",
      "F4 开关菜单，回车选择，ESC 返回；↑ ↓ 或 W/S 选择，← → 或 A/D 调整" },
    { "@menu.on",             "开" },
    { "@menu.off",            "关" },
    { "@menu.footer.pos",     "第 %d / %d 行" },

    { "@settings.page",       "ScriptHook 设置" },
    { "@settings.load",       "加载插件（总开关）" },
    { "@settings.plugins",    "各插件开关" },
    { "@settings.cpu",        "CPU 调度" },
    { "@settings.language",   "菜单语言" },
    { "@settings.hint",
      "插件开关改变需下次启动生效；其他立即生效。" },
    { "@settings.order",      "菜单排序" },
    { "@settings.order.hint",
      "改变菜单显示顺序。← → 把当前行上移 / 下移。" },
    { "@settings.order.empty", "还没有可排序的插件菜单。" },
    { "@settings.order.saved", "菜单显示顺序已保存" },

    { "@forge.hint",
      "把 mods\\ 下的文件覆盖到已有 .forge 游戏文件上。此项开关改变需下次启动生效。" },
    { "@forge.enabled",       "启用" },
    { "@forge.dryrun",        "仅验证能否覆盖成功，不实际应用" },
    { "@forge.status.off",    "已关闭（[forgemod] enabled=0）。" },
    { "@forge.status.nomods", "已开启，但 mods\\ 下没有 mod 文件。" },
    { "@forge.status.mods",
      "%d 个 mod：%d 已生效，%d 被拒绝或覆盖" },

    { "@settings.saved",      "已保存。重启后生效。" },
    { "@settings.restart",    "这些改动需重启游戏才生效。" },
    { "@settings.plugins.note",
      "没有 [plugins] 行即为关闭。在这里打开；删除 scripthook.ini 会把所有"
      "插件重置为关闭。" },
    { "@menu.offnow",         "当前不可用" },
    { "@cpu.now",             "当前：%s - 核心 %s - 优先级 %s" },
    { "@cpu.hint",
      "设定每个启动阶段的处理器集合与优先级，改动需重启。" },
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

    { "@settings.diag",       "译文诊断" },
    { "@settings.diag.hint",
      "文件还欠这个版本哪些文本。\n仍是英文：该行的值还是英文，就地改值即可。\n完全没有文本：只有内部才有的key。"
      "\n导出会把它们写到\nlang\\<语言>.missing.ini，填好即可用。\n没有源码的插件也走这条路："
      "它的英文原文就是键。" },
    { "@settings.diag.export", "导出到 lang\\<语言>.missing.ini" },
    { "@diag.english",        "仍是英文：" },
    { "@diag.missing",        "完全没有文本：" },
    { "@diag.orphan",         "内部未声明的 “@” 键：" },
    { "@diag.dup",            "重复行：" },
    { "@diag.dropped",        "超限丢弃：" },
    { "@diag.framework",      "（框架）" },
    { "@diag.exported",       "已写入 %d 行：%s" },
    { "@diag.exportfail",     "导出失败，详见 logs\\scripthook_text.log" },

    { "@lang.name.zh-CN",     "简体中文" },
    { "@lang.name.en-US",     "English" }
};

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
}
