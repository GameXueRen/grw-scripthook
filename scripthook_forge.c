/* Forge Mod Loader: the mods\ side of it.
 *
 * What it does, from the top:
 *
 *   1. reads [forgemod] from scripthook.ini;
 *   2. walks <gamedir>\mods and turns every file it finds into a claim on
 *      one entry of one archive, honouring the GRB Tweaks layout rules
 *      (flat layout wins, then mod folder name order, "~" disables a
 *      folder, ".delete" is recognised and skipped for now);
 *   3. resolves each claim against that archive's own tables - by the
 *      numeric prefix in the file name, or by the entry name - and
 *      rejects any payload that does not fit the room the entry already
 *      has, because this design never moves another entry;
 *   4. hands the I/O layer (scripthook_forge_io.c) a per-archive list of
 *      byte ranges to answer with mod bytes instead of the file's own.
 *
 * Nothing is written anywhere and no archive is modified: the vanilla
 * .forge stays byte for byte as shipped, which is the whole point of the
 * override-folder idea (FusionFix ModLoader and GRB Tweaks both work
 * this way).
 *
 * One reality this module also has to handle, from the community notes:
 * the same resource often sits in several archives. Change one copy and
 * the game may still load another, which looks exactly like the mod did
 * nothing. So the FileDataIDs a mod targets are checked against every
 * installed archive and the other copies are named in the log (and can
 * be overridden too, with apply_all_copies).
 *
 * Layout it accepts, matching GRB Tweaks:
 *
 *   mods/DataPC_patch_01/123_-_GR_PLAYER_Template.data      flat, wins
 *   mods/My Vest/DataPC_patch_01/123_-_....data             mod folder,
 *                                                           ordered by
 *                                                           folder name
 *   mods/~Old mod/...                                       disabled
 *   mods/X/DataPC/7_-_Graffiti.data.delete                  recognised,
 *                                                           skipped (log)
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "forge.h"
#include "log.h"

#define MODS_MAX       512
#define ARCHIVES_MAX   64
#define OVL_MAX        64
#define COPY_IDX_MAX   2000000

typedef struct {
    char base[128];            /* archive file name without ".forge"    */
    char path[SH_FORGE_PATH_MAX];
} ArchiveRef;

typedef struct {
    char     archive[128];     /* the archive folder it was found under */
    char     rel[192];         /* path under mods\, for the log         */
    char     path[SH_FORGE_PATH_MAX];
    int      priority;         /* lower wins; flat layout is priority 0 */
    /* resolved against the archive's tables */
    int      ok;               /* 1 applied, -1 overridden, -2 rejected */
    uint64_t id;
    uint32_t index;
    uint32_t len;              /* payload length on disk                */
    uint64_t room;             /* room the entry has                    */
} ForgeMod;

static int g_enabled, g_dryRun, g_strict, g_reportCopies, g_applyAll;
static int g_logReads;
static int g_started;

static ArchiveRef  g_arch[ARCHIVES_MAX];
static int         g_narch;

static ForgeMod   *g_mods;
static int         g_nmods;

static ShForgeOverlay **g_ovl;
static int              g_novl;

static char g_status[220] = "Forge Mod Loader: off";
static CRITICAL_SECTION g_lock;

/* ---- small helpers -------------------------------------------------- */

static int StrEqI(const char *a, const char *b) {
    return a && b && _stricmp(a, b) == 0;
}

static int IsDir(const char *p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* "2 mod" sorts before "10 mod": compare digit runs numerically. */
static int NaturalLess(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            unsigned long x = 0, y = 0;
            while (isdigit((unsigned char)*a)) x = x * 10 + (unsigned)(*a++ - '0');
            while (isdigit((unsigned char)*b)) y = y * 10 + (unsigned)(*b++ - '0');
            if (x != y) return x < y;
            continue;
        }
        {
            int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
            if (ca != cb) return ca < cb;
            a++; b++;
        }
    }
    return *a ? 0 : (*b ? 1 : 0);
}

static void BaseNameOf(const char *p, char *out, int n) {
    const char *s = strrchr(p, '\\');
    snprintf(out, (size_t)n, "%s", s ? s + 1 : p);
}

