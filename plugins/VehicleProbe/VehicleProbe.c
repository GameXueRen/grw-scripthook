/* Vehicle id probe. Answers one question: which vehicles this game has that
 * the framework's catalogue (scripthook_spawn.c, g_vehicles) does not.
 *
 * The id is readable without any catalogue, which is the whole reason this can
 * work: a vehicle is named by a masked handle - the kind hash (VEH_KIND_HASH,
 * 0x8F2CBBBA) in the low dword and the vehicle id in the high one - so any
 * 8 bytes in memory whose low dword is that constant carries an id. The
 * framework's spec scan reads handles exactly that way; the one place that does
 * not is scripthook_api.c's VehicleIdAt, which compares dwords against the
 * catalogue - which is why a vehicle the catalogue has never heard of comes
 * back as identified = 0 and its id is never read at all.
 *
 * Two collections, both read only, neither of them spawns anything:
 *
 *   F5         what is around the player right now: the vehicle being occupied
 *              first, then the vehicles inside [Settings] radius metres.
 *   Ctrl+F5    the whole address space, once, on its own thread: every qword
 *              whose low dword is the kind hash, deduplicated by id, with a
 *              vote on which relative offset below the handle holds a pointer
 *              to the spec vtable. That vote is what tells a real vehicle from
 *              a random 8 bytes, and the value it settles on is the same one
 *              scripthook_spawn.c learns at start up ("spawn: spec vtable
 *              learned: ..."), so the two can be compared in the logs.
 *
 * Found ids go three places: logs\VehicleProbe.log (the narrative, with the
 * evidence), this plugin's own ini under [Found] (so a find survives the
 * session that made it), and logs\VehicleProbe-found.txt (one line per id,
 * sorted by how often it was seen, with the unknown-but-real ones also printed
 * as catalogue lines to paste a name into - a name is still something a person
 * has to look at, which is what the catalogue says about itself where it is
 * listed: named by eye, one spawn at a time).
 *
 * Off by default: [Settings] enabled=0 means the plugin loads, reads nothing,
 * scans nothing and writes nothing.
 */

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "scripthook.h"
#include "log.h"

/* What this plugin needs of the framework: nothing newer than the first
 * version of the plugin API. Name the last thing you use, not the header you
 * happened to build against. */
SH_REQUIRES_API(1);

/* The constant the framework scans for: low dword = "this is a vehicle",
 * high dword = which one. */
#define VEH_KIND_HASH    0x8F2CBBBAu

/* How far below a handle the spec object may start (scripthook_spawn.c probes
 * the same eight-byte positions and calls them SPEC_HANDLE_OFF..+0x38). */
#define HANDLE_OFF_MAX   0x38

/* The whole user address space, as everywhere else in this framework: no
 * smaller ceiling, or Windows (whose heap sits far above 60GB) finds nothing. */
#define ADDR_MAX         0x800000000000ULL

#define CHUNK            (256 * 1024)
#define BLOCK_MAX        0x2000
/* Every catalogue id lives in this range, and a vehicle id is a tagged value
 * rather than a plain hash, so an "id" outside it is a coincidence of two
 * dwords and not a vehicle. */
#define ID_LO            0x40000000u
#define ID_HI            0x42000000u
#define VOTE_SIGHTINGS   8        /* sightings per id the vote is based on */
#define VOTE_PAIRS       4        /* (offset, value) pairs kept per id */
#define TABLE_SLOTS      8192
#define MAX_IDS          4096
#define PTR_TRIES        192      /* pointer candidates tried per block */
#define TICK_MS          100
#define REPORT_UNKNOWN   200      /* unknown ids printed as catalogue lines */

typedef struct {
    uint32_t    id;
    uint32_t    hits;
    /* The vote: which offset below a handle pointed at which value, and how
     * many of this id's sightings agreed. */
    uint32_t    off[VOTE_PAIRS];
    uint64_t    vt[VOTE_PAIRS];
    uint32_t    cnt[VOTE_PAIRS];
    int         voted;
    int         claimed;          /* the catalogue already knows this id */
    const char *name;             /* ... and calls it this */
    uint64_t    sample;           /* one handle address, for the report */
    uint64_t    lastVoted;        /* sighting address the vote already used */
} Found;

