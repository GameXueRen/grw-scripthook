/* A read-only view of an Anvil `.forge` container's tables.
 *
 * The Forge Mod Loader never rewrites an archive: it keeps an eye on the
 * bytes the engine is about to read and swaps in mod bytes for the few
 * ranges a mod replaces. To do that it has to know, for the archive the
 * engine opened, where every entry's payload lives and how much room it
 * has before the next one starts. That is all this is.
 *
 * Only the tables are read - the header, the per-section tables and the
 * index/name tables - which live below the first payload, so opening a
 * 19.89 GB WorldMap archive costs a few megabytes of reads, not 19.89 GB.
 *
 * Format (docs/02-formats.md, cross-checked against ForgeArchive.cs) is
 * the Wildlands dialect: magic "scimitar", version 27, index records of
 * 20 bytes and name/info records of 192 bytes with the name at offset
 * 44. Everything is little-endian.
 */
#ifndef SH_FORGE_H
#define SH_FORGE_H

#include <stdint.h>

#define SH_FORGE_NAME_MAX 128
#define SH_FORGE_PATH_MAX 260

/** One entry, as the tables describe it. */
typedef struct {
    uint32_t index;        /**< position in the index table (0 based)  */
    uint64_t id;           /**< FileDataID: the only unique key        */
    char     name[SH_FORGE_NAME_MAX]; /**< from the info record       */
    uint32_t nameLen;

    uint64_t offset;       /**< payload start, absolute in the file    */
    uint32_t length;       /**< RawDataSize from the index table       */

    /** Space before the next entry's payload (or the file's end), so a
     *  replacement of this length needs no entry moved. */
    uint64_t room;

    /** The two places RawDataSize is written. A replacement that is
     *  shorter than the original has to patch both, or the engine reads
     *  the wrong count of bytes. */
    uint64_t locSizeOff;   /**< index record +16                       */
    uint64_t infoSizeOff;  /**< info record +0                         */
} ShForgeEntry;

typedef struct {
    int            version;
    int            entryCount;
    ShForgeEntry  *entries;
    unsigned long long fileSize;
    char           path[SH_FORGE_PATH_MAX];
} ShForge;

/** Read the tables of `path`. Returns 1 on success, 0 on any failure -
 *  a wrong magic, an unsupported version, a short read or a table that
 *  points outside the file. On success the caller owns f->entries and
 *  frees it with ShForgeClose. */
int  ShForgeOpen(const char *path, ShForge *f);
void ShForgeClose(ShForge *f);

/** Lookups. NULL when absent; names are compared case-insensitively. */
const ShForgeEntry *ShForgeByIndex(const ShForge *f, uint32_t index);
const ShForgeEntry *ShForgeById(const ShForge *f, uint64_t id);
const ShForgeEntry *ShForgeByName(const ShForge *f, const char *name);

/* ---- the modded view of one archive --------------------------------
 * Built by scripthook_forge.c, served by scripthook_forge_io.c. A patch
 * is a byte range of the file whose reads answer with `bytes` instead.
 */

typedef struct {
    uint64_t off;
    uint32_t len;
    uint8_t *bytes;    /**< what the range should read as, owned here  */
    /** What the archive itself holds there, read once when the overlay
     *  is built. Before a patch is written the buffer is checked against
     *  this: if it does not match, the read did not land where it was
     *  thought to and writing would corrupt live data, so nothing is
     *  written at all. */
    uint8_t *orig;
} ShForgePatch;

typedef struct {
    char           path[SH_FORGE_PATH_MAX];
    ShForgePatch  *patches;
    int            patchCount;
    int            modCount;      /**< entries replaced                */
    int            dropped;       /**< mods rejected (too big etc.)    */
    int            rejected;      /**< patches refused by the check    */
    char           note[160];     /**< what to say about it, if short */
} ShForgeOverlay;

/** Load [forgemod] and scan mods\. Called from the loader thread. */
void ShForgeStartup(void);

/** 1 when the feature is on; 0 when off or unavailable. */
int  ShForgeEnabled(void);
/** 1 when dry_run: overlays are built and logged but not served. */
int  ShForgeDryRun(void);
/** 1 when [forgemod] log_reads=1: every read of a modded archive is
 *  recorded, which is how the asynchronous path is checked. */
int  ShForgeLogReads(void);

/** The overlay for an archive path, or NULL when it has no mods. The
 *  result is cached and owned by this module; do not free it. */
ShForgeOverlay *ShForgeOverlayFor(const char *archivePath);

/** Replace the bytes of [off,off+len) that any patch covers, in place.
 *  A read that misses every patch is left exactly as it was, and a patch
 *  whose target bytes do not match the archive is refused rather than
 *  written. */
void ShForgeOverlayFixup(ShForgeOverlay *o, uint64_t off, uint8_t *buf,
                         size_t len);

/** One line for the menu status row. */
const char *ShForgeStatusLine(void);

/** Register the menu page. Called with the root menu's id. */
void ShForgeMenuRegister(void);

/** Install the file I/O hooks. Called once, from the loader thread. */
void ShForgeIoStartup(void);

/** How many reads have been answered with patched bytes so far. */
int ShForgeIoFixups(void);

#endif /* SH_FORGE_H */
