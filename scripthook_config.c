/* Path helpers and the main scripthook.ini.
 *
 * The layout every path helper builds is the one the loader
 * uses too:
 *
 *   <gamedir>\scripthook.ini        main config
 *   <gamedir>\logs\                 every log file
 *   <gamedir>\lang.ini              the framework's own text
 *   <gamedir>\plugins\              the plugins, each with its files beside
 *       <name>.asi                  its .asi: at the root ...
 *       <name>.ini
 *       <name>\                     ... or one folder in
 *           <name>.asi
 *           <name>.ini
 *           lang.ini
 *
 * What counts as a plugin, and what its name is, is plugin_scan.h's; the
 * two helpers below ask that scan where a plugin's .asi ended up rather
 * than building a folder name out of the plugin name, because the folder is
 * no longer always called after the plugin.
 *
 * The main config is parsed once by the loader, before any
 * plugin loads. Plugins can query it as well, and keep their
 * own settings in the .ini beside their .asi (see
 * ShPluginIniPath).
 */
#include <windows.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "log.h"
#include "plugin_scan.h"

extern void ShSetError(int err);

/* ---- paths -------------------------------------------------- */

#define GAME_DIR_MAX MAX_PATH

static char g_gameDir[GAME_DIR_MAX];
static volatile LONG g_dirInit = 0;

/* The folder holding GRW.exe, with a trailing backslash.
 * Anchored to the module file: the game is free to change
 * the working directory.
 *
 * Loader thread, state-watch thread, menu thread and plugin threads all
 * reach this. The flag used to be set before the string was filled, so a
 * second thread could take the flag and use an empty path. It is now
 * published after the string, with interlocked accesses; a reader that
 * loses the race (or sees 0) simply computes it too, and both writers
 * produce the same bytes. */