static Found           *g_tab;
static int              g_ids;
static int              g_truncated;
static volatile LONG    g_busy;
static volatile LONG    g_walked;
static int              g_enabled;
static int              g_radius = 150;
static int              g_hotkey = VK_F5;
static int              g_minHits = 2;
static int              g_scanOnLoad;
static char             g_ini[MAX_PATH];
static volatile LONG    g_lockInit;
static CRITICAL_SECTION g_lock;
static uint64_t         g_scanBytes;
static uint32_t         g_scanMs;
static int              g_scanRegions;
static uint64_t         g_specVt;      /* what the ids agreed on */
static uint32_t         g_specOff;
static volatile int     g_wasDown;

/* The table is touched by two threads - the one polling the hotkey and the one
 * walking memory - so the lock is made once, before either of them starts,
 * rather than lazily from whichever gets there first. */
static void EnsureLock(void) {
    LONG s = InterlockedCompareExchange(&g_lockInit, 0, 0);

    if (s == 2) return;
    if (s == 1) {
        while (InterlockedCompareExchange(&g_lockInit, 0, 0) != 2) Sleep(0);
        return;
    }
    if (InterlockedCompareExchange(&g_lockInit, 1, 0) == 0) {
        InitializeCriticalSection(&g_lock);
        InterlockedExchange(&g_lockInit, 2);
        return;
    }
    while (InterlockedCompareExchange(&g_lockInit, 0, 0) != 2) Sleep(0);
}

static void Lock(void) { EnterCriticalSection(&g_lock); }
static void Unlock(void) { LeaveCriticalSection(&g_lock); }

/* ---- the table of ids seen -------------------------------------------- */

static uint32_t Hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/* Is this id in the framework's catalogue? A linear walk of 65 entries, which
 * is cheaper than trusting a second lookup to agree with the first. */
static int CatName(uint32_t id, const char **name) {
    int i, n = ShVehicleCount();

    for (i = 0; i < n; i++) {
        const ShVehicle *v = ShVehicleAt(i);

        if (v && v->id == id) {
            if (name) *name = v->name;
            return 1;
        }
    }
    return 0;
}

/* Open addressing over a fixed table: an id is inserted once and only ever
 * visited again. Called with the lock held. */
static Found *TabAt(uint32_t id, int make) {
    uint32_t h = Hash32(id) & (TABLE_SLOTS - 1);
    int probe;

    if (!id) return NULL;
    for (probe = 0; probe < 8; probe++) {
        Found *f = &g_tab[(h + (uint32_t)probe) & (TABLE_SLOTS - 1)];

        if (f->id == id) return f;
        if (!f->id) {
            if (!make) return NULL;
            if (g_ids >= MAX_IDS) { g_truncated = 1; return NULL; }
            memset(f, 0, sizeof(*f));
            f->id = id;
            f->claimed = CatName(id, &f->name);
            g_ids++;
            return f;
        }
    }
    g_truncated = 1;
    return NULL;
}

static uint64_t ReadQ(uint64_t addr) {
    uint64_t v = 0;

    if (!ShReadBytes(addr, &v, 8)) return 0;
    return v;
}

/* The loaded image of the game itself - what image.h calls ShImageBase /
 * ShInImage, which a plugin cannot include (that header is the framework's
 * own). Same thing, read from the PE header of the process's main module. */
static uint64_t g_imgLo, g_imgHi;

static void ModuleRange(HMODULE m, uint64_t *lo, uint64_t *hi) {
    *lo = 0;
    *hi = 0;
    if (!m) return;
    {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)m;

        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        {
            IMAGE_NT_HEADERS *nt =
                (IMAGE_NT_HEADERS *)((uint8_t *)m + dos->e_lfanew);

            if (nt->Signature != IMAGE_NT_SIGNATURE) return;
            *lo = (uint64_t)(uintptr_t)m;
            *hi = *lo + nt->OptionalHeader.SizeOfImage;
        }
    }
}

static int InImage(uint64_t v) {
    return g_imgLo && v >= g_imgLo && v < g_imgHi;
}

/* The spec object carries the masked handle at a fixed offset below its start,
 * so from a handle at `at` the object may start at one of the eight-byte
 * positions under it - and its first qword is then the spec vtable, the same
 * value for every vehicle in the game. Two sightings of one id landing on the
 * same pair is what a real vehicle looks like; a random qword that happens to
 * have the constant in its low half agrees with nothing.
 *
 * The value has to be INSIDE the image, which is the test scripthook_spawn.c's
 * own SpecProbe makes (ShInImage(vt)) and the reason this vote works at all:
 * the qword at at-0 is the handle itself, and it agrees with every sighting of
 * its own id by construction. Without the image test the best pair of every id
 * is (0, its own handle) and no value is ever agreed on by two ids - which is
 * exactly what the first run of this probe reported. */
