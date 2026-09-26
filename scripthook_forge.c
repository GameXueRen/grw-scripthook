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
    int      moved;            /* 1 = larger than its room: it takes the
                                * room of the entries that follow it     */
    int      isAdd;            /* 1 = a file named +something.data: the
                                * entry the archive has never held        */
    uint32_t ext;              /* an addition: class hash of its root     */
    char     addName[SH_FORGE_NAME_MAX]; /* an addition: its entry name   */
} ForgeMod;

static int g_enabled, g_dryRun, g_strict, g_reportCopies, g_applyAll;
static int g_logReads;
/* 1 = keep the read ledger: which .forge archives this session has
 * actually read. It rides scripthook_forge_io.c, so the I/O layer is
 * installed for it even with nothing to serve - that is the whole cost
 * of being able to answer "which mode loaded this". */
static int g_ledger;
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

/* ---- reading a complete .data container ------------------------------ */

/* A container is two LZO streams: an index, which lists the id and the size
 * of every resource, and a payload, which holds them one after another. Each
 * stream is a magic, a seven byte header, a block table and then the blocks,
 * every block prefixed with a checksum over the bytes stored. The loader needs
 * one id, one class hash and one name out of the front of all that, which is
 * what the four functions below do - the same thing DataFile.Read does in the
 * toolkit, with the parts the loader has no use for left out.
 *
 * The decompressor is the toolkit's, ported line for line: a state machine
 * over the token stream rather than the reference implementation, because
 * that is the description of the format this loader can be held to. Every
 * read here is bounded, and the checksum decides whether the output is
 * believed - so a mistake in any of it refuses a container rather than adds
 * a wrong one. */

