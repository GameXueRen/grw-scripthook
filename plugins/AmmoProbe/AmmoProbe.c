/* Ammo probe, round four: find the player's weapon inventory object.
 *
 * The object that holds the rounds was known all along. The module that used
 * to read ammo (scripthook_ammo.c, removed together with ShGetAmmo, still
 * standing in another checkout) found it by its vtable - SH_IMG(0x3905CF0) -
 * plus an owner handle at +0x250 that resolves to the player root, and read
 * the rounds per slot at slot * 0x28 + 0x180 (or +0x130) as one of the
 * framework's protected ints. Round two had already killed the capacity side
 * (the engine asks that function for a capacity 22 times a session, for many
 * objects, and nothing in them moves by one per shot), so this is the only
 * remaining answer to where the rounds are.
 *
 * Rounds three and four chased that object and learned three things:
 *
 *   - the player's root and entity are the SAME object, carrying 56
 *     components; no component IS the inventory, and no component's fields
 *     point at one (2646 pointers tested);
 *   - the fire path reported nothing at all across two whole sessions.
 *     logs\scripthook_hit.log says why: the projectile site this framework
 *     patches holds 42 68 39 1A, which is not the CALL rel32 the patch
 *     assumes - that pin is stale in this build, so ShOnFire, ShGetShots and
 *     ShShotCount are all dead here. The marker rows therefore carry the job
 *     of tying a change to what the player just did;
 *   - the sweep's "two vtable hits" were the PROBE'S OWN STACK. Both dumps
 *     are one frame: the needle as a qword (Log passes it as a vararg, which
 *     spills it), beside char logFile[] = "AmmoProbe.log" and an ShPlayer
 *     local holding the player root - and ShReflectClassHash answered 0 for
 *     both, as it would for any text. A sweep that does not skip the thread
 *     running it finds the searcher, not the game.
 *
 * Round five ran that corrected sweep and answered both of its questions. The
 * control found the entity vtable 11938 times, with the player root exactly
 * where it had to be - so the search works. The needle found NOTHING. The
 * constant the old module identified is therefore stale in this build: the
 * class it names is not in the game's memory any more, and looking for it
 * harder will not help. The decode net over the objects at hand found nothing
 * either - three offsets decoded, all three were garbage churning by
 * billions, and not one line held a number small enough to be a count.
 *
 * Round six found it, by decoding the objects the engine asks for a CAPACITY
 * (ShGetAmmoLook's first argument). On four different weapons in one session:
 *
 *     pi:  cap0+180  ... = 30      pi:  cap6+180  ... = 30
 *     pi:  cap2+180  ... = 30      pi:  cap7+180  ... = 20
 *
 * +0x180 is the offset the old module called OFF_AMMO, and 20/30 are magazine
 * sizes. The rounds are a protected int at weapon + 0x180 (or +0x130 for the
 * weapons that keep them there), where the weapon is the object the engine
 * hands its own capacity function - a path with no sweep, no heap scan and no
 * class constant in it. It also closed the last loop: the health module's sub
 * component route works (the control line found the player's health), and the
 * sub components carry no ammo array.
 *
 * Round seven then said what +0x180 IS: the CAPACITY. It reads 30 on one
 * weapon and 20 on another, it follows the equipped weapon the moment the
 * player switches to it (measured live), and it does not move while firing.
 * So the object is the right one and that offset is not - which is why this
 * round stops trusting any single offset and diffs the objects RAW as well.
 * A count does not have to be a protected int (health keeps its maximum as a
 * plain one), and a decode scan cannot see a plain number at all. Only changes
 * that look like a count are reported, and an offset that keeps moving is
 * named once instead of every poll.
 *
 * The class hunt stays in the file for the record: the shape of a magazine,
 * on the route the framework itself walks:
 *
 *   1. every holder component's first 0x200 bytes are scanned for pointers to
 *      sub components owned by the player (the owner at +0x28). That is
 *      exactly how scripthook_health.c reaches the player's health; no
 *      earlier round looked at a component's WINDOW, only at its own fields;
 *   2. every object found that way is dumped, checked against the health
 *      layout as a CONTROL (finding that one proves the route reaches real
 *      player components), and searched for the shape a magazine has: a run
 *      of protected ints one slot stride (0x28) apart, every one of them a
 *      small number. Chance alignment does not produce that run;
 *   3. every entry of such a run is then watched, so firing says which slot
 *      the current weapon lives in - which is the one thing no amount of
 *      guessing at offsets has managed so far.
 *
 * The decode is still what makes any of it visible. A count kept as a
 * protected int does NOT move by one in memory: its four plane bytes
 * scramble, and the number itself is never stored. Round two diffed raw
 * windows looking for a value that went down by one per shot and saw nothing,
 * because it was looking at encoded planes.
 *
 * Still read only: no hook of its own (the projectile hook is the
 * framework's), nothing written to engine memory, nothing done until the
 * switch is on.
 *
 * Script: F4 -> the page -> switch on (the hand objects and any capacity
 * arguments are scanned there and then) -> press "Sweep all memory for it"
 * (about half a minute) -> close the menu -> fire a few rounds, reload, take
 * an ammo crate, pressing the matching marker each time. Then read
 * logs\AmmoProbe.log.
 *
 * Text: kEn / kZh here, overridable by plugins\AmmoProbe\lang.ini.
 */

#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scripthook.h"
#include "image.h"
#include "log.h"

/* ---- the inventory object's shape ----------------------------------- */

/* From the module that used to read ammo, unchanged. The vtable is what
 * identifies the class; +0x250 is the owner the old finder insisted on and
 * this round merely reports; the rounds are protected ints per slot. */
#define INV_VTABLE   SH_IMG(0x3905CF0)
#define OFF_OWNER    0x250
#define SLOT_STRIDE  0x28
#define OFF_AMMO     0x180
#define OFF_AMMO_ALT 0x130
#define MAX_SLOT     8

/* How much of each object is searched and decoded, and how long the lists
 * that come out of it may grow. Every list is a fixed array: nothing here
 * allocates. */
#define CAND_MAX     192
#define COMP_MAX     96
#define WIN_MAX      0x800
#define PTR_MAX      20000
#define INV_MAX      8        /* vtable hits kept and analysed */
#define WATCH_MAX    256      /* protected ints re-read every poll */
#define DEC_MAX      6        /* decode lines per object per scan */
#define CAP_MAX      24       /* capacity arguments scanned for counts */
#define SUB_WINDOW   0x200    /* a holder's window the sub objects sit in */
#define SUB_MAX      48       /* sub objects analysed */
#define OFF_SUBOWNER 0x28     /* a sub component points at its owner here */
#define OFF_MAXHP    0xF0     /* the health layout, as a control */
#define OFF_HP       0xF8
#define ARR_MIN      3        /* a run this long at the slot stride is an array */
#define ARR_MAXVAL   999      /* and every entry in it is this small */