static void Vote(Found *f, uint64_t at) {
    int o, i;

    for (o = 0; o <= HANDLE_OFF_MAX; o += 8) {
        uint64_t vt = ReadQ(at - (uint64_t)o);

        if (!vt || !InImage(vt)) continue;
        for (i = 0; i < VOTE_PAIRS; i++) {
            if (f->cnt[i] && f->off[i] == (uint32_t)o && f->vt[i] == vt) {
                f->cnt[i]++;
                break;
            }
            if (!f->cnt[i]) {
                f->off[i] = (uint32_t)o;
                f->vt[i] = vt;
                f->cnt[i] = 1;
                break;
            }
        }
    }
    f->voted++;
}

/* The best pair for this id: the one the most of its sightings agreed on. */
static int BestPair(const Found *f, uint32_t *off, uint64_t *vt, uint32_t *cnt) {
    int i, best = -1;

    for (i = 0; i < VOTE_PAIRS; i++)
        if (f->cnt[i] && (best < 0 || f->cnt[i] > f->cnt[best]))
            best = i;
    if (best < 0) return 0;
    if (off) *off = f->off[best];
    if (vt)  *vt = f->vt[best];
    if (cnt) *cnt = f->cnt[best];
    return 1;
}

static void IniSave(const char *key, const char *value) {
    if (g_ini[0]) WritePrivateProfileStringA("Found", key, value, g_ini);
}

/* Record one sighting of an id, say so in the log, and remember it in the ini
 * so a find is not lost when the session ends. */
static void Report(uint32_t id, const char *how, uint32_t hits) {
    Found *f;
    char key[24];

    if (!id) return;
    Lock();
    f = TabAt(id, 1);
    if (f) {
        if (hits) f->hits += hits;
        else f->hits++;
    }
    Unlock();
    if (!f) {
        Log("id %08X: no room in the table (already %d ids, %s)",
            id, g_ids, g_truncated ? "table full" : "no free slot");
        return;
    }
    Log("id %08X  %s  %s%s", id, how,
        f->claimed ? "in the catalogue as " : "NOT IN THE CATALOGUE",
        f->claimed ? f->name : "");
    snprintf(key, sizeof(key), "0x%08X", id);
    IniSave(key, "1");
}

/* ---- the id of a vehicle that is right here --------------------------- */

/* What reading one entity's blocks found, evidence included. "id NOT found"
 * on its own says nothing about whether the blocks were read at all, how many
 * there were, or what the id-shaped numbers inside them were - and the first
 * run of this probe ended with a page of those lines and no way to tell which
 * of the questions was the answer. */
typedef struct {
    int      components;      /* what ShGetComponents answered */
    int      bases;           /* blocks read successfully */
    int      handles;         /* masked handles seen in them */
    int      known;           /* dwords matching an id we already have */
    int      rangeN;          /* distinct dwords in the id range, in order */
    uint32_t rangeV[8];
    int      rangeC[8];
    uint32_t id;
    char     how[64];
} IdFind;

/* A dword that could be a vehicle id: every catalogue id lives in
 * 0x40000000..0x41FFFFFF, and the low 16 bits have to say something, because
 * the round float constants live in the same range (2.0f is 0x40000000, 4.0f
 * is 0x40800000) and are everywhere. */
static void RangeNote(IdFind *f, uint32_t v) {
    int max = (int)(sizeof(f->rangeV) / sizeof(f->rangeV[0]));
    int i;

    if (v < ID_LO || v >= ID_HI) return;
    if ((v & 0xFFFFu) == 0) return;
    for (i = 0; i < f->rangeN && i < max; i++) {
        if (f->rangeV[i] == v) { f->rangeC[i]++; return; }
    }
    if (f->rangeN < max) {
        f->rangeV[f->rangeN] = v;
        f->rangeC[f->rangeN] = 1;
        f->rangeN++;
    }
}

/* What an IdFind holds, in one line: the evidence a miss is read from. */
static void LogFind(const char *where, const IdFind *f) {
    char   list[160];
    size_t at = 0;
    int    i, shown = 0;

    list[0] = 0;
    for (i = 0; i < f->rangeN && shown < 4; i++) {
        if (at >= sizeof(list)) break;
        at += (size_t)snprintf(list + at, sizeof(list) - at, "%s%08X x%d",
                               shown ? " " : "", f->rangeV[i], f->rangeC[i]);
        shown++;
    }
    Log("%s  no id: components=%d blocks=%d handles=%d known=%d  in range: %s",
        where, f->components, f->bases, f->handles, f->known,
        list[0] ? list : "(none)");
}

