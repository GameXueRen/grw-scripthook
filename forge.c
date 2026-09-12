/* Reading a .forge container's tables. See forge.h for what this is for
 * and the format reference.
 *
 * Only the tables are read. In this container the header and both tables
 * sit below the first payload, so a 19.89 GB archive is opened for a few
 * megabytes of reads. The file is opened, read and closed here; nothing
 * is kept open, which matters because the mod loader may inspect every
 * archive at start up.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge.h"

#define FORGE_INFO_SIZE 192
#define FORGE_LOC_SIZE  20
#define FORGE_MAGIC     "scimitar"
#define FORGE_VERSION   27

static uint32_t RdU32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int32_t  RdI32(const uint8_t *p) { int32_t  v; memcpy(&v, p, 4); return v; }
static uint64_t RdU64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static int64_t  RdI64(const uint8_t *p) { int64_t  v; memcpy(&v, p, 8); return v; }

static int ReadAt(FILE *fp, unsigned long long off, void *buf, size_t len) {
    if (_fseeki64(fp, (long long)off, SEEK_SET) != 0) return 0;
    return fread(buf, 1, len, fp) == len;
}

static void Fail(ShForge *f) {
    free(f->entries);
    f->entries = NULL;
    f->entryCount = 0;
}

/* Sort helper for the room pass: entry indices ordered by payload
 * offset. Payloads are stored contiguously in index order, so this is
 * almost the identity, but the room figure has to be right even if a
 * build ever stores them otherwise. */
typedef struct { uint64_t off; int idx; } OffIdx;

static int CmpOff(const void *a, const void *b) {
    uint64_t x = ((const OffIdx *)a)->off, y = ((const OffIdx *)b)->off;
    return x < y ? -1 : (x > y ? 1 : 0);
}