/* ---- state ---------------------------------------------------------- */

static char     g_ini[MAX_PATH];
static char     g_name[64];
static uint32_t g_menu;

/* The menu thread writes these; the probe thread reads them. */
static volatile LONG g_on;
static volatile LONG g_mark;      /* 0 = none, else the row that was pressed */
static volatile LONG g_act;       /* 0 = none, 1 = find now, 2 = sweep now */
static volatile LONG g_base;      /* where in an object the search starts */
static volatile LONG g_len;       /* how much of it is searched */

/* The probe thread alone writes these; the status line reads them. */
static volatile LONG g_candN;     /* objects at hand on the last pass */
static volatile LONG g_shots;     /* projectiles reported so far */
static volatile LONG g_ammoVal = -1;  /* the rounds off the last weapon, -1 none */
static uint64_t      g_ammoAddr;
static uint64_t      g_inv[INV_MAX];
static int           g_nInv;
static volatile LONG g_nInvLive;

typedef struct {
    uint64_t addr;
    char     tag[24];
    uint32_t val;
    int      known;
} Watch;

static Watch g_watch[WATCH_MAX];
static int   g_nWatch;

/* ---- settings ------------------------------------------------------- */

static void SaveInt(const char *key, LONG value) {
    char text[24];

    snprintf(text, sizeof(text), "%ld", (long)value);
    if (!WritePrivateProfileStringA("Settings", key, text, g_ini))
        Log("ap: could not write %s=%s to %s", key, text, g_ini);
}

static void LoadSettings(void) {
    LONG on, base, len;

    on   = (LONG)GetPrivateProfileIntA("Settings", "enabled", 0, g_ini);
    base = (LONG)GetPrivateProfileIntA("Settings", "base", 0, g_ini);
    len  = (LONG)GetPrivateProfileIntA("Settings", "len", 0x400, g_ini);

    if (base < 0 || base > 0x800) base = 0;
    base &= ~7L;
    if (len < 8 || len > WIN_MAX) len = 0x400;
    len &= ~7L;

    InterlockedExchange(&g_on, on ? 1 : 0);
    InterlockedExchange(&g_base, base);
    InterlockedExchange(&g_len, len);
}

/* ---- reading -------------------------------------------------------- */

/* Only the framework's guarded reads are used: a freed page fails instead of
 * killing the game, which matters for everything below - all of it walks
 * memory the engine owns. */

static int Sane(uint64_t p) {
    return p >= 0x10000ULL && p < 0x800000000000ULL;
}

/* This thread's own stack, from the TIB. The sweep has to skip it: the needle
 * itself is spilled there by Log's varargs (a 64-bit argument lands in the
 * caller's frame), so a sweep that does not skip it finds the searcher - which
 * is exactly what rounds three and four did. */
static void OwnStack(uint64_t *lo, uint64_t *hi) {
    *hi = __readgsqword(0x08);      /* NT_TIB.StackBase, the high end */
    *lo = __readgsqword(0x10);      /* NT_TIB.StackLimit, the low end */
}

static uint64_t ReadQ(uint64_t addr) {
    uint64_t v = 0;

    if (!ShReadBytes(addr, &v, 8)) return 0;
    return v;
}

/* A masked handle slot: the pointer at +0, the flags dword at +0xC, live
 * when the flags are negative. The same shape every other reader here uses. */
static uint64_t ResolveHandle(uint64_t slot) {
    int32_t flags = 0;
    uint64_t val;

    if (!slot || !Sane(slot)) return 0;
    if (!ShReadBytes(slot + 0xC, &flags, 4)) return 0;
    if (flags >= 0) return 0;
    val = ReadQ(slot);
    return Sane(val) ? val : 0;
}

/* ---- the numbers to watch ------------------------------------------- */

/* One entry per protected int this probe has decided is worth following, no
 * matter which object it turned out to be in: a value that moves by one per
 * shot names the inventory better than any field offset can.
 *
 * The decode says "this looks like a protected int", not "this IS a count":
 * within a heap block most qwords point somewhere readable, so plenty of
 * offsets decode and the numbers behind them are ordinary garbage. Round six
 * filled the whole table with that garbage and pushed the one value that
 * mattered (a capacity argument's +0x180) out of it. So a value is only
 * followed when it is small enough to be a count, and callers that already
 * know what they have pass force. */
static void WatchAdd(uint64_t addr, const char *tag, int force) {
    uint32_t v;
    int i;

    if (!addr || g_nWatch >= WATCH_MAX) return;
    if (!ShStatRead(addr, &v)) return;              /* not a stat at all */
    if (!force && v > ARR_MAXVAL) return;           /* a count is small */
    for (i = 0; i < g_nWatch; i++)
        if (g_watch[i].addr == addr) return;
    g_watch[g_nWatch].addr = addr;
    snprintf(g_watch[g_nWatch].tag, sizeof(g_watch[0].tag), "%s", tag);
    g_watch[g_nWatch].val = v;                      /* just read: the baseline */
    g_watch[g_nWatch].known = 1;
    g_nWatch++;
}

/* Re-read every watched number, and log the ones that moved next to how many
 * shots happened since the previous poll. Every line here is evidence: the
 * first time a value goes down by one with a shot, the object it lives in is
 * the inventory, and its tag says where in it that is. */
static void WatchPoll(uint32_t shots) {
    int i, shown = 0, more = 0;

    for (i = 0; i < g_nWatch; i++) {
        uint32_t v;

        if (!ShStatRead(g_watch[i].addr, &v)) continue;
        if (!g_watch[i].known) {
            g_watch[i].known = 1;
            g_watch[i].val = v;
            continue;
        }
        if (v == g_watch[i].val) continue;
        /* A poll can only say "somewhere in the last 250 ms", and a decode
         * that is not a stat at all churns. Log the first few and count the
         * rest, so a busy window cannot bury the line that matters. */
        if (shown >= 16) { more++; g_watch[i].val = v; continue; }
        Log("chg: %-10s %016llX %u -> %u (shots +%u)", g_watch[i].tag,
            (unsigned long long)g_watch[i].addr, g_watch[i].val, v,
            (unsigned)shots);
        g_watch[i].val = v;
        shown++;
    }
    if (more) Log("chg: ... (%d more changed this poll)", more);
}

/* Every offset in a window that decodes as one of the framework's protected
 * ints. That decode is the filter this round leans on: it needs four readable
 * plane pointers and a key behind the address, which a plain field is not. */