static const char *GameDir(void) {
    char *slash;

    if (InterlockedCompareExchange(&g_dirInit, 0, 0)) return g_gameDir;
    if (!GetModuleFileNameA(NULL, g_gameDir, GAME_DIR_MAX)) {
        g_gameDir[0] = 0;
        InterlockedExchange(&g_dirInit, 1);
        return g_gameDir;
    }
    slash = strrchr(g_gameDir, '\\');
    if (slash) slash[1] = 0;
    else g_gameDir[0] = 0;
    InterlockedExchange(&g_dirInit, 1);
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

/* The config file that belongs beside the plugin of that name: its folder
 * plus its name plus .ini.
 *
 * The folder is the one the .asi was installed in, and it comes from the one
 * scan (plugin_scan.h) rather than being built out of the name - because it
 * is no longer always a folder called after the plugin. plugins\foo.asi and
 * plugins\pack\foo.asi are both plugin "foo", and each reads the .ini sitting
 * next to its own .asi. For the layout that has always been used, plugins\
 * <name>\<name>.asi, the scan's answer and the old `plugins\<name>\<name>.ini`
 * are the same string, so nothing about an existing install moves.
 *
 * There is deliberately no second guess: the name either is an installed
 * plugin's or it is not. A name that is not gets 0 and SH_ERR_NO_PLUGIN
 * rather than a path that was merely formatted - the old form always handed
 * back a path, and a plugin could not tell "my config is here" from "my
 * config would be here if I existed". */
SH_API int ShPluginIniPath(const char *plugin, char *buf, int size) {
    const ShPluginEnt *e;
    int n;

    if (!plugin || !buf || size <= 0) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    e = ShPluginFind(plugin);
    if (!e) {
        buf[0] = 0;
        ShSetError(SH_ERR_NO_PLUGIN);
        return 0;
    }
    /* A cut path is not a path. snprintf only reports failure with a
     * negative return, so a long install directory used to hand back a
     * truncated name, and the plugin silently read a file nobody had
     * written. Emptying the buffer says so as plainly as the 0 does. */
    n = snprintf(buf, size, "%s%s.ini", e->dir, e->name);
    if (n < 0 || n >= size) {
        buf[0] = 0;
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}

/* <the plugin's folder>\lang.ini, the text file beside it. Read by the
 * framework for that plugin's menus; see @ref lang.
 *
 * Same folder lookup as ShPluginIniPath, which is also what makes a shared
 * lang.ini work: plugins that live in one folder resolve to one file, and
 * the owner each of them declares (ShLangDeclare) is what keeps their rows
 * apart inside it. */
SH_API int ShPluginLangPath(const char *plugin, char *buf, int size) {
    const ShPluginEnt *e;
    int n;

    if (!plugin || !buf || size <= 0) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    e = ShPluginFind(plugin);
    if (!e) {
        buf[0] = 0;
        ShSetError(SH_ERR_NO_PLUGIN);
        return 0;
    }
    n = snprintf(buf, size, "%slang.ini", e->dir);
    if (n < 0 || n >= size) {
        buf[0] = 0;
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

/* The default written on first launch: only the switches that have no
 * other home. README.md documents the rest (and the reference
 * scripthook.ini in the repo keeps the long form).
 *
 * Every key left out reads exactly the value it was given here before:
 * the code's own default is that same value - read_dial / read_prio take a
 * ceiling and fall back to 0, and each ShConfigGet* passes the value the
 * old template wrote - so omitting a line does not change behaviour. It
 * only stops the generated file from being the manual. The CPU dials are
 * listed all the same - they are the part a player is most likely to tune
 * by hand, and they are written in the order the CPU page shows them. */
static const char *DEFAULT_CONFIG =
    "[loader]\n"
    "load_plugins=1\n"
    "cpu_boot=0\n"
    "cpu_window=0\n"
    "cpu_eco_boot=0\n"
    "cpu_play=0\n"
    "cpu_prio_play=0\n"
    "cpu_cores=0\n"
    "\n"
    "[plugins]\n"
    "\n"
    "[forgemod]\n"
    "enabled=0\n"
    "\n"
    "[Settings]\n";
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

/* Declared up here because the parser reports a cut line through it. */
static void TextLog(const char *fmt, ...);

/* Advance over one physical line, NUL-terminating it in place.
 * Returns 0 at end of text. */
static int NextLine(const char **p, char *line, size_t cap) {
    size_t i = 0;
    const char *q = *p;
    int cut = 0;

    if (!*q) return 0;
    while (*q && *q != '\n' && *q != '\r') {
        if (i < cap - 1) line[i++] = *q;
        else cut = 1;               /* the rest of this line is dropped */
        q++;
    }
    line[i] = 0;
    if (*q == '\r') q++;
    if (*q == '\n') q++;
    *p = q;
    /* The whole line is consumed even when it did not fit. Stopping at the
     * buffer and leaving the pointer in the middle of the line turned the
     * remainder into a line of its own, which the parser then read as a
     * fresh key=value - a long comment or a long translation became a
     * setting nobody wrote. */
    if (cut)
        TextLog("a line longer than %u chars was cut", (unsigned)cap);
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
/* The picker's list. The game ships sixteen languages and the framework
 * declares a name row for each, so the list a fresh install builds is that
 * long; a [Settings] Languages line shorter than this is a custom list. */
#define LANG_LIST_MAX   16
#define LANG_KEY_MAX    512     /* a literal key can be a whole hint */
#define LANG_VAL_MAX    768     /* and its translation can be longer */
#define LROW_MAX        2048
#define BASE_MAX        4096
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
/* [Settings] Language= was in the file. Only its ABSENCE is a reason to
 * pick one: as soon as the line is there - even empty, even a code the
 * menu has no text for - the file is the answer and LangAutoChoose never
 * runs. */
static int  g_langSet;
/* The code picked on that first run, waiting for LoadConfig to write it
 * back into the settings file - which is what makes it a first run only.
 * Empty whenever the file already had the line. */
static char g_langAuto[LANG_CODE_MAX];
/* Why it was picked, and how the write went: the one line the loader puts
 * next to the version in logs\scripthook.log, through ShLangPickLine. The
 * line is not written from here - logging during this load would ask this
 * layer for the log level while this layer is still loading (ShLogLevel
 * reads [Settings] LogLevel through ShConfigGetStr), which re-enters
 * LoadConfig and leaves the log unopened. */
static char g_langWhy[96];
static char g_langNote[200];

static CRITICAL_SECTION g_textLock;
static volatile LONG    g_textLockReady = 0;
static volatile LONG    g_textLogging;

static void LoadLang(const char *owner);
static int  EnsureRows(void);
static void AddRow(const char *owner, const char *key,
                   const char *value);

static void TextLog(const char *fmt, ...) {
    va_list ap;

    /* As ApiLog: one winner opens the file, so the other thread cannot
     * overwrite the FILE* and leak it. */
    if (InterlockedExchange(&g_textLogging, 1) == 0)
        LogInit("scripthook_text.log");
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

/* A section name that names a language rather than a settings group: a tag
 * with a region or script subtag ("zh-CN", "en_US").
 *
 * A bare two or three letter name used to count as well - "en", "zh" - and
 * that is how "[ui]", "[mod]", "[npc]" and every other short name a plugin
 * is entitled to use had all of their rows dropped from the config without
 * a word. A bare name is a settings section now. The cost is the other
 * direction: a leftover "[zh]" block in scripthook.ini is read as settings
 * instead of being dropped, and that is the harmless way to fail - a
 * meaningless row costs nothing, a missing one costs a setting.
 *
 * A longer bare word is a settings section too ("loader", "forgemod",
 * "playmode"), as it always was. */
static int IsLangCodeLike(const char *sec) {
    const char *p = sec;
    int n = 0;

    if (!sec || !*sec) return 0;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
        p++;
        n++;
    }
    if (!*p) return 0;                      /* bare name: a settings section */
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
            g_langSet = 1;              /* present, whatever it says */
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
/* Twice the languages the game ships: a player's own [LanguageNames] rows
 * for regional variants are then not the ones dropped at the cap. */
#define DISP_MAX 32
static struct { char code[LANG_CODE_MAX]; char label[32]; } g_disp[DISP_MAX];
static int g_nDisp;

static void LangNameAdd(const char *code, const char *label);

/* ---- picking a language nobody picked ---------------------------- */

/* The Windows user interface language, as a BCP-47 tag: "zh-CN" on a
 * Chinese Windows, "en-US" on an English one. The user's own choice comes
 * first - that is the language they read - and the system's only if the
 * first has nothing to say. */
static int SysLangCode(char *out, int cap) {
    WCHAR tag[LOCALE_NAME_MAX_LENGTH];
    LANGID id = GetUserDefaultUILanguage();
    int n;

    if (!id) id = GetSystemDefaultUILanguage();
    if (!id) return 0;
    /* LCIDToLocaleName is the Unicode entry point: it takes the tag as
     * wchar_t and hands back the same "zh-CN" every other part of this
     * file uses only after the conversion. */
    if (!LCIDToLocaleName(MAKELCID(id, SORT_DEFAULT), tag,
                          (int)(sizeof(tag) / sizeof(tag[0])), 0))
        return 0;
    n = WideCharToMultiByte(CP_UTF8, 0, tag, -1, out, cap, NULL, NULL);
    return n > 0 && out[0] != 0;
}

/* "zh-TW" and "zh-CN" are one language in two scripts, "en-GB" and
 * "en-US" one language in two regions: the primary subtag is what makes
 * them the same language, and a menu in the right language with the wrong
 * variant beats English. Two different languages are never matched this
 * way - no spelling of "ja-JP" makes it "zh-CN". */
static int LangPrimaryEq(const char *a, const char *b) {
    size_t na = 0, nb = 0;

    if (!a || !b) return 0;
    while (a[na] && a[na] != '-' && a[na] != '_') na++;
    while (b[nb] && b[nb] != '-' && b[nb] != '_') nb++;
    if (!na || na != nb) return 0;
    return _strnicmp(a, b, na) == 0;
}

/* The region subtag of a tag: "en-GB" -> "GB", "zh-Hant-TW" -> "TW", ""
 * for a tag that carries none. Compared with _stricmp by the caller. */
static const char *LangRegion(const char *code) {
    const char *p;

    if (!code || !code[0]) return "";
    p = strrchr(code, '-');
    if (!p) p = strrchr(code, '_');
    return p ? p + 1 : "";
}

/* Whether a tag is the Chinese written in traditional characters. A Windows
 * user language says zh-HK, not zh-Hant-HK - the script is not in the tag -
 * so the regions that use it are what to go by. */
static int LangChineseTraditional(const char *code) {
    const char *r = LangRegion(code);

    return !_stricmp(r, "TW") || !_stricmp(r, "HK") || !_stricmp(r, "MO");
}

/* The code this session will use, out of the languages the menu can
 * actually show: the exact one, else the same language in another variant,
 * else English - which every build carries (the framework declares en-US
 * text, and the lookup falls back to it per key even when en-US is not on
 * offer). *how says which of the three it was, for the log line.
 *
 * Among the variants, the same region wins first and then the same Chinese
 * script, because neither is the order the list happens to be declared in:
 * a Windows set to zh-HK asks for "zh", and zh-CN answers that just as well
 * as zh-TW does - one of them is an accident of declaration order, the
 * other hands a Hong Kong player the script they do not read. Region and
 * script only ever decide between variants of the SAME language
 * (LangPrimaryEq above); two languages are still never matched this way.
 */
static const char *LangPickOffered(const char *code, const char **how) {
    const char *region = LangRegion(code);
    int i;

    if (code && code[0]) {
        for (i = 0; i < g_nLangList; i++)
            if (LangEq(g_langList[i], code)) {
                *how = "exact match";
                return g_langList[i];
            }
        if (region[0])
            for (i = 0; i < g_nLangList; i++)
                if (LangPrimaryEq(g_langList[i], code) &&
                    !_stricmp(LangRegion(g_langList[i]), region)) {
                    *how = "same language, same region";
                    return g_langList[i];
                }
        if (LangChineseTraditional(code))
            for (i = 0; i < g_nLangList; i++)
                if (LangPrimaryEq(g_langList[i], code) &&
                    LangChineseTraditional(g_langList[i])) {
                    *how = "same language, same script";
                    return g_langList[i];
                }
        for (i = 0; i < g_nLangList; i++)
            if (LangPrimaryEq(g_langList[i], code)) {
                *how = "same language, another variant";
                return g_langList[i];
            }
        *how = "not offered - using en-US";
    } else {
        *how = "nothing to match - using en-US";
    }
    return "en-US";
}

/* Called once, from ResolveLanguage, and only when [Settings] Language= is
 * not in the file at all: pick one for this session out of the Windows
 * user language, and leave the code in g_langAuto for LoadConfig to write
 * back. That write is what makes this a first run only - from the second
 * launch the line is in the file, so nothing here is reached. Deleting the
 * line is therefore also how a player has the pick made again, which is
 * what the README tells them to do when the guess is wrong or the language
 * they want has changed. */
static void LangAutoChoose(void) {
    char sys[LANG_CODE_MAX];
    const char *pick, *how = "nothing to match";
    int have = SysLangCode(sys, sizeof(sys));

    if (!have) sys[0] = 0;
    pick = LangPickOffered(sys, &how);
    CopyN(g_langName, sizeof(g_langName), pick);
    CopyN(g_langAuto, sizeof(g_langAuto), pick);
    /* Where it came from, for the line the loader writes - LoadConfig adds
     * how the write back went. See ShLangPickLine. */
    if (have)
        snprintf(g_langWhy, sizeof(g_langWhy),
                 "Windows UI language %s, %s", sys, how);
    else
        snprintf(g_langWhy, sizeof(g_langWhy),
                 "the Windows UI language could not be read, %s", how);
}

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
    /* Nobody picked one and the file does not say: pick it now, once (see
     * LangAutoChoose). An EMPTY Language= is not this case - the line is in
     * the file, so the file is the answer, and the fallback below (the
     * first language on offer) is what an empty line has always meant. */
    if (!g_langName[0] && !g_langSet)
        LangAutoChoose();
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

/* ---- exporting a lang.ini for each menu ---------------------------------
 *
 * Two rows on the settings page write these out: one for the framework's own
 * text, one for every installed plugin's.
 *
 * Filled in, never replaced. A key already in the section for the language in
 * force is left exactly as it stands - that row is the player's translation,
 * and pressing the button a second time must not cost it. Only what is missing
 * is appended, at the end of that same section. A file with nothing missing is
 * not opened for writing at all, so its mtime does not even move.
 *
 * That is what makes a plugin installed later harmless: it gets a file of its
 * own, and every file already translated is skipped on its own merit rather
 * than by a list of names that would go stale.
 *
 * The write is the other half. The bytes go to lang.ini.tmp, the handle is
 * flushed through to the disk, the result is measured back, and only then is
 * it renamed over lang.ini. Anything that goes wrong before the rename leaves
 * lang.ini as it was - which is why there is no .bak: a half-written original
 * cannot be produced, so a copy would protect against nothing the rename does
 * not already.
 *
 * Three sources for a value, in this order: the text this language declares,
 * the en-US text, then the key itself. That last step is what fills the rows
 * of a plugin with no compiled-in table at all - its keys ARE the English
 * words, and a row is looked up by exactly that string.
 */

#define LF_OWNERS     96
#define LF_PATHS      48
#define LF_PER_PATH   8        /* plugins one folder may serve */
#define LF_ROWS       1024     /* distinct keys one export looks at */
#define LF_MENU_MAX   384      /* menu literals, for the plugins without tables */
#define LF_BUF        65536    /* a lang.ini is read and rebuilt in this */

typedef struct {
    const char *owner;         /* points into lfOwn */
    const char *key;
} LfKey;

typedef struct {
    char path[MAX_PATH];
    int  own[LF_PER_PATH];
    int  nOwn;
    int  fw;                   /* 1 = the framework's own lang.ini */
} LfPath;

static LfKey   lfKey[LF_ROWS];
static int     lfNKey;
static char    lfOwn[LF_OWNERS][48];
static int     lfNOwn;
static LfPath  lfPath[LF_PATHS];
static int     lfNPath;
static char    lfBuf[LF_BUF];  /* the file as it was found */
static char    lfOut[LF_BUF];  /* the file as it will be written */
static int     lfCrlf;         /* the file uses CRLF, so new rows must too */
static int     lfAdded;        /* rows this file gained */
static size_t  lfSegB, lfSegE; /* the section that is there, when one is */
static int     lfSkipped;      /* text this file could not hold in one line */

typedef struct {
    char owner[48];
    char key[128];
} LfMenuEnt;

static LfMenuEnt lfMenu[LF_MENU_MAX];
static int       lfNMenu;

/* A plugin name, remembered once. "" is not one: the framework's own text is
 * collected on its own pass and never goes through this table. */
static int LfOwnIndex(const char *name) {
    int i;

    if (!name || !name[0]) return -1;
    for (i = 0; i < lfNOwn; i++)
        if (!_stricmp(lfOwn[i], name)) return i;
    if (lfNOwn >= LF_OWNERS) return -1;
    CopyN(lfOwn[lfNOwn], sizeof(lfOwn[0]), name);
    return lfNOwn++;
}

/* The file this owner's text goes in, grouped by path rather than by owner -
 * several plugins in one folder share one file, as the loader reads them. */
static int LfPathIndex(const char *path, int owner) {
    int i, j;

    for (i = 0; i < lfNPath; i++) {
        if (_stricmp(lfPath[i].path, path)) continue;
        for (j = 0; j < lfPath[i].nOwn; j++)
            if (lfPath[i].own[j] == owner) return i;
        if (lfPath[i].nOwn >= LF_PER_PATH) {
            TextLog("langexport: %s already serves %d plugins; \"%s\" left out",
                    path, LF_PER_PATH, lfOwn[owner]);
            return i;
        }
        lfPath[i].own[lfPath[i].nOwn++] = owner;
        return i;
    }
    if (lfNPath >= LF_PATHS) return -1;
    CopyN(lfPath[lfNPath].path, sizeof(lfPath[0].path), path);
    lfPath[lfNPath].fw = 0;
    lfPath[lfNPath].nOwn = 1;
    lfPath[lfNPath].own[0] = owner;
    return lfNPath++;
}

/* CRLF when the file mostly uses it. A file a player edited in an editor set
 * to LF is a file whose new rows must be LF too - mixing the two puts a stray
 * carriage return at the end of every value this writes, and the value is what
 * gets handed to the game. */
static int LfDetectCrlf(const char *buf, size_t len) {
    size_t i, crlf = 0, nl = 0;

    for (i = 0; i < len; i++) {
        if (buf[i] != '\n') continue;
        nl++;
        if (i && buf[i - 1] == '\r') crlf++;
    }
    return nl && crlf * 2 >= nl;
}

/* The whole section named `lang`, header line included: [start, end). 0 when
 * the file has none. `end` is the next section's '[' or the terminator, so a
 * section can be addressed as one range without walking it twice. */
static int LfSection(const char *text, const char *lang, const char **start,
                     const char **end) {
    const char *p = text;

    while (p && *p) {
        const char *line = p;

        while (*p && *p != '\n') p++;
        while (line < p && (*line == ' ' || *line == '\t')) line++;
        if (line < p && *line == '[') {
            const char *close = line + 1;

            while (close < p && *close != ']') close++;
            if (close < p) {
                char name[64];
                size_t n = (size_t)(close - line - 1);

                if (n >= sizeof name) n = sizeof name - 1;
                memcpy(name, line + 1, n);
                name[n] = 0;
                if (LangEq(name, lang)) {
                    const char *e = *p ? p + 1 : p;

                    while (e && *e) {
                        const char *l2 = e;

                        while (*e && *e != '\n') e++;
                        while (l2 < e && (*l2 == ' ' || *l2 == '\t')) l2++;
                        if (*l2 == '[') { e = l2; break; }
                        if (*e) e++;
                    }
                    *start = line;
                    *end = e;
                    return 1;
                }
            }
        }
        if (*p) p++;
    }
    *start = *end = NULL;
    return 0;
}

/* The key on one line, or 0 when it is blank, a comment, or has no
 * "key = value" shape. Both quoted and bare keys count: a player editing by
 * hand is as likely to leave the quotes off as not, and a row this fails to
 * see is a row that gets appended a second time. */
static int LfLineKey(const char *p, const char *end, char *out, size_t cap) {
    const char *s = p, *k;
    size_t n;

    while (s < end && (*s == ' ' || *s == '\t')) s++;
    if (s >= end) return 0;
    if (*s == ';' || *s == '#' || *s == '\r' || *s == '\n') return 0;

    if (*s == '"') {
        k = ++s;
        while (s < end && *s != '"') s++;
        if (s >= end) return 0;             /* an unclosed quote: not a row */
        n = (size_t)(s - k);
        s++;
        while (s < end && (*s == ' ' || *s == '\t')) s++;
        if (s >= end || *s != '=') return 0;
    } else {
        k = s;
        while (s < end && *s != '=' && *s != '\r' && *s != '\n') s++;
        if (s >= end || *s != '=') return 0;
        n = (size_t)(s - k);
        while (n && (k[n - 1] == ' ' || k[n - 1] == '\t')) n--;
    }
    if (!n || n >= cap) return 0;
    memcpy(out, k, n);
    out[n] = 0;
    return 1;
}

/* A row is one line. A key or a value carrying a quote would end the value
 * early; one carrying a newline would break the row in two, leaving the key
 * unreadable and the rest as loose text. Text like that cannot be expressed
 * in this file at all - the parser reads a line at a time - so it is left out
 * rather than written broken, and counted so the log says how many. */
static int LfLineSafe(const char *s) {
    if (!s || !s[0]) return 0;
    for (; *s; s++)
        if (*s == '"' || *s == '\n' || *s == '\r') return 0;
    return 1;
}

/* Is `key` already a row of the section at [b, e)? Scanning the section each
 * time costs a few hundred comparisons per key and saves a table that would
 * need its own storage and its own way of going wrong. */
static int LfHasKey(size_t b, size_t e, const char *key) {
    size_t p = b;
    char line[160];

    while (p < e) {
        size_t q = p;

        while (q < e && lfBuf[q] != '\n') q++;
        if (LfLineKey(lfBuf + p, lfBuf + q, line, sizeof line) &&
            !strcmp(line, key))
            return 1;
        p = q < e ? q + 1 : e;
    }
    return 0;
}

/* Build the file into lfOut and return its length, or 0 when this path has
 * nothing missing and the file should not be touched at all. */
static size_t LfRender(const LfPath *g, size_t len, int had, int found) {
    size_t at, b = lfSegB, e = lfSegE;
    int i, j;

    lfAdded = 0;
    lfSkipped = 0;

    if (found) {
        memcpy(lfOut, lfBuf, e);
        at = e;
        /* A file whose last row carries no newline would otherwise get the
         * first added row glued onto it. */
        if (at && lfOut[at - 1] != '\n') {
            if (at + 2 >= LF_BUF) return 0;
            if (lfCrlf) lfOut[at++] = '\r';
            lfOut[at++] = '\n';
        }
    } else {
        if (len) {
            memcpy(lfOut, lfBuf, len);
            at = len;
        } else {
            at = 0;
        }
        if (at + 32 >= LF_BUF) return 0;
        at += (size_t)snprintf(lfOut + at, LF_BUF - at, "%s[%s]%s\n",
                               at ? (lfCrlf ? "\r\n" : "\n") : "", g_langName,
                               lfCrlf ? "\r" : "");
    }

    for (i = 0; i < lfNKey; i++) {
        const char *val;
        int mine = g->fw;

        if (!mine) {
            for (j = 0; j < g->nOwn; j++)
                if (!_stricmp(lfOwn[g->own[j]], lfKey[i].owner)) { mine = 1; break; }
        }
        if (!mine) continue;
        if (found && LfHasKey(b, e, lfKey[i].key)) continue;   /* theirs, keep it */
        val = BaseFind(lfKey[i].owner, g_langName, lfKey[i].key);
        if (!val || !val[0]) val = BaseFind(lfKey[i].owner, "en-US", lfKey[i].key);
        if (!val || !val[0]) val = lfKey[i].key;
        if (!LfLineSafe(lfKey[i].key) || !LfLineSafe(val)) { lfSkipped++; continue; }
        if (at + strlen(lfKey[i].key) + strlen(val) + 8 >= LF_BUF) {
            TextLog("langexport: %s full, %d key(s) left for a later run",
                    g->path, lfNKey - i);
            break;
        }
        at += (size_t)snprintf(lfOut + at, LF_BUF - at, "\"%s\" = \"%s\"%s\n",
                               lfKey[i].key, val, lfCrlf ? "\r" : "");
        lfAdded++;
    }

    /* The literals a plugin put on screen. They only ever go into the language
     * in force: they are English words, and there is no other language to file
     * them under. */
    if (!g->fw) {
        for (i = 0; i < lfNMenu; i++) {
            int mine = 0;

            for (j = 0; j < g->nOwn; j++)
                if (!_stricmp(lfOwn[g->own[j]], lfMenu[i].owner)) { mine = 1; break; }
            if (!mine) continue;
            if (found && LfHasKey(b, e, lfMenu[i].key)) continue;
            if (!LfLineSafe(lfMenu[i].key)) { lfSkipped++; continue; }
            if (at + strlen(lfMenu[i].key) * 2 + 8 >= LF_BUF) break;
            at += (size_t)snprintf(lfOut + at, LF_BUF - at, "\"%s\" = \"%s\"%s\n",
                                   lfMenu[i].key, lfMenu[i].key,
                                   lfCrlf ? "\r" : "");
            lfAdded++;
        }
    }

    if (lfSkipped)
        TextLog("langexport: %s - %d row(s) left out, their text spans lines "
                "and cannot be held in one", g->path, lfSkipped);

    if (!lfAdded) return 0;                 /* complete: nothing to write */

    if (found) {
        if (at + (len - e) + 1 > LF_BUF) return 0;
        memcpy(lfOut + at, lfBuf + e, len - e);
        at += len - e;
    }
    lfOut[at] = 0;

    if (!had) {
        /* A new file gets one line saying which build made it and how many
         * rows it holds - the only thing a translator needs to know is that
         * the count should match the menu. */
        SYSTEMTIME st;
        char head[192];
        size_t hn;

        GetLocalTime(&st);
        hn = (size_t)snprintf(head, sizeof head,
                              "; ScriptHook %s · %04d-%02d-%02d %02d:%02d"
                              " · Export %d\r\n",
                              SH_VERSION, st.wYear, st.wMonth, st.wDay,
                              st.wHour, st.wMinute, lfAdded);
        if (at + hn + 1 <= LF_BUF) {
            memmove(lfOut + hn, lfOut, at + 1);
            memcpy(lfOut, head, hn);
            at += hn;
        }
    }
    return at;
}

/* Put `len` bytes where `path` is, without ever leaving a half-written file
 * where a whole one used to be. Failure before the rename leaves the original
 * untouched; failure at the rename leaves the new file beside it, named .tmp,
 * for a player whose lang.ini is held open by an editor to move himself. */
static int LfAtomicWrite(const char *path, const char *buf, size_t len) {
    char tmp[MAX_PATH];
    FILE *f;
    HANDLE h;
    size_t back = 0;
    int ok;

    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) < 0) return 0;
    /* A .tmp left by a crash is no evidence of anything - it is overwritten,
     * never restored from. */
    if (GetFileAttributesA(tmp) != INVALID_FILE_ATTRIBUTES) DeleteFileA(tmp);

    f = fopen(tmp, "wb");
    if (!f) return 0;
    ok = fwrite(buf, 1, len, f) == len && fflush(f) == 0;
    if (ok) {
        h = (HANDLE)_get_osfhandle(_fileno(f));
        /* fclose only hands the bytes to the file system; without this the
         * rename can land before the data does, and a power cut then leaves a
         * correctly named empty file. */
        ok = h != INVALID_HANDLE_VALUE && FlushFileBuffers(h) != 0;
    }
    if (fclose(f) != 0) ok = 0;
    if (!ok) { DeleteFileA(tmp); return 0; }

    /* Measure it back: a short file is not one to put where a good one was,
     * and fwrite's own count does not prove the bytes reached the disk. */
    f = fopen(tmp, "rb");
    if (!f) { DeleteFileA(tmp); return 0; }
    for (;;) {
        char chunk[4096];
        size_t n = fread(chunk, 1, sizeof chunk, f);

        back += n;
        if (n < sizeof chunk) break;
    }
    fclose(f);
    if (back != len) { DeleteFileA(tmp); return 0; }

    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        /* ReplaceFile keeps the original's attributes and ACLs, and promises
         * to leave the target exactly as it was if it cannot finish. */
        ok = ReplaceFileA(path, tmp, NULL, REPLACEFILE_IGNORE_MERGE_ERRORS,
                          NULL, NULL) != 0;
    } else {
        ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
    }
    if (!ok) {
        TextLog("langexport: %s could not be put in place; the new file is %s",
                path, tmp);
        return 0;
    }
    return 1;
}

/* Everything one export will write, gathered before a file is touched: a
 * plugin with nothing to say should not cost an empty file. */
static void LfCollect(int framework) {
    int i, j;

    lfNKey = lfNOwn = lfNPath = lfNMenu = 0;

    TextLock();
    for (i = 0; i < g_nbase && lfNKey < LF_ROWS; i++) {
        int isFw = !g_base[i].owner[0];
        int oi;

        if (isFw != framework) continue;
        /* The names of the languages themselves are not a translation: a row
         * here would let a player rename zh-CN in the picker that offers it,
         * which is a way to lose the language rather than add one. */
        if (!_strnicmp(g_base[i].key, "@lang.name.", 11)) continue;
        if (isFw) {
            if (lfNOwn == 0) { lfNOwn = 1; lfOwn[0][0] = 0; }
            oi = 0;
        } else {
            oi = LfOwnIndex(g_base[i].owner);
            if (oi < 0) continue;
        }
        for (j = 0; j < lfNKey; j++)
            if (!_stricmp(lfKey[j].owner, lfOwn[oi]) &&
                !strcmp(lfKey[j].key, g_base[i].key)) break;
        if (j < lfNKey) continue;           /* the same key declared twice */
        lfKey[lfNKey].owner = lfOwn[oi];
        lfKey[lfNKey].key = g_base[i].key;
        lfNKey++;
    }
    TextUnlock();

    /* The literals a plugin put on screen: no text lock is held across this,
     * and ShMenuTextAt takes and releases the menu lock itself, so the two are
     * never nested. Only plugins need them - the framework's own text is all
     * in its baseline. */
    if (!framework) {
        int n = ShMenuTextCount();

        for (i = 0; i < n && lfNMenu < LF_MENU_MAX; i++) {
            char owner[48], key[128];

            if (!ShMenuTextAt(i, owner, sizeof owner, key, sizeof key)) continue;
            if (!owner[0] || !key[0]) continue;      /* a built-in page */
            if (key[0] == '@') continue;             /* an id: has a baseline */
            /* A number, a size or a multiplier is the same word in every
             * language, and a key like "1" is claimed by whichever plugin is
             * walked first. One character is a key name or a symbol. */
            if (key[0] >= '0' && key[0] <= '9') continue;
            if (key[1] == 0) continue;
            if (LfOwnIndex(owner) < 0) continue;
            for (j = 0; j < lfNMenu; j++)
                if (!_stricmp(lfMenu[j].owner, owner) &&
                    !strcmp(lfMenu[j].key, key)) break;
            if (j < lfNMenu) continue;
            CopyN(lfMenu[lfNMenu].owner, sizeof lfMenu[0].owner, owner);
            CopyN(lfMenu[lfNMenu].key, sizeof lfMenu[0].key, key);
            lfNMenu++;
        }
    }

    if (framework) {
        lfPath[0].fw = 1;
        lfPath[0].nOwn = 0;
        if (snprintf(lfPath[0].path, sizeof lfPath[0].path, "%slang.ini",
                     GameDir()) < 0) return;
        lfNPath = 1;
        return;
    }
    for (i = 1; i < lfNOwn; i++) {          /* 0 is the framework's, unused here */
        char path[MAX_PATH];

        if (!ShPluginLangPath(lfOwn[i], path, sizeof path)) continue;
        LfPathIndex(path, i);
    }
}

/* Fill one file's section for the language in force in, leaving every row that
 * is already there alone.
 * Returns 1 when it was written, 0 when nothing was missing, -1 on failure. */
static int LfWriteOne(LfPath *g) {
    FILE *f;
    size_t len = 0, built;
    const char *b, *e;
    int had = 0, found;

    lfBuf[0] = 0;
    f = fopen(g->path, "rb");
    if (f) {
        len = fread(lfBuf, 1, LF_BUF - 1, f);
        fclose(f);
        lfBuf[len] = 0;
        lfCrlf = LfDetectCrlf(lfBuf, len);
        had = 1;
    } else {
        lfCrlf = 1;                         /* a new file: CRLF, ships on Windows */
    }

    found = LfSection(lfBuf, g_langName, &b, &e);
    if (found) { lfSegB = (size_t)(b - lfBuf); lfSegE = (size_t)(e - lfBuf); }
    else       { lfSegB = lfSegE = 0; }
    if (found && lfSegE < lfSegB) found = 0;

    built = LfRender(g, len, had, found);
    if (!built) return 0;                   /* complete already, left untouched */

    return LfAtomicWrite(g->path, lfOut, built) ? 1 : -1;
}

static int LfExport(int framework) {
    int i, written = 0;

    LoadConfig();
    LfCollect(framework);
    if (!lfNPath) { ShSetError(SH_ERR_NO_PLUGIN); return 0; }
    for (i = 0; i < lfNPath; i++) {
        int r = LfWriteOne(&lfPath[i]);

        if (r > 0) written++;
        else if (r < 0)
            TextLog("langexport: %s could not be written", lfPath[i].path);
    }
    ShSetError(SH_OK);
    return written;
}

SH_API int ShLangExportFramework(void) {
    return LfExport(1);
}

SH_API int ShLangExportPlugins(void) {
    return LfExport(0);
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

/* ---- translation cache ------------------------------------------
 *
 * A menu is translated on every capture - every frame while it is open - and
 * one lookup walks a row table and a baseline table of up to 2048 entries
 * each, with a case-insensitive compare per entry, under the text lock. A
 * page of twelve rows therefore costs a few hundred string compares per
 * frame to produce an answer that only changes when the language does.
 *
 * The key is (owner, key), both copied so a caller's buffer going away
 * cannot leave a dangling pointer behind - and the value is copied too,
 * once, on the miss: what the caller is handed back is that copy and
 * nothing else is ever allowed to write over it. Two earlier shapes had it
 * otherwise, and the second one is the field report of 2026-09-17. The
 * first stored the pointer the lookup returned, which the miss path
 * answers with one of Readable()'s four rotating buffers, so entries ended
 * up sharing four buffers between them. The second copied the text but
 * still handed out the slot it lived in - 1 key in 256 shared it with a
 * different one, which is the same wrong text in a narrower window - and
 * remembered at most 159 bytes of it, so a row longer than that read
 * complete the first time and cut in half on every hit after. A key is a
 * whole hint here, and a caller may keep a pointer for as long as it
 * likes: the copy is made once and never recycled, and the language
 * changing is the only thing that releases it.
 *
 * The index is open-addressed and never evicts, so two keys whose hashes
 * collide share the table instead of each other's text. It holds about
 * 500 keys in the tree; on the day it cannot take another one, that
 * lookup is answered but not remembered, with one line saying so - the
 * old shape silently answered from wherever the 1-in-256 slot had got to.
 * All of it is touched under the text lock, the one ShLangText holds. */
#define LCA_MAX    1024                 /* translations remembered */
#define LCA_SLOTS  2048                 /* index: power of two, > LCA_MAX */
#define LCA_PROBE  8                    /* slots looked at per lookup */

typedef struct {
    char    *owner;                     /* ours; both are what we hand back */
    char    *key;
    char    *text;
} LangEntry;

static LangEntry g_lt[LCA_MAX];
static int       g_ltN;
static struct {
    uint32_t hash;
    int      ent;                       /* entry + 1; 0 = a free slot */
} g_lx[LCA_SLOTS];
static uint32_t  g_lcGen = 1;
static uint32_t  g_ltGen = 1;           /* the language g_lt belongs to */
static int       g_ltNoRoom;

static uint32_t LcHash(const char *owner, const char *key) {
    uint32_t h = 2166136261u;

    for (; owner && *owner; owner++) h = (h ^ (unsigned char)*owner) * 16777619u;
    h = (h ^ 0x7Cu) * 16777619u;
    for (; key && *key; key++) h = (h ^ (unsigned char)*key) * 16777619u;
    return h;
}

/* A copy we own, or NULL. Lock held. */
static char *LcDup(const char *s) {
    size_t n = s ? strlen(s) : 0;
    char *p = (char *)HeapAlloc(GetProcessHeap(), 0, n + 1);

    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* Drop everything: the text belonged to the language being left, and the
 * language is the only thing that makes a remembered answer wrong. Lock
 * held. */
static void LcWipe(void) {
    int i;

    for (i = 0; i < g_ltN; i++) {
        HeapFree(GetProcessHeap(), 0, g_lt[i].owner);
        HeapFree(GetProcessHeap(), 0, g_lt[i].key);
        HeapFree(GetProcessHeap(), 0, g_lt[i].text);
    }
    g_ltN = 0;
    memset(g_lx, 0, sizeof(g_lx));
}

/* The remembered text for this key, or NULL. Lock held. A probe that lands
 * on a free slot ends the chain - nothing was ever put past it - and one
 * that lands on another key's entry keeps walking. */
static const char *LcFind(uint32_t h, const char *own, const char *key) {
    int i, home = (int)(h % LCA_SLOTS);

    for (i = 0; i < LCA_PROBE; i++) {
        int s = (home + i) % LCA_SLOTS;
        const LangEntry *e;

        if (!g_lx[s].ent) return NULL;
        if (g_lx[s].hash != h) continue;
        e = &g_lt[g_lx[s].ent - 1];
        if (strcmp(e->owner, own) == 0 && strcmp(e->key, key) == 0)
            return e->text;
    }
    return NULL;
}

/* Remember one translation and answer with the copy. Lock held. When there
 * is no room the translation is still the answer - it is just not kept. */
static const char *LcAdd(uint32_t h, const char *own, const char *key,
                         const char *v) {
    char *o, *k, *t;
    int i, home = (int)(h % LCA_SLOTS), free_at = -1;

    for (i = 0; i < LCA_PROBE; i++) {
        int s = (home + i) % LCA_SLOTS;

        if (!g_lx[s].ent) { free_at = s; break; }
    }
    if (free_at < 0 || g_ltN >= LCA_MAX) {
        if (!g_ltNoRoom) {
            g_ltNoRoom = 1;
            TextLog("no room to remember \"%.40s\" (%d keys held): the text "
                    "is translated but not cached", key, g_ltN);
        }
        return v;
    }
    o = LcDup(own);
    k = LcDup(key);
    t = LcDup(v);
    if (!o || !k || !t) {
        if (o) HeapFree(GetProcessHeap(), 0, o);
        if (k) HeapFree(GetProcessHeap(), 0, k);
        if (t) HeapFree(GetProcessHeap(), 0, t);
        return v;                   /* answered, not remembered, as above */
    }
    g_lt[g_ltN].owner = o;
    g_lt[g_ltN].key = k;
    g_lt[g_ltN].text = t;
    g_ltN++;
    g_lx[free_at].hash = h;
    g_lx[free_at].ent = g_ltN;
    return t;
}

/** Translate one key for one owner (NULL or "" = framework text). */
SH_API const char *ShLangText(const char *owner, const char *key) {
    const char *v;
    const char *own = owner ? owner : "";
    uint32_t h;

    if (!key) return "";
    LoadConfig();
    TextLock();
    if (g_ltGen != g_lcGen) {           /* every remembered answer belonged
                                         * to the language just left */
        LcWipe();
        g_ltGen = g_lcGen;
    }
    h = LcHash(owner, key);
    v = LcFind(h, own, key);
    if (v) {
        TextUnlock();
        return v;
    }
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
    v = LcAdd(h, own, key, v);
    TextUnlock();
    return v;
}

/** The language generation: it moves whenever the answer to "what does this key
 *  say" can change. Not exported - it is for a caller inside this module that
 *  caches translations of its own (scripthook_menu.c caches row labels, which
 *  are otherwise looked up on every frame the menu is open). */
uint32_t ShLangGeneration(void) {
    return g_lcGen;
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
    /* Every remembered translation belongs to the language being left. */
    g_lcGen++;
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

/** Internal, for the loader: the one line that says where this session's
 *  menu language came from, when this run is the run that picked it -
 *  which is exactly the run whose settings file had no [Settings]
 *  Language= row. Returns 0, and empties buf, on every later run.
 *
 *  The loader writes it rather than this layer logging it: the line goes
 *  into logs\scripthook.log at every level, and a line logged from inside
 *  this load would ask this layer for the log level - which is one of the
 *  settings still being loaded (ShLogLevel reads [Settings] LogLevel
 *  through ShConfigGetStr), so the log ends up not opened at all. */
SH_API int ShLangPickLine(char *buf, int size) {
    if (!buf || size <= 0) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    LoadConfig();
    buf[0] = 0;
    if (!g_langNote[0]) {
        ShSetError(SH_OK);
        return 0;
    }
    CopyN(buf, (size_t)size, g_langNote);
    ShSetError(SH_OK);
    return 1;
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
        /* A UTF-8 BOM would glue itself to the first section header and
         * drop that whole section - for the shipped file that is [loader],
         * so load_plugins and every CPU dial would silently fall back to
         * their defaults. Same check LoadLangFile makes for lang.ini. */
        if (n >= 3 && (unsigned char)g_config[0] == 0xEF &&
            (unsigned char)g_config[1] == 0xBB &&
            (unsigned char)g_config[2] == 0xBF)
            ParseConfig(g_config + 3);
        else
            ParseConfig(g_config);
        ResolveLanguage();
        /* Parse once. The flag below was read but never set, so every
         * ShConfigGet* / ShLang call re-read the file and appended the
         * same rows to the entry table again. */
        g_configReady = 1;
        /* The one thing a first run writes on its own: the language
         * LangAutoChoose picked. After the flag above, so that the call
         * below finds a loaded config and returns, instead of parsing this
         * file into the entry table a second time. A read-only game folder
         * fails the write and costs nothing else: the session keeps the
         * language it picked, and the next launch picks again. */
        if (g_langAuto[0]) {
            if (ShConfigSetStr("Settings", "Language", g_langAuto))
                snprintf(g_langNote, sizeof(g_langNote),
                         "picked on this run - no [Settings] Language= line; "
                         "%s; written back, delete the line to have it picked "
                         "again", g_langWhy);
            else
                snprintf(g_langNote, sizeof(g_langNote),
                         "picked on this run - no [Settings] Language= line; "
                         "%s; scripthook.ini could not be written, so the "
                         "next launch picks again", g_langWhy);
        }
    } else {
        /* No file, and no second one either: a read-only folder, or the
         * file held open by something else. The flag goes up anyway, because
         * leaving it down meant every later ShConfigGet* ran this function
         * again - and each attempt wrote the default file first
         * (WriteDefaultConfig opens with "w"), so a session that could not
         * read its configuration also rewrote it on every query. The
         * defaults hold until the next launch. */
        TextLog("scripthook.ini unreadable: defaults for this session");
        g_configReady = 1;
    }
    LeaveCriticalSection(&g_cfgLock);
}

static int FindEntryCopy(const char *section, const char *key,
                         char *out, int n) {
    int i, found = 0;
    /* SetEntry rewrites entries field-by-field under this lock; a reader
     * without it could strcmp a half-written key. The value is copied out
     * before the lock is dropped - handing back the internal pointer let a
     * concurrent ShConfigSetStr rewrite the string under the reader. */
    EnsureConfigLock();
    EnterCriticalSection(&g_cfgLock);
    for (i = 0; i < g_nentries; i++)
        if (!strcmp(g_entries[i].section, section) &&
            !strcmp(g_entries[i].key, key)) {
            if (n > 0) {
                strncpy(out, g_entries[i].value, (size_t)n - 1);
                out[n - 1] = 0;
            }
            found = 1;
            break;
        }
    LeaveCriticalSection(&g_cfgLock);
    if (!found && n > 0) out[0] = 0;
    return found;
}

/** Parse scripthook.ini now. The loader calls this before
 *  plugins load; harmless to call more than once. */
SH_API void ShConfigInit(void) {
    LoadConfig();
}

/** Integer setting from the main config; def when missing. */
SH_API int ShConfigGetInt(const char *section, const char *key,
                          int def) {
    char v[256];
    char *end;
    long r;

    if (!section || !key) { ShSetError(SH_ERR_BAD_ARG); return def; }
    LoadConfig();
    if (!FindEntryCopy(section, key, v, sizeof(v))) {
        ShSetError(SH_OK);
        return def;
    }
    /* Base 10, not 0: a value written with a leading zero - "cpu_cores=08",
     * which is what a hand-edited file grows - was read as octal, stopped at
     * the 8 and came back 0. The setting looked applied and was not. */
    r = strtol(v, &end, 10);
    if (end == v) { ShSetError(SH_OK); return def; }
    ShSetError(SH_OK);
    return (int)r;
}

/** Boolean setting: 1/0, true/false, yes/no, on/off. */
SH_API int ShConfigGetBool(const char *section, const char *key,
                           int def) {
    char v[256];

    if (!section || !key) { ShSetError(SH_ERR_BAD_ARG); return def; }
    LoadConfig();
    if (!FindEntryCopy(section, key, v, sizeof(v))) {
        ShSetError(SH_OK);
        return def;
    }
    if (!_stricmp(v, "1") || !_stricmp(v, "true") ||
        !_stricmp(v, "yes") || !_stricmp(v, "on")) {
        ShSetError(SH_OK);
        return 1;
    }
    if (!_stricmp(v, "0") || !_stricmp(v, "false") ||
        !_stricmp(v, "no") || !_stricmp(v, "off")) {
        ShSetError(SH_OK);
        return 0;
    }
    ShSetError(SH_OK);
    return def;
}

/** String setting; returns 1 and copies the value (or def).
 *  ShErrorString explains a bad argument. */
SH_API int ShConfigGetStr(const char *section, const char *key,
                          const char *def, char *out, int size) {
    char v[256];
    const char *src;
    size_t n;

    if (!out || size <= 0) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (!section || !key) {
        ShSetError(SH_ERR_BAD_ARG);
        out[0] = 0;
        return 0;
    }
    LoadConfig();
    if (FindEntryCopy(section, key, v, sizeof(v))) src = v;
    else src = def ? def : "";
    n = strlen(src);
    if (n >= (size_t)size) n = (size_t)size - 1;
    memcpy(out, src, n);
    out[n] = 0;
    ShUtf8Trim(out);
    ShSetError(SH_OK);
    return 1;
}

/* "debug" -> LOG_DBG and friends. An unknown word keeps def and says so,
 * because a typo in a log switch must not quietly mean "info". */
static int LogLevelFromName(const char *s, int def) {
    if (!_stricmp(s, "none"))  return LOG_NONE;
    if (!_stricmp(s, "error")) return LOG_ERR;
    if (!_stricmp(s, "warn"))  return LOG_WARN;
    if (!_stricmp(s, "info"))  return LOG_INFO;
    if (!_stricmp(s, "debug")) return LOG_DBG;
    TextLog("LogLevel \"%s\" is not none/error/warn/info/debug - keeping "
            "\"%s\"", s, LogLevelName(def));
    return def;
}

/** [Settings] LogLevel, parsed here and nowhere else. Every translation
 *  unit asks through the DLL (see log.h), so a plugin's own log obeys
 *  the same setting as the framework's. */
SH_API int ShLogLevel(void) {
    static int level = -1;
    static int busy;
    char buf[24];

    if (level >= 0) return level;
    /* The first ask can arrive from inside the config's own first read:
     * that read logs, opening a log asks for the level. Answer with the
     * build's default then instead of recursing. */
    if (busy) return LOG_DEFAULT;
    busy = 1;
    level = LOG_DEFAULT;
    if (ShConfigGetStr("Settings", "LogLevel", "", buf, sizeof(buf)) &&
        buf[0])
        level = LogLevelFromName(buf, level);
    busy = 0;
    return level;
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
    if (g_nentries >= ENTRIES_MAX) {
        /* The value is on disk by now, so this session would keep reading
         * the old one back while the file says otherwise. Say so once
         * instead of letting the two disagree in silence. */
        static int said;

        if (!said) {
            said = 1;
            TextLog("config table full at %d entries: '%s' is on disk but "
                    "not in memory", ENTRIES_MAX, key);
        }
        return;
    }
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
    int n = snprintf(buf, size, "%sscripthook.ini", GameDir());

    if (n < 0 || n >= size) return 0;
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
 * its section (the section itself is created if absent). A blank line
 * sitting just before the header that ends the section stays before
 * that header - i.e. below the appended key, not above it, which is
 * what copying the blanks as they came would look like.
 * The file is treated as opaque bytes, so UTF-8 content is preserved.
 * Returns 1 when the file was rewritten. */
static int IniWriteValueTo(const char *path, const char *section,
                           const char *key, const char *value) {
    char tmp[GAME_DIR_MAX];
    FILE *f;
    long len;
    char *whole;
    const char *end, *p;
    const char *eol = "\n";
    char curSec[48];
    int curSecSet = 0;
    int inSec = 0;
    /* Blank lines held back at the end of the target section: they are
     * the separator that belongs to the header ending it, so they are
     * flushed after a key this call appends (see the loop). */
    const char *pendBlanks = NULL;
    size_t pendLen = 0;
    int replaced = 0;
    int wrote = 0;
    char *out = NULL;
    size_t outLen = 0, outCap = 0;
    int ok = 0;

    if (!path || !path[0] || !section || !key || !value) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }

    /* A missing file is an empty file: the section and the key are then
     * created from nothing, which is what a plugin's first save needs.
     * The main config keeps its old behaviour of laying down the default
     * file first - see IniWriteValue below. */
    f = fopen(path, "rb");
    if (!f) {
        len = 0;
        whole = (char *)malloc(1);
        if (!whole) { ShSetError(SH_ERR_BAD_ARG); return 0; }
        whole[0] = 0;
        goto have_file;
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

have_file:
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

        /* Inside the target section a blank line is held back rather than
         * copied: it is the separator that belongs to the header ending
         * this section, and a key appended below it would read as though
         * the section had grown a line too many. */
        if (inSec && !replaced && q >= li.s + li.n) {
            if (!pendLen) pendBlanks = p;
            pendLen += li.total;
            p = next;
            continue;
        }
        /* Held blanks go before this line - except before a header, which
         * flushes them after the key it appends (see the copy below). */
        if (pendLen && !(q < li.s + li.n && *q == '[')) {
            OutPrint(&out, &outLen, &outCap, "%.*s", (int)pendLen, pendBlanks);
            pendLen = 0;
            pendBlanks = NULL;
        }

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

        /* The header that closed the target section has already appended
         * its key above; the held blanks follow it, so they stay the
         * separator before this header. */
        if (pendLen) {
            OutPrint(&out, &outLen, &outCap, "%.*s", (int)pendLen, pendBlanks);
            pendLen = 0;
            pendBlanks = NULL;
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
        /* The section is the last thing in the file: the blanks held
         * above stay where they were, at the end of it. */
        if (pendLen)
            OutPrint(&out, &outLen, &outCap, "%.*s", (int)pendLen,
                     pendBlanks);
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

/* The main config's writer: same engine, path fixed to scripthook.ini, and
 * a missing file is created from the defaults first (unchanged behaviour). */
static int IniWriteValue(const char *section, const char *key,
                         const char *value) {
    char path[GAME_DIR_MAX];
    FILE *f;

    if (!IniPath(path, sizeof(path))) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    f = fopen(path, "rb");
    if (!f) LoadConfig();          /* lay down the default file first */
    else fclose(f);
    return IniWriteValueTo(path, section, key, value);
}

/* One key out of one section of one file. Deliberately no table and no
 * cache: a plugin ini is a couple of kilobytes and this is called at
 * startup and on a preset switch, not per frame. */
static int IniReadValueFrom(const char *path, const char *section,
                            const char *key, char *out, int n) {
    FILE *f;
    char line[1024];
    char cur[64];
    int inSec = 0, got = 0;

    if (!path || !section || !key || !out || n <= 0) return 0;
    out[0] = 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    cur[0] = 0;
    while (fgets(line, sizeof(line), f)) {
        char *s = line, *e;
        size_t kl, vl;

        while (*s == ' ' || *s == '\t') s++;
        e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' ||
                         e[-1] == ' ' || e[-1] == '\t'))
            *--e = 0;
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char *c = strchr(s, ']');
            size_t ln;
            if (!c) continue;
            ln = (size_t)(c - s - 1);
            if (ln >= sizeof(cur)) ln = sizeof(cur) - 1;
            memcpy(cur, s + 1, ln);
            cur[ln] = 0;
            inSec = (strcmp(cur, section) == 0);
            continue;
        }
        if (!inSec) continue;
        {
            char *eq = strchr(s, '=');
            char *v, *c;

            if (!eq) continue;
            kl = (size_t)(eq - s);
            while (kl > 0 && (s[kl - 1] == ' ' || s[kl - 1] == '\t')) kl--;
            if (kl != strlen(key) || strncmp(s, key, kl) != 0) continue;
            v = eq + 1;
            while (*v == ' ' || *v == '\t') v++;
            c = strstr(v, " ;");            /* keep "a ; note" working */
            if (!c) c = strstr(v, "\t;");
            vl = c ? (size_t)(c - v) : strlen(v);
            while (vl > 0 && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
            if (vl >= (size_t)n) vl = (size_t)n - 1;
            memcpy(out, v, vl);
            out[vl] = 0;
            got = 1;
            break;
        }
    }
    fclose(f);
    return got;
}

/* Where a plugin's own ini is. The loader's answer only: it knows which
 * folder the .asi was found in, and it deliberately refuses to guess a
 * path for a name it never scanned - a guess would make a plugin write a
 * file nothing reads. */
static int PluginIniPath(const char *plugin, char *buf, int size) {
    if (!plugin || !plugin[0] || !buf || size <= 0) return 0;
    return ShPluginIniPath(plugin, buf, size);
}

static int PluginCfgGet(const char *plugin, const char *section,
                        const char *key, const char *def, char *out, int n) {
    char path[GAME_DIR_MAX];

    if (!plugin || !section || !key || !out || n <= 0) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    out[0] = 0;
    if (def) {
        strncpy(out, def, (size_t)n - 1);
        out[n - 1] = 0;
    }
    if (!PluginIniPath(plugin, path, sizeof(path))) return 0;
    if (!IniReadValueFrom(path, section, key, out, n)) return 0;
    ShSetError(SH_OK);
    return 1;
}

/** @addtogroup config
 *  A plugin's own ini, section by section. Same engine and same file
 *  layout rules as the main config: the key is replaced where it is,
 *  a missing key is appended to its section, a missing section is
 *  created, and every other line - other keys, other sections,
 *  comments - survives byte for byte. */
SH_API int ShPluginConfigGetStr(const char *plugin, const char *section,
                                const char *key, const char *def,
                                char *out, int size) {
    return PluginCfgGet(plugin, section, key, def, out, size);
}

SH_API int ShPluginConfigGetInt(const char *plugin, const char *section,
                                const char *key, int def) {
    char buf[64];

    if (!PluginCfgGet(plugin, section, key, NULL, buf, sizeof(buf)))
        return def;
    return (int)strtol(buf, NULL, 0);
}

SH_API int ShPluginConfigGetBool(const char *plugin, const char *section,
                                 const char *key, int def) {
    char buf[64];

    if (!PluginCfgGet(plugin, section, key, NULL, buf, sizeof(buf)))
        return def ? 1 : 0;
    if (!buf[0]) return def ? 1 : 0;
    if (!_stricmp(buf, "true") || !_stricmp(buf, "yes") ||
        !_stricmp(buf, "on"))
        return 1;
    if (!_stricmp(buf, "false") || !_stricmp(buf, "no") ||
        !_stricmp(buf, "off"))
        return 0;
    return atoi(buf) ? 1 : 0;
}

SH_API int ShPluginConfigSetStr(const char *plugin, const char *section,
                                const char *key, const char *value) {
    char path[GAME_DIR_MAX];

    if (!plugin || !section || !key || !value) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    if (!PluginIniPath(plugin, path, sizeof(path))) {
        ShSetError(SH_ERR_NO_PLUGIN);
        return 0;
    }
    EnsureConfigLock();
    EnterCriticalSection(&g_cfgLock);
    if (!IniWriteValueTo(path, section, key, value)) {
        LeaveCriticalSection(&g_cfgLock);
        return 0;
    }
    LeaveCriticalSection(&g_cfgLock);
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShPluginConfigSetInt(const char *plugin, const char *section,
                                const char *key, int value) {
    char buf[32];

    snprintf(buf, sizeof(buf), "%d", value);
    return ShPluginConfigSetStr(plugin, section, key, buf);
}

SH_API int ShPluginConfigSetBool(const char *plugin, const char *section,
                                 const char *key, int value) {
    return ShPluginConfigSetStr(plugin, section, key, value ? "1" : "0");
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
