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
#include "log.h"

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

/** plugins\<name>\lang.ini, the text file beside the plugin. Read by
 *  the framework for that plugin's menus; see @ref lang. */
SH_API int ShPluginLangPath(const char *plugin, char *buf, int size) {
    if (!plugin || !buf || size <= 0) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    if (snprintf(buf, size, "%splugins\\%s\\lang.ini",
                 GameDir(), plugin) < 0) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}

/* ---- main config --------------------------------------------- */

/* Forward declarations: the parser, the text tables and the
 * lookup helpers are defined below in an order that is easy to
 * read, so the cross calls get a prototype up front. */
static void LoadConfig(void);
static void ResolveLanguage(void);
static void PeekLanguages(const char *text);
static int  IsLangCodeLike(const char *sec);
static int  LangSectionIgnored(const char *sec);
void        ShTextInitFramework(void);      /* scripthook_text.c */
SH_API const char *ShLangText(const char *owner, const char *key);

#define CONFIG_MAX  65536u
#define ENTRIES_MAX 256

typedef struct {
    char section[64];
    char key[96];
    char value[256];
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
    "; The play stage's priority: 0 leave alone, 1 normal, 2 above normal,\n"
    "; 3 high. 0 is the default. The play stage is everything from the\n"
    "; first main menu on. Realtime is offered nowhere: it can starve the\n"
    "; desktop and the audio threads. The engine sets a class of its own\n"
    "; during start up, so a stage that holds one keeps it held.\n"
    "cpu_prio_play=0\n"
    "; The two loading stages - the logo screen and the first load into the\n"
    "; menu - have this one switch between them: 1 = hold the Windows 11\n"
    "; efficiency mode (EcoQoS, the column Task Manager shows) while they\n"
    "; last, which lets the scheduler run the process slower and on the\n"
    "; efficiency cores: 0 (the default) leaves the class alone, 1 holds\n"
    "; the mode, 2 drops it for those two stages. 0 and 1 keep the meaning\n"
    "; the old on/off switch had.\n"
    "; Where the machine cannot do efficiency mode - it is a Windows 11\n"
    "; feature, and before it the same request only marks the process\n"
    "; LowQoS - 1 holds the LOW priority class instead: a loading screen\n"
    "; yielding the machine is the same intent, said with what is there.\n"
    "; One log line and the CPU page both say when that happens. Either\n"
    "; way the play stage puts it back, and a state the process arrived\n"
    "; with is never touched at all.\n"
    "cpu_eco_boot=0\n"
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
    "; One line per plugin folder, and a plugin with NO line here is not\n"
    "; loaded. Write <name>=1 to load it, or switch it on in the mod menu's\n"
    "; Plugins page; either way it takes effect on the next launch. The\n"
    "; first scan writes a line for every folder it finds, so this list\n"
    "; always spells out each plugin and its state - a third-party .asi\n"
    "; dropped into plugins\\ is off until it is asked for, like any other.\n"
    "; Deleting this file is the reset: a fresh default is written, every\n"
    "; plugin off, and the [loader] dials and the language go back to their\n"
    "; defaults as well.\n"
    "; firstperson - the camera work, with a page of its own in the menu.\n"
    "; GhostNoWipe - the wipe of a Ghost Mode save when a run ends.\n"
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
    "; LastRites_dlcfix - the crash on entering the \"Last Rite\"\n"
    "; DLC. One switch, off by default, in the mod menu; the single id\n"
    "; it answers for is built in and cannot be pointed elsewhere.\n"
    "; Single player only, and delete the plugin folder once the game\n"
    "; itself is patched.\n"
    "; GhostRevive - experimental, answered, switched off in its own ini.\n"
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
    "[forgemod]\n"
    "; Forge Mod Loader: loose files under <gamedir>\\mods override entries\n"
    "; that already exist in the .forge archives. No archive is modified and\n"
    "; nothing is written to disk. Layout:\n"
    ";     mods\\<archive name>\\<file>              flat, wins over\n"
    ";     mods\\<mod name>\\<archive name>\\<file>   ordered by folder name\n"
    "; A folder whose name starts with \"~\" is skipped, a file ending in\n"
    "; \".delete\" is recognised and skipped, and \"<n>_-_<name>.data\" names\n"
    "; the entry by index and name. A replacement has to fit the room the\n"
    "; entry already has, because no other entry is ever moved.\n"
    "; Off by default: create mods\\ and set enabled=1 to use it.\n"
    "enabled=0\n"
    "; 1 = resolve and log only; nothing is served.\n"
    "dry_run=0\n"
    "; 1 = report the other archives a targeted resource also lives in.\n"
    "report_copies=1\n"
    "; 1 = override those other copies as well.\n"
    "apply_all_copies=0\n"
    "; Diagnostic rounds, off in normal use.\n"
    "probe=0\n"
    "log_reads=0\n"
    "\n"
    "[Settings]\n"
    "; Menu language: a standard code - zh-CN = Chinese (default),\n"
    "; en-US = English. Case does not matter.\n"
    "Language=zh-CN\n"
    "; Languages the menu language switch offers (comma separated;\n"
    "; the order here is the order shown; both codes are built in).\n"
    "Languages=zh-CN,en-US\n"
    "; Mod menu / plugin window UI scale, driven by the game resolution:\n"
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
    "; Text lives in a lang.ini now, not here (docs/i18n-refactor.md):\n"
    ";     <gamedir>\\lang.ini           the framework text\n"
    ";     plugins\\<name>\\lang.ini      one per plugin\n"
    "; A section is a language code ([zh-CN], [en-US]) and a row\n"
    "; overrides the text this build already ships for that key, so a\n"
    "; file carries only the lines it changes. A key starting with\n"
    "; \"@\" is a stable ID; any other key is the English literal.\n"
    "; Delete a lang.ini and the menu still reads: Chinese and English\n"
    "; text ship inside the build.\n"
    "; ------------------------------------------------------------\n";

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
 * return 0; a key=value row fills *keyOut and *valueOut and returns 1.
 * With langRules set (a lang.ini, not the settings file) a row may
 * quote its key and split on " = ", so a key or a translation that
 * contains '=' still reads back whole. */
static int ParseIniLine(char *line, char *section, size_t secCap,
                        char **keyOut, char **valueOut, int langRules) {
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
    if (langRules) {
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
    } else if (!langRules) {
        /* A trailing comment, but only when the ';' or '#' is separated
         * by whitespace - the rule this parser has always stated. It has
         * to be applied here or "probe=1  ; why" reads back as the whole
         * string and a value silently falls back to its default. The
         * translation tables are left verbatim, so a translation may
         * still contain one. */
        char *c = s;
        while (*c) {
            if ((*c == ';' || *c == '#') && c > s &&
                (c[-1] == ' ' || c[-1] == '\t')) {
                *c = 0;
                while (c > s && (c[-1] == ' ' || c[-1] == '\t')) *--c = 0;
                break;
            }
            c++;
        }
    }
    *valueOut = s;
    return 1;
}

static void ParseConfig(const char *text) {
    char section[48] = "";

    PeekLanguages(text);
    while (*text) {
        char line[512];
        char *key, *val;
        size_t i;

        if (!NextLine(&text, line, sizeof(line))) break;
        if (!ParseIniLine(line, section, sizeof(section), &key, &val, 0))
            continue;

        if (!*val) continue;

        /* A language table belongs in a lang.ini now. Rows of a
         * leftover [zh_cn] (or [zh_cn.Some page]) section are dropped
         * here so they can neither pass as settings nor crowd the
         * entry table, and one line says where they went. */
        if (LangSectionIgnored(section)) continue;
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

/* ---- text (localization) ---------------------------------------
 *
 * One rule and four sources.
 *
 *   keys      a key starting with '@' is a stable ID ("@camo.page.
 *             visibility"); anything else is a literal and is its own
 *             key. An ID survives a renamed row or page title, which
 *             is what the old "[lang.<menu title>]" scope could not,
 *             and a literal is how a plugin whose source you do not
 *             have gets translated at all.
 *   sources   plugins\<owner>\lang.ini -> <gamedir>\lang.ini -> the
 *             compiled-in baseline (ShLangDeclare) for the active
 *             language -> the same baseline for en-US -> the ID made
 *             readable, or the literal itself.
 *
 * Only the active language is held in memory; a switch re-reads the
 * files. Nothing here ever writes a lang.ini: the settings file is
 * ours, the text belongs to whoever translated it.
 */
#define LANG_CODE_MAX   16
#define LANG_LIST_MAX   8
#define LANG_KEY_MAX    512     /* a literal key can be a whole hint */
#define LANG_VAL_MAX    768     /* and its translation can be longer */
#define LROW_MAX        2048
#define BASE_MAX        2048
#define PLOAD_MAX       128

typedef struct {
    char owner[48];
    char key[LANG_KEY_MAX];
    char value[LANG_VAL_MAX];
} LangRow;

typedef struct {
    const char *owner;          /* "" = framework */
    const char *lang;
    const char *key;
    const char *text;
} BaseRow;

/* Disk layer: the rows of the files, active language only. */
static LangRow *g_rows;
static int      g_nrows;
static int      g_rowsDropped;
static int      g_truncated;

/* Compiled-in baseline: pointers into the declaring module. */
static BaseRow  g_base[BASE_MAX];
static int      g_nbase;

/* One load attempt per owner. */
static struct { char owner[48]; int done; } g_load[PLOAD_MAX];
static int      g_nload;
static int      g_fwLoaded;

static char g_langName[LANG_CODE_MAX] = "";
static char g_langList[LANG_LIST_MAX][LANG_CODE_MAX];
static int  g_nLangList;
static char g_langDisp[LANG_LIST_MAX][32];

static CRITICAL_SECTION g_textLock;
static volatile LONG    g_textLockReady = 0;
static int              g_textLogging;

static void LoadLang(const char *owner);
static int  EnsureRows(void);
static void AddRow(const char *owner, const char *key,
                   const char *value);

static void TextLog(const char *fmt, ...) {
    va_list ap;

    if (!g_textLogging) {
        LogInit("scripthook_text.log");
        g_textLogging = 1;
    }
    va_start(ap, fmt);
    Logv(fmt, ap);
    va_end(ap);
}

static void TextLock(void) {
    for (;;) {
        LONG s = InterlockedCompareExchange(&g_textLockReady, 0, 0);
        if (s == 1) break;
        if (s == 2) { Sleep(0); continue; }
        if (InterlockedCompareExchange(&g_textLockReady, 2, 0)) continue;
        InitializeCriticalSection(&g_textLock);
        InterlockedExchange(&g_textLockReady, 1);
    }
    EnterCriticalSection(&g_textLock);
}

static void TextUnlock(void) { LeaveCriticalSection(&g_textLock); }

/* Language codes are standard BCP-47 tags - zh-CN, en-US - compared
 * case-insensitively and nothing else: no short-tag fallback and no
 * "zh_cn" spelling, because no file ships one any more. */
static int LangEq(const char *a, const char *b) {
    if (!a || !b) return 0;
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

/* A section name that names a language rather than a settings group:
 * a bare primary subtag ("en", "zh") or a tag with a region subtag
 * ("zh-CN", "en_US"). A longer bare word is a settings section -
 * "loader", "forgemod", "playmode" - and must never be read as a
 * language: that mistake drops the section's rows from the config, and
 * it is silent, so the rule stays narrow on purpose. */
static int IsLangCodeLike(const char *sec) {
    const char *p = sec;
    int n = 0;

    if (!sec || !*sec) return 0;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
        p++;
        n++;
    }
    if (!*p) return n >= 2 && n <= 3;       /* "en", "zh", not "loader" */
    if (n < 2 || n > 8) return 0;
    if (*p != '-' && *p != '_') return 0;
    p++;
    n = 0;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
        p++;
        n++;
    }
    return *p == 0 && n >= 2 && n <= 4;
}

/* A leftover language section in scripthook.ini. Its rows belong in a
 * lang.ini now, so they must not pass as settings either - drop them,
 * and say where they went. Repeats of the same section stay quiet. */
static int LangSectionIgnored(const char *sec) {
    static char seen[8][24];
    static int  nSeen;
    char head[24];
    const char *dot;
    size_t n;
    int i;

    if (!sec || !sec[0]) return 0;

    /* "[zh_cn.First person]" is a language table too: judge the part
     * before the first dot. The rule in IsLangCodeLike is what tells a
     * language from a settings section, so nothing is listed here. */
    dot = strchr(sec, '.');
    n = dot ? (size_t)(dot - sec) : strlen(sec);
    if (n >= sizeof(head)) return 0;
    memcpy(head, sec, n);
    head[n] = 0;
    if (!IsLangCodeLike(head)) return 0;

    for (i = 0; i < nSeen; i++)
        if (!_stricmp(seen[i], sec)) return 1;
    if (nSeen < 8) {
        strncpy(seen[nSeen], sec, sizeof(seen[0]) - 1);
        seen[nSeen][sizeof(seen[0]) - 1] = 0;
        nSeen++;
        TextLog("scripthook.ini [%s] ignored: text lives in a lang.ini now",
                sec);
    }
    return 1;
}

/* End a string one character earlier when its last bytes are only part
 * of a character. Text cut mid-sequence is what a renderer draws as
 * "?", and a menu that shows one is worse off than one that shows a
 * shorter string.
 *
 * A COMPLETE last character must come through untouched. Walking back
 * over its continuation bytes finds its lead byte; where that character
 * ends decides what survives:
 *   ends past the string   -> it was cut: drop the lead byte too
 *   ends inside it         -> complete, but anything after it is an
 *                             orphaned tail: keep up to that point
 * An earlier version stopped at the lead byte either way, so every
 * translated row lost its last character and kept the orphaned lead
 * byte - which is the "?" that was on screen. */
void ShUtf8Trim(char *s) {
    size_t n = strlen(s);
    size_t keep = n;
    size_t end;
    unsigned char b;
    int need;

    while (keep > 0 && ((unsigned char)s[keep - 1] & 0xC0) == 0x80) keep--;
    if (keep == 0) { s[0] = 0; return; }    /* nothing but tail bytes */
    b = (unsigned char)s[keep - 1];
    need = (b < 0xC2) ? 1 : (b < 0xE0) ? 2 : (b < 0xF0) ? 3 : 4;
    end = keep - 1 + (size_t)need;
    if (end > n)
        keep--;                             /* incomplete: drop the lead */
    else
        keep = end;                         /* whole: keep it, drop orphans */
    s[keep] = 0;
}

/* Copy with a cap, reporting the first truncation it causes: a
 * silently shortened translation is exactly what this layer must not
 * do. */
static void CopyN(char *dst, size_t cap, const char *src) {
    size_t n = src ? strlen(src) : 0;

    if (n >= cap) {
        n = cap - 1;
        if (!g_truncated) {
            g_truncated = 1;
            TextLog("text longer than %u chars truncated: \"%.40s\"",
                    (unsigned)(cap - 1), src ? src : "");
        }
    }
    if (n && src) memcpy(dst, src, n);
    dst[n] = 0;
    ShUtf8Trim(dst);
}

/* Copy a value, expanding "\n" into a line break: one lang.ini row is
 * one line of file, and a hint or a toast is several lines. */
static void CopyValue(char *dst, size_t cap, const char *src) {
    size_t n = 0;

    while (src && *src && n < cap - 1) {
        if (src[0] == '\\' && src[1] == 'n') {
            dst[n++] = '\n';
            src += 2;
            continue;
        }
        dst[n++] = *src++;
    }
    dst[n] = 0;
    ShUtf8Trim(dst);
}

/* ---- disk layer ------------------------------------------------- */

static void AddRow(const char *owner, const char *key,
                   const char *value) {
    LangRow *e;

    if (!key || !key[0] || !value || !value[0]) return;
    if (!g_rows) return;                /* nothing to write into */
    if (g_nrows >= LROW_MAX) {
        if (g_rowsDropped++ == 0)
            TextLog("%d rows kept, the rest dropped (raise LROW_MAX)",
                    LROW_MAX);
        return;
    }
    e = &g_rows[g_nrows++];
    CopyN(e->owner, sizeof(e->owner), owner ? owner : "");
    CopyN(e->key, sizeof(e->key), key);
    CopyValue(e->value, sizeof(e->value), value);
}

/* The owner's rows win over the shared ones. First match wins, so a
 * key written twice in one file keeps the line that came first. */
static const char *RowFind(const char *owner, const char *key) {
    int i, pass;
    int wantOwner = (owner && owner[0]) ? 1 : 0;

    for (pass = 0; pass < 2; pass++) {
        if (pass == 0 && !wantOwner) continue;
        for (i = 0; i < g_nrows; i++) {
            if (pass == 0) {
                if (_stricmp(g_rows[i].owner, owner)) continue;
            } else if (g_rows[i].owner[0]) {
                continue;
            }
            if (!strcmp(g_rows[i].key, key)) return g_rows[i].value;
        }
    }
    return NULL;
}

/* The compiled-in baseline: the owner's rows first, then the
 * framework's, both for one language. */
static const char *BaseFind(const char *owner, const char *lang,
                            const char *key) {
    int i;

    if (!lang || !lang[0] || !key) return NULL;
    if (owner && owner[0]) {
        for (i = 0; i < g_nbase; i++)
            if (g_base[i].owner[0] &&
                !_stricmp(g_base[i].owner, owner) &&
                LangEq(g_base[i].lang, lang) &&
                !strcmp(g_base[i].key, key))
                return g_base[i].text;
    }
    for (i = 0; i < g_nbase; i++)
        if (!g_base[i].owner[0] && LangEq(g_base[i].lang, lang) &&
            !strcmp(g_base[i].key, key))
            return g_base[i].text;
    return NULL;
}

/* The readable form of a key with no text anywhere: an ID loses its
 * '@' and gains word breaks ("@camo.page.visibility" -> "Camo Page
 * Visibility"); a literal is already readable. Four rotating buffers,
 * because a menu capture holds a title, a hint and a status line at
 * once - and each as long as a key can be, because a literal key IS a
 * whole hint. At 64 bytes this cut one (the order page's hint, which a
 * caller had handed over translated rather than as the key it is). */
static const char *Readable(const char *key) {
    static char buf[4][LANG_KEY_MAX];
    static int  slot;
    char *out;
    size_t n = 0;
    int upper = 1;

    if (!key) return "";
    out = buf[slot = (slot + 1) & 3];
    if (key[0] != '@') {
        CopyN(out, sizeof(buf[0]), key);
        return out;
    }
    for (key++; *key && n < sizeof(buf[0]) - 1; key++) {
        char c = *key;
        if (c == '.' || c == '_' || c == '-') {
            c = ' ';
            upper = 1;
        } else if (upper) {
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            upper = 0;
        }
        out[n++] = c;
    }
    out[n] = 0;
    return out;
}

/* ---- [Settings] Language / Languages ---------------------------- */

/* "zh-CN, en-US" -> the picker's list, in the order written. */
static void ParseLangList(const char *text) {
    int n = 0;

    g_nLangList = 0;
    while (*text && n < LANG_LIST_MAX) {
        char code[LANG_CODE_MAX];
        const char *comma = strchr(text, ',');
        size_t len = comma ? (size_t)(comma - text) : strlen(text);
        size_t lead = 0, i;

        while (lead < len && (text[lead] == ' ' || text[lead] == '\t'))
            lead++;
        while (len > lead && (text[len - 1] == ' ' || text[len - 1] == '\t'))
            len--;
        if (len > lead && (text[lead] == '"' || text[lead] == '\''))
            lead++;
        if (len > lead && (text[len - 1] == text[lead - 1]))
            len--;
        i = len - lead;
        if (i >= sizeof(code)) i = sizeof(code) - 1;
        if (i) {
            memcpy(code, text + lead, i);
            code[i] = 0;
            CopyN(g_langList[n], sizeof(g_langList[n]), code);
            n++;
        }
        if (!comma) break;
        text = comma + 1;
    }
    g_nLangList = n;
}

/* One quick pass for the language settings, because "is this section a
 * language table" has to be answered while the rows are read. */
static void PeekLanguages(const char *text) {
    const char *p = text;
    int inSettings = 0;

    while (*p) {
        char line[256];
        const char *s;
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
        if (!inSettings) continue;
        if (!_strnicmp(s, "Languages=", 10)) {
            ParseLangList(s + 10);
            continue;
        }
        if (!_strnicmp(s, "Language=", 9)) {
            const char *v = s + 9;
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
        }
    }
}

/* ---- languages -------------------------------------------------- */

/* The languages this build ships text for, in declaration order. */
static char g_builtin[LANG_LIST_MAX][LANG_CODE_MAX];
static int  g_nBuiltin;

/* [LanguageNames] rows: the label to show for a code. Not gated by
 * the active language, so the picker reads in any language. */
#define DISP_MAX 16
static struct { char code[LANG_CODE_MAX]; char label[32]; } g_disp[DISP_MAX];
static int g_nDisp;

static void LangNameAdd(const char *code, const char *label);

/* The languages the build declares text for, then what the file asked
 * for. [Settings] Languages wins when it lists one (it may offer a
 * subset, or a language that only some plugins translate yet);
 * otherwise the picker offers everything the build ships - so adding
 * a language to the compile-time tables is enough to offer it. */
static void ResolveLanguage(void) {
    int i, keep;

    ShTextInitFramework();              /* the framework's own text */

    TextLock();
    for (i = 0; i < g_nbase && g_nBuiltin < LANG_LIST_MAX; i++) {
        int j, dup = 0;
        if (!g_base[i].lang || !g_base[i].lang[0]) continue;
        for (j = 0; j < g_nBuiltin; j++)
            if (LangEq(g_builtin[j], g_base[i].lang)) { dup = 1; break; }
        if (dup) continue;
        CopyN(g_builtin[g_nBuiltin], sizeof(g_builtin[0]), g_base[i].lang);
        g_nBuiltin++;
    }
    TextUnlock();

    if (!g_nLangList) {
        keep = g_nBuiltin;
        for (i = 0; i < keep; i++)
            CopyN(g_langList[i], sizeof(g_langList[i]), g_builtin[i]);
        g_nLangList = keep;
    }
    if (!g_langName[0]) {
        if (g_nLangList)
            CopyN(g_langName, sizeof(g_langName), g_langList[0]);
        else
            CopyN(g_langName, sizeof(g_langName), "en-US");
    }
}

/* ---- compiled-in baseline --------------------------------------- */

/** Declare one language's worth of this module's text. */
SH_API int ShLangDeclare(const char *owner, const char *lang,
                         const ShText *rows, int n) {
    int i, kept = 0;

    if (!lang || !lang[0] || !rows || n <= 0) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    if (!owner) owner = "";
    TextLock();
    for (i = 0; i < n; i++) {
        if (!rows[i].id || !rows[i].id[0]) continue;
        if (g_nbase >= BASE_MAX) {
            TextLog("baseline full at %d rows: \"%s\" dropped",
                    BASE_MAX, rows[i].id);
            break;
        }
        g_base[g_nbase].owner = owner;
        g_base[g_nbase].lang  = lang;
        g_base[g_nbase].key   = rows[i].id;
        g_base[g_nbase].text  = rows[i].text ? rows[i].text : "";
        g_nbase++;
        kept++;
    }
    TextUnlock();
    ShSetError(SH_OK);
    return kept ? 1 : 0;
}

/** One of the languages the build ships text for: "code<TAB>label". */
SH_API int ShLangBuiltin(int i, char *buf, int size) {
    int n;

    LoadConfig();
    if (!buf || size <= 0 || i < 0 || i >= g_nBuiltin) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    n = snprintf(buf, size, "%s\t%s", g_builtin[i], ShLangLabel(g_builtin[i]));
    ShSetError(n < 0 ? SH_ERR_BAD_ARG : SH_OK);
    return n >= 0 && n < size;
}

/** The label to show for a language code: a [LanguageNames] row in a
 *  lang.ini, else the compiled-in name ("@lang.name.zh-CN"), else the
 *  code itself - so the picker reads with no file at all. */
SH_API const char *ShLangLabel(const char *code) {
    static char buf[32];
    int i;

    if (!code || !code[0]) return "";
    LoadConfig();
    TextLock();
    LoadLang(NULL);                     /* [LanguageNames] lives there */
    for (i = 0; i < g_nDisp; i++) {
        if (!LangEq(g_disp[i].code, code)) continue;
        CopyN(buf, sizeof(buf), g_disp[i].label);
        TextUnlock();
        return buf;
    }
    for (i = 0; i < g_nbase; i++) {
        const char *k = g_base[i].key;

        if (!k || _strnicmp(k, "@lang.name.", 11)) continue;
        if (!LangEq(k + 11, code)) continue;
        CopyN(buf, sizeof(buf), g_base[i].text);
        TextUnlock();
        return buf;
    }
    TextUnlock();
    return code;
}

/* A key with no text anywhere is worth one line, not one per frame:
 * menu rows are translated on every capture. */
static void LangMissOnce(const char *key) {
    static char seen[32][LANG_KEY_MAX];
    static int  nSeen;
    int i;

    for (i = 0; i < nSeen; i++)
        if (!strcmp(seen[i], key)) return;
    if (nSeen >= 32) return;
    CopyN(seen[nSeen++], sizeof(seen[0]), key);
    TextLog("no text for \"%s\"", key);
}

/* ---- lookup ----------------------------------------------------- */

/** Translate one key for one owner (NULL or "" = framework text). */
SH_API const char *ShLangText(const char *owner, const char *key) {
    const char *v = NULL;

    if (!key) return "";
    LoadConfig();
    TextLock();
    if (EnsureRows()) {
        LoadLang(owner);
        v = RowFind(owner, key);
    }
    if (!v) v = BaseFind(owner, g_langName, key);
    if (!v && !LangEq(g_langName, "en-US"))
        v = BaseFind(owner, "en-US", key);
    if (!v) {
        v = Readable(key);              /* never empty, never a failure */
        if (key[0] == '@') LangMissOnce(key);
    }
    TextUnlock();
    return v;
}

/** Is there text for this key anywhere? */
SH_API int ShLangHas(const char *owner, const char *key) {
    const char *v = NULL;

    if (!key || !key[0]) return 0;
    LoadConfig();
    TextLock();
    if (EnsureRows()) {
        LoadLang(owner);
        v = RowFind(owner, key);
    }
    if (!v) v = BaseFind(owner, g_langName, key);
    if (!v && !LangEq(g_langName, "en-US"))
        v = BaseFind(owner, "en-US", key);
    TextUnlock();
    return v != NULL;
}

/** The framework's own comparison, for callers that keep a code of
 *  their own: same as the one used to match tables and labels. */
SH_API int ShLangMatch(const char *a, const char *b) {
    return LangEq(a, b);
}

/** Switch the active language now: the text layer drops what it read
 *  for the old one and the next lookup reads the files again. Stored
 *  text (a status line, a toast) is put away rather than left in the
 *  language it was written in - a page that refreshes itself fills its
 *  line again on the next tick. This does not write scripthook.ini;
 *  the caller decides whether the choice is worth persisting. */
SH_API int ShLangSet(const char *code) {
    if (!code || !code[0]) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    LoadConfig();
    TextLock();
    CopyN(g_langName, sizeof(g_langName), code);
    g_nrows = 0;                /* the files are read for this one only */
    g_nload = 0;
    g_fwLoaded = 0;
    g_nDisp = 0;                /* [LanguageNames] is a file's to give */
    g_rowsDropped = 0;
    TextUnlock();

    ShMenuStatusResetAll();
    ShToastClear();
    TextLog("language is now %s", g_langName);
    ShSetError(SH_OK);
    return 1;
}

/** Translate framework text. */
SH_API const char *ShLang(const char *text) {
    return ShLangText(NULL, text);
}

/** Retained spelling: a key decides what text a row gets, so the
 *  scope is not used any more. */
SH_API const char *ShLangFor(const char *scope, const char *text) {
    (void)scope;
    return ShLangText(NULL, text);
}

/* ---- template formatting ----------------------------------------
 *
 * A translated template may reorder the values with "%n$" (docs/
 * i18n-refactor.md 3.3, plan B). The English template is the one the
 * caller wrote and the one the compiler checked, so its conversions
 * decide the types on the argument list: a translation says which
 * argument each of its own conversions uses, and the two have to agree
 * at every index.
 *
 * A template that does not agree - a bare "%", an index that is not
 * there, "%s" where the English has "%d", an unsupported "*", a mix of
 * indexed and plain conversions - is logged once and formatted from the
 * ENGLISH template instead. A wrong "%" in a lang.ini must never reach
 * vsnprintf: it would read the wrong type off the argument list, which
 * is garbage output at best and a crash at worst.
 *
 * A template with no index at all - every template in the tree today -
 * still goes straight to vsnprintf, after the same type check, so the
 * cost on that path is one scan of two short strings.
 */

typedef enum {
    CK_INT = 0,     /* %d %i %u %o %x %X %c, with or without h/hh */
    CK_LONG,        /* the l length */
    CK_LLONG,       /* ll, I64, and z/j/t (size_t and its neighbours) */
    CK_DOUBLE,      /* %e %f %g %a, with or without l */
    CK_LDOUBLE,     /* the same with L */
    CK_STR,         /* %s */
    CK_PTR,         /* %p */
    CK_PCT,         /* %% - takes no argument */
    CK_BAD          /* unsupported or malformed */
} ConvKind;

typedef struct {
    int      len;       /* bytes of the conversion, '%' included */
    int      index;     /* 1-based argument, 0 when none was written */
    int      idxAt;     /* offset of the index digits, 0 when none */
    int      idxLen;    /* bytes of "n$" */
    ConvKind kind;
} Conv;

#define CONV_MAX 16

/* Parse the conversion at p, which points at '%'. Returns its length in
 * bytes, or 0 when it is malformed ("%" at the end, "*" width, "%n",
 * "%ls" - anything whose value this code cannot fetch safely). */
static int ScanConv(const char *p, Conv *c)
{
    int i = 1;
    int len = 0;            /* 0 none, 1 h/hh, 2 l, 3 ll/I64/z/j/t, 4 L */
    ConvKind k;

    c->len = c->idxAt = c->idxLen = 0;
    c->index = 0;
    c->kind = CK_BAD;
    if (p[0] != '%') return 0;
    if (p[1] == '%') { c->len = 2; c->kind = CK_PCT; return 2; }

    while (p[i] && strchr("-+ #0'", p[i])) i++;
    if (p[i] >= '0' && p[i] <= '9') {
        const char *d = p + i;
        int n = 0;

        while (*d >= '0' && *d <= '9') { n = n * 10 + (*d - '0'); d++; }
        if (*d == '$') {
            /* an argument index, then the flags and width that follow
             * it (POSIX order: %[n$][flags][width][.prec][length]) */
            c->index = n;
            c->idxAt = i;
            c->idxLen = (int)(d - (p + i)) + 1;
            i += c->idxLen;
            while (p[i] && strchr("-+ #0'", p[i])) i++;
            if (p[i] >= '0' && p[i] <= '9')
                while (p[i] >= '0' && p[i] <= '9') i++;
        } else {
            i = (int)(d - p);           /* it was the width */
        }
    }
    if (p[i] == '*') return 0;          /* a width from the argument list */
    if (p[i] == '.') {
        i++;
        if (p[i] == '*') return 0;      /* so is a precision */
        while (p[i] >= '0' && p[i] <= '9') i++;
    }
    switch (p[i]) {
    case 'h': i++; if (p[i] == 'h') i++; len = 1; break;
    case 'l': i++; if (p[i] == 'l') { i++; len = 3; } else len = 2; break;
    case 'L': i++; len = 4; break;
    case 'z': case 'j': case 't': i++; len = 3; break;
    case 'I':
        i++;
        if (p[i] == '3' && p[i + 1] == '2') i += 2;
        else if (p[i] == '6' && p[i + 1] == '4') i += 2;
        len = 3;
        break;
    default: break;
    }

    switch (p[i]) {
    case 'd': case 'i': case 'u': case 'o': case 'x': case 'X':
        k = (len == 0 || len == 1) ? CK_INT
          : (len == 2)            ? CK_LONG : CK_LLONG;
        break;
    case 'c':
        k = (len == 0 || len == 1) ? CK_INT : CK_BAD;
        break;
    case 'e': case 'E': case 'f': case 'F': case 'g': case 'G':
    case 'a': case 'A':
        k = (len == 4) ? CK_LDOUBLE : CK_DOUBLE;
        break;
    case 's':
        k = (len == 0) ? CK_STR : CK_BAD;   /* %ls is a wide string */
        break;
    case 'p':
        k = (len == 0) ? CK_PTR : CK_BAD;
        break;
    default:
        return 0;                           /* %n, and anything unknown */
    }
    c->kind = k;
    c->len = i + 1;
    return c->len;
}

/* Every conversion of a template, in file order, "%%" left out because
 * it takes no argument. Returns the count, or -1 when malformed. */
static int CollectConvs(const char *t, Conv *list)
{
    int n = 0;

    while (t && *t) {
        Conv c;
        int step;

        if (*t != '%') { t++; continue; }
        step = ScanConv(t, &c);
        if (step <= 0) return -1;
        if (c.kind != CK_PCT) {
            if (n >= CONV_MAX) return -1;
            list[n++] = c;
        }
        t += step;
    }
    return n;
}

/* Append a piece, keeping the buffer terminated. Returns the new
 * offset, which stops growing once the buffer is full. */
static int AppendText(char *dst, size_t cap, int at, const char *piece)
{
    size_t n = strlen(piece);

    if (cap == 0) return at;
    if ((size_t)at >= cap - 1) return at;
    if (n > cap - 1 - (size_t)at) n = cap - 1 - (size_t)at;
    memcpy(dst + at, piece, n);
    at += (int)n;
    dst[at] = 0;
    return at;
}

/* Take one argument of the given kind off the list and drop it: a
 * translation is free to leave values out of order, so the ones it does
 * not use still have to be stepped over. */
static void SkipArg(va_list *ap, ConvKind k)
{
    switch (k) {
    case CK_INT:     (void)va_arg(*ap, int); break;
    case CK_LONG:    (void)va_arg(*ap, long); break;
    case CK_LLONG:   (void)va_arg(*ap, long long); break;
    case CK_DOUBLE:  (void)va_arg(*ap, double); break;
    case CK_LDOUBLE: (void)va_arg(*ap, long double); break;
    case CK_STR:     (void)va_arg(*ap, const char *); break;
    case CK_PTR:     (void)va_arg(*ap, void *); break;
    default: break;                     /* %% and CK_BAD take none */
    }
}

/* Render one conversion (its own flags and width, no index) from the
 * next argument of the given kind, and append the result. */
static int RenderOne(char *dst, size_t cap, int at, const char *norm,
                     ConvKind k, va_list *ap)
{
    char piece[384];
    int n;

    /* Every value is taken into a local first: MSVC's va_arg macro does
     * not splice cleanly into another call's argument list. */
    switch (k) {
    case CK_INT: {
        int v = va_arg(*ap, int);
        n = snprintf(piece, sizeof(piece), norm, v);
        break;
    }
    case CK_LONG: {
        long v = va_arg(*ap, long);
        n = snprintf(piece, sizeof(piece), norm, v);
        break;
    }
    case CK_LLONG: {
        long long v = va_arg(*ap, long long);
        n = snprintf(piece, sizeof(piece), norm, v);
        break;
    }
    case CK_DOUBLE: {
        double v = va_arg(*ap, double);
        n = snprintf(piece, sizeof(piece), norm, v);
        break;
    }
    case CK_LDOUBLE: {
        long double v = va_arg(*ap, long double);
        n = snprintf(piece, sizeof(piece), norm, v);
        break;
    }
    case CK_STR: {
        const char *v = va_arg(*ap, char *);
        n = snprintf(piece, sizeof(piece), norm, v ? v : "");
        break;
    }
    case CK_PTR: {
        void *v = va_arg(*ap, void *);
        n = snprintf(piece, sizeof(piece), norm, v);
        break;
    }
    default:
        piece[0] = 0;
        n = 0;
        break;
    }
    if (n < 0) piece[0] = 0;
    return AppendText(dst, cap, at, piece);
}

/* Format a template whose conversions all carry an index, taking every
 * value from the position the translation names. `en` is the English
 * conversion list: it says what kind each argument is, which is what
 * decides how the value is fetched. */
static int FormatPositional(char *dst, size_t cap, const char *t,
                            const Conv *en, va_list *base)
{
    int at = 0;
    const char *p = t;

    if (cap) dst[0] = 0;
    while (*p) {
        const char *pc;
        char norm[40];
        Conv c;
        va_list a;
        int step, k, n;

        if (*p != '%') {
            pc = strchr(p, '%');
            n = pc ? (int)(pc - p) : (int)strlen(p);
            if ((size_t)n > sizeof(norm) - 1) n = sizeof(norm) - 1;
            memcpy(norm, p, (size_t)n);
            norm[n] = 0;
            at = AppendText(dst, cap, at, norm);
            p += n;
            continue;
        }
        step = ScanConv(p, &c);
        if (step <= 0) break;               /* cannot happen: checked */
        if (c.kind == CK_PCT) {
            at = AppendText(dst, cap, at, "%");
            p += step;
            continue;
        }

        /* the same conversion with its "n$" taken out */
        n = 0;
        for (k = 0; k < c.idxAt && n < (int)sizeof(norm) - 1; k++)
            norm[n++] = p[k];
        for (k = c.idxAt + c.idxLen; k < c.len && n < (int)sizeof(norm) - 1;
             k++)
            norm[n++] = p[k];
        norm[n] = 0;

        va_copy(a, *base);
        for (k = 1; k < c.index; k++) SkipArg(&a, en[k - 1].kind);
        at = RenderOne(dst, cap, at, norm, en[c.index - 1].kind, &a);
        va_end(a);
        p += step;
    }
    return at;
}

/* One line per bad template, not one per frame: a status line is
 * formatted ~25 times a second, and a translator needs the line once. */
static void RejectLog(const char *tr, const char *why)
{
    static unsigned seen[16];
    static int nSeen;
    unsigned h = 2166136261u;
    const char *p;
    int i;

    for (p = tr; p && *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    for (i = 0; i < nSeen; i++)
        if (seen[i] == h) return;
    if (nSeen >= 16) return;
    seen[nSeen++] = h;
    TextLog("template \"%.60s\" rejected (%s): using the English one",
            tr ? tr : "", why);
}

int ShTextFormatV(char *dst, size_t cap, const char *en, const char *tr,
                  va_list ap)
{
    Conv ctr[CONV_MAX], cen[CONV_MAX];
    int ntr, nen, i, indexed = 0, used[CONV_MAX + 1];
    const char *why = NULL;
    va_list a;
    int r;

    if (!dst || cap == 0) return 0;
    dst[0] = 0;
    if (!tr || !tr[0]) tr = en;
    if (!tr || !tr[0]) return 0;
    if (!en || !en[0]) en = tr;

    ntr = CollectConvs(tr, ctr);
    nen = CollectConvs(en, cen);

    if (nen < 0) {
        /* The English side is compiled and the compiler checked it, so
         * this is not a case to report: format the translation as it
         * stands, which is what the caller would have done itself. */
        va_copy(a, ap);
        r = vsnprintf(dst, cap, tr, a);
        va_end(a);
        if (r < 0) r = 0;
        ShUtf8Trim(dst);
        return r;
    }

    if (ntr < 0) {
        why = "a % that is not a conversion";
    } else {
        for (i = 0; i < ntr; i++) {
            if (ctr[i].kind == CK_BAD) {
                why = "a conversion this build cannot read a value for";
                break;
            }
            if (ctr[i].index > 0) indexed = 1;
        }
    }

    if (!why && indexed) {
        if (ntr != nen) {
            why = "a different number of values";
        } else {
            for (i = 0; i <= nen; i++) used[i] = 0;
            for (i = 0; i < ntr; i++) {
                int n = ctr[i].index;

                if (n < 1 || n > nen) {
                    why = "an index that is not there";
                    break;
                }
                if (used[n]) {
                    why = "the same value twice";
                    break;
                }
                if (ctr[i].kind != cen[n - 1].kind) {
                    why = "a value of the wrong type";
                    break;
                }
                used[n] = 1;
            }
            for (i = 1; i <= nen && !why; i++)
                if (!used[i]) why = "a value left out";
        }
    } else if (!why) {
        if (ntr != nen) {
            why = "a different number of values";
        } else {
            for (i = 0; i < ntr; i++)
                if (ctr[i].kind != cen[i].kind) {
                    why = "a value of the wrong type";
                    break;
                }
        }
    }

    va_copy(a, ap);
    if (why) {
        RejectLog(tr, why);
        r = vsnprintf(dst, cap, en, a);
    } else if (indexed) {
        FormatPositional(dst, cap, tr, cen, &a);
        r = (int)strlen(dst);
    } else {
        r = vsnprintf(dst, cap, tr, a);
    }
    va_end(a);
    if (r < 0) r = 0;
    ShUtf8Trim(dst);
    return r;
}

int ShTextFormat(char *dst, size_t cap, const char *en, const char *tr, ...)
{
    va_list ap;
    int r;

    va_start(ap, tr);
    r = ShTextFormatV(dst, cap, en, tr, ap);
    va_end(ap);
    return r;
}

/** Internal: the en-US text for a key, or NULL when this build has
 *  none. A template is checked against it, and a rejected translation
 *  is formatted from it. */
const char *ShTextEnUS(const char *owner, const char *key)
{
    const char *v = NULL;

    if (!key || !key[0]) return NULL;
    LoadConfig();
    TextLock();
    v = BaseFind(owner, "en-US", key);
    TextUnlock();
    return v;
}

/* ---- diagnostics ------------------------------------------------
 *
 * What a translator needs to see, and what this can honestly say. The
 * baseline's en-US rows are the authority for what the build can say: a
 * baseline key with no text in the active language exists in English
 * only. The other half is a row whose value is still plain ASCII - a
 * file that carries the English line back has not been translated yet,
 * which is exactly how the hint rows for plugins without source looked.
 *
 * Not covered: a literal key that no file mentions. Those live in a
 * plugin's code and cannot be enumerated, so the report names what it
 * can prove.
 */

#define DIAG_MAX 256

/* A value with no byte above 0x7F. A translated hint never looks like
 * that. The language's own name for English does, so those rows are
 * left out by key. */
static int TextIsAscii(const char *s) {
    if (!s) return 1;
    for (; *s; s++)
        if ((unsigned char)*s >= 0x80) return 0;
    return 1;
}

/* A row an earlier row of the same owner and key already answers:
 * RowFind stops at the first match, so this one is dead, and a dead row
 * must not be reported as text that is still wanted. Hand-edited files
 * grow them easily - a translated row added at the top while the
 * English line it replaces stays where it was. */
static int RowShadowed(int idx) {
    int j;

    for (j = 0; j < idx; j++)
        if (!_stricmp(g_rows[j].owner, g_rows[idx].owner) &&
            !strcmp(g_rows[j].key, g_rows[idx].key))
            return 1;
    return 0;
}

static void MissFill(ShLangMissRow *out, int max, int *used,
                     const char *owner, const char *key, const char *en) {
    if (!out || !used || *used >= max) return;
    CopyN(out[*used].owner, sizeof(out[0].owner), owner ? owner : "");
    CopyN(out[*used].key, sizeof(out[0].key), key ? key : "");
    CopyN(out[*used].en, sizeof(out[0].en), en ? en : "");
    (*used)++;
}

int ShLangDiag(ShLangMissRow *out, int max, int *missing, int *english,
               int *orphan, int *dup, int *dropped) {
    int i, used = 0;
    int nMiss = 0, nEng = 0, nOrph = 0, nDup = 0, nDrop = 0;

    if (out && max > 0) memset(out, 0, sizeof(out[0]) * (size_t)max);

    LoadConfig();
    TextLock();
    if (EnsureRows()) {
        LoadLang(NULL);         /* the shared file and the framework's */

        /* keys the baseline has in English only */
        for (i = 0; i < g_nbase; i++) {
            const char *own = g_base[i].owner;

            if (!g_base[i].lang || _stricmp(g_base[i].lang, "en-US"))
                continue;
            if (!g_base[i].key || !g_base[i].key[0]) continue;
            if (!_strnicmp(g_base[i].key, "@lang.name.", 11)) continue;
            if (BaseFind(own, g_langName, g_base[i].key)) continue;
            nMiss++;
            MissFill(out, max, &used, own, g_base[i].key, g_base[i].text);
        }

        /* rows still carrying the English line, the effective one only */
        for (i = 0; i < g_nrows; i++) {
            if (RowShadowed(i)) continue;
            if (!_strnicmp(g_rows[i].key, "@lang.name.", 11)) continue;
            if (!TextIsAscii(g_rows[i].value)) continue;
            nEng++;
            MissFill(out, max, &used, g_rows[i].owner, g_rows[i].key,
                     g_rows[i].value);
        }

        /* "@" keys nothing declares, and rows an earlier one shadows */
        for (i = 0; i < g_nrows; i++) {
            if (RowShadowed(i)) {
                nDup++;
                continue;
            }
            if (g_rows[i].key[0] == '@' &&
                !BaseFind(g_rows[i].owner, "en-US", g_rows[i].key))
                nOrph++;
        }
        nDrop = g_rowsDropped;
    }
    TextUnlock();

    if (missing) *missing = nMiss;
    if (english) *english = nEng;
    if (orphan)  *orphan  = nOrph;
    if (dup)     *dup     = nDup;
    if (dropped) *dropped = nDrop;
    return used;
}

int ShLangSkeleton(char *path, int cap) {
    ShLangMissRow *rows;
    char dir[GAME_DIR_MAX];
    char file[GAME_DIR_MAX];
    FILE *f;
    const char *p;
    int n, i, miss = 0, eng = 0, orph = 0, dup = 0, drop = 0;

    rows = (ShLangMissRow *)calloc(DIAG_MAX, sizeof(ShLangMissRow));
    if (!rows) return -1;

    n = ShLangDiag(rows, DIAG_MAX, &miss, &eng, &orph, &dup, &drop);
    if (snprintf(dir, sizeof(dir), "%slang", GameDir()) < 0 ||
        snprintf(file, sizeof(file), "%s\\%s.missing.ini", dir,
                 g_langName) < 0) {
        free(rows);
        return -1;
    }
    CreateDirectoryA(dir, NULL);
    if (path && cap > 0) CopyN(path, (size_t)cap, file);

    f = fopen(file, "wb");
    if (!f) {
        TextLog("skeleton: cannot write %s", file);
        free(rows);
        return -1;
    }
    fprintf(f,
            "; Text this build still wants in %s.\n"
            "; Each key is followed by what it says in English: fill a\n"
            "; value in and move the row to the file it belongs to -\n"
            ";   (framework)  ->  <gamedir>\\lang.ini\n"
            ";   <owner>      ->  plugins\\<owner>\\lang.ini\n"
            "; all of them under a [%s] section. A key starting with @ is a\n"
            "; stable ID; any other key is the English literal a plugin\n"
            "; passes, which is how a plugin without source is translated.\n"
            "; An empty value is ignored, so an unfinished row is safe.\n"
            ";\n"
            "; %d row(s) still in English, %d with no text at all,\n"
            "; %d \"@\" key(s) nothing declares, %d row(s) repeated,\n"
            "; %d row(s) dropped (table full).\n",
            g_langName, g_langName, eng, miss, orph, dup, drop);

    for (i = 0; i < n; i++) {
        if (i == 0 || _stricmp(rows[i].owner, rows[i - 1].owner))
            fprintf(f, "\n; ---- %s ----\n",
                    rows[i].owner[0] ? rows[i].owner : "(framework)");
        fputs("; ", f);
        for (p = rows[i].en; p && *p; p++) {
            if (*p == '\n') fputs("\n; ", f);
            else fputc(*p, f);
        }
        fputc('\n', f);
        fprintf(f, "\"%s\" = \"\"\n", rows[i].key);
    }
    fclose(f);
    free(rows);
    TextLog("skeleton: %s (%d row(s) to translate)", file, n);
    return n;
}

/* ---- lang.ini --------------------------------------------------- */

/* [LanguageNames] rows: the label to show for a code, so the picker
 * reads the same in every language. */
static void LangNameAdd(const char *code, const char *label) {
    int i;

    if (!code || !code[0] || !label || !label[0]) return;
    for (i = 0; i < g_nDisp; i++) {
        if (!LangEq(g_disp[i].code, code)) continue;
        CopyN(g_disp[i].label, sizeof(g_disp[i].label), label);
        return;
    }
    if (g_nDisp >= DISP_MAX) {
        TextLog("[LanguageNames] full at %d rows: \"%s\" dropped",
                DISP_MAX, code);
        return;
    }
    CopyN(g_disp[g_nDisp].code, sizeof(g_disp[0].code), code);
    CopyN(g_disp[g_nDisp].label, sizeof(g_disp[0].label), label);
    g_nDisp++;
}

/* One lang.ini. Rows of the active language are kept; rows of another
 * language are skipped on purpose (a switch re-reads the file), and
 * one line says what was found so a missing language is not silent. */
static void ParseLangText(const char *owner, const char *text) {
    char section[64] = "";
    int kept = 0, others = 0;

    while (text && *text) {
        char line[1024];
        char *key, *val;

        if (!NextLine(&text, line, sizeof(line))) break;
        if (!ParseIniLine(line, section, sizeof(section), &key, &val, 1))
            continue;
        if (!val || !*val || !key || !key[0]) continue;

        if (!_stricmp(section, "LanguageNames")) {
            LangNameAdd(key, val);
            continue;
        }
        if (LangEq(section, g_langName)) {
            AddRow(owner, key, val);
            kept++;
        } else if (IsLangCodeLike(section)) {
            others++;
        }
    }
    TextLog("%s lang.ini: %d row(s) for %s%s",
            owner && owner[0] ? owner : "(framework)", kept, g_langName,
            others ? ", other languages skipped" : "");
}

/* <gamedir>\lang.ini for the framework, plugins\<owner>\lang.ini for a
 * plugin. One attempt per owner, and a missing file counts as done:
 * a plugin without text costs one fopen, not one per lookup. */
static void LoadLangFile(const char *owner) {
    char path[GAME_DIR_MAX];
    static char text[CONFIG_MAX];
    FILE *f;
    size_t n;
    int i, slot = -1;

    if (owner && owner[0]) {
        for (i = 0; i < g_nload; i++)
            if (!_stricmp(g_load[i].owner, owner)) { slot = i; break; }
        if (slot >= 0 && g_load[slot].done) return;
        if (slot < 0) {
            if (g_nload >= PLOAD_MAX) {
                TextLog("too many owners (%d): \"%s\" has no lang.ini",
                        PLOAD_MAX, owner);
                return;
            }
            slot = g_nload++;
            CopyN(g_load[slot].owner, sizeof(g_load[0].owner), owner);
        }
        g_load[slot].done = 1;
        if (!ShPluginLangPath(owner, path, sizeof(path))) return;
    } else {
        if (g_fwLoaded) return;
        g_fwLoaded = 1;
        if (snprintf(path, sizeof(path), "%slang.ini", GameDir()) < 0)
            return;
    }

    f = fopen(path, "rb");
    if (!f) return;
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    /* A UTF-8 BOM would become part of the first section name and cost
     * that whole table; step over it. */
    if (n >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        ParseLangText(owner, text + 3);
    else
        ParseLangText(owner, text);
}

/* The rows the files are read into. Allocated on the first read, here
 * rather than in the caller: ShLangLabel reads the framework file for
 * [LanguageNames] and allocates nothing itself, so a read can reach
 * AddRow with no table at all - which is a write to NULL, and it is
 * what a menu asks for first. */
static int EnsureRows(void) {
    if (!g_rows) g_rows = (LangRow *)calloc(LROW_MAX, sizeof(LangRow));
    return g_rows != NULL;
}

/* The framework file first: [LanguageNames] and the shared override
 * rows live there, and any plugin may lean on them. */
static void LoadLang(const char *owner) {
    if (!EnsureRows()) {
        TextLog("no table for %d rows: text is not loaded", LROW_MAX);
        return;
    }
    LoadLangFile(NULL);
    if (owner && owner[0]) LoadLangFile(owner);
}

/** Retained spelling with an owner: the key decides what text a row
 *  gets, so the scope is not used any more. NULL or "" is the
 *  framework's own text. */
SH_API const char *ShLangForOwned(const char *owner, const char *scope,
                                  const char *text) {
    (void)scope;
    return ShLangText(owner, text);
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
        /* Parse once. The flag below was read but never set, so every
         * ShConfigGet* / ShLang call re-read the file and appended the
         * same rows to the entry table again. */
        g_configReady = 1;
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
    ShUtf8Trim(out);
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

/* The line ending the file itself uses, so a line appended to a CRLF
 * ini does not end up the only LF in it. */
static const char *IniEol(const char *whole, long len) {
    long i;

    for (i = 0; i + 1 < len; i++)
        if (whole[i] == '\r' && whole[i + 1] == '\n') return "\r\n";
    return "\n";
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
    const char *eol = "\n";
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
    eol = IniEol(whole, len);

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
                OutPrint(&out, &outLen, &outCap, "%s=%s%s",
                         key, value, eol);
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
            OutPrint(&out, &outLen, &outCap, "%s", eol);
        if (!curSecSet || !inSec) {
            /* A new section must be created. */
            if (outLen > 0)
                OutPrint(&out, &outLen, &outCap, "%s", eol);
            OutPrint(&out, &outLen, &outCap, "[%s]%s", section, eol);
        }
        OutPrint(&out, &outLen, &outCap, "%s=%s%s", key, value, eol);
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