static void ScanProtints(uint64_t obj, const char *tag, int force) {
    uint64_t base = obj + (uint64_t)(LONG)g_base;
    uint64_t off;
    char label[24];
    int n = 0;

    for (off = 0; off + 0x28 <= (uint64_t)(LONG)g_len; off += 8) {
        uint32_t v;

        if (!ShStatRead(base + off, &v)) continue;
        if (!force && v > ARR_MAXVAL) continue;     /* decodes, but is not a count */
        snprintf(label, sizeof(label), "%s+%03llX", tag,
                 (unsigned long long)((uint64_t)(LONG)g_base + off));
        Log("pi:  %-16s %016llX = %u", label, (unsigned long long)(base + off), v);
        WatchAdd(base + off, label, force);
        if (++n >= DEC_MAX) {
            Log("pi:  %s ... further decodes in this object not listed", tag);
            break;
        }
    }
}

/* ---- what one vtable hit is ----------------------------------------- */

/* The owner chain, raw and resolved: the one thing the old finder required
 * and the round three sweep could not satisfy. Printing every hop of it is
 * what will say whether the rule was wrong or the field has moved. */
static void LogOwner(uint64_t obj) {
    uint64_t h = ReadQ(obj + OFF_OWNER);
    int32_t flags = 0;
    uint64_t res;
    ShPlayer p;

    if (!ShReadBytes(h + 0xC, &flags, 4)) flags = 0;
    res = ResolveHandle(h);
    Log("own: %016llX +0x%X -> %016llX (flags %08X) -> %016llX",
        (unsigned long long)obj, OFF_OWNER, (unsigned long long)h,
        (unsigned)flags, (unsigned long long)res);
    if (!ShGetPlayer(&p)) return;
    {
        uint64_t root = p.root ? p.root : p.entity;

        if (h == root) Log("own: that field itself IS the player root");
        if (h == p.entity) Log("own: that field itself IS the player entity");
        if (res == root) Log("own: the handle resolves to the player root");
        else if (res == p.entity) Log("own: the handle resolves to the player entity");
        else Log("own: resolves to neither (root %016llX entity %016llX)",
                 (unsigned long long)root, (unsigned long long)p.entity);
    }
}

static void DumpObj(uint64_t obj, int bytes) {
    char line[256];
    int q, k, n = bytes / 8;

    for (q = 0; q < n; q += 4) {
        int off = 0;

        off += snprintf(line + off, sizeof(line) - (size_t)off, "hdr: +0x%02X:",
                        q * 8);
        for (k = q; k < q + 4 && k < n; k++)
            off += snprintf(line + off, sizeof(line) - (size_t)off,
                            " %016llX",
                            (unsigned long long)ReadQ(obj + (uint64_t)k * 8));
        Log("%s", line);
    }
}

/* Keep one vtable hit: what it is, where it came from, and everything in it
 * that could be a count. */
static void AddInv(uint64_t obj, const char *how) {
    MEMORY_BASIC_INFORMATION mbi;
    static const uint32_t offs[2] = { OFF_AMMO, OFF_AMMO_ALT };
    char label[24];
    int i, s;

    if (!obj || g_nInv >= INV_MAX) return;
    for (i = 0; i < g_nInv; i++)
        if (g_inv[i] == obj) return;
    g_inv[g_nInv++] = obj;
    InterlockedExchange(&g_nInvLive, g_nInv);

    Log("hit: %016llX  (%s)", (unsigned long long)obj, how);
    if (VirtualQuery((void *)(uintptr_t)obj, &mbi, sizeof(mbi)))
        Log("hit: region base %016llX size %llX type %X protect %X",
            (unsigned long long)(uintptr_t)mbi.BaseAddress,
            (unsigned long long)mbi.RegionSize, (unsigned)mbi.Type,
            (unsigned)mbi.Protect);
    LogOwner(obj);
    DumpObj(obj, 0x60);
    Log("hit: reflected class hash %08X", ShReflectClassHash(obj));

    for (s = 0; s < MAX_SLOT; s++) {
        for (i = 0; i < 2; i++) {
            uint64_t a = obj + (uint64_t)s * SLOT_STRIDE + offs[i];
            uint32_t v;

            if (!ShStatRead(a, &v)) continue;
            snprintf(label, sizeof(label), "slot%d+%03X", s, offs[i]);
            Log("slot: %s %016llX = %u", label, (unsigned long long)a, v);
            WatchAdd(a, label, 0);
        }
    }
    /* And whatever else in it decodes: the slot offsets come from the old
     * module, but this object may not be the one they were measured on. */
    ScanProtints(obj, "hit", 0);
}

/* ---- the objects at hand -------------------------------------------- */

typedef struct { uint64_t obj; const char *tag; } Cand;

static Cand g_cand[CAND_MAX];
static int  g_nCand;

static void AddCand(uint64_t obj, const char *tag) {
    uint64_t probe;
    int i;

    if (!obj || !Sane(obj) || (obj & 7)) return;
    if (!ShReadBytes(obj, &probe, 8)) return;      /* not an object */
    for (i = 0; i < g_nCand; i++)
        if (g_cand[i].obj == obj) return;
    if (g_nCand >= CAND_MAX) return;
    g_cand[g_nCand].obj = obj;
    g_cand[g_nCand].tag = tag;
    g_nCand++;
}

/* One line per component, with its class hash and whether its owner field
 * resolves to the player: if the inventory is one of them, this says so. */
static void LogComps(const char *who, uint64_t root, const ShComponent *comps,
                     int n) {
    int i;

    for (i = 0; i < n; i++)
        Log("find: %s comp %d hash=%08X obj=%016llX owner=%016llX", who, i,
            comps[i].classHash, (unsigned long long)comps[i].component,
            (unsigned long long)ResolveHandle(comps[i].component + OFF_OWNER));
}

static void Collect(uint64_t root, uint64_t ent) {
    static ShComponent comps[COMP_MAX];
    static int lastRootN = -1, lastEntN = -1;
    static uint64_t lastProj;
    uint64_t proj = 0, shooter = 0;
    ShShot shot[1];
    int n, i;

    g_nCand = 0;
    AddCand(root, "root");
    if (ent && ent != root) AddCand(ent, "ent");

    n = ShGetComponents(root, comps, COMP_MAX);
    if (n != lastRootN) {
        lastRootN = n;
        Log("find: root carries %d components", n);
        LogComps("root", root, comps, n);
    }
    for (i = 0; i < n; i++) AddCand(comps[i].component, "comp");

    if (ent && ent != root) {
        n = ShGetComponents(ent, comps, COMP_MAX);
        if (n != lastEntN) {
            lastEntN = n;
            Log("find: entity carries %d components", n);
            LogComps("ent", root, comps, n);
        }
        for (i = 0; i < n; i++) AddCand(comps[i].component, "comp");
    }

    /* The fire path's own objects, now that the hook is installed: a shot is
     * reported on the projectile's first step, so the newest record names a
     * projectile and the entity that fired it. */
    if (ShGetShots(shot, 1) == 1) {
        proj = shot[0].projectile;
        shooter = shot[0].shooter;
        AddCand(proj, "proj");
        AddCand(shooter, "fire");
    }
    if (proj != lastProj) {
        lastProj = proj;
        Log("find: fire path gave projectile %016llX shooter %016llX",
            (unsigned long long)proj, (unsigned long long)shooter);
    }
    InterlockedExchange(&g_candN, g_nCand);
}