int ShForgeOpen(const char *path, ShForge *f) {
    FILE *fp = NULL;
    uint8_t hdr[21], dh[44];
    unsigned long long size;
    int64_t dhOff, firstSet;
    int fileSets, total = 0, i, idx = 0;

    memset(f, 0, sizeof(*f));
    if (!path || !path[0]) return 0;
    snprintf(f->path, sizeof(f->path), "%s", path);

    fp = fopen(path, "rb");
    if (!fp) return 0;

    _fseeki64(fp, 0, SEEK_END);
    size = (unsigned long long)_ftelli64(fp);
    if (size < 64) goto bad;

    if (!ReadAt(fp, 0, hdr, sizeof(hdr))) goto bad;
    if (memcmp(hdr, FORGE_MAGIC, 8) != 0) goto bad;
    f->version = (int)RdU32(hdr + 9);
    if (f->version != FORGE_VERSION) goto bad;

    dhOff = RdI64(hdr + 13);
    if (dhOff <= 0 || (unsigned long long)dhOff + sizeof(dh) > size) goto bad;
    if (!ReadAt(fp, (unsigned long long)dhOff, dh, sizeof(dh))) goto bad;

    fileSets  = RdI32(dh + 32);
    firstSet  = RdI64(dh + 36);
    if (fileSets <= 0 || fileSets > 64) goto bad;

    /* Pass one: how many entries in total, following the section chain. */
    {
        int64_t cur = firstSet;
        for (i = 0; i < fileSets && cur >= 0; i++) {
            uint8_t sh[48];
            int n;
            if (cur <= 0 || (unsigned long long)cur + sizeof(sh) > size) goto bad;
            if (!ReadAt(fp, (unsigned long long)cur, sh, sizeof(sh))) goto bad;
            n = RdI32(sh + 0);
            if (n < 0 || n > 4000000) goto bad;
            total += n;
            if (total < 0 || total > 4000000) goto bad;
            cur = RdI64(sh + 16);
        }
    }

    f->entries = (ShForgeEntry *)calloc((size_t)total ? (size_t)total : 1,
                                        sizeof(ShForgeEntry));
    if (!f->entries) goto bad;

    /* Pass two: fill them in. */
    {
        int64_t cur = firstSet;
        for (i = 0; i < fileSets && cur >= 0; i++) {
            uint8_t sh[48];
            uint8_t *loc = NULL, *inf = NULL;
            uint64_t locationTable, infoTable;
            int n, j;

            if (cur <= 0 || (unsigned long long)cur + sizeof(sh) > size) goto bad;
            if (!ReadAt(fp, (unsigned long long)cur, sh, sizeof(sh))) goto bad;
            n = RdI32(sh + 0);
            if (n <= 0) { cur = RdI64(sh + 16); continue; }
            locationTable = cur + 48;           /* inline after the header */
            infoTable     = (uint64_t)RdI64(sh + 32);

            if (locationTable + (uint64_t)n * FORGE_LOC_SIZE > size) goto bad;
            if (infoTable + (uint64_t)n * FORGE_INFO_SIZE > size) goto bad;

            loc = (uint8_t *)malloc((size_t)n * FORGE_LOC_SIZE);
            inf = (uint8_t *)malloc((size_t)n * FORGE_INFO_SIZE);
            if (!loc || !inf) { free(loc); free(inf); goto bad; }
            if (!ReadAt(fp, locationTable, loc, (size_t)n * FORGE_LOC_SIZE) ||
                !ReadAt(fp, infoTable, inf, (size_t)n * FORGE_INFO_SIZE)) {
                free(loc); free(inf); goto bad;
            }

            for (j = 0; j < n; j++) {
                ShForgeEntry *e = &f->entries[idx];
                const uint8_t *lp = loc + (size_t)j * FORGE_LOC_SIZE;
                const uint8_t *ip = inf + (size_t)j * FORGE_INFO_SIZE;
                const uint8_t *nm = ip + 44;
                int k = 0;

                e->index      = (uint32_t)idx;
                e->offset     = RdU64(lp + 0);
                e->id         = RdU64(lp + 8);
                e->length     = RdU32(lp + 16);
                e->locSizeOff = locationTable + (uint64_t)j * FORGE_LOC_SIZE + 16;
                e->infoSizeOff = infoTable + (uint64_t)j * FORGE_INFO_SIZE;

                while (k < SH_FORGE_NAME_MAX - 1 && nm[k]) {
                    char c = (char)nm[k];
                    e->name[k] = (c >= 32 && c != '"') ? c : '_';
                    k++;
                }
                e->name[k] = 0;
                e->nameLen = (uint32_t)k;
                idx++;
            }

            free(loc);
            free(inf);
            cur = RdI64(sh + 16);
        }
    }

    if (idx != total) goto bad;
    f->entryCount = total;
    f->fileSize = size;

    /* Room per entry: up to the next payload's start, or the file end. */
    if (total > 0) {
        OffIdx *ord = (OffIdx *)malloc((size_t)total * sizeof(OffIdx));
        if (!ord) goto bad;
        for (i = 0; i < total; i++) {
            ord[i].off = f->entries[i].offset;
            ord[i].idx = i;
        }
        qsort(ord, (size_t)total, sizeof(OffIdx), CmpOff);
        for (i = 0; i < total; i++) {
            ShForgeEntry *e = &f->entries[ord[i].idx];
            uint64_t next = (i + 1 < total) ? ord[i + 1].off : size;
            e->room = (next > e->offset) ? next - e->offset : 0;
        }
        free(ord);
    }

    fclose(fp);
    return 1;

bad:
    if (fp) fclose(fp);
    Fail(f);
    return 0;
}

void ShForgeClose(ShForge *f) {
    if (!f) return;
    free(f->entries);
    f->entries = NULL;
    f->entryCount = 0;
}

const ShForgeEntry *ShForgeByIndex(const ShForge *f, uint32_t index) {
    if (!f || !f->entries || index >= (uint32_t)f->entryCount) return NULL;
    return &f->entries[index];
}

const ShForgeEntry *ShForgeById(const ShForge *f, uint64_t id) {
    int i;
    if (!f || !f->entries) return NULL;
    for (i = 0; i < f->entryCount; i++)
        if (f->entries[i].id == id) return &f->entries[i];
    return NULL;
}

const ShForgeEntry *ShForgeByName(const ShForge *f, const char *name) {
    int i;
    if (!f || !f->entries || !name) return NULL;
    for (i = 0; i < f->entryCount; i++)
        if (_stricmp(f->entries[i].name, name) == 0) return &f->entries[i];
    return NULL;
}
