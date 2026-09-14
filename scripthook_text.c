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
 * Keys that start with '@' are stable IDs, so renaming a page or a row
 * never breaks a translation. The other keys are the English literals
 * the settings module passes - the same keys the old [zh_cn] tables
 * used, so those rows move here without touching that module.
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
      "Left / right moves the highlighted page one place. Every move is "
      "saved at once and the root menu follows it; a plugin switched off "
      "keeps the place it will come back to." },
    { "@settings.order.empty", "No plugin menu to order yet." },
    { "@settings.order.saved", "Order saved" },

    /* lines and hints those pages carry */
    { "Saved. Restart to apply.",
      "Saved. Restart to apply." },
    { "These changes take effect after a game restart.",
      "These changes take effect after a game restart." },
    { "No [plugins] line means off. Switch it on here; deleting "
      "scripthook.ini resets every plugin to off.",
      "No [plugins] line means off. Switch it on here; deleting "
      "scripthook.ini resets every plugin to off." },
    { "Off now",              "Off now" },
    { "Now: %s - cores %s - priority %s",
      "Now: %s - cores %s - priority %s" },
    { "Processor set and priority for each start up stage - changes need "
      "a restart.",
      "Processor set and priority for each start up stage - changes need "
      "a restart." },
    { "E-cores off: not applicable on this CPU.",
      "E-cores off: not applicable on this CPU." },
    { "E-cores off: this CPU has no E-cores.",
      "E-cores off: this CPU has no E-cores." },
    { "E-cores off: detection failed.",
      "E-cores off: detection failed." },
    { "Efficiency mode: not available on this system - the loading stages "
      "hold the low priority instead.",
      "Efficiency mode: not available on this system - the loading stages "
      "hold the low priority instead." },
    { "Efficiency mode: the call failed - the loading stages hold the low "
      "priority instead.",
      "Efficiency mode: the call failed - the loading stages hold the low "
      "priority instead." },
    { "blacklist line thread failed", "blacklist line thread failed" },
    { "CPU line thread failed",       "CPU line thread failed" },

    /* CPU page rows */
    { "Boot cores",           "Boot cores" },
    { "Loading cores",        "Loading cores" },
    { "Efficiency mode while loading", "Efficiency mode while loading" },
    { "Play cores",           "Play cores" },
    { "Play priority",        "Play priority" },
    { "Play max cores",       "Play max cores" },

    /* the stage scales (row options, and the CPU status line) */
    { "Leave alone",          "Leave alone" },
    { "All cores",            "All cores" },
    { "SMT off",              "SMT off" },
    { "E-cores off",          "E-cores off" },
    { "SMT + E-cores off",    "SMT + E-cores off" },
    { "CPU 0 off",            "CPU 0 off" },
    { "SMT + CPU0 off",       "SMT + CPU0 off" },
    { "E-cores + CPU0 off",   "E-cores + CPU0 off" },
    { "SMT + E-cores + CPU0 off", "SMT + E-cores + CPU0 off" },
    { "Normal",               "Normal" },
    { "Above normal",         "Above normal" },
    { "High",                 "High" },
    { "Efficiency mode",      "Efficiency mode" },
    { "Low",                  "Low" },

    /* the three start up stages, as the CPU status line names them */
    { "boot",                 "boot" },
    { "window",               "window" },
    { "play",                 "play" },

    /* the languages this build ships, named in their own language, so
     * the picker reads with no lang.ini present (a [LanguageNames] row
     * in a file overrides these) */
    { "@lang.name.zh-CN",     "简体中文" },
    { "@lang.name.en-US",     "English" }
};

/* ---- Chinese ---------------------------------------------------- */

static const ShText kZhCN[] = {
    { "@menu.root.hint",
      "F4 开关菜单，回车选择，ESC 返回\n"
      "↑ ↓ 或 W/S 选择，← → 或 A/D 调整" },
    { "@menu.on",             "开" },
    { "@menu.off",            "关" },
    { "@menu.footer.pos",     "第 %d / %d 行" },

    { "@settings.page",       "ScriptHook 设置" },
    { "@settings.load",       "加载插件（总开关）" },
    { "@settings.plugins",    "各插件开关" },
    { "@settings.cpu",        "CPU 调度" },
    { "@settings.language",   "菜单语言" },
    { "@settings.hint",
      "插件开关下次启动生效；语言立即生效。" },
    { "@settings.order",      "菜单排序" },
    { "@settings.order.hint",
      "← → 把当前行上移 / 下移一位；每次改动立即存盘，根菜单顺序同时生效。"
      "被关掉的插件会记住它原来的位置。" },
    { "@settings.order.empty", "还没有可排序的插件菜单。" },
    { "@settings.order.saved", "顺序已保存" },

    { "Saved. Restart to apply.",
      "已保存。重启后生效。" },
    { "These changes take effect after a game restart.",
      "这些改动需重启游戏才生效。" },
    { "No [plugins] line means off. Switch it on here; deleting "
      "scripthook.ini resets every plugin to off.",
      "没有 [plugins] 行即为关闭。在这里打开；删除 scripthook.ini 会把所有"
      "插件重置为关闭。" },
    { "Off now",              "当前不可用" },
    { "Now: %s - cores %s - priority %s",
      "当前：%s - 核心 %s - 优先级 %s" },
    { "Processor set and priority for each start up stage - changes need "
      "a restart.",
      "每个启动阶段的处理器集合与优先级 —— 改动需重启。" },
    { "E-cores off: not applicable on this CPU.",
      "关闭能效核：本 CPU 不适用。" },
    { "E-cores off: this CPU has no E-cores.",
      "关闭能效核：本 CPU 没有能效核。" },
    { "E-cores off: detection failed.",
      "关闭能效核：检测失败。" },
    { "Efficiency mode: not available on this system - the loading stages "
      "hold the low priority instead.",
      "效率模式：本系统不支持 —— 加载阶段改用低优先级。" },
    { "Efficiency mode: the call failed - the loading stages hold the low "
      "priority instead.",
      "效率模式：调用失败 —— 加载阶段改用低优先级。" },
    { "blacklist line thread failed",
      "黑名单状态行线程创建失败" },
    { "CPU line thread failed",
      "CPU 状态行线程创建失败" },

    { "Boot cores",           "启动核心" },
    { "Loading cores",        "加载核心" },
    { "Efficiency mode while loading", "加载期间效率模式" },
    { "Play cores",           "游戏核心" },
    { "Play priority",        "游戏优先级" },
    { "Play max cores",       "游戏最大核心数" },

    { "Leave alone",          "不干预" },
    { "All cores",            "全部核心" },
    { "SMT off",              "关闭超线程" },
    { "E-cores off",          "关闭能效核" },
    { "SMT + E-cores off",    "关闭超线程+能效核" },
    { "CPU 0 off",            "关闭 CPU 0" },
    { "SMT + CPU0 off",       "关闭超线程+CPU 0" },
    { "E-cores + CPU0 off",   "关闭能效核+CPU 0" },
    { "SMT + E-cores + CPU0 off", "关闭超线程+能效核+CPU 0" },
    { "Normal",               "普通" },
    { "Above normal",         "高于普通" },
    { "High",                 "高" },
    { "Efficiency mode",      "效率模式" },
    { "Low",                  "低" },

    { "boot",                 "启动" },
    { "window",               "窗口加载" },
    { "play",                 "游玩" },

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