/* Read one entity's vehicle id, four ways, cheapest first:
 *
 *   handle     a masked handle sits in the entity's own blocks;
 *   candidate  a dword in those blocks matches an id the catalogue or the walk
 *              already knows - what scripthook_api.c does for the occupied
 *              vehicle, but against every id we have rather than only 65;
 *   range      a dword in 0x40000000..0x41FFFFFF, where every catalogue id
 *              lives, seen at least twice: an id no name list has yet;
 *   spec       a pointer in those blocks leads to an object whose handle field
 *              carries the id (the framework's own model of where the id is).
 *
 * The order is the cost order: the first three read nothing but the block, the
 * fourth is eight guarded reads per pointer tried, so it runs only after the
 * others came back empty. */
static int IdFromEntity(uint64_t entity, IdFind *f) {
    static uint8_t block[BLOCK_MAX];
    ShComponent    comps[96];
    uint32_t       knownBest = 0, rangeBest = 0;
    int            knownHits = 0, rangeHits = 0;
    int            n, c, b, pass;

    memset(f, 0, sizeof(*f));
    if (!entity) return 0;
    n = ShGetComponents(entity, comps, 96);
    f->components = n;

    for (pass = 0; pass < 2; pass++) {
        for (c = -1; c < n; c++) {
            uint64_t bases[2];
            int      nb, off, bytes;

            if (c < 0) {
                bases[0] = entity;
                nb = 1;
            } else {
                bases[0] = comps[c].component;
                bases[1] = comps[c].dataBlock;
                nb = 2;
            }
            for (b = 0; b < nb; b++) {
                uint64_t base = bases[b];

                if (!base) continue;
                bytes = BLOCK_MAX;
                if (!ShReadBytes(base, block, (uint32_t)bytes)) {
                    /* A short block is common (the entity's own head), and the
                     * framework reads 0x400 in exactly this case. */
                    bytes = 0x400;
                    if (!ShReadBytes(base, block, (uint32_t)bytes)) continue;
                }

                if (pass) {
                    int tries = 0;

                    /* 4. a pointer to the spec: its handle field carries the id */
                    for (off = 0; off + 8 <= bytes && tries < PTR_TRIES; off += 8) {
                        uint64_t p;
                        int      o;

                        memcpy(&p, block + off, 8);
                        if (p < 0x10000ULL || p >= ADDR_MAX || (p & 7)) continue;
                        tries++;
                        for (o = 0; o <= HANDLE_OFF_MAX; o += 8) {
                            uint64_t h = ReadQ(p + (uint64_t)o);

                            if ((uint32_t)h == VEH_KIND_HASH && (uint32_t)(h >> 32)) {
                                f->id = (uint32_t)(h >> 32);
                                snprintf(f->how, sizeof(f->how), "spec %llX+%X",
                                         (unsigned long long)p, o);
                                return 1;
                            }
                        }
                    }
                    continue;
                }

                f->bases++;

                /* 1. the handle itself */
                for (off = 0; off + 8 <= bytes; off += 8) {
                    uint64_t v;

                    memcpy(&v, block + off, 8);
                    if ((uint32_t)v == VEH_KIND_HASH && (uint32_t)(v >> 32)) {
                        f->handles++;
                        f->id = (uint32_t)(v >> 32);
                        snprintf(f->how, sizeof(f->how), "handle %llX+%X",
                                 (unsigned long long)base, off);
                        return 1;
                    }
                }

                /* 2. an id we already know, most-referenced wins (one
                 *    catalogue-looking dword on its own is common state) and
                 *    3. any dword in the id range, counted */
                {
                    struct { uint32_t id; int hits; } seen[4];
                    int k;

                    memset(seen, 0, sizeof(seen));
                    for (off = 0; off + 4 <= bytes; off += 4) {
                        uint32_t v;

                        memcpy(&v, block + off, 4);
                        if (!v) continue;
                        if (CatName(v, NULL) || TabAt(v, 0)) {
                            f->known++;
                            for (k = 0; k < 4; k++) {
                                if (seen[k].hits && seen[k].id == v) { seen[k].hits++; break; }
                                if (!seen[k].hits) { seen[k].id = v; seen[k].hits = 1; break; }
                            }
                        }
                        RangeNote(f, v);
                    }
                    for (k = 0; k < 4; k++) {
                        if (seen[k].hits > knownHits) {
                            knownHits = seen[k].hits;
                            knownBest = seen[k].id;
                        }
                    }
                }
            }
        }
        if (!pass) {
            int i;

            if (knownHits >= 2) {
                f->id = knownBest;
                snprintf(f->how, sizeof(f->how), "candidate (%d refs)", knownHits);
                return 1;
            }
            for (i = 0; i < f->rangeN; i++)
                if (f->rangeC[i] > rangeHits) {
                    rangeHits = f->rangeC[i];
                    rangeBest = f->rangeV[i];
                }
            if (rangeHits >= 2) {
                f->id = rangeBest;
                snprintf(f->how, sizeof(f->how), "range (%d refs)", rangeHits);
                return 1;
            }
        }
    }
    return 0;
}