/* Tier one: is any object at hand carrying the inventory's vtable, or holding
 * a pointer to one that does? The test is the vtable alone - the owner rule
 * is reported by AddInv, not required by it. */
static void FindNear(uint64_t root, int *tested) {
    static uint8_t buf[WIN_MAX];
    uint64_t off = (uint64_t)(LONG)g_base;
    int n = (int)g_len / 8;
    char how[32];
    int i, q;

    (void)root;
    *tested = 0;
    for (i = 0; i < g_nCand; i++) {
        uint64_t obj = g_cand[i].obj, v;

        if (ReadQ(obj) == INV_VTABLE) {
            snprintf(how, sizeof(how), "at hand (%s)", g_cand[i].tag);
            AddInv(obj, how);
        }
        if (!ShReadBytes(obj + off, buf, (uint32_t)g_len)) continue;
        for (q = 0; q < n; q++) {
            memcpy(&v, buf + (size_t)q * 8, 8);
            if (!Sane(v) || (v & 7)) continue;
            if (*tested >= PTR_MAX) {
                Log("find: pointer budget spent at %s %016llX +0x%llX",
                    g_cand[i].tag, (unsigned long long)obj,
                    (unsigned long long)(off + (uint64_t)q * 8));
                return;
            }
            (*tested)++;
            if (ReadQ(v) != INV_VTABLE) continue;
            snprintf(how, sizeof(how), "%s +0x%llX", g_cand[i].tag,
                     (unsigned long long)(off + (uint64_t)q * 8));
            Log("find: %s %016llX +0x%llX points at a vtable hit %016llX",
                g_cand[i].tag, (unsigned long long)obj,
                (unsigned long long)(off + (uint64_t)q * 8), (unsigned long long)v);
            AddInv(v, how);
        }
    }
}

/* ---- tier three: the sweep ------------------------------------------ */

/* The removed module's finder, with the cost measured rather than assumed:
 * every committed read/write region, chunked kernel reads, every aligned
 * qword compared against the needle. Two things it must not do, and rounds
 * three and four did both: it must not scan the thread running it - the
 * needle itself is spilled on that stack by Log's own varargs - and it must
 * not be the only thing asserting itself. The control needle is a value whose
 * exact address is already known (the player root holds ShEntityVtable() at
 * +0), so a run can tell "the game does not have it" apart from "the search
 * is broken". */
static void Sweep(uint64_t root) {
    static uint8_t buf[0x10000];
    char needle[24], ctl[24], addr[24];
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = (uint8_t *)0x10000;
    DWORD t0 = GetTickCount();
    uint64_t needleV = INV_VTABLE, ctlV = ShEntityVtable();
    uint64_t sLo = 0, sHi = 0, bytes = 0;
    int regions = 0, hits = 0, ctlHits = 0, ctlShown = 0, stacks = 0;

    OwnStack(&sLo, &sHi);
    /* The needle is written out as TEXT and passed as text: handing it to Log
     * as a 64-bit argument is what put it on the stack to be found. */
    snprintf(needle, sizeof(needle), "%016llX", (unsigned long long)needleV);
    snprintf(ctl, sizeof(ctl), "%016llX", (unsigned long long)ctlV);
    Log("sweep: needle %s, control %s (the player root holds it at +0)",
        needle, ctl);
    Log("sweep: this thread's stack %016llX..%016llX is skipped",
        (unsigned long long)sLo, (unsigned long long)sHi);

    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;

        if (next <= scan) break;
        if ((uint64_t)(uintptr_t)mbi.BaseAddress >= 0x800000000000ULL) break;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & PAGE_GUARD)) {
            uint8_t *b = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize, o, k, got;

            if ((uint64_t)(uintptr_t)b < sHi &&
                (uint64_t)(uintptr_t)b + sz > sLo) {
                stacks++;
                scan = next;
                continue;
            }
            regions++;
            /* Chunks overlap by the object head, so no candidate spans a
             * seam; a page freed mid scan simply fails and is skipped. */
            for (o = 0; o + 0x260 <= sz; o += sizeof(buf) - 0x260) {
                got = sz - o;
                if (got > sizeof(buf)) got = sizeof(buf);
                if (!ShReadBytes((uint64_t)(uintptr_t)(b + o), buf,
                                 (uint32_t)got))
                    continue;
                bytes += got;
                for (k = 0; k + 0x260 <= got; k += 8) {
                    uint64_t vt, obj = (uint64_t)(uintptr_t)(b + o + k);
                    char how[24];

                    memcpy(&vt, buf + k, 8);
                    if (vt == ctlV) {
                        ctlHits++;
                        if (ctlShown < 4) {
                            snprintf(addr, sizeof(addr), "%016llX",
                                     (unsigned long long)obj);
                            Log("sweep: control found at %s%s", addr,
                                obj == root ? "  (this is the player root)" : "");
                            ctlShown++;
                        }
                        continue;
                    }
                    if (vt != needleV) continue;
                    hits++;
                    snprintf(how, sizeof(how), "sweep hit %d", hits);
                    AddInv(obj, how);
                }
            }
        }
        scan = next;
    }
    Log("sweep: done, %llu MB in %lu ms over %d regions (%d stack regions "
        "skipped), needle hits %d, control hits %d",
        (unsigned long long)(bytes / 0x100000),
        (unsigned long)GetTickCount() - t0, regions, stacks, hits, ctlHits);
}

/* ---- the sub objects the holders point at ---------------------------- */

/* How the framework's own health module reaches the player's health: a
 * component's first 0x200 bytes carry pointers to SUB components, they are
 * not in the entity's own component array, and a sub component owned by the
 * player carries the player itself at +0x28. Health was found that way; the
 * weapon inventory may be a sibling of it, and no earlier round looked - they
 * read the components' own fields and never scanned their windows.
 *
 * Nothing here assumes which sub is which. Each one found is dumped, checked
 * against the health layout as a CONTROL (a shape already known to be right),
 * and searched for the shape a magazine has: a run of protected ints one slot
 * stride apart, every one of them a small number. Chance alignments do not
 * produce that run, so a run IS an ammo array - and every entry of it is then
 * watched, which is what firing will confirm. */

static uint64_t g_sub[SUB_MAX];
static int      g_nSub;

/* The raw window differ, defined below with the rest of that machinery: a
 * count need not be a protected int, so the objects found here are diffed
 * raw as well as decoded. */
static void WinAdd(uint64_t obj, const char *tag);

/* A run of small protected ints at the slot stride, starting here. Its length
 * is 0 when the address is not a stat or holds something too big to be a
 * count. */