static uint16_t RdU16at(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint64_t RdU64at(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static uint32_t BlobChecksum(const uint8_t *p, size_t n) {
    uint32_t a = 0, b = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

enum { LzoToken, LzoShortMatch, LzoMatch, LzoTrailing };

static int LzoDecompress(const uint8_t *in, size_t inLen, uint8_t *out,
                         size_t outLen) {
    size_t ip = 0, op = 0, count;
    int token = 0, step = LzoToken;

    if (inLen < 1 || outLen < 1) return 0;

    if (in[ip] > 17) {
        count = (size_t)in[ip++] - 17;
        if (count > inLen - ip || count > outLen - op) return 0;
        memcpy(out + op, in + ip, count);
        ip += count;
        op += count;
        if (count < 4) {
            if (ip >= inLen) return 0;
            token = in[ip++];
            step = LzoMatch;
        } else {
            step = LzoShortMatch;
        }
    }

    for (;;) {
        if (step == LzoToken || step == LzoShortMatch) {
            if (ip >= inLen) return 0;
            token = in[ip++];
            if (token >= 16) { step = LzoMatch; continue; }

            if (step == LzoShortMatch) {
                size_t distance, i, from;
                if (ip >= inLen) return 0;
                distance = 0x0801 + (size_t)(token >> 2) + ((size_t)in[ip++] << 2);
                if (distance > op || op + 3 > outLen) return 0;
                from = op - distance;
                for (i = 0; i < 3; i++) out[op + i] = out[from + i];
                op += 3;
                step = LzoTrailing;
                continue;
            }

            if (token == 0) {
                size_t extra = 0;
                while (ip < inLen && in[ip] == 0) { extra += 255; ip++; }
                if (ip >= inLen) return 0;
                count = 15 + extra + in[ip++];
            } else {
                count = (size_t)token;
            }
            count += 3;
            if (count > inLen - ip || count > outLen - op) return 0;
            memcpy(out + op, in + ip, count);
            ip += count;
            op += count;
            step = LzoShortMatch;
            continue;
        }

        if (step == LzoMatch) {
            size_t distance, length;
            size_t i, from;

            if (token >= 64) {
                if (ip >= inLen) return 0;
                distance = 1 + (size_t)((token >> 2) & 7) + ((size_t)in[ip++] << 3);
                length = (size_t)(token >> 5) + 1;
            } else if (token >= 32) {
                length = (size_t)(token & 31);
                if (length == 0) {
                    size_t extra = 0;
                    while (ip < inLen && in[ip] == 0) { extra += 255; ip++; }
                    if (ip >= inLen) return 0;
                    length = 31 + extra + in[ip++];
                }
                length += 2;
                if (inLen - ip < 2) return 0;
                distance = 1 + (size_t)(RdU16at(in + ip) >> 2);
                ip += 2;
            } else if (token >= 16) {
                distance = (size_t)(token & 8) << 11;
                length = (size_t)(token & 7);
                if (length == 0) {
                    size_t extra = 0;
                    while (ip < inLen && in[ip] == 0) { extra += 255; ip++; }
                    if (ip >= inLen) return 0;
                    length = 7 + extra + in[ip++];
                }
                length += 2;
                if (inLen - ip < 2) return 0;
                distance += (size_t)(RdU16at(in + ip) >> 2);
                ip += 2;
                if (distance == 0) return op == outLen;   /* the stream ends */
                distance += 0x4000;
            } else {
                if (ip >= inLen) return 0;
                distance = 1 + (size_t)(token >> 2) + ((size_t)in[ip++] << 2);
                length = 2;
            }

            if (distance == 0 || distance > op || length > outLen - op) return 0;
            from = op - distance;
            for (i = 0; i < length; i++) out[op + i] = out[from + i];
            op += length;
            step = LzoTrailing;
            continue;
        }

        /* LzoTrailing: the two low bits of the byte before the token are the
         * literals that follow the match that just ran. */
        if (ip < 2) return 0;
        count = (size_t)(in[ip - 2] & 3);
        if (count == 0) { step = LzoToken; continue; }
        if (count > inLen - ip || count > outLen - op) return 0;
        memcpy(out + op, in + ip, count);
        ip += count;
        op += count;
        if (ip >= inLen) return 0;
        token = in[ip++];
        step = LzoMatch;
    }
}

/* One stream, decompressed as far as wanted: the blocks are read until the
 * output holds that much, since every block has to be decompressed whole. */
static int BlobRead(const uint8_t *base, size_t len, size_t *off, size_t want,
                    uint8_t **out, size_t *outLen) {
    size_t ip = *off, total = 0, op = 0, i;
    int version, blockCount, wide, ok = 0;
    size_t *unsized = NULL, *compressed = NULL;
    uint8_t *buf = NULL;

    if (len - ip < 19) return 0;
    if (RdU64at(base + ip) != 0x1004FA9957FBAA33ULL) return 0;
    ip += 8;
    version = (int)(int16_t)RdU16at(base + ip);
    ip += 7;                                  /* algorithm and block sizes */
    memcpy(&blockCount, base + ip, 4);
    ip += 4;
    if (blockCount <= 0 || blockCount > 65536) return 0;
    wide = version >= 2;

    unsized = (size_t *)malloc(sizeof(size_t) * (size_t)blockCount);
    compressed = (size_t *)malloc(sizeof(size_t) * (size_t)blockCount);
    if (!unsized || !compressed) goto done;

    for (i = 0; i < (size_t)blockCount; i++) {
        if (wide) {
            int32_t u, c;
            if (len - ip < 8) goto done;
            memcpy(&u, base + ip, 4);
            memcpy(&c, base + ip + 4, 4);
            ip += 8;
            if (u <= 0 || c < 0 || c > u) goto done;
            unsized[i] = (size_t)u;
            compressed[i] = (size_t)c;
        } else {
            if (len - ip < 4) goto done;
            unsized[i] = RdU16at(base + ip);
            compressed[i] = RdU16at(base + ip + 2);
            ip += 4;
            if (unsized[i] == 0 || compressed[i] > unsized[i]) goto done;
        }
        if (total < want) total += unsized[i];
    }
    if (total == 0 || total > (64u << 20)) goto done;

    buf = (uint8_t *)malloc(total);
    if (!buf) goto done;

    for (i = 0; i < (size_t)blockCount && op < total; i++) {
        uint32_t expected;
        if (len - ip < 4 || len - ip - 4 < compressed[i]) goto done;
        memcpy(&expected, base + ip, 4);
        ip += 4;
        if (BlobChecksum(base + ip, compressed[i]) != expected) goto done;
        if (compressed[i] == unsized[i]) {
            memcpy(buf + op, base + ip, unsized[i]);
        } else if (!LzoDecompress(base + ip, compressed[i], buf + op, unsized[i])) {
            goto done;
        }
        ip += compressed[i];
        op += unsized[i];
    }
    if (op < total) goto done;

    *out = buf;
    *outLen = total;
    *off = ip;
    buf = NULL;
    ok = 1;

done:
    free(unsized);
    free(compressed);
    free(buf);
    return ok;
}

/* The root resource of a container: its id is the first index entry, and its
 * class hash, the length of its data and its name are the first bytes of its
 * payload. Nothing past the first payload block is read, so a container of
 * any size costs about one block. */
#define SH_CONTAINER_HEAD (2u << 20)

static int ContainerRoot(const char *path, uint64_t *id, uint32_t *classHash,
                         char *nameOut, int nameLen) {
    uint8_t *head = NULL, *index = NULL, *payload = NULL;
    size_t got, off = 0, indexLen = 0, payloadLen = 0;
    FILE *fp;
    int ok = 0;

    fp = fopen(path, "rb");
    if (!fp) return 0;
    head = (uint8_t *)malloc(SH_CONTAINER_HEAD);
    if (!head) { fclose(fp); return 0; }
    got = fread(head, 1, SH_CONTAINER_HEAD, fp);
    fclose(fp);
    if (got < 19) goto done;

    if (!BlobRead(head, got, &off, 64u * 1024u, &index, &indexLen)) goto done;
    if (!BlobRead(head, got, &off, 4096u, &payload, &payloadLen)) goto done;

    if (indexLen < 16) goto done;
    {
        uint16_t count;
        memcpy(&count, index, 2);
        if (count == 0) goto done;
        *id = RdU64at(index + 2);
        if (*id == 0 || *id > 0x0000FFFFFFFFFFFFULL) goto done;
    }
    if (payloadLen < 13) goto done;
    memcpy(classHash, payload, 4);
    {
        int32_t nameBytes;
        memcpy(&nameBytes, payload + 8, 4);
        if (*classHash == 0) goto done;
        if (nameBytes <= 0 || nameBytes >= nameLen) goto done;
        if ((size_t)nameBytes + 12 > payloadLen) goto done;
        memcpy(nameOut, payload + 12, (size_t)nameBytes);
        nameOut[nameBytes] = 0;
    }
    ok = 1;                                   /* the toolkit's own sanity, kept:
                                               * an ordinary id, a class, a name */

done:
    free(head);
    free(index);
    free(payload);
    return ok;
}

/* A file named +something.data adds a container the archive has never held.
 * Its own bytes name it: the root resource inside carries the id and the
 * class the new entry is filed under, so nothing here has to invent either.
 * The name after the marker is the entry name, and the folder names the
 * archive to add into. A file whose id the archive already holds is a
 * replacement that was named the other way, and is said so rather than
 * appended twice. */
static int ReadAddition(ForgeMod *m, const char *file, const ShForge *f) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    const char *p = file + 1;
    size_t len = strlen(p);
    char rootName[SH_FORGE_NAME_MAX];
    uint64_t id = 0;
    uint32_t classHash = 0;

    if (len < 6 || !StrEqI(p + len - 5, ".data")) {
        Log("mods: %s: a file that adds an entry has to be named +<entry "
            "name>.data, since the payload is a whole forge entry", m->rel);
        return 0;
    }
    if (len >= SH_FORGE_NAME_MAX) {
        Log("mods: %s: an entry name may not be longer than %d bytes", m->rel,
            SH_FORGE_NAME_MAX - 1);
        return 0;
    }
    /* The file is named after the entry it adds, with ".data" because that is
     * what the payload is. The entry name itself is stored without it: an
     * archive's own records hold the bare name, and the ".data" the toolkit
     * shows is derived from the id. */
    memcpy(m->addName, p, len - 5);
    m->addName[len - 5] = 0;

    if (!GetFileAttributesExA(m->path, GetFileExInfoStandard, &fad)) {
        Log("mods: %s: cannot stat the file", m->rel);
        return 0;
    }
    m->len = (uint32_t)fad.nFileSizeLow;
    if (m->len < 19) {
        Log("mods: %s: %u bytes is too short to be a container", m->rel, m->len);
        return 0;
    }

    if (!ContainerRoot(m->path, &id, &classHash, rootName, sizeof(rootName))) {
        Log("mods: %s: this could not be read as a complete .data container - "
            "its two blobs are LZO, every block carries a checksum, and a file "
            "that fails either is refused here rather than added", m->rel);
        return 0;
    }
    m->id = id;
    m->ext = classHash;
    if (ShForgeById(f, m->id)) {
        Log("mods: %s: %s already holds entry id 0x%llX, so this is a "
            "replacement and not an addition - name it <index>_-_%s.data "
            "instead", m->rel, m->archive, (unsigned long long)m->id,
            m->addName);
        return 0;
    }
    if (!StrEqI(m->addName, rootName))
        Log("mods: %s: the container calls its root resource '%s'", m->rel,
            rootName);

    m->isAdd = 1;
    Log("mods: %s -> %s: adds entry '%s' id=0x%llX class=0x%08X (%u bytes)",
        m->rel, m->archive, m->addName, (unsigned long long)m->id, m->ext,
        m->len);
    return 1;
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

        /* A file whose name starts with '+' adds a container this archive has
         * never held. Its own bytes name it: a complete .data starts with the
         * id and the class of its root resource, and those are what the new
         * entry is filed under. There is no entry to look up and no room to
         * fit into, so the overlay takes both from the end of the file. */
        if (file[0] == '+') {
            if (ReadAddition(m, file, &f)) m->ok = 1;
            ShForgeClose(&f);
            continue;
        }

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
        m->moved = 0;

        if ((uint64_t)m->len > e->room) {
            if (g_strict) {
                Log("mods: %s: %u bytes does not fit %s entry %u ('%s', %llu "
                    "bytes of room) - strict=1 refuses it; strict=0 lets it "
                    "take the room of the entries that follow", m->rel, m->len,
                    m->archive, e->index, e->name, (unsigned long long)e->room);
                m->ok = -2;
                ShForgeClose(&f);
                continue;
            }
            m->moved = 1;
            Log("mods: %s: %u bytes does not fit %s entry %u ('%s', %llu bytes "
                "of room) - it takes the room of the entries that follow it",
                m->rel, m->len, m->archive, e->index, e->name,
                (unsigned long long)e->room);
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
        /* An addition has no entry index to claim, so it cannot be the loser
         * of one: two of them in one archive are judged by entry id, where
         * the overlay builds them. */
        if (g_mods[i].isAdd) continue;
        for (j = 0; j < i; j++) {
            if (g_mods[j].ok != 1 || g_mods[j].isAdd) continue;
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
    /* This opens every archive on disk to index FileDataIDs. Those reads
     * are the loader's, not the engine's: letting them reach the read
     * ledger fills it with all 23 archives in every mode, which is
     * exactly what makes it useless as mode evidence. */
    ShForgeIoOwn(1);
    CopyIndexBuild();
    if (g_reportCopies) CopyReport();
    ShForgeIoOwn(0);
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

/* Is this entry the target of a mod of its own? A payload that takes room
 * from the entries after it cannot do that if one of them is being replaced
 * too: the put-back would cover that mod. A folder that lost to a higher
 * priority one is not served, so it does not stand in the way. */
static int IsModdedEntry(const char *base, uint32_t index) {
    int i;
    for (i = 0; i < g_nmods; i++) {
        if (g_mods[i].ok != 1) continue;
        if (!StrEqI(g_mods[i].archive, base)) continue;
        if (g_mods[i].index == index) return 1;
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

/* A replacement whose length differs has to patch both places RawDataSize is
 * written, or the engine reads the old count of bytes. An equal one changes
 * neither. */
static int AddSizePatches(ShForgeOverlay *o, FILE *af, const ShForgeEntry *e,
                          uint32_t len) {
    uint8_t *p1, *p2;
    int ok1 = 0, ok2 = 0;

    if (len == e->length) return 1;
    p1 = (uint8_t *)malloc(4);
    p2 = (uint8_t *)malloc(4);
    if (p1) { memcpy(p1, &len, 4); ok1 = AddPatch(o, af, e->locSizeOff, 4, p1); }
    if (p2) { memcpy(p2, &len, 4); ok2 = AddPatch(o, af, e->infoSizeOff, 4, p2); }
    return ok1 && ok2;
}

/* One entry that a larger payload covers. */
typedef struct { const ShForgeEntry *e; uint32_t covered; } ShForgeDisplaced;

#define SH_FORGE_DISPLACE_MAX 32

/* Serve `len` bytes at `off`, whether or not the archive has room for them
 * there. Bytes larger than the room an entry has are possible only because
 * they are served, not written: whatever payloads the range covers are read
 * first and spliced back into it at the offsets they came from - one range,
 * so a read that spans them (the engine streams, it does not read one entry
 * at a time) ends up with the right bytes everywhere.
 *
 * Two overlapping patches would not do: the second would be judged against
 * the output of the first one rather than against what the read produced,
 * and a put-back would look like a mismatch.
 *
 * Nothing is invented past the end of the archive, and no read is answered
 * by the loader itself: every offset the engine asks for is an offset the
 * file really holds, which is why an asynchronous read keeps completing
 * exactly the way it did before.
 *
 * `desc` is what the log calls the thing being served - a path under mods\
 * for a payload, or a sentence for a table. Takes ownership of `bytes` on
 * success (AddPatch does) and frees it on failure, so the caller only has to
 * roll back the patches it added. */
static int PlaceServed(ShForgeOverlay *o, FILE *af, const ShForge *f,
                       const char *base, uint64_t off, uint32_t len,
                       uint8_t *bytes, const char *desc) {
    ShForgeDisplaced d[SH_FORGE_DISPLACE_MAX];
    uint64_t end = off + (uint64_t)len;
    int n = 0, i;

    if (end > f->fileSize) {
        Log("forge: %s: %s: %u bytes at 0x%llX would reach 0x%llX and %s is "
            "0x%llX bytes long - that is past the end of the archive, where "
            "the loader would have to invent bytes; skipped", base, desc, len,
            (unsigned long long)off, (unsigned long long)end, base,
            (unsigned long long)f->fileSize);
        free(bytes);
        return 0;
    }

    /* What it covers, before anything is added: a modded entry among them
     * would be put back over its own replacement, and a check that fails
     * here leaves nothing to undo but what the caller rolls back. */
    for (i = 0; i < f->entryCount; i++) {
        const ShForgeEntry *o2 = &f->entries[i];
        uint64_t cov;

        if (o2->offset <= off || o2->offset >= end) continue;
        if (n == SH_FORGE_DISPLACE_MAX) {
            Log("forge: %s: %s: would cover more than %d entries - that is not "
                "a replacement any more; skipped", base, desc,
                SH_FORGE_DISPLACE_MAX);
            free(bytes);
            return 0;
        }
        if (IsModdedEntry(base, o2->index)) {
            Log("forge: %s: %s: it would cover entry %u, which a mod of its "
                "own is replacing; skipped", base, desc, o2->index);
            free(bytes);
            return 0;
        }
        cov = (uint64_t)o2->length < end - o2->offset ? o2->length
                                                     : end - o2->offset;
        d[n].e = o2;
        d[n].covered = (uint32_t)cov;
        n++;
    }

    /* The covered bytes go back inside this patch, spliced in at the offsets
     * they came from. */
    for (i = 0; i < n; i++) {
        uint8_t *orig = ReadOrig(af, d[i].e->offset, d[i].covered);
        size_t at = (size_t)(d[i].e->offset - off);

        if (!orig) {
            Log("forge: %s: %s: entry %u could not be read back for its "
                "put-back", base, desc, d[i].e->index);
            free(bytes);
            return 0;
        }
        if (at + d[i].covered > len) {
            /* The coverage scan says otherwise, so this cannot happen; the
             * check is here because a wrong put-back writes into live data. */
            free(orig);
            free(bytes);
            return 0;
        }
        memcpy(bytes + at, orig, d[i].covered);
        free(orig);

        if (d[i].covered == d[i].e->length)
            Log("forge: %s: entry %u ('%s') is covered by %s and is served "
                "from its own bytes", base, d[i].e->index, d[i].e->name, desc);
        else
            Log("forge: %s: entry %u ('%s') is covered for its first %u of %u "
                "bytes; what is left of it still comes from the archive",
                base, d[i].e->index, d[i].e->name, d[i].covered,
                d[i].e->length);
    }

    if (!AddPatch(o, af, off, len, bytes))
        return 0;
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

/* ---- adding an entry ------------------------------------------------ */

static uint64_t Fnv64(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ULL; }
    return h;
}

static uint32_t RdU32at(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* An info record carries a umac, which the toolkit hashes from the identity
 * the archive was written with and the entry id. The loader writes no
 * archives, so it seeds from the id and the name instead, and retries with a
 * suffix rather than repeat one the archive already holds. */
static int UmacForEntry(const ShForge *f, uint64_t id, const char *name,
                        uint64_t *out) {
    char seed[192];
    int attempt, i;

    for (attempt = 0; attempt < 64; attempt++) {
        uint64_t candidate;
        int taken = 0;

        if (attempt == 0)
            snprintf(seed, sizeof(seed), "entry:%llX:%s",
                     (unsigned long long)id, name);
        else
            snprintf(seed, sizeof(seed), "entry:%llX:%s:%d",
                     (unsigned long long)id, name, attempt);
        candidate = Fnv64(seed);
        if (candidate == 0) continue;
        for (i = 0; i < f->entryCount; i++)
            if (f->entries[i].umac == candidate) { taken = 1; break; }
        if (!taken) { *out = candidate; return 1; }
    }
    return 0;
}

/* Patch one fixed size field in place. It goes through AddPatch like every
 * other patch, so the archive's own bytes there are read back and compared
 * before anything is written. */
static int PatchField(ShForgeOverlay *o, FILE *af, uint64_t off,
                      const void *value, int size) {
    uint8_t *p = (uint8_t *)malloc((size_t)size);
    if (!p) return 0;
    memcpy(p, value, (size_t)size);
    return AddPatch(o, af, off, (uint32_t)size, p);
}

/* Add one entry the archive has never held.
 *
 * The engine finds entries through the archive's tables, so a new entry means
 * a table with one more row - and a table cannot simply grow, because the
 * location table sits right after its file set header and the info table
 * after that. What can move is where the header says they are: both are
 * plain pointers. So a copy of the tables with the new records on the end is
 * placed in room borrowed from the payloads at the end of the file - the same
 * way a payload larger than its room is placed - and the pointers, the counts
 * and the two places the list ends are patched in place.
 *
 * Nothing is written to the archive and nothing is invented past the end of
 * it: every byte the engine reads is either the file's own or a patch, which
 * is what keeps the rest of the safety story unchanged. */
static int AddNewEntry(ShForgeOverlay *o, FILE *af, const ShForge *f,
                       const char *base, const ForgeMod *m) {
    const ShForgeSet *set;
    const ShForgeEntry *tail, *tmplEntry = NULL;
    uint8_t *locRow = NULL, *infoRow = NULL, *tmpl = NULL;
    uint8_t *payload = NULL, *lastIdx = NULL, *tailNext = NULL;
    uint64_t locRowOff = 0, infoRowOff = 0, payloadOff = 0, setsEnd, umac = 0;
    int newCount, i, ok = 0;
    FILE *fp;

    if (f->fileSets != 1) {
        Log("forge: %s: %s: this archive has %d file sets, and adding an entry "
            "is only verified for one; skipped", base, m->rel, f->fileSets);
        return 0;
    }
    set = &f->sets[0];
    if (set->count <= 0 || m->len == 0) {
        Log("forge: %s: %s: the file set holds nothing to append to; skipped",
            base, m->rel);
        return 0;
    }
    if (ShForgeById(f, m->id)) {
        Log("forge: %s: %s: entry id 0x%llX is already in this archive; skipped",
            base, m->rel, (unsigned long long)m->id);
        return 0;
    }

    /* An entry the archive never held has to be registered where the engine
     * looks entries up by name rather than by table row: the prefetch registry
     * and the global meta. The patch archive a mod of the game's own kind
     * ships proves it - one of its .forge files holds a single weapon and then
     * GlobalMetaFile and PrefetchingFileInfos, and nothing else - and it is
     * why an entry missing from them is not an entry that fails to load but
     * one the engine trips over before it loads anything at all.
     *
     * Everything below this point is exercised and correct: the container is
     * read, the payload is placed, the spare table rows are written. It waits
     * only on those two companions, which this loader does not rebuild yet, so
     * the addition is refused here instead of applied half way. */
    Log("forge: %s: %s: adding '%s' also has to register it in %s's "
        "PrefetchingFileInfos (entry 145) and its global meta (entry 16), and "
        "this loader does not rebuild those yet - refused rather than added "
        "half way. The patch archive a mod of its own kind ships carries both "
        "alongside the payloads; use that mechanism until this one can.",
        base, m->rel, m->addName, base);
    return 0;
    for (i = 0; i < g_nmods; i++) {
        if (&g_mods[i] == m) break;
        if (g_mods[i].ok == 1 && g_mods[i].isAdd && g_mods[i].id == m->id &&
            StrEqI(g_mods[i].archive, base)) {
            Log("forge: %s: %s adds entry 0x%llX that %s already adds; skipped",
                base, m->rel, (unsigned long long)m->id, g_mods[i].rel);
            return 0;
        }
    }
    /* The template the toolkit would pick: an entry of the same class, and not
     * one whose id makes it a meta rather than a .data. An entry's name is
     * stored without its extension, so the id is the only thing that says
     * which kind it is - the same way the toolkit derives the extension it
     * shows. */
    for (i = 0; i < f->entryCount; i++) {
        const ShForgeEntry *e = &f->entries[i];
        if (e->extension != m->ext) continue;
        if (e->id == 0 || e->id == 16 || e->id == 145 || e->id == 193 ||
            e->id == 4090) continue;
        tmplEntry = e;
        break;
    }
    if (!tmplEntry) {
        Log("forge: %s: %s: this archive holds no container of class 0x%08X to "
            "copy a record from; skipped", base, m->rel, m->ext);
        return 0;
    }
    if (!UmacForEntry(f, m->id, m->addName, &umac)) {
        Log("forge: %s: %s: no unused umac could be made; skipped", base, m->rel);
        return 0;
    }
    if (set->infoTable < set->locationTable + (uint64_t)set->count * 20) {
        Log("forge: %s: %s: this archive's tables are not where the loader "
            "expects them; skipped", base, m->rel);
        return 0;
    }

    /* An archive lays its tables out for one more row than it holds: the
     * meta's limit is that capacity, it is one past the count, and the end of
     * the info table is where the tables stop. That is what a patch archive
     * of the game's own kind relies on too, and it is measured here rather
     * than assumed, because everything below stands on it. */
    newCount = set->count + 1;
    if (f->limit != newCount ||
        set->locationTable + (uint64_t)f->limit * 20 != set->infoTable ||
        set->infoTable + (uint64_t)f->limit * 192 != set->infoEnd) {
        Log("forge: %s: %s: this archive is laid out for %d rows with %d used, "
            "not one spare row as the loader expects - it has no room to "
            "append; skipped", base, m->rel, f->limit, set->count);
        return 0;
    }
    locRowOff  = set->locationTable + (uint64_t)set->count * 20;
    infoRowOff = set->infoTable + (uint64_t)set->count * 192;

    /* The payload is the one thing that needs room, and it comes from the end
     * of the archive, on a page boundary so an unbuffered read of it lands
     * where the loader says it does. The size is asked of the file system,
     * not of ShForge: that one comes from a seek through the loader's own
     * hooks and can answer with a number the file does not have, and a
     * payload placed past the real end of an archive is a payload the engine
     * cannot read. */
    {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        uint64_t physical;

        if (!GetFileAttributesExA(f->path, GetFileExInfoStandard, &fad)) {
            Log("forge: %s: %s: cannot read the size of %s; skipped", base,
                m->rel, base);
            return 0;
        }
        physical = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        setsEnd = set->infoEnd + 176ULL * (uint64_t)(f->limit > 0 ? f->limit : 1);
        if (physical < (uint64_t)m->len + 0x1000) {
            Log("forge: %s: %s: %s is 0x%llX bytes, less than the %u byte "
                "payload; skipped", base, m->rel, base,
                (unsigned long long)physical, m->len);
            return 0;
        }
        payloadOff = (physical - m->len) & ~(uint64_t)0xFFF;
        if (payloadOff <= setsEnd || payloadOff + (uint64_t)m->len > physical) {
            Log("forge: %s: %s: the room between the tables (0x%llX) and the "
                "end of %s (0x%llX) is smaller than the %u byte payload; "
                "skipped", base, m->rel, (unsigned long long)setsEnd, base,
                (unsigned long long)physical, m->len);
            return 0;
        }
    }

    tail     = &f->entries[set->firstEntry + set->count - 1];
    tmpl     = ReadOrig(af, tmplEntry->infoOff, 192);
    lastIdx  = ReadOrig(af, set->infoEnd, 4);
    tailNext = ReadOrig(af, tail->infoOff + 28, 4);
    if (!tmpl || !lastIdx || !tailNext) {
        Log("forge: %s: %s: the tables could not be read back; skipped",
            base, m->rel);
        goto done;
    }
    /* Both ends of the list have to say what the loader believes they say, or
     * the row would be appended to something else. */
    if (RdU32at(tailNext) != 0xFFFFFFFFu) {
        Log("forge: %s: %s: the last info record does not end the list; skipped",
            base, m->rel);
        goto done;
    }
    if (RdU32at(lastIdx) != (uint32_t)(set->count - 1)) {
        Log("forge: %s: %s: the trailing set names entry %u as the last, not "
            "%u; skipped", base, m->rel, RdU32at(lastIdx), set->count - 1);
        goto done;
    }

    /* The payload, whole: a mod file is a forge entry exactly as an archive
     * stores one, which is what every other payload path already relies on. */
    fp = fopen(m->path, "rb");
    if (!fp) { Log("forge: %s: %s: cannot open the payload", base, m->rel); goto done; }
    payload = (uint8_t *)malloc(m->len);
    if (!payload) { fclose(fp); goto done; }
    if (fread(payload, 1, m->len, fp) != m->len) {
        Log("forge: %s: %s: short read", base, m->rel);
        fclose(fp);
        goto done;
    }
    fclose(fp);

    /* The two rows, written into the spare ones. The info row starts as a copy
     * of the template - the same class, so the same meaning for every field -
     * and only what describes this entry is changed. */
    locRow = (uint8_t *)calloc(20, 1);
    infoRow = (uint8_t *)calloc(192, 1);
    if (!locRow || !infoRow) {
        Log("forge: %s: %s: out of memory", base, m->rel);
        goto done;
    }
    memcpy(locRow, &payloadOff, 8);
    memcpy(locRow + 8, &m->id, 8);
    memcpy(locRow + 16, &m->len, 4);
    {
        int32_t next = -1, prev = set->count - 1;
        FILETIME ft;
        uint64_t stamp;
        uint32_t seconds;
        size_t nameLen = strlen(m->addName);

        memcpy(infoRow, tmpl, 192);
        memcpy(infoRow, &m->len, 4);
        memcpy(infoRow + 4, &umac, 8);
        memcpy(infoRow + 28, &next, 4);
        memcpy(infoRow + 32, &prev, 4);
        GetSystemTimeAsFileTime(&ft);
        stamp = (((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ULL;
        stamp = stamp > 11644473600ULL ? stamp - 11644473600ULL : 0;
        seconds = (uint32_t)stamp;
        memcpy(infoRow + 40, &seconds, 4);
        memset(infoRow + 44, 0, 128);
        memcpy(infoRow + 44, m->addName, nameLen < 128 ? nameLen : 127);
    }

    /* Serve the payload, patch the rows and the counts that describe them.
     * The set header's table pointers are not touched: the tables did not
     * move, they only grew into the room they already had. */
    if (!PlaceServed(o, af, f, base, payloadOff, m->len, payload, m->rel)) {
        payload = NULL;              /* PlaceServed freed it */
        goto done;
    }
    payload = NULL;                  /* the patch owns it now */
    if (!AddPatch(o, af, locRowOff, 20, locRow)) { locRow = NULL; goto done; }
    locRow = NULL;
    if (!AddPatch(o, af, infoRowOff, 192, infoRow)) { infoRow = NULL; goto done; }
    infoRow = NULL;

    {
        int32_t v;

        v = f->totalCount + 1;
        if (!PatchField(o, af, f->metaOff + 0, &v, 4)) goto done;
        v = newCount;
        if (!PatchField(o, af, set->headerOff + 0, &v, 4)) goto done;
        if (!PatchField(o, af, set->headerOff + 28, &v, 4)) goto done;
        v = set->count;                  /* the new record is the last of the list */
        if (!PatchField(o, af, tail->infoOff + 28, &v, 4)) goto done;
        v = newCount - 1;                /* what the trailing set names as last */
        if (!PatchField(o, af, set->infoEnd, &v, 4)) goto done;
    }

    Log("forge: %s: added entry '%s' id=0x%llX class=0x%08X (%u bytes) at "
        "0x%llX, into the spare rows at 0x%llX and 0x%llX - the set holds %d "
        "of its %d rows now", base, m->addName, (unsigned long long)m->id,
        m->ext, m->len, (unsigned long long)payloadOff,
        (unsigned long long)locRowOff, (unsigned long long)infoRowOff,
        newCount, f->limit);
    ok = 1;

done:
    free(locRow); free(infoRow); free(tmpl);
    free(lastIdx); free(tailNext); free(payload);
    return ok;
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

        /* A '+something.data' file adds a container rather than replacing
         * one: the room and the table row both come from the end of this
         * file, so it has its own path through the builder. */
        if (m->isAdd) {
            int begin = o->patchCount;
            if (StrEqI(m->archive, base)) {
                if (AddNewEntry(o, af, &f, base, m))
                    chosen++;
                else
                    RollbackPatches(o, begin);
            }
            continue;
        }

        if (StrEqI(m->archive, base)) {
            e = ShForgeByIndex(&f, m->index);
            add = 1;
        } else if (g_applyAll && m->id) {
            e = ShForgeById(&f, m->id);
            add = 1;
        }
        if (!add || !e) continue;

        /* A payload larger than its room is served too when strict=0: the
         * resolver marked it moved, and AddMovedPayload takes the room of
         * the entries that follow it. */
        if ((uint64_t)m->len > e->room && !m->moved) {
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
        /* PlaceServed owns `buf` from the moment it adds the patch; anything
         * it leaves behind on failure is freed by the rollback below, which
         * frees every patch added since `start`. */
        if (!PlaceServed(o, af, &f, base, e->offset, m->len, buf, m->rel) ||
            !AddSizePatches(o, af, e, m->len)) {
            Log("forge: %s: %s could not be placed, skipped", base, m->rel);
            RollbackPatches(o, start);
            continue;
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
    int i, dropped = 0;

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
    if (g_novl < OVL_MAX) {
        g_ovl[g_novl++] = o;
    } else {
        /* The table is full: the overlay would never be reachable again,
         * and it holds a whole archive's worth of patch bytes, so it is
         * released instead of dropped. The caller gets NULL and leaves the
         * archive's reads alone - unpatched beats patched with a leak. */
        FreeOverlay(o);
        o = NULL;
        dropped = 1;
    }
    LeaveCriticalSection(&g_lock);
    if (dropped)
        Log("forge: %s: all %d overlay slot(s) are in use - this archive "
            "is read unpatched", base, OVL_MAX);
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
    ShMenuStatus(menu, "@settings.saved");
}

static void OnDryRun(uint32_t menu, uint32_t item, int v, void *u) {
    (void)item; (void)u;
    ShConfigSetBool("forgemod", "dry_run", v);
    ShMenuStatus(menu, "@settings.saved");
}

/* The page's key: what [MenuOrder] is keyed by, both for the default
 * weight written at start up and for the mod menu's ordering page - and
 * what the capture translates the title by. An ID, like every other page
 * (the framework's table is in scripthook_text.c); it used to be the
 * English literal, which left this one title untranslated. */
#define SH_FORGE_PAGE "@forge.page"

void ShForgeMenuRegister(void) {
    uint32_t m;
    int applied = 0, bad = 0, i;

    if (!g_started) return;
    m = ShMenuCreate(SH_FORGE_PAGE);
    if (!m) return;

    for (i = 0; i < g_nmods; i++) {
        if (g_mods[i].ok == 1) applied++;
        else if (g_mods[i].ok < 0) bad++;
    }

    ShMenuToggle(m, "@forge.enabled", g_enabled, OnEnabled, NULL);
    ShMenuToggle(m, "@forge.dryrun", g_dryRun, OnDryRun, NULL);
    /* The hint travels as a key: as a literal it is the longest text in
     * the build, and it was the first thing the text layer cut. */
    ShMenuHint(m, "@forge.hint");

    /* StatusF so the line is translated as this menu's own scope first:
     * the template and its translation keep the same placeholders. */
    if (!g_enabled)
        ShMenuStatusF(m, "@forge.status.off");
    else if (g_nmods == 0)
        ShMenuStatusF(m, "@forge.status.nomods");
    else
        ShMenuStatusF(m, "@forge.status.mods", g_nmods, applied, bad);
}

/* ---- startup -------------------------------------------------------- */

void ShForgeStartup(void) {
    if (g_started) return;
    g_started = 1;

    LogInit("scripthook_forge.log");
    InitializeCriticalSection(&g_lock);

    g_enabled      = ShConfigGetBool("forgemod", "enabled", 0);
    g_dryRun       = ShConfigGetBool("forgemod", "dry_run", 0);
    /* On by default, which is the behaviour this loader has always had: a
     * payload that does not fit its room is refused. Setting it to 0 lets
     * such a payload take the room of the entries that follow it. */
    g_strict       = ShConfigGetBool("forgemod", "strict", 1);
    g_strict       = ShConfigGetBool("forgemod", "strict", 1);
    g_reportCopies = ShConfigGetBool("forgemod", "report_copies", 1);
    g_applyAll     = ShConfigGetBool("forgemod", "apply_all_copies", 0);
    g_logReads     = ShConfigGetBool("forgemod", "log_reads", 0);

    /* Default place in the root menu: right behind the settings page.
     * Written only when the key is missing, so a move made on the mod
     * menu's ordering page sticks instead of being undone here. */
    if (ShConfigGetInt("MenuOrder", SH_FORGE_PAGE, -1) < 0)
        ShConfigSetInt("MenuOrder", SH_FORGE_PAGE, 10);

    /* The read ledger defaults on: it is what makes "which mode is this"
     * answerable from the archives a session loaded. The evidence probe
     * hooks ReadFile itself, and MinHook keeps one hook per target, so
     * with probe=1 its own log is the list and this stands down. */
    g_ledger = ShConfigGetBool("forgemod", "ledger", 1);
    if (g_ledger && ShConfigGetBool("forgemod", "probe", 0)) {
        Log("ledger: [forgemod] probe=1 owns ReadFile; "
            "forge_probe.log carries the archive list instead");
        g_ledger = 0;
    }

    g_ovl = (ShForgeOverlay **)calloc(OVL_MAX, sizeof(void *));
    if (!g_ovl) {
        /* Every path that registers an overlay writes into this table, and
         * the only thing standing between it and a null write is the dry
         * run flag. */
        LogAlways("forge mod loader: no memory for %d overlay slots - dry run "
            "instead", OVL_MAX);
        g_dryRun = 1;
    }

    LogAlways("forge mod loader: enabled=%d dry_run=%d strict=%d report_copies=%d "
        "apply_all_copies=%d ledger=%d", g_enabled, g_dryRun, g_strict,
        g_reportCopies, g_applyAll, g_ledger);

    ScanArchives();
    LogAlways("find: %d archive(s)", g_narch);

    if (!g_enabled) {
        snprintf(g_status, sizeof(g_status),
                 "Forge Mod Loader: off ([forgemod] enabled=0)");
        /* The ledger is not a mod, so it is installed with the feature
         * off as well - that is the case a stock install runs in. */
        if (g_ledger) ShForgeIoStartup();
        /* The page is registered even when the feature is off: it is the
         * only place the switch lives, so hiding it would make the
         * feature unreachable. */
        ShForgeMenuRegister();
        return;
    }

    ScanMods();
    if (g_nmods > 0) {
        int applied = 0, bad = 0, i;
        /* Same as the index thread: these are the loader's own reads. */
        ShForgeIoOwn(1);
        ResolveMods();
        ShForgeIoOwn(0);
        for (i = 0; i < g_nmods; i++) {
            if (g_mods[i].ok == 1) applied++;
            else if (g_mods[i].ok < 0) bad++;
        }
        snprintf(g_status, sizeof(g_status),
                 "Forge Mod Loader: %d mod(s), %d applied, %d rejected",
                 g_nmods, applied, bad);
        LogAlways("mods: %s", g_status);
        /* The ledger needs the I/O layer even when nothing is served; a
         * dry run is handled inside it, by dropping the overlay rather
         * than the resolve. */
        if (applied > 0 || g_ledger) ShForgeIoStartup();
        if (g_reportCopies || g_applyAll) {
            HANDLE h = CreateThread(NULL, 0, CopyIndexThread, NULL, 0, NULL);

            if (h) CloseHandle(h);   /* never waited on */
        }
    } else {
        snprintf(g_status, sizeof(g_status),
                 "Forge Mod Loader: on, mods\\ has no files");
    }

    ShForgeMenuRegister();
}