/* The archive base name of a file name: "x\DataPC.forge" -> "DataPC". */
static void BaseFromArchivePath(const char *path, char *out, int n) {
    const char *slash = strrchr(path, '\\');
    const char *base = slash ? slash + 1 : path;
    int k = 0;
    while (base[k] && base[k] != '.' && k < n - 1) { out[k] = base[k]; k++; }
    out[k] = 0;
}

/* ---- installed archives --------------------------------------------- */

static void AddArchive(const char *path) {
    ArchiveRef *a;
    if (g_narch >= ARCHIVES_MAX) return;
    a = &g_arch[g_narch];
    BaseFromArchivePath(path, a->base, sizeof(a->base));
    snprintf(a->path, sizeof(a->path), "%s", path);
    g_narch++;
}

static void ScanArchives(void) {
    char dir[SH_FORGE_PATH_MAX], pat[SH_FORGE_PATH_MAX];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    g_narch = 0;
    ShGameDir(dir, sizeof(dir));

    snprintf(pat, sizeof(pat), "%s\\*.forge", dir);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                char full[SH_FORGE_PATH_MAX];
                snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
                AddArchive(full);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    /* dlc_2, dlc_10, ... each carry their own archives. */
    snprintf(pat, sizeof(pat), "%s\\dlc_*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                char sub[SH_FORGE_PATH_MAX], subpat[SH_FORGE_PATH_MAX];
                WIN32_FIND_DATAA sfd;
                HANDLE sh;
                snprintf(sub, sizeof(sub), "%s\\%s", dir, fd.cFileName);
                snprintf(subpat, sizeof(subpat), "%s\\*.forge", sub);
                sh = FindFirstFileA(subpat, &sfd);
                if (sh != INVALID_HANDLE_VALUE) {
                    do {
                        if (!(sfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                            char full[SH_FORGE_PATH_MAX];
                            snprintf(full, sizeof(full), "%s\\%s", sub, sfd.cFileName);
                            AddArchive(full);
                        }
                    } while (FindNextFileA(sh, &sfd));
                    FindClose(sh);
                }
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
}

static const ArchiveRef *ArchiveByBase(const char *base) {
    int i;
    for (i = 0; i < g_narch; i++)
        if (StrEqI(g_arch[i].base, base)) return &g_arch[i];
    return NULL;
}

/* ---- file name -> entry -------------------------------------------- */

/* The two shapes a mod file name may take:
 *   "123_-_GR_PLAYER_Template.data"   (the toolkit's export naming)
 *   "GR_PLAYER_Template.data"         (a plain export)
 * Returns 1 and fills index when the numeric prefix is there; `nameOut`
 * always receives the candidate entry name. */
static int ParseModFileName(const char *file, uint32_t *index, char *nameOut,
                            int nameLen) {
    const char *p = file;
    unsigned long v = 0;
    int digits = 0;

    while (isdigit((unsigned char)*p)) { v = v * 10 + (unsigned)(*p - '0'); digits++; p++; }
    if (digits > 0 && p[0] == '_' && p[1] == '-' && p[2] == '_') {
        *index = (uint32_t)v;
        snprintf(nameOut, (size_t)nameLen, "%s", p + 3);
        return 1;
    }
    snprintf(nameOut, (size_t)nameLen, "%s", file);
    return 0;
}

/* Entry names carry no extension; a file name may. Strip a trailing
 * ".xxx" for the comparison, and try the toolkit's own suffixes. */
static const ShForgeEntry *FindEntryByName(const ShForge *f, const char *name) {
    const ShForgeEntry *e = ShForgeByName(f, name);
    char buf[SH_FORGE_NAME_MAX];
    int i, n;

    if (e) return e;

    n = (int)strlen(name);
    for (i = n - 1; i > 0; i--)
        if (name[i] == '.') {
            if (i >= (int)sizeof(buf)) i = (int)sizeof(buf) - 1;
            memcpy(buf, name, (size_t)i);
            buf[i] = 0;
            e = ShForgeByName(f, buf);
            if (e) return e;
            break;
        }

    snprintf(buf, sizeof(buf), "%s.data", name);
    return ShForgeByName(f, buf);
}

/* ---- scanning mods\ ------------------------------------------------- */

static void AddMod(const char *archive, const char *rel, const char *path,
                   int priority) {
    ForgeMod *m;
    char file[192];

    if (strlen(rel) > 7 && StrEqI(rel + strlen(rel) - 7, ".delete")) {
        Log("mods: %s: '.delete' is recognised but not implemented yet; skipped",
            rel);
        return;
    }
    if (g_nmods >= MODS_MAX) {
        Log("mods: too many files, ignoring %s", rel);
        return;
    }

    BaseNameOf(rel, file, sizeof(file));
    m = &g_mods[g_nmods];
    memset(m, 0, sizeof(*m));
    snprintf(m->archive, sizeof(m->archive), "%s", archive);
    snprintf(m->rel, sizeof(m->rel), "%s", rel);
    snprintf(m->path, sizeof(m->path), "%s", path);
    m->priority = priority;
    g_nmods++;
}

typedef void (*FileFn)(const char *rel, const char *full, void *user);

/* Files under a directory, a few levels down. Any intermediate folder
 * is organisational here: the file itself names the entry. */
static void WalkDir(const char *root, const char *rel, int depth,
                    FileFn fn, void *user) {
    char pat[SH_FORGE_PATH_MAX];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    if (depth > 4) return;
    snprintf(pat, sizeof(pat), "%s\\*", root);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const char *n = fd.cFileName;
        char childFull[SH_FORGE_PATH_MAX], childRel[192];
        if (n[0] == '.') continue;
        if (rel[0]) snprintf(childRel, sizeof(childRel), "%s\\%s", rel, n);
        else        snprintf(childRel, sizeof(childRel), "%s", n);
        snprintf(childFull, sizeof(childFull), "%s\\%s", root, n);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            WalkDir(childFull, childRel, depth + 1, fn, user);
        else
            fn(childRel, childFull, user);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

typedef struct { const char *archive; int priority; } CollectCtx;

static void CollectFile(const char *rel, const char *full, void *user) {
    CollectCtx *c = (CollectCtx *)user;
    AddMod(c->archive, rel, full, c->priority);
}

static void ScanArchiveFolder(const char *modsRoot, const char *archiveName,
                              int priority) {
    char full[SH_FORGE_PATH_MAX];
    CollectCtx c;
    snprintf(full, sizeof(full), "%s\\%s", modsRoot, archiveName);
    c.archive = archiveName;
    c.priority = priority;
    WalkDir(full, archiveName, 0, CollectFile, &c);
}

static void ScanMods(void) {
    char modsRoot[SH_FORGE_PATH_MAX], gameDir[SH_FORGE_PATH_MAX];
    char pat[SH_FORGE_PATH_MAX];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char **folders = NULL;
    int nfolders = 0, cap = 0, flatSeen = 0, i;

    g_mods = (ForgeMod *)calloc(MODS_MAX, sizeof(ForgeMod));
    g_nmods = 0;
    if (!g_mods) return;

    ShGameDir(gameDir, sizeof(gameDir));
    snprintf(modsRoot, sizeof(modsRoot), "%s\\mods", gameDir);
    if (!IsDir(modsRoot)) {
        Log("mods: %s does not exist, nothing to load", modsRoot);
        return;
    }

    snprintf(pat, sizeof(pat), "%s\\*", modsRoot);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        const char *name = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (name[0] == '.') continue;
        if (name[0] == '~') {   /* GRB Tweaks: "~" switches a mod off */
            Log("mods: '%s' is disabled ('~' prefix), skipped", name);
            continue;
        }

        /* Flat layout: the folder is named after an archive. It wins. */
        if (ArchiveByBase(name)) {
            ScanArchiveFolder(modsRoot, name, 0);
            flatSeen++;
            continue;
        }

        if (nfolders == cap) {
            int ncap = cap ? cap * 2 : 16;
            char **np = (char **)realloc(folders, (size_t)ncap * sizeof(char *));
            if (!np) break;
            folders = np;
            cap = ncap;
        }
        folders[nfolders] = _strdup(name);
        if (folders[nfolders]) nfolders++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    /* Stable order by folder name, digit runs compared as numbers. */
    for (i = 1; i < nfolders; i++) {
        char *key = folders[i];
        int j = i - 1;
        while (j >= 0 && NaturalLess(key, folders[j])) {
            folders[j + 1] = folders[j];
            j--;
        }
        folders[j + 1] = key;
    }

    for (i = 0; i < nfolders; i++) {
        char modDir[SH_FORGE_PATH_MAX], apat[SH_FORGE_PATH_MAX];
        WIN32_FIND_DATAA afd;
        HANDLE ah;
        int rank = 100 + i;     /* after every flat folder */

        snprintf(modDir, sizeof(modDir), "%s\\%s", modsRoot, folders[i]);
        snprintf(apat, sizeof(apat), "%s\\*", modDir);
        ah = FindFirstFileA(apat, &afd);
        if (ah != INVALID_HANDLE_VALUE) {
            do {
                if (!(afd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (afd.cFileName[0] == '.') continue;
                if (!ArchiveByBase(afd.cFileName)) {
                    Log("mods: %s\\%s is not an archive name, skipped",
                        folders[i], afd.cFileName);
                    continue;
                }
                ScanArchiveFolder(modDir, afd.cFileName, rank);
            } while (FindNextFileA(ah, &afd));
            FindClose(ah);
        }
        free(folders[i]);
    }
    free(folders);
    Log("mods: %d file(s), %d flat archive folder(s)", g_nmods, flatSeen);
}

/* ---- resolving a mod against its archive ---------------------------- */

static void ResolveMods(void) {
    int i;
    for (i = 0; i < g_nmods; i++) g_mods[i].ok = 0;

    for (i = 0; i < g_nmods; i++) {
        ForgeMod *m = &g_mods[i];
        const ArchiveRef *ar = ArchiveByBase(m->archive);
        ShForge f;
        const ShForgeEntry *e;
        char file[192], name[192];
        WIN32_FILE_ATTRIBUTE_DATA fad;
        uint32_t idx = 0;
        int hasIndex;

        if (!ar) { Log("mods: %s: no archive named '%s'", m->rel, m->archive); continue; }

        if (!ShForgeOpen(ar->path, &f)) {
            Log("mods: %s: cannot read %s", m->rel, ar->path);
            continue;
        }

        BaseNameOf(m->rel, file, sizeof(file));
        hasIndex = ParseModFileName(file, &idx, name, sizeof(name));

        e = hasIndex ? ShForgeByIndex(&f, idx) : FindEntryByName(&f, name);
        if (hasIndex && e) {
            const char *dot = strrchr(name, '.');
            char stripped[192];
            size_t n = dot ? (size_t)(dot - name) : strlen(name);
            if (n >= sizeof(stripped)) n = sizeof(stripped) - 1;
            memcpy(stripped, name, n);
            stripped[n] = 0;
            if (!StrEqI(stripped, e->name) && !StrEqI(name, e->name))
                Log("mods: %s: index %u is '%s', file says '%s' - using the index",
                    m->rel, idx, e->name, stripped);
        }
        if (!e) {
            Log("mods: %s: no entry '%s' (index %u) in %s", m->rel, name,
                idx, m->archive);
            ShForgeClose(&f);
            continue;
        }

        if (!GetFileAttributesExA(m->path, GetFileExInfoStandard, &fad)) {
            Log("mods: %s: cannot stat the file", m->rel);
            ShForgeClose(&f);
            continue;
        }

        m->id    = e->id;
        m->index = e->index;
        m->len   = (uint32_t)fad.nFileSizeLow;
        m->room  = e->room;

        if ((uint64_t)m->len > e->room) {
            Log("mods: %s: %u bytes does not fit %s entry %u ('%s', %llu bytes "
                "of room) - it would need the entry moved, which this build "
                "does not do", m->rel, m->len, m->archive, e->index, e->name,
                (unsigned long long)e->room);
            m->ok = -2;
            ShForgeClose(&f);
            continue;
        }

        m->ok = 1;
        Log("mods: %s -> %s entry %u '%s' id=0x%llX (%u bytes, %llu room, "
            "priority %d)", m->rel, m->archive, e->index, e->name,
            (unsigned long long)e->id, m->len, (unsigned long long)e->room,
            m->priority);
        ShForgeClose(&f);
    }

    /* Two folders claiming one entry: the higher priority one wins, and
     * the loser is said out loud rather than silently dropped. */
    for (i = 0; i < g_nmods; i++) {
        int j;
        if (g_mods[i].ok != 1) continue;
        for (j = 0; j < i; j++) {
            if (g_mods[j].ok != 1) continue;
            if (StrEqI(g_mods[i].archive, g_mods[j].archive) &&
                g_mods[i].index == g_mods[j].index) {
                Log("mods: %s overrides %s (priority %d < %d)",
                    g_mods[j].rel, g_mods[i].rel,
                    g_mods[j].priority, g_mods[i].priority);
                g_mods[i].ok = -1;
                break;
            }
        }
    }
}

/* ---- the cross-archive copy index ----------------------------------- */

typedef struct { uint64_t id; uint16_t arch; } IdRef;
static IdRef *g_idrefs;
static int    g_nidrefs;
static int    g_idrefsReady;

static int CmpIdRef(const void *a, const void *b) {
    uint64_t x = ((const IdRef *)a)->id, y = ((const IdRef *)b)->id;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Every installed archive contributes its entry ids. The tables live
 * below the first payload, so this reads a few MB per archive rather
 * than tens of GB. */
static void CopyIndexBuild(void) {
    int i;
    g_idrefs = (IdRef *)malloc((size_t)COPY_IDX_MAX * sizeof(IdRef));
    if (!g_idrefs) return;

    for (i = 0; i < g_narch; i++) {
        ShForge f;
        int j;
        if (!ShForgeOpen(g_arch[i].path, &f)) continue;
        for (j = 0; j < f.entryCount; j++) {
            if (g_nidrefs >= COPY_IDX_MAX) break;
            /* 16 GlobalMetaFile and 145 PrefetchingFileInfos are in every
             * archive by design and are not "copies". */
            if (f.entries[j].id == 16 || f.entries[j].id == 145) continue;
            g_idrefs[g_nidrefs].id = f.entries[j].id;
            g_idrefs[g_nidrefs].arch = (uint16_t)i;
            g_nidrefs++;
        }
        ShForgeClose(&f);
    }
    qsort(g_idrefs, (size_t)g_nidrefs, sizeof(IdRef), CmpIdRef);
    g_idrefsReady = 1;
    Log("copies: indexed %d ids across %d archives", g_nidrefs, g_narch);
}

static void CopyReport(void) {
    int i;
    for (i = 0; i < g_nmods; i++) {
        ForgeMod *m = &g_mods[i];
        int lo, hi, k, others = 0;
        char list[300];
        size_t used = 0;

        if (m->ok != 1 || !m->id) continue;

        lo = 0; hi = g_nidrefs;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (g_idrefs[mid].id < m->id) lo = mid + 1; else hi = mid;
        }
        list[0] = 0;
        for (k = lo; k < g_nidrefs && g_idrefs[k].id == m->id; k++) {
            const char *base = g_arch[g_idrefs[k].arch].base;
            if (StrEqI(base, m->archive)) continue;
            others++;
            if (used + strlen(base) + 3 < sizeof(list))
                used += (size_t)snprintf(list + used, sizeof(list) - used,
                                         "%s%s", used ? ", " : "", base);
        }
        if (others > 0)
            Log("copies: %s also lives in %s - change those too, or the game "
                "may keep loading them", m->rel, list);
    }
}

static DWORD WINAPI CopyIndexThread(LPVOID p) {
    (void)p;
    CopyIndexBuild();
    if (g_reportCopies) CopyReport();
    return 0;
}

/* ---- overlays ------------------------------------------------------- */

static int ArchiveHasMods(const char *base) {
    int i;
    for (i = 0; i < g_nmods; i++) {
        if (g_mods[i].ok != 1) continue;
        if (StrEqI(g_mods[i].archive, base)) return 1;
        if (g_applyAll && g_mods[i].id) return 1;
    }
    return 0;
}

/* Read len bytes of the archive itself at off, for the safety check. */
static uint8_t *ReadOrig(FILE *af, uint64_t off, uint32_t len) {
    uint8_t *p;
    if (!af) return NULL;
    p = (uint8_t *)malloc(len ? len : 1);
    if (!p) return NULL;
    if (_fseeki64(af, (long long)off, SEEK_SET) != 0 ||
        fread(p, 1, len, af) != len) {
        free(p);
        return NULL;
    }
    return p;
}

/* Append one patch. Takes ownership of `bytes` on success and frees it
 * on failure, so the caller never has to. */
static int AddPatch(ShForgeOverlay *o, FILE *af, uint64_t off, uint32_t len,
                    uint8_t *bytes) {
    ShForgePatch *np = (ShForgePatch *)realloc(
        o->patches, (size_t)(o->patchCount + 1) * sizeof(ShForgePatch));
    uint8_t *orig;

    if (!np) { free(bytes); return 0; }
    o->patches = np;
    orig = ReadOrig(af, off, len);
    if (!orig) { free(bytes); return 0; }

    o->patches[o->patchCount].off = off;
    o->patches[o->patchCount].len = len;
    o->patches[o->patchCount].bytes = bytes;
    o->patches[o->patchCount].orig = orig;
    o->patchCount++;
    return 1;
}

/* Undo the patches added since `from`, freeing what they hold. */
static void RollbackPatches(ShForgeOverlay *o, int from) {
    int i;
    for (i = from; i < o->patchCount; i++) {
        free(o->patches[i].bytes);
        free(o->patches[i].orig);
    }
    o->patchCount = from;
}

static void FreeOverlay(ShForgeOverlay *o) {
    if (!o) return;
    RollbackPatches(o, 0);
    free(o->patches);
    free(o);
}

static ShForgeOverlay *OverlayBuild(const char *path, const char *base) {
    ShForge f;
    ShForgeOverlay *o;
    FILE *af;
    int i, chosen = 0;

    if (!ArchiveHasMods(base)) return NULL;
    if (!ShForgeOpen(path, &f)) {
        Log("forge: cannot read %s", path);
        return NULL;
    }

    /* A second, plain handle on the archive, used only to read the
     * original bytes of the ranges that are about to be patched. Those
     * are small - one entry's worth - so this costs almost nothing, and
     * it is what lets the fixup refuse a patch instead of corrupting a
     * read that landed somewhere unexpected. */
    af = fopen(path, "rb");
    if (!af) {
        Log("forge: %s: cannot open a reference handle", base);
        ShForgeClose(&f);
        return NULL;
    }

    o = (ShForgeOverlay *)calloc(1, sizeof(*o));
    if (!o) { fclose(af); ShForgeClose(&f); return NULL; }
    snprintf(o->path, sizeof(o->path), "%s", path);

    for (i = 0; i < g_nmods; i++) {
        ForgeMod *m = &g_mods[i];
        const ShForgeEntry *e = NULL;
        int add = 0, start;
        FILE *fp;
        uint8_t *buf;

        if (m->ok != 1) continue;
        if (StrEqI(m->archive, base)) {
            e = ShForgeByIndex(&f, m->index);
            add = 1;
        } else if (g_applyAll && m->id) {
            e = ShForgeById(&f, m->id);
            add = 1;
        }
        if (!add || !e) continue;

        if ((uint64_t)m->len > e->room) {
            Log("forge: %s: %s does not fit entry %u, skipped", base, m->rel,
                e->index);
            continue;
        }

        fp = fopen(m->path, "rb");
        if (!fp) { Log("forge: %s: cannot open the payload", m->rel); continue; }
        buf = (uint8_t *)malloc(m->len ? m->len : 1);
        if (!buf) { fclose(fp); continue; }
        if (fread(buf, 1, m->len, fp) != m->len) {
            Log("forge: %s: short read", m->rel);
            free(buf); fclose(fp); continue;
        }
        fclose(fp);

        if (!StrEqI(m->archive, base))
            Log("copies: %s also applied to %s entry %u", m->rel, base, e->index);

        start = o->patchCount;
        if (!AddPatch(o, af, e->offset, m->len, buf)) {
            Log("forge: %s: %s: the archive could not be read back for the "
                "safety check, skipped", base, m->rel);
            RollbackPatches(o, start);
            continue;
        }

        /* A shorter payload needs both RawDataSize fields rewritten, or
         * the engine reads the old count of bytes. An equal one does not
         * change them. */
        if (m->len != e->length) {
            uint8_t *p1 = (uint8_t *)malloc(4);
            uint8_t *p2 = (uint8_t *)malloc(4);
            int ok1 = 0, ok2 = 0;
            if (p1) { memcpy(p1, &m->len, 4); ok1 = AddPatch(o, af, e->locSizeOff, 4, p1); }
            if (p2) { memcpy(p2, &m->len, 4); ok2 = AddPatch(o, af, e->infoSizeOff, 4, p2); }
            if (!ok1 || !ok2) {
                Log("forge: %s: %s: the size fields could not be prepared, "
                    "skipped", base, m->rel);
                RollbackPatches(o, start);
                continue;
            }
        }
        chosen++;
    }

    fclose(af);
    ShForgeClose(&f);

    if (chosen == 0) {
        FreeOverlay(o);
        return NULL;
    }

    o->modCount = chosen;
    Log("forge: %s: %d entr%s patched", base, chosen,
        chosen == 1 ? "y" : "ies");
    return o;
}

ShForgeOverlay *ShForgeOverlayFor(const char *archivePath) {
    char base[128];
    ShForgeOverlay *o;
    int i;

    if (!g_enabled || g_dryRun || !archivePath) return NULL;

    EnterCriticalSection(&g_lock);
    for (i = 0; i < g_novl; i++)
        if (StrEqI(g_ovl[i]->path, archivePath)) {
            o = g_ovl[i];
            LeaveCriticalSection(&g_lock);
            return o;
        }
    LeaveCriticalSection(&g_lock);

    BaseFromArchivePath(archivePath, base, sizeof(base));
    o = OverlayBuild(archivePath, base);
    if (!o) return NULL;

    EnterCriticalSection(&g_lock);
    if (g_novl < OVL_MAX) g_ovl[g_novl++] = o;
    LeaveCriticalSection(&g_lock);
    return o;
}

void ShForgeOverlayFixup(ShForgeOverlay *o, uint64_t off, uint8_t *buf,
                         size_t len) {
    int i;
    if (!o || !buf || !len) return;
    for (i = 0; i < o->patchCount; i++) {
        ShForgePatch *p = &o->patches[i];
        uint64_t pEnd = p->off + p->len;
        uint64_t rEnd = off + len;
        uint64_t s = p->off > off ? p->off : off;
        uint64_t e = pEnd < rEnd ? pEnd : rEnd;
        size_t n;

        if (s >= e) continue;
        n = (size_t)(e - s);

        /* Never write blind. If what the read produced at the target is
         * not what the archive holds there, this read did not land where
         * it was believed to - and writing would corrupt live data, so
         * the patch is refused instead. */
        if (p->orig &&
            memcmp(buf + (size_t)(s - off), p->orig + (size_t)(s - p->off),
                   n) != 0) {
            if (o->rejected++ < 8)
                Log("fixup: refused a patch at %llu (a read at off=%llu "
                    "len=%zu did not match the archive)",
                    (unsigned long long)p->off, (unsigned long long)off, len);
            continue;
        }
        memcpy(buf + (size_t)(s - off), p->bytes + (size_t)(s - p->off), n);
    }
}

/* ---- status and menu ------------------------------------------------ */

const char *ShForgeStatusLine(void) { return g_status; }

int ShForgeEnabled(void) { return g_enabled && !g_dryRun; }
int ShForgeDryRun(void)  { return g_dryRun; }
int ShForgeLogReads(void) { return g_logReads; }

static void OnEnabled(uint32_t menu, uint32_t item, int v, void *u) {
    (void)item; (void)u;
    ShConfigSetBool("forgemod", "enabled", v);
    ShMenuStatus(menu, "Saved. Restart to apply.");
}

static void OnDryRun(uint32_t menu, uint32_t item, int v, void *u) {
    (void)item; (void)u;
    ShConfigSetBool("forgemod", "dry_run", v);
    ShMenuStatus(menu, "Saved. Restart to apply.");
}

void ShForgeMenuRegister(void) {
    uint32_t m;
    int applied = 0, bad = 0, i;

    if (!g_started) return;
    m = ShMenuCreate("Forge Mod Loader");
    if (!m) return;

    for (i = 0; i < g_nmods; i++) {
        if (g_mods[i].ok == 1) applied++;
        else if (g_mods[i].ok < 0) bad++;
    }

    ShMenuToggle(m, "Enabled", g_enabled, OnEnabled, NULL);
    ShMenuToggle(m, "Dry run", g_dryRun, OnDryRun, NULL);
    ShMenuHint(m,
        "Loads loose files from mods\\ over existing .forge entries without "
        "touching the archives. Lay them out as mods\\<archive>\\<file> or "
        "mods\\<mod name>\\<archive>\\<file>; '~' disables a mod folder, and "
        "'<n>_-_<name>.data' names the entry. Changes need a restart.");

    /* StatusF so the line is translated as this menu's own scope first:
     * the template and its translation keep the same placeholders. */
    if (!g_enabled)
        ShMenuStatusF(m, "Off ([forgemod] enabled=0).");
    else if (g_nmods == 0)
        ShMenuStatusF(m, "On, but mods\\ has no mod files.");
    else
        ShMenuStatusF(m, "%d mod(s): %d applied, %d rejected or overridden.",
                      g_nmods, applied, bad);
}

/* ---- startup -------------------------------------------------------- */

void ShForgeStartup(void) {
    if (g_started) return;
    g_started = 1;

    LogInit("scripthook_forge.log");
    InitializeCriticalSection(&g_lock);

    g_enabled      = ShConfigGetBool("forgemod", "enabled", 0);
    g_dryRun       = ShConfigGetBool("forgemod", "dry_run", 0);
    g_strict       = ShConfigGetBool("forgemod", "strict", 1);
    g_reportCopies = ShConfigGetBool("forgemod", "report_copies", 1);
    g_applyAll     = ShConfigGetBool("forgemod", "apply_all_copies", 0);
    g_logReads     = ShConfigGetBool("forgemod", "log_reads", 0);

    g_ovl = (ShForgeOverlay **)calloc(OVL_MAX, sizeof(void *));

    Log("forge mod loader: enabled=%d dry_run=%d strict=%d report_copies=%d "
        "apply_all_copies=%d", g_enabled, g_dryRun, g_strict, g_reportCopies,
        g_applyAll);

    ScanArchives();
    Log("find: %d archive(s)", g_narch);

    if (!g_enabled) {
        snprintf(g_status, sizeof(g_status),
                 "Forge Mod Loader: off ([forgemod] enabled=0)");
        /* The page is registered even when the feature is off: it is the
         * only place the switch lives, so hiding it would make the
         * feature unreachable. */
        ShForgeMenuRegister();
        return;
    }

    ScanMods();
    if (g_nmods > 0) {
        int applied = 0, bad = 0, i;
        ResolveMods();
        for (i = 0; i < g_nmods; i++) {
            if (g_mods[i].ok == 1) applied++;
            else if (g_mods[i].ok < 0) bad++;
        }
        snprintf(g_status, sizeof(g_status),
                 "Forge Mod Loader: %d mod(s), %d applied, %d rejected",
                 g_nmods, applied, bad);
        Log("mods: %s", g_status);
        /* Nothing to serve means no hooks at all: an install with no
         * mods costs the game zero. */
        if (applied > 0 && !g_dryRun) ShForgeIoStartup();
        if (g_reportCopies || g_applyAll)
            CreateThread(NULL, 0, CopyIndexThread, NULL, 0, NULL);
    } else {
        snprintf(g_status, sizeof(g_status),
                 "Forge Mod Loader: on, mods\\ has no files");
    }

    ShForgeMenuRegister();
}