static int AmmoRun(uint64_t addr, uint32_t *first) {
    uint32_t v;
    int n = 1;

    if (!ShStatRead(addr, &v)) return 0;
    if (v > ARR_MAXVAL) return 0;
    *first = v;
    while (n < MAX_SLOT) {
        uint32_t w;

        if (!ShStatRead(addr + (uint64_t)n * SLOT_STRIDE, &w)) break;
        if (w > ARR_MAXVAL) break;
        n++;
    }
    return n;
}

static void AnalyzeSub(uint64_t obj, const char *how) {
    MEMORY_BASIC_INFORMATION mbi;
    uint64_t off, best = 0;
    uint32_t mp = 0, hp = 0, first = 0;
    char label[24];
    int i, n, bestN = 0;

    if (!obj || g_nSub >= SUB_MAX) return;
    for (i = 0; i < g_nSub; i++)
        if (g_sub[i] == obj) return;             /* already done this one */
    g_sub[g_nSub++] = obj;
    WinAdd(obj, "sub");

    Log("sub: %016llX (%s)", (unsigned long long)obj, how);
    if (VirtualQuery((void *)(uintptr_t)obj, &mbi, sizeof(mbi)))
        Log("sub: region base %016llX size %llX type %X protect %X",
            (unsigned long long)(uintptr_t)mbi.BaseAddress,
            (unsigned long long)mbi.RegionSize, (unsigned)mbi.Type,
            (unsigned)mbi.Protect);
    DumpObj(obj, 0x40);

    /* The control: this is the layout the health module documents, so a hit
     * here proves the search reaches real player components. */
    if (ShReadBytes(obj + OFF_MAXHP, &mp, 4) && mp > 0 && mp <= 100000 &&
        ShStatRead(obj + OFF_HP, &hp) && hp <= mp)
        Log("sub: this one is the HEALTH component (max %u, now %u)", mp, hp);

    for (off = 0; off + SLOT_STRIDE <= (uint64_t)(LONG)g_len; off += 8) {
        n = AmmoRun(obj + off, &first);
        if (n < ARR_MIN) continue;
        Log("sub: ARRAY of %d at +0x%llX, first value %u", n,
            (unsigned long long)off, first);
        if (n > bestN) { bestN = n; best = off; }
    }
    if (best) {
        for (i = 0; i < bestN; i++) {
            uint64_t a = obj + best + (uint64_t)i * SLOT_STRIDE;

            snprintf(label, sizeof(label), "arr%d", i);
            WatchAdd(a, label, 1);
        }
        Log("sub: watching %d entries of it", bestN);
    }
    ScanProtints(obj, "sub", 0);
}

static void FindSubs(uint64_t root) {
    static uint8_t win[SUB_WINDOW];
    int i, q;

    for (i = 0; i < g_nCand; i++) {
        uint64_t c = g_cand[i].obj, p, owner;
        char how[40];

        if (!ShReadBytes(c, win, sizeof(win))) continue;
        for (q = 0; q + 8 <= SUB_WINDOW; q += 8) {
            memcpy(&p, win + q, 8);
            if (!Sane(p) || (p & 7)) continue;
            if (!ShReadBytes(p + OFF_SUBOWNER, &owner, 8)) continue;
            if (owner != root) continue;
            snprintf(how, sizeof(how), "%s +0x%X", g_cand[i].tag, q);
            WinAdd(c, g_cand[i].tag);            /* the holder itself too */
            AnalyzeSub(p, how);
            if (g_nSub >= SUB_MAX) {
                Log("sub: list is full at %d", SUB_MAX);
                return;
            }
        }
    }
}

/* ---- raw windows ----------------------------------------------------- */

/* +0x180 came back as the CAPACITY: 30 on one weapon and 20 on another, and
 * constant while firing - but it DOES follow the equipped weapon, which is
 * what makes this object the right place to look. The rounds themselves do
 * not have to be a protected int: health keeps its maximum as a plain one,
 * and a decode scan cannot see a plain number at all. So the objects that
 * matter are diffed RAW as well - one bulk read per object per poll, then a
 * qword by qword comparison, and only changes that look like a count are kept.
 *
 * Round two diffed raw windows and saw nothing; it was diffing the objects the
 * capacity function is called with, which is right, but it had no way to tell
 * a change that matters from a field that churns. This keeps both: the filter
 * below for shape, and one line per offset for anything that keeps moving. */

#define WIN_OBJ_MAX 48
#define BASE_SHOW   8          /* small numbers listed per object as a baseline */
#define CHG_SHOW    12         /* raw change lines per poll */

typedef struct {
    uint64_t obj;
    char     tag[20];
    uint8_t  prev[WIN_MAX];
} Win;

static Win     g_win[WIN_OBJ_MAX];
static int     g_nWin;
static uint8_t g_churn[WIN_OBJ_MAX][WIN_MAX / 8];

/* A move a count could make: both sides small, or one a step from the other.
 * Float bit patterns and pointers fail all three tests. */
static int CountShaped(uint64_t was, uint64_t now) {
    uint32_t o = (uint32_t)was, n = (uint32_t)now;

    if (o < 1000u && n < 1000u) return 1;
    if (o > n && o - n <= 4u && n < 100000u) return 1;      /* fired */
    if (n > o && n - o <= 128u && n < 100000u) return 1;    /* reloaded */
    return 0;
}

static void WinAdd(uint64_t obj, const char *tag) {
    static uint8_t buf[WIN_MAX];
    uint64_t base = obj + (uint64_t)(LONG)g_base;
    int i, q, shown = 0;

    if (!obj || g_nWin >= WIN_OBJ_MAX) return;
    for (i = 0; i < g_nWin; i++)
        if (g_win[i].obj == obj) return;
    if (!ShReadBytes(base, buf, (uint32_t)g_len)) return;

    g_win[g_nWin].obj = obj;
    snprintf(g_win[g_nWin].tag, sizeof(g_win[0].tag), "%s", tag);
    memcpy(g_win[g_nWin].prev, buf, (size_t)g_len);
    Log("raw: %016llX (%s) is diffed from +0x%lX", (unsigned long long)obj,
        tag, (long)g_base);
    /* Every small number in it, as a baseline: the count is one of these. */
    for (q = 0; q * 8 < (int)g_len && shown < BASE_SHOW; q++) {
        uint64_t v;

        memcpy(&v, buf + (size_t)q * 8, 8);
        if ((uint32_t)v > 999u) continue;
        Log("raw: %s +0x%llX = %u", tag,
            (unsigned long long)((uint64_t)(LONG)g_base + (uint64_t)q * 8),
            (unsigned)v);
        shown++;
    }
    g_nWin++;
}