/* ---- the two collections ---------------------------------------------- */

/* What is around the player: the vehicle being occupied, then the vehicles
 * within the configured radius. */
static void ScanNearby(void) {
    ShOccupiedVehicle ov;
    ShEntity          ents[64];
    IdFind            find;
    char              where[96];
    int               i, n;

    Log("near: scanning (radius %d m)", g_radius);
    memset(&ov, 0, sizeof(ov));
    if (ShGetOccupiedVehicle(&ov) && ov.entity) {
        if (IdFromEntity(ov.entity, &find)) {
            Report(find.id, find.how, 0);
        } else {
            snprintf(where, sizeof(where), "near: occupied %llX (catalogue: %s)",
                     (unsigned long long)ov.entity,
                     ov.identified ? ov.name : "does not know it either");
            LogFind(where, &find);
        }
    } else {
        Log("near: not in a vehicle");
    }

    if (g_radius <= 0) return;
    n = ShFindEntities(SH_KIND_VEHICLE, (float)g_radius, SH_FIND_UNNAMED,
                       ents, 64);
    Log("near: %d vehicle entit(ies) within %d m", n, g_radius);
    for (i = 0; i < n; i++) {
        if (!IdFromEntity(ents[i].entity, &find)) {
            snprintf(where, sizeof(where), "near: %6.1f m  entity %llX%s%s",
                     ents[i].distance, (unsigned long long)ents[i].entity,
                     ents[i].name[0] ? "  " : "", ents[i].name);
            LogFind(where, &find);
            continue;
        }
        Log("near: %6.1f m  entity %llX  id %08X via %s", ents[i].distance,
            (unsigned long long)ents[i].entity, find.id, find.how);
        Report(find.id, find.how, 0);
    }
}

/* Our own image: the constant this scan looks for is in it, and so is the
 * buffer the scan reads into, so a hit there would be the probe looking at
 * itself (the lesson AmmoProbe's log records about its own stack). */
static void OwnImage(uint64_t *lo, uint64_t *hi) {
    HMODULE m = NULL;
    uint8_t *p = (uint8_t *)(uintptr_t)&ScanNearby;

    *lo = 0;
    *hi = 0;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)p, &m) || !m)
        return;
    ModuleRange(m, lo, hi);
}

/* This thread's own stack, from the TIB. Every read of ours goes through a
 * buffer, but the handle addresses themselves are passed to Log and land in
 * this frame - a scan that does not skip it finds the searcher. */
static void OwnStack(uint64_t *lo, uint64_t *hi) {
    *hi = __readgsqword(0x08);
    *lo = __readgsqword(0x10);
}

static int Overlaps(uint64_t lo, uint64_t hi, uint64_t a, uint64_t b) {
    return a && b && lo < b && hi > a;
}

/* Settle what the sightings agreed on: the value that the most distinct ids
 * picked at the same relative offset is the spec vtable, and an id whose best
 * pair carries it is a real vehicle. */
