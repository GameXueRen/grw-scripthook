#pragma once
/* Plugin discovery: what is under plugins\, decided in one place.
 *
 * Three modules used to walk plugins\ themselves - the loader, the Plugins
 * settings page and the plugin blacklist - each carrying its own copy of the
 * same rule and nothing making them agree. The rule is one thing ("which
 * plugins are installed"), so it is read in one place now and the three ask
 * for the answer. That is the whole reason this exists: a layout change used
 * to be a change in three files, and missing one showed up as a plugin the
 * settings page offers and the loader does not load, or a menu row that
 * appears for something that never ran.
 *
 * ---- what counts as a plugin -------------------------------------------
 *
 * An .asi file, at the root of plugins\ or one folder in:
 *
 *   plugins\AmmoControl.asi              the file alone
 *   plugins\AmmoControl\AmmoControl.asi  the file in a folder of its own
 *   plugins\pack\AmmoControl.asi         the file in someone else's folder
 *   plugins\AmmoControl\AmmoControl.ini  its config   (see ShPluginIniPath)
 *   plugins\pack\lang.ini                its text     (see ShPluginLangPath)
 *
 * The two shapes are the same plugin and neither is preferred for loading.
 * The folder is how a plugin keeps its assets beside it, and several plugins
 * may share one - what is a plugin is the .asi, not the folder it sits in.
 * Nothing deeper than one folder is read: plugins\a\b\x.asi is not looked for
 * at all, so a plugin put there is simply not found. (The game folder is not
 * the place to nest a modpack three deep, and half-supporting it would make
 * "why did nothing load" a question with no line in the log answering it.)
 *
 * The identity is the FILE's stem, not the folder's name:
 *
 *   name = the .asi with its extension taken off ("AmmoControl")
 *
 * That is what the [plugins] switch, the menu row and the lang owner are
 * keyed by, and it is deliberately not the folder: under this layout the
 * folder may be anything at all, may be shared, or may not exist.
 *
 * Every name a plugin already uses is already this value: most plugins ask
 * for their own config through ShPluginIniPath with a name they derived from
 * their own module file (GetModuleFileNameA), which is the same thing, and
 * the rest hardcode the name of the file they ship as. So a plugin installed
 * today under the old rule - plugins\<name>\<name>.asi - has the same name
 * before and after this, and [plugins] lines, menu rows and lang files do not
 * move. That was the constraint the whole design was fitted to.
 *
 * ---- collisions ---------------------------------------------------------
 *
 * Two .asi files with the same stem - plugins\X.asi and plugins\sub\X.asi -
 * cannot both be a plugin called X: the switch, the menu row and the lang
 * owner are one slot each. One of them is refused, with a line in the log
 * naming both paths, and which one wins is decided and stable: the file
 * closer to plugins\ first (the root beats a folder), then the path that
 * sorts first. Refusing rather than inventing a second identity ("sub\X") is
 * on purpose - the ini key and the menu row would carry a backslash, and a
 * duplicate name is a mistake worth seeing, not a configuration worth
 * supporting.
 *
 * ---- when it runs ------------------------------------------------------
 *
 * The scan runs once per process and the result is kept. plugins\ does not
 * change under a running game in any way worth re-reading, three callers
 * asking for the same list should get the same list, and every one of them
 * wants it early - the blacklist at start up, the loader a moment later, the
 * settings page when the menu is built. The entries stay valid for the life
 * of the process, so callers may keep the pointers.
 *
 * Thread safety: the table is written once, inside a lock, and only read
 * afterwards - a reader never needs the lock, and the pointer it gets back is
 * stable. Two threads reaching the first call at the same moment both get the
 * same table (the second one waits for the first).
 */
#include <windows.h>

/* As many plugins as the framework will carry. The blacklist's own registry
 * and the settings page use the same ceiling, so nothing downstream has to
 * deal with more than this. */
#define SH_PLUGIN_MAX 64

typedef struct {
    char name[64];       /* The identity: the .asi's stem. 64 is the settings
                          * page's own name buffer, and the ceiling for a
                          * [plugins] key. A file whose stem does not fit is
                          * not a plugin this build can carry - it is passed
                          * over, because a truncated name could not be the
                          * key of anything. */
    char dir[MAX_PATH];  /* its folder, with the trailing backslash, so a file
                          * beside the .asi is dir + something. For a plugin at
                          * the root this is plugins\ itself. This is what the
                          * config and text lookups are built from. */
    char asi[MAX_PATH];  /* the full path to the .asi */
    char rel[MAX_PATH];  /* what the log calls it, relative to plugins\:
                          * "AmmoControl.asi" or "AmmoControl\AmmoControl.asi".
                          * A full MAX_PATH, not a short display buffer, so a
                          * long folder name cannot truncate it - it is a path
                          * a reader has to be able to look up, and the paths
                          * it is built from already fit in MAX_PATH. */
    int  depth;          /* how many folders deep: 0 at the root of plugins\,
                          * 1 one level in. A field and not a read-back out of
                          * `rel`, because it decides a name collision and must
                          * not be the thing a long name can break. */
} ShPluginEnt;

typedef struct {
    int count;           /* plugins in the table */
    int found;           /* .asi files seen, before collisions were settled */
    int dupes;           /* refused: the name was already taken */
    int overflow;        /* dropped: the table was full */
} ShPluginScanInfo;

/* Scan plugins\, keep the result, and hand back the table.
 *
 * Returns the number of entries and sets *ents to a table that stays valid
 * for the life of the process. `info` may be NULL when only the list is
 * wanted. Creates plugins\ if it is not there yet, so a fresh install finds
 * the folder it is told to fill rather than an error. Returns 0 with *ents
 * unset when the game directory cannot be found (only possible when running
 * outside the game).
 */
int ShPluginScan(const ShPluginEnt **ents, ShPluginScanInfo *info);

/* The entry with this name, or NULL. Case-insensitive, as everything else
 * that keys on a plugin name is. Scans first if nothing has yet. */
const ShPluginEnt *ShPluginFind(const char *name);