static void WinPoll(uint32_t shots) {
    static uint8_t cur[WIN_MAX];
    int i, q, n = (int)g_len / 8, shown = 0, more = 0;

    for (i = 0; i < g_nWin; i++) {
        uint64_t base = g_win[i].obj + (uint64_t)(LONG)g_base;

        if (!ShReadBytes(base, cur, (uint32_t)g_len)) continue;
        for (q = 0; q < n; q++) {
            uint64_t old, now;

            memcpy(&old, g_win[i].prev + (size_t)q * 8, 8);
            memcpy(&now, cur + (size_t)q * 8, 8);
            if (old == now) { g_churn[i][q] = 0; continue; }
            if (g_churn[i][q]) continue;        /* keeps moving: said once */
            g_churn[i][q] = 1;
            if (!CountShaped(old, now)) continue;
            if (shown >= CHG_SHOW) { more++; continue; }
            Log("raw: %s +0x%llX %llu -> %llu (shots +%u)", g_win[i].tag,
                (unsigned long long)((uint64_t)(LONG)g_base + (uint64_t)q * 8),
                (unsigned long long)old, (unsigned long long)now,
                (unsigned)shots);
            shown++;
        }
        memcpy(g_win[i].prev, cur, (size_t)g_len);
    }
    if (more) Log("raw: ... (%d more this poll)", more);
}

/* ---- the sampler ---------------------------------------------------- */

/* Both are defined with the page below and used from the loop above. */
static const char *MarkName(LONG m);
static void RefreshStatus(void);

/* The objects at hand: the cheap pass, repeated while nothing is known. */
static void Identify(void) {
    ShPlayer p;
    uint64_t root;
    int tested = 0;

    if (!ShGetPlayer(&p)) { Log("find: no player yet"); return; }
    root = p.root ? p.root : p.entity;
    Collect(root, p.entity);
    Log("find: root %016llX entity %016llX, %d objects at hand",
        (unsigned long long)root, (unsigned long long)p.entity, g_nCand);
    FindNear(root, &tested);
    WinAdd(root, "root");
    FindSubs(root);
    Log("find: done, %d vtable hits, %d pointers tested, %d sub objects, "
        "%d numbers watched", g_nInv, tested, g_nSub, g_nWatch);
}

/* And every protected int that decodes in those objects. Its own pass, only
 * when asked: it is the expensive half (a few tens of thousands of reads). */
static void ScanAll(void) {
    int i;

    for (i = 0; i < g_nCand; i++)
        ScanProtints(g_cand[i].obj, g_cand[i].tag, 0);
    Log("pi:  scan done, %d numbers watched", g_nWatch);
}

/* The capacity hook is the only thing that exposes the engine's own capacity
 * arguments, and ShGetAmmoLook reports nothing without it: ask for 2/1 once,
 * put 1/1 straight back, and the game's own numbers stay in force while the
 * hook - and with it the arguments - stays visible. */
static void ArmOnce(void) {
    static int armed;
    int a, b;

    if (armed) return;
    armed = 1;
    a = ShSetAmmoScale(2, 1);
    b = ShSetAmmoScale(1, 1);
    Log("ap: capacity hook armed %d/%d (scale back to 1/1, game numbers "
        "untouched)", a, b);
    if (!a)
        Log("ap: could not install the capacity hook (%08x, %s)", ShLastError(),
            ShErrorString(ShLastError()));
}

/* ---- what a capacity argument IS ------------------------------------ */

static void FmtSlot(char *out, size_t cap, int ok, uint32_t v) {
    if (ok) snprintf(out, cap, "%u", v);
    else snprintf(out, cap, "-");
}

/* The identity of one capacity argument, printed once per object: its class
 * word, the owner handle the old finder insisted on (+0x250, pointer at
 * +0x00 and flags at +0x0C, live when the flags are negative), and BOTH
 * candidate slot arrays - eight protected ints at +0x130 and eight at +0x180,
 * one slot stride (0x28) apart. If one of these objects is the player's
 * weapon inventory then these sixteen numbers are the player's weapons (and
 * one of them is the number a plugin has to read); if none of them is, each
 * object is a single weapon and the two arrays say which offset carries its
 * rounds and which one is empty. */
static void LogShape(uint64_t obj, const char *tag) {
    ShPlayer p;
    uint64_t root = 0, h, res;
    int k;

    Log("shp: %s %016llX class %016llX", tag, (unsigned long long)obj,
        (unsigned long long)ReadQ(obj));
    if (ShGetPlayer(&p)) root = p.root ? p.root : p.entity;
    h = ReadQ(obj + OFF_OWNER);
    res = ResolveHandle(h);
    Log("shp: %s +0x%X %016llX -> %016llX | root %016llX %s", tag, OFF_OWNER,
        (unsigned long long)h, (unsigned long long)res,
        (unsigned long long)root,
        (root && res == root) ? "OWNED BY THE PLAYER" : "not the player");
    for (k = 0; k < MAX_SLOT; k++) {
        char a[12], b[12];
        uint32_t va = 0, vb = 0;
        int ha = ShStatRead(obj + OFF_AMMO_ALT + (uint64_t)k * SLOT_STRIDE, &va);
        int hb = ShStatRead(obj + OFF_AMMO + (uint64_t)k * SLOT_STRIDE, &vb);

        FmtSlot(a, sizeof(a), ha, va);
        FmtSlot(b, sizeof(b), hb, vb);
        Log("shp: %s slot %d  +0x130 = %-6s +0x180 = %-6s", tag, k, a, b);
    }
}

/* Does this object point at the player? Scanned rather than looked up at a
 * known offset, because the offsets are the open question in this build: a
 * plain pointer to the root or the entity, or a masked handle (a pointer with
 * negative flags at +0x0C) that resolves to either, is what says "this one is
 * his". Another entity's weapon has neither - and that would be exactly the
 * test a plugin needs for "which of the objects the engine asks about is the
 * player's weapon". */
static void LogRefs(uint64_t obj, const char *tag) {
    ShPlayer p;
    uint64_t root, ent;
    int q, n = 0;

    if (!ShGetPlayer(&p)) return;
    root = p.root ? p.root : p.entity;
    ent = p.entity;
    if (!root) return;
    Log("ref: %s %016llX root %016llX entity %016llX", tag,
        (unsigned long long)obj, (unsigned long long)root,
        (unsigned long long)ent);
    for (q = 0; q + 8 <= 0x400 && n < 12; q += 8) {
        uint64_t v = ReadQ(obj + (uint64_t)q), res;

        if (v == root || (ent && v == ent)) {
            Log("ref: %s +0x%03X = %016llX (%s)", tag, q,
                (unsigned long long)v,
                v == root ? "the player root" : "the entity");
            n++;
            continue;
        }
        if (!Sane(v)) continue;
        res = ResolveHandle(v);
        if (res == root || (ent && res == ent)) {
            Log("ref: %s +0x%03X = %016llX -> %016llX (%s)", tag, q,
                (unsigned long long)v, (unsigned long long)res,
                res == root ? "the player root" : "the entity");
            n++;
        }
    }
    if (!n) Log("ref: %s holds no reference to the player in its first 0x400",
                tag);
}