static void FinishVotes(void) {
    struct { uint64_t vt; uint32_t off; int ids; int ctl; } win[16];
    int i, k, nwin = 0, best = -1;

    memset(win, 0, sizeof(win));
    Lock();
    for (i = 0; i < TABLE_SLOTS; i++) {
        uint64_t vt;
        uint32_t off, cnt;

        if (!g_tab[i].id) continue;
        if (!BestPair(&g_tab[i], &off, &vt, &cnt) || cnt < 2) continue;
        for (k = 0; k < nwin; k++) {
            if (win[k].vt == vt && win[k].off == off) {
                win[k].ids++;
                if (g_tab[i].claimed) win[k].ctl++;
                break;
            }
        }
        if (k == nwin && nwin < (int)(sizeof(win) / sizeof(win[0]))) {
            win[nwin].vt = vt;
            win[nwin].off = off;
            win[nwin].ids = 1;
            win[nwin].ctl = g_tab[i].claimed ? 1 : 0;
            nwin++;
        }
    }
    Unlock();

    /* The catalogue is the control the vote is read against: its 65 ids are
     * vehicles this game certainly has, so the pair they agree on is the right
     * one - and a winner with none of them behind it is proven by nothing. */
    for (k = 0; k < nwin; k++) {
        if (best < 0) { best = k; continue; }
        if (win[k].ctl && !win[best].ctl) { best = k; continue; }
        if (win[k].ctl == win[best].ctl && win[k].ids > win[best].ids) best = k;
    }
    if (best >= 0 && win[best].ids >= 2 && win[best].ctl) {
        g_specVt = win[best].vt;
        g_specOff = win[best].off;
        Log("vote: spec vtable looks like %llX at handle - 0x%X (%d id(s) "
            "agreed, %d of them in the catalogue; scripthook_spawn.c logs its "
            "own answer as 'spec vtable learned')",
            (unsigned long long)g_specVt, g_specOff, win[best].ids,
            win[best].ctl);
    } else if (best >= 0 && win[best].ids >= 2) {
        Log("vote: %llX at handle - 0x%X was agreed on by %d id(s), but not by "
            "one in the catalogue - the catalogue is the control here, so "
            "nothing is called real this run",
            (unsigned long long)win[best].vt, win[best].off, win[best].ids);
    } else {
        Log("vote: no pair was agreed on by two ids - every hit looks like a "
            "stray value, so nothing here can be called a real vehicle yet");
    }
}

/* A real vehicle, as far as this run can tell: its handle sits above the spec
 * vtable the ids agreed on, at the offset they agreed on, and the id is in the
 * vehicle range. One sighting is enough for that - the first run showed the
 * catalogue's own vehicles coming out with a single agreement each, because a
 * handle that lives in a table and in a live instance does not sit above a
 * spec in both places. The count is what the candidate block is gated on. */
static int InIdRange(uint32_t id) {
    return id >= ID_LO && id < ID_HI;
}

static int IsReal(const Found *f) {
    uint64_t vt;
    uint32_t off, cnt;

    if (!InIdRange(f->id)) return 0;
    if (!g_specVt) return 0;
    if (!BestPair(f, &off, &vt, &cnt) || cnt < 1) return 0;
    return vt == g_specVt && off == g_specOff;
}

/* The whole address space, once: every qword whose low dword is the kind hash
 * carries an id in its high dword. */
static void CollectAll(void) {
    static uint8_t          *buf;
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t                 *scan = (uint8_t *)0x10000;
    uint64_t                 sLo = 0, sHi = 0, iLo = 0, iHi = 0;
    uint64_t                 handles = 0;
    DWORD                    t0, last;
    int                      regions = 0, skipped = 0;

    if (InterlockedCompareExchange(&g_busy, 1, 0)) {
        Log("all: a walk is already running");
        return;
    }
    if (!buf)
        buf = (uint8_t *)VirtualAlloc(NULL, CHUNK, MEM_COMMIT, PAGE_READWRITE);
    if (!buf) {
        Log("all: could not allocate %d bytes", (int)CHUNK);
        InterlockedExchange(&g_busy, 0);
        return;
    }

    OwnStack(&sLo, &sHi);
    OwnImage(&iLo, &iHi);
    g_scanBytes = 0;
    g_scanRegions = 0;
    t0 = last = GetTickCount();
    Log("all: walking the address space (own image %llX..%llX and this "
        "thread's stack skipped)", (unsigned long long)iLo,
        (unsigned long long)iHi);

    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;

        if (next <= scan) break;
        if ((uint64_t)(uintptr_t)mbi.BaseAddress >= ADDR_MAX) break;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & PAGE_GUARD)) {
            uint8_t *b = (uint8_t *)mbi.BaseAddress;
            size_t   sz = mbi.RegionSize, o, k, got;

            if (Overlaps((uint64_t)(uintptr_t)b, (uint64_t)(uintptr_t)(b + sz),
                         sLo, sHi) ||
                Overlaps((uint64_t)(uintptr_t)b, (uint64_t)(uintptr_t)(b + sz),
                         iLo, iHi)) {
                skipped++;
                scan = next;
                continue;
            }
            regions++;
            for (o = 0; o + 8 <= sz; o += CHUNK - 8) {
                got = sz - o;
                if (got > CHUNK) got = CHUNK;
                if (!ShReadBytes((uint64_t)(uintptr_t)(b + o), buf,
                                 (uint32_t)got))
                    continue;
                g_scanBytes += got;
                for (k = 0; k + 8 <= got; k += 8) {
                    uint64_t v;
                    uint64_t at = (uint64_t)(uintptr_t)(b + o + k);
                    Found   *f;

                    memcpy(&v, buf + k, 8);
                    if ((uint32_t)v != VEH_KIND_HASH) continue;
                    handles++;
                    Lock();
                    f = TabAt((uint32_t)(v >> 32), 1);
                    if (f) {
                        f->hits++;
                        if (f->voted < VOTE_SIGHTINGS && f->lastVoted != at) {
                            f->lastVoted = at;
                            f->sample = at;
                            Vote(f, at);
                        }
                    }
                    Unlock();
                }
                if (GetTickCount() - last > 2000) {
                    last = GetTickCount();
                    Log("all: %llu MB, %d region(s), %d handle(s), %d id(s) so "
                        "far", (unsigned long long)(g_scanBytes >> 20),
                        regions, (int)handles, g_ids);
                }
            }
        }
        scan = next;
    }
    g_scanRegions = regions;
    g_scanMs = GetTickCount() - t0;
    FinishVotes();
    Log("all: done, %llu MB in %lu ms over %d region(s), %d skipped, %d "
        "handle(s), %d id(s)%s", (unsigned long long)(g_scanBytes >> 20),
        (unsigned long)g_scanMs, regions, skipped, (int)handles, g_ids,
        g_truncated ? " (TABLE FULL - some ids were not recorded)" : "");
    InterlockedExchange(&g_walked, 1);
    InterlockedExchange(&g_busy, 0);
}