/* The objects the engine asks for a capacity. They are weapon shaped, they
 * are the closest thing to the inventory this framework can already reach,
 * and there are only about twenty of them a session - so every distinct
 * argument is decoded and then watched. */
static void ScanCapArgs(void) {
    static uint64_t seen[CAP_MAX];
    static uint32_t lastCount;
    static int nSeen;
    ShAmmoLook look;
    char label[24];
    int i, k, dup;

    if (!ShGetAmmoLook(&look)) return;
    if (look.count == lastCount) return;
    lastCount = look.count;
    Log("cap: look #%u raw %u args %016llX %016llX %016llX %016llX",
        (unsigned)look.count, (unsigned)look.raw,
        (unsigned long long)look.args[0], (unsigned long long)look.args[1],
        (unsigned long long)look.args[2], (unsigned long long)look.args[3]);

    for (i = 0; i < 4; i++) {
        uint64_t a = look.args[i];

        if (!Sane(a) || (a & 7)) continue;
        dup = 0;
        for (k = 0; k < nSeen; k++)
            if (seen[k] == a) dup = 1;
        if (dup) continue;
        if (nSeen >= CAP_MAX) {
            Log("cap: more than %d arguments - not following any more", nSeen);
            return;
        }
        seen[nSeen++] = a;
        snprintf(label, sizeof(label), "cap%d", nSeen - 1);
        WinAdd(a, label);                        /* the weapon: diff it raw */
        ScanProtints(a, label, 0);
        LogShape(a, label);
        LogRefs(a, label);
    }
}

/* ---- the order the engine asks in ----------------------------------- */

/* A weapon switch runs BOTH weapons through the capacity function - the one
 * going down and the one coming up - and nothing about those objects says
 * which is which. So the trace is the evidence: this prints every change of
 * the object being asked about, in order, with the rounds that object carries,
 * and on an object's first appearance its shape and its references too. A
 * switch is then a burst in the log: the order of the two objects in it is
 * what would let a plugin tell the weapon raised from the weapon stowed. */
static void TraceCalls(void) {
    static ShAmmoCall calls[32];
    static uint64_t shaped[CAP_MAX];
    static uint64_t lastSeq;
    static int nShaped;
    int n, i, k, seen;

    n = ShGetAmmoCalls(calls, 32);
    for (i = 0; i < n; i++) {
        char label[24];
        uint32_t v = 0;
        int have;

        if (calls[i].seq <= lastSeq) continue;
        lastSeq = calls[i].seq;
        if (!Sane(calls[i].obj)) continue;
        have = ShStatRead(calls[i].obj + OFF_AMMO, &v);
        if (!have) have = ShStatRead(calls[i].obj + OFF_AMMO_ALT, &v);
        Log("ord: #%llu %016llX +0x180 = %s%u",
            (unsigned long long)calls[i].seq, (unsigned long long)calls[i].obj,
            have ? "" : "?", have ? v : 0);

        seen = 0;
        for (k = 0; k < nShaped; k++)
            if (shaped[k] == calls[i].obj) seen = 1;
        if (seen || nShaped >= CAP_MAX) continue;
        shaped[nShaped++] = calls[i].obj;
        snprintf(label, sizeof(label), "ord%d", nShaped - 1);
        LogShape(calls[i].obj, label);
        LogRefs(calls[i].obj, label);
        WinAdd(calls[i].obj, label);
        ScanProtints(calls[i].obj, label, 0);
        WatchAdd(calls[i].obj + OFF_AMMO, label, 1);
    }
}

/* The engine hands us the weapon whenever it computes a capacity, and the
 * offsets off that object are the old module's own: the rounds are a protected
 * int at +0x180, or +0x130 for the weapons that keep them there. Round six's
 * log found exactly that on four different weapons - 30, 30, 30, 20 - so this
 * reads it on every poll and reports every change, which is what says whether
 * the number is the rounds or the capacity. */
static void TrackAmmo(void) {
    ShAmmoLook look;
    uint64_t obj, addr = 0;
    uint32_t v = 0;

    if (!ShGetAmmoLook(&look)) return;
    obj = look.args[0];
    if (!Sane(obj)) return;
    if (ShStatRead(obj + OFF_AMMO, &v)) addr = obj + OFF_AMMO;
    else if (ShStatRead(obj + OFF_AMMO_ALT, &v)) addr = obj + OFF_AMMO_ALT;
    else return;

    if (addr != g_ammoAddr) {
        g_ammoAddr = addr;
        InterlockedExchange(&g_ammoVal, (LONG)v);
        Log("ammo: %016llX +0x%llX = %u", (unsigned long long)obj,
            (unsigned long long)(addr - obj), v);
        WatchAdd(addr, "weapon", 1);
        return;
    }
    if ((LONG)v != g_ammoVal) {
        Log("ammo: %016llX +0x%llX %u -> %u", (unsigned long long)obj,
            (unsigned long long)(addr - obj), (unsigned)g_ammoVal, v);
        InterlockedExchange(&g_ammoVal, (LONG)v);
    }
}

static void Poll(void) {
    static uint32_t lastShots;
    static uint32_t polls, lastFind;
    static int armed;
    ShPlayer p;
    uint64_t root;
    uint32_t shots = ShShotCount(), delta;
    LONG act, m;
    int first;

    first = (polls == 0);
    polls++;

    /* The projectile hook is not installed until somebody asks for it, and
     * this build's projectile site is stale (see the header), so this may
     * well report nothing at all - the marker rows carry the correlation
     * either way. */
    if (!armed) {
        armed = 1;
        Log("ap: projectile hook install %d, ready %d", ShHitHookInstall(),
            ShHitHookReady());
        ArmOnce();
    }

    if (!ShGetPlayer(&p)) return;
    root = p.root ? p.root : p.entity;
    delta = shots - lastShots;
    if (delta && !first) {
        ShShot shot[1];
        int mine = (ShGetShots(shot, 1) == 1) && shot[0].byPlayer;

        Log("shots: +%u (total %u)%s", (unsigned)delta, (unsigned)shots,
            mine ? ", the last one mine" : "");
    }
    lastShots = shots;
    InterlockedExchange(&g_shots, (LONG)shots);

    m = InterlockedExchange(&g_mark, 0);
    if (m) Log("mark %s (shots %u, hits %d)", MarkName(m), (unsigned)shots,
               g_nInv);

    act = InterlockedExchange(&g_act, 0);
    if (polls == 1) {                   /* once, when the switch goes on */
        Identify();
        ScanAll();
    } else if (act == 1) {              /* the cheap pass, on demand */
        Identify();
        ScanAll();
    } else if (act == 2) {              /* the sweep, on demand */
        Sweep(root);
        Log("find: after the sweep, %d vtable hits, %d numbers watched",
            g_nInv, g_nWatch);
    } else if (!g_nInv && polls - lastFind >= 16) {
        lastFind = polls;
        Identify();
    }

    ScanCapArgs();
    TraceCalls();
    TrackAmmo();
    WinPoll(delta);
    WatchPoll(delta);
    RefreshStatus();
}

/* ---- the page ------------------------------------------------------- */

enum { ROW_ON = 1, ROW_FIND, ROW_SWEEP, ROW_M1, ROW_M2, ROW_M3 };

static const char *MarkName(LONG m) {
    switch (m) {
    case ROW_M1: return "SHOT";
    case ROW_M2: return "RELOAD";
    case ROW_M3: return "CRATE";
    default:     return "?";
    }
}

static void OnRow(uint32_t menu, uint32_t item, int value, void *user) {
    int which = (int)(intptr_t)user;

    (void)menu; (void)item;

    if (which == ROW_ON) {
        InterlockedExchange(&g_on, value ? 1 : 0);
        SaveInt("enabled", value ? 1 : 0);
        Log("ap: probe %s", value ? "on" : "off");
        return;
    }
    if (which == ROW_FIND || which == ROW_SWEEP) {
        InterlockedExchange(&g_act, which == ROW_FIND ? 1 : 2);
        return;
    }
    InterlockedExchange(&g_mark, which);
}

static void BuildMenu(void) {
    ShMenuHint(g_menu, "@ap.hint");
    ShMenuToggle(g_menu, "@ap.on", (int)g_on, OnRow, (void *)(intptr_t)ROW_ON);
    ShMenuAction(g_menu, "@ap.find", OnRow, (void *)(intptr_t)ROW_FIND);
    ShMenuAction(g_menu, "@ap.sweep", OnRow, (void *)(intptr_t)ROW_SWEEP);
    ShMenuAction(g_menu, "@ap.m1", OnRow, (void *)(intptr_t)ROW_M1);
    ShMenuAction(g_menu, "@ap.m2", OnRow, (void *)(intptr_t)ROW_M2);
    ShMenuAction(g_menu, "@ap.m3", OnRow, (void *)(intptr_t)ROW_M3);
    ShMenuStatus(g_menu, "@ap.st.none");
}

static void RefreshStatus(void) {
    LONG ammo = g_ammoVal;

    if (!ShMenuIsShowing(g_menu)) return;
    if (!g_on) { ShMenuStatus(g_menu, "@ap.st.none"); return; }
    ShMenuStatusF(g_menu, "@ap.status", (unsigned)(ammo < 0 ? 0 : ammo),
                  g_nWin, g_nWatch, (unsigned)g_shots);
}

/* ---- the plugin's own name ------------------------------------------ */

/* From the module path, like every other plugin here: the ini, the log and
 * the text owner all follow the file name. */
static void NameFromModule(HINSTANCE inst) {
    char mod[MAX_PATH];
    char *base, *dot;
    size_t n;

    g_name[0] = 0;
    if (!inst || !GetModuleFileNameA(inst, mod, sizeof(mod))) return;
    base = strrchr(mod, '\\');
    base = base ? base + 1 : mod;
    dot = strrchr(base, '.');
    n = dot ? (size_t)(dot - base) : strlen(base);
    if (n == 0 || n >= sizeof(g_name)) { g_name[0] = 0; return; }
    memcpy(g_name, base, n);
    g_name[n] = 0;
}

/* ---- text ------------------------------------------------------------ */

static const ShText kEn[] = {
    { "@ap.page",    "Ammo probe" },
    { "@ap.hint",    "Read only. Switch on, then switch weapons once or twice: "
                     "the engine asks for a capacity then, and the object it "
                     "hands over follows the equipped weapon. Then fire a burst "
                     "of several rounds, reload and take a crate, pressing the "
                     "matching marker each time - every number that moves is in "
                     "logs\\AmmoProbe.log" },
    { "@ap.on",      "Probe on" },
    { "@ap.find",    "Find what is at hand (fast)" },
    { "@ap.sweep",   "Sweep all memory for it (slow)" },
    { "@ap.m1",      "Mark: single shot" },
    { "@ap.m2",      "Mark: reloaded" },
    { "@ap.m3",      "Mark: ammo crate" },
    { "@ap.status",  "obj+180 %u  raw %d  pi %d  shots %u" },
    { "@ap.st.none", "off - nothing is read" }
};

static const ShText kZh[] = {
    { "@ap.page",    "弹药探针" },
    { "@ap.hint",    "只读取证。打开开关后换一两次枪 —— 引擎这时才会问容量，"
                     "交出的对象跟着当前武器走。然后连打几发、换弹、补给，"
                     "各点一次对应打点行；每一个会动的数都记在 "
                     "logs\\AmmoProbe.log" },
    { "@ap.on",      "探针开关" },
    { "@ap.find",    "立即查找手头对象（快）" },
    { "@ap.sweep",   "全内存扫描一次（慢）" },
    { "@ap.m1",      "打点：开了一枪" },
    { "@ap.m2",      "打点：换弹了" },
    { "@ap.m3",      "打点：补给了" },
    { "@ap.status",  "对象+180 %u  原始 %d  解码 %d  枪声 %u" },
    { "@ap.st.none", "未开启 —— 不读任何东西" }
};

/* ---- start up ------------------------------------------------------- */

static DWORD WINAPI ProbeThread(LPVOID param) {
    char logFile[80];

    NameFromModule((HINSTANCE)param);
    if (!g_name[0]) strcpy(g_name, "AmmoProbe");

    snprintf(logFile, sizeof(logFile), "%s.log", g_name);
    LogInitAlways(logFile);

    while (!GetModuleHandleA("dinput8.dll")) Sleep(500);

    if (!ShPluginIniPath(g_name, g_ini, sizeof(g_ini)))
        g_ini[0] = 0;
    LoadSettings();

    ShLangDeclare(g_name, "en-US", kEn, (int)(sizeof(kEn) / sizeof(kEn[0])));
    ShLangDeclare(g_name, "zh-CN", kZh, (int)(sizeof(kZh) / sizeof(kZh[0])));

    g_menu = ShMenuCreate("@ap.page");
    if (!g_menu) {
        Log("ap: no menu page (%08x) - nothing to drive", ShLastError());
        return 0;
    }
    BuildMenu();
    Log("ap: page up, probe is %s (window +0x%lX len 0x%lX), inventory vtable "
        "%016llX", g_on ? "on" : "off", (long)g_base, (long)g_len,
        (unsigned long long)INV_VTABLE);

    for (;;) {
        Sleep(250);
        if (!g_on) continue;
        if (ShGetGameState() != SH_STATE_INGAME &&
            ShGetGameState() != SH_STATE_PAUSED)
            continue;
        Poll();
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    HANDLE h;

    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    DisableThreadLibraryCalls(inst);
    h = CreateThread(NULL, 0, ProbeThread, (LPVOID)inst, 0, NULL);
    if (h) CloseHandle(h);
    return TRUE;
}