/* ---- the report -------------------------------------------------------- */

static int ByHitsDesc(const void *a, const void *b) {
    const Found *x = *(const Found *const *)a;
    const Found *y = *(const Found *const *)b;

    if (x->hits != y->hits) return x->hits < y->hits ? 1 : -1;
    return x->id < y->id ? -1 : 1;
}

/* logs\VehicleProbe-found.txt: one line per id, most seen first, and the
 * unknown ones that look real printed as catalogue lines with the name left
 * empty to be filled in by eye. */
static void WriteReport(void) {
    static Found *list[MAX_IDS];
    char   dir[MAX_PATH], path[MAX_PATH];
    int    i, n = 0, unknown = 0, unnamed = 0;
    FILE  *f;

    if (!ShGameDir(dir, sizeof(dir))) {
        Log("report: no game directory to write into");
        return;
    }
    snprintf(path, sizeof(path), "%s\\logs\\VehicleProbe-found.txt", dir);
    f = fopen(path, "w");
    if (!f) {
        Log("report: could not open %s", path);
        return;
    }
    Lock();
    for (i = 0; i < TABLE_SLOTS && n < MAX_IDS; i++)
        if (g_tab[i].id) list[n++] = &g_tab[i];
    qsort(list, (size_t)n, sizeof(list[0]), ByHitsDesc);

    fprintf(f, "; VehicleProbe - vehicles seen in memory, most seen first.\n");
    fprintf(f, "; full walk: %s (%llu MB in %lu ms over %d region(s))\n",
            g_walked ? "yes" : "no", (unsigned long long)(g_scanBytes >> 20),
            (unsigned long)g_scanMs, g_scanRegions);
    fprintf(f, "; spec vtable voted on: %llX at handle - 0x%X%s\n",
            (unsigned long long)g_specVt, g_specOff,
            g_specVt ? "" : "  (nothing agreed - 'real' is empty below)");
    fprintf(f, "; hits counts every sighting of that id's handle. 'real' means a\n");
    fprintf(f, "; sighting of it sits above the spec vtable above and the id is in the\n");
    fprintf(f, "; vehicle range (0x40000000..0x41FFFFFF). An id only a nearby scan has\n");
    fprintf(f, "; seen was never voted on, so it reads 'no' here whatever it is.\n");
    fprintf(f, "; id          hits  real  pair            catalogue\n");
    for (i = 0; i < n; i++) {
        const Found *e = list[i];
        uint64_t vt = 0;
        uint32_t off = 0, cnt = 0;

        BestPair(e, &off, &vt, &cnt);
        fprintf(f, "  0x%08X  %6u  %-4s  0x%X/%-12llX %s\n", e->id, e->hits,
                IsReal(e) ? "yes" : "no", off, (unsigned long long)vt,
                e->claimed ? e->name : "-");
        if (!e->claimed) {
            unknown++;
            if (IsReal(e) && e->hits >= (uint32_t)g_minHits) unnamed++;
        }
    }
    fprintf(f, "\n; %d of %d id(s) are not in the catalogue; %d of them look "
            "like real vehicles.\n", unknown, n, unnamed);
    fprintf(f, "; The ones below look real and have no name yet. Give one a "
            "name (a spawn, a look, a line) and paste it back:\n\n");
    {
        int shown = 0;

        for (i = 0; i < n && shown < REPORT_UNKNOWN; i++) {
            const Found *e = list[i];

            if (e->claimed || !IsReal(e) ||
                e->hits < (uint32_t)g_minHits)
                continue;
            fprintf(f, "{ 0x%08X, \"\" },   /* %u hit(s), sample %llX */\n",
                    e->id, e->hits, (unsigned long long)e->sample);
            shown++;
        }
        if (!shown)
            fprintf(f, "; (none this run)\n");
    }
    Unlock();
    fclose(f);
    Log("report: %d id(s), %d unknown and real -> %s", n, unknown, path);
}

/* ---- settings, loop and entry ------------------------------------------ */

static int IniInt(const char *key, int def) {
    if (!g_ini[0]) return def;
    return GetPrivateProfileIntA("Settings", key, def, g_ini);
}

static void LoadSettings(void) {
    g_enabled    = IniInt("enabled", 0) ? 1 : 0;
    g_radius     = IniInt("radius", 150);
    g_hotkey     = IniInt("hotkey", VK_F5);
    g_minHits    = IniInt("min_hits", 3);
    g_scanOnLoad = IniInt("scan_on_load", 0) ? 1 : 0;
    if (g_radius < 0) g_radius = 0;
    if (g_minHits < 1) g_minHits = 1;
    if (g_hotkey <= 0 || g_hotkey > 255) g_hotkey = VK_F5;
}

static DWORD WINAPI WalkThread(LPVOID p) {
    (void)p;
    CollectAll();
    WriteReport();
    ShToast("VehicleProbe: walk done - see logs\\VehicleProbe-found.txt");
    return 0;
}

static DWORD WINAPI Track(LPVOID p) {
    int g_autoStarted = 0;

    (void)p;
    LogInitAlways("VehicleProbe.log");
    while (!GetModuleHandleA("dinput8.dll")) Sleep(500);
    if (!ShPluginIniPath("VehicleProbe", g_ini, sizeof(g_ini)))
        g_ini[0] = 0;
    LoadSettings();
    if (!g_enabled) {
        Log("VehicleProbe: enabled=0 in its own ini - nothing read, nothing "
            "scanned, nothing written");
        return 0;
    }
    EnsureLock();
    ModuleRange(GetModuleHandleA(NULL), &g_imgLo, &g_imgHi);
    Log("VehicleProbe: game image %llX..%llX - a spec vtable outside it is "
        "not accepted by the vote", (unsigned long long)g_imgLo,
        (unsigned long long)g_imgHi);
    g_tab = (Found *)VirtualAlloc(NULL, sizeof(Found) * TABLE_SLOTS,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_tab) {
        Log("VehicleProbe: no table memory");
        return 0;
    }
    Log("VehicleProbe: enabled - 0x%X scans what is nearby (radius %d m), "
        "Ctrl+0x%X walks the whole address space; min_hits %d, scan_on_load %d",
        g_hotkey, g_radius, g_hotkey, g_minHits, g_scanOnLoad);

    for (;;) {
        int down;

        Sleep(TICK_MS);
        if (!ShGameFocused()) {
            g_wasDown = 0;
            continue;
        }
        /* Once, not once per tick: the walk takes a minute and the loop runs
         * ten times a second, so a flag is what keeps it one thread. */
        if (g_scanOnLoad && !g_autoStarted && !g_busy && ShIsInGame()) {
            HANDLE t;

            g_autoStarted = 1;
            Log("VehicleProbe: scan_on_load - starting the walk");
            t = CreateThread(NULL, 0, WalkThread, NULL, 0, NULL);
            if (t) CloseHandle(t);
            else Log("VehicleProbe: could not start the walk thread");
        }
        down = (GetAsyncKeyState(g_hotkey) & 0x8000) != 0;
        if (down && !g_wasDown) {
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
                HANDLE t;

                if (g_busy) {
                    Log("VehicleProbe: the walk is already running");
                } else {
                    Log("VehicleProbe: starting the walk (Ctrl held)");
                    t = CreateThread(NULL, 0, WalkThread, NULL, 0, NULL);
                    if (t) CloseHandle(t);
                    else Log("VehicleProbe: could not start the walk thread");
                }
            } else {
                ScanNearby();
                WriteReport();
            }
        }
        g_wasDown = down;
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE t;

        DisableThreadLibraryCalls(inst);
        t = CreateThread(NULL, 0, Track, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
