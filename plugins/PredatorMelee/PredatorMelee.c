/* PredatorMelee - a crowd of one archetype, for watching a fight.
 *
 * The archetype is a setting, not a constant: [Settings] id in this plugin's
 * own ini, defaulting to the Predator. Everything else is a row: how many per
 * summon, how far away, how much health they arrive with, how many may stand at
 * once, and a 60-second cooldown between summons. The shape, the facing and the
 * angles are fixed and are not rows: random scatter, a spin step of 360 / count,
 * and 40 degrees further round the player per batch, all of which the ini
 * explains.
 *
 * The batch itself is the framework's own job API, not a loop of ShSpawnNpc:
 * ShNpcSpawnBegin starts a worker of the API's own and ShNpcSpawnPoll hands back
 * the entities as they appear. That matters here more than elsewhere -
 * ShSpawnNpc blocks until the entity exists and waits on the physics pump, which
 * runs on the game thread, so a batch of ten summoned from a menu callback would
 * deadlock the frame it was pressed in.
 *
 * Two things the framework cannot do, and the page says so rather than
 * pretending otherwise:
 *
 *   - Health is a fraction of the maximum, 0.1 to 1.0. The maximum itself is
 *     readable and not writable (scripthook_health.c reads OFF_MAXHP and never
 *     writes it), so "ten times as tough" is not a setting this can have.
 *     Arriving weakened is what a fight wants anyway.
 *   - Who fights whom is the engine's business. The faction is private and no
 *     API exposes it, so this plugin can put a crowd in one place and no more
 *     than that: whether they turn on each other is the AI's decision.
 *
 * Health is written once per entity, the moment it first appears, and never
 * again: EnemyReinforce's header records that hardening what it spawned used
 * to freeze the game (scripthook_health.c, the component lookup), and a single
 * write guarded by that history is the smaller thing to do. Entities that
 * arrive at full health are not written to at all. The one other write is the
 * health-zero row, which asks for it and then checks a second later whether it
 * took.
 *
 * What a summon was refused for, and when one finishes, is said twice: a line in
 * the log, which is the record, and a toast on screen, because a row that does
 * nothing when it is pressed reads as a freeze. The field count behind the
 * ceiling and the status line is a health read over what this page made, four
 * times a second, so a kill shows up while the operator is still looking.
 *
 * The ini is an override, never a requirement. The id has a default in this
 * file - PM_DEF_ID is the Predator's own archetype, not a placeholder waiting
 * to be filled in - so an ini that is missing, unreadable, or has that line
 * blanked still summons what the plugin always summons. A file that is not
 * there at all is written once at load, so
 * the operator has something to edit.
 *
 * 2026-09-28: the rows write back. Until today the file was read and left
 * alone - the reasoning was that a plugin should not rewrite the file the
 * operator keeps their notes in - and the field reported what that means in
 * practice: every setting changed on the page was gone the next time the game
 * started. So a row marks the file dirty and the worker writes it out, at most
 * once every PM_SAVE_MS (a slider dragged across its range is one write, not one
 * per step). Comments in this file are lost when that happens, which is why the
 * settings this plugin ships are a bare list; anything else an operator adds to
 * it is theirs to re-add.
 *
 * 2026-09-27: before this, the ini was read-only and the id lived in it and
 * nowhere else. A package that dropped that one file therefore shipped a plugin
 * that could not summon anything, said so only in the log, and looked broken to
 * the player who pressed the row - which is exactly what happened.
 *
 * Rules this follows, from the plugins that came before it:
 *   - the page is built either way; [Settings] enabled decides whether the
 *     summon row goes through, and the row at the top of the page flips that
 *     for the session (it used to decide whether the page existed at all,
 *     which left a plugin nobody could find and no way to switch on in the
 *     game). The default is 0 (2026-09-27, on request): a row that spawns
 *     hostiles on a keypress is not something to hand a player who has not
 *     asked for it, and the switch that turns it on is the first row of the
 *     page this plugin just put up;
 *   - nothing engine-side is called from a menu callback except the job API,
 *     which is made for it; the health write runs in a worker, and the reads
 *     run on the frame hook, which is the game thread;
 *   - every string a player reads goes through @pm keys declared for en-US and
 *     zh-CN, so lang.ini only carries the rows it changes.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "scripthook.h"
#include "log.h"

#define PM_OWNER    "PredatorMelee"
#define PM_TICK_MS  100
#define PM_POLL_MS  120          /* how often the frame hook asks the job */

/* ---- the plugin's text -------------------------------------------------- */

static const ShText kEn[] = {
    { "@pm.page",         "Predator melee" },
    { "@pm.hint",         "Summons fight you. Consecutive summons must be 60 s apart." },
    { "@pm.enabled",      "Summoning enabled" },
    { "@pm.summon",       "Summon the Predator (60 s cooldown)" },
    { "@pm.toast.off",    "The summon switch is off - the row at the top of this page turns it on." },
    { "@pm.toast.noid",   "Nothing to summon: no archetype id - see the log." },
    { "@pm.toast.cd",     "Summon on cooldown: %d s to go." },
    { "@pm.toast.cap",    "The field is full (%d of %d) - zero their health, or raise the ceiling." },
    { "@pm.toast.busy",   "The last batch is still arriving." },
    { "@pm.toast.ok",     "Summoned: %d this batch, %d so far." },
    { "@pm.toast.fail",   "Nothing came out this time (see the log). %d so far." },
    { "@pm.cap",          "Most on the field at once" },
    { "@pm.count",        "Per summon" },
    { "@pm.distance",     "Distance ahead" },
    { "@pm.hp",           "Arrival health" },
    { "@pm.unit.m",       " m" },
    { "@pm.unit.pct",     "%" },
    { "@pm.kill",         "Zero the health of what is out (one shot drops them)" },
    { "@pm.st",           "summon CD %d s | %.0f m | hp %.0f%% | summoned %d | alive %d" },
    { "@pm.st.busy",      "summon CD %d s | making %d of %d | summoned %d | alive %d" }
};

static const ShText kZh[] = {
    { "@pm.page",         "铁血战士大混战" },
    { "@pm.hint",         "召唤铁血战士与你战斗。连续召唤的间隔必须 > 60 秒。" },
    { "@pm.enabled",      "启用召唤" },
    { "@pm.summon",       "召唤铁血战士（CD 60s）" },
    { "@pm.toast.off",    "召唤开关是关的——打开本页最上面那一行" },
    { "@pm.toast.noid",   "没有可召唤的对象：id 没设（见日志）" },
    { "@pm.toast.cd",     "召唤冷却中，还剩 %d 秒可召唤" },
    { "@pm.toast.cap",    "场上已满（%d/%d）—— 清零它们的血量，或抬高上限" },
    { "@pm.toast.busy",   "上一批还在出" },
    { "@pm.toast.ok",     "召唤成功（本次 %d 个，累计 %d 个）" },
    { "@pm.toast.fail",   "这一次没有出来（看日志），累计 %d 个" },
    { "@pm.cap",          "召唤数量上限" },
    { "@pm.count",        "每次召唤数量" },
    { "@pm.distance",     "出场距离" },
    { "@pm.hp",           "出场血量" },
    { "@pm.unit.m",       " 米" },
    { "@pm.unit.pct",     "%" },
    { "@pm.kill",         "已召唤的血量清零（一枪即倒）" },
    { "@pm.st",           "召唤CD %ds | %.0f 米 | 血量 %.0f%% | 已召唤 %d | 存活 %d" },
    { "@pm.st.busy",      "召唤CD %ds | 正在出 %d / %d | 已召唤 %d | 存活 %d" }
};

/* ---- settings ----------------------------------------------------------- */

/* One shape, one facing, one set of angles: the rows that chose them are gone.
 * Random scatter is what a crowd looks like, and two constants keep consecutive
 * summons from landing on the same spot - a fixed 40 degrees round the player
 * each time (the framework's own note names 40 as the value that puts nine
 * batches round the circle), and a spin step of 360 / count so the members of
 * one batch fan out instead of stacking on one heading. */
#define PM_SPREAD_DEG 40.0f
#define PM_CD_MS      60000        /* between summons, as the hint says */
#define PM_COUNT_MAX  6            /* per summon                        */
#define PM_CAP_MIN    6
#define PM_CAP_MAX    30
#define PM_CAP_DEF    20
#define PM_DIST_MIN   5.0f         /* metres ahead of the player        */
#define PM_DIST_MAX   50.0f
#define PM_DIST_STEP  5.0f
#define PM_ENT_MAX    256

/* What this plugin summons when the ini says nothing - which includes the case
 * of no ini at all. The id is the Predator's archetype in this build, the same
 * one the shipped ini carries: the file overrides these values, it is not what
 * makes the plugin work. */
#define PM_DEF_ID   0x0154BBB495E1ULL

/* There is deliberately no built-in name to go with it: [Settings] name is the
 * operator's own note for whichever id they point this at, printed beside the id
 * in the log, and a plugin has no business inventing one. */

static char     g_ini[MAX_PATH];
static volatile LONG g_on;           /* [Settings] enabled, and the row at the
                                      * top of the page: 0 = summons refuse */
static int      g_idFromIni;         /* the ini named an id; the log says which */
static uint64_t g_id = PM_DEF_ID;
static char     g_name[64];          /* the operator's note for the id, or "" */
static int      g_count = 1;         /* how many per summon       */
static int      g_cap = PM_CAP_DEF;  /* most on the field at once */
static float    g_distM = PM_DIST_MIN;
static float    g_hp = 0.5f;         /* arrival health fraction   */
static volatile int g_live;          /* made, and still up; counted on the
                                      * worker, read here and by the menu */
static ULONGLONG g_liveAt;           /* when the worker was last asked */
static ULONGLONG g_cdUntil;          /* no summon before this     */

/* ---- the batch ---------------------------------------------------------- */

/* Written by the menu thread when a batch is asked for, read by the frame
 * callback that polls it. volatile because the two are different threads and the
 * ordering matters: see the publish order in OnSummon. */
static volatile uint32_t g_job;
static int      g_jobMade;           /* entities the live job has produced */
static int      g_lastMade;          /* what the last finished batch made  */
static uint32_t g_menu;
static int      g_menuReady;

static uint64_t g_ent[PM_ENT_MAX];
/* Written by the frame callback (PollJob), read by the worker and by the menu
 * thread. The handle goes into its slot first and this count is published after
 * it (InterlockedIncrement is a full barrier), so a reader that sees the new
 * count sees the handle too; a reader that sees the count from before it was
 * published is one entity short, which every loop here already allows for. */
static volatile LONG g_entCount;
static int      g_applied;           /* how many have had health applied */
static volatile int g_entFull;       /* set by the frame callback, cleared by
                                      * the menu thread's next summon */
static ULONGLONG g_lastPoll;
static ULONGLONG g_jobAt;            /* when the live job was asked for; 0 while
                                      * the job is being handed over */
static volatile LONG g_kill;         /* the worker is asked to zero health  */
static volatile LONG g_killCheck;    /* the worker looks afterwards        */
static volatile LONG g_killDone;     /* ...and the frame callback reports  */
static volatile LONG g_killUp;       /* how many were up when it looked    */
static volatile LONG g_liveWanted;   /* the frame asks the worker to count  */
static ULONGLONG g_killAt;

/* ---- binding ------------------------------------------------------------ */

/* Late bound on purpose: a plugin that calls an export the running dinput8
 * does not have must say so rather than fail to load. Eight exports, and each
 * one is here because a row needs it: the batch (begin/poll/end), the layout
 * policy behind the placement, health (read for the arrival figure and the field
 * count, written for the health-zero row), the frame hook the batch is polled
 * from, and the toast a refused or finished summon answers with. Nothing else is
 * bound: the rows that needed ShNpcSpawnCancel, ShDespawn, ShSetGodModePlayer
 * and ShSetVisibility are gone, and the calls went with them. */
typedef uint32_t (*t_begin)(const ShNpcSpawnRequest *);
typedef int      (*t_poll)(uint32_t, uint64_t *, int, int *);
typedef int      (*t_end)(uint32_t);
typedef int      (*t_layout)(const ShNpcSpawnLayout *);
typedef int      (*t_hpGet)(uint64_t, uint32_t *, uint32_t *);
typedef int      (*t_hpSet)(uint64_t, uint32_t);
typedef int      (*t_frameCb)(void (*)(void *), void *);
typedef uint32_t (*t_toast)(const char *, uint32_t, uint32_t);

static t_begin     p_begin;
static t_poll      p_poll;
static t_end       p_end;
static t_layout    p_layout;
static t_hpGet     p_hpGet;
static t_hpSet     p_hpSet;
static t_frameCb   p_frameCb;
static t_toast     p_toast;

/* One line on screen, through the toast above. It is defined with the menu and
 * used by both the summon row and the batch poll, so it is declared here. */
static void Say(const char *key, uint32_t rgb, int a, int b);

static int Bind(void) {
    HMODULE m = GetModuleHandleA("dinput8.dll");
    if (!m) return 0;
    *(FARPROC *)&p_begin     = GetProcAddress(m, "ShNpcSpawnBegin");
    *(FARPROC *)&p_poll      = GetProcAddress(m, "ShNpcSpawnPoll");
    *(FARPROC *)&p_end       = GetProcAddress(m, "ShNpcSpawnEnd");
    *(FARPROC *)&p_layout    = GetProcAddress(m, "ShNpcSpawnSetLayout");
    *(FARPROC *)&p_hpGet     = GetProcAddress(m, "ShGetHealthEntity");
    *(FARPROC *)&p_hpSet     = GetProcAddress(m, "ShSetHealthEntity");
    *(FARPROC *)&p_frameCb   = GetProcAddress(m, "ShRegisterFrameCallback");
    /* Wanted, not required: a refused summon is answered in the log either way,
     * and the row works without the toast. */
    *(FARPROC *)&p_toast     = GetProcAddress(m, "ShToastEx");
    return p_begin && p_poll && p_end;
}

/* ---- the plugin's own ini, read only ------------------------------------ */

static int IniInt(const char *key, int def) {
    if (!g_ini[0]) return def;
    return GetPrivateProfileIntA("Settings", key, def, g_ini);
}

static float IniFloat(const char *key, float def) {
    char buf[64];
    buf[0] = 0;
    if (g_ini[0]) GetPrivateProfileStringA("Settings", key, "", buf,
                                           (int)sizeof(buf), g_ini);
    if (!buf[0]) return def;
    return (float)atof(buf);
}

static void IniStr(const char *key, char *out, int n) {
    out[0] = 0;
    if (g_ini[0]) GetPrivateProfileStringA("Settings", key, "", out, n, g_ini);
}

/* The ini this plugin reads, written once when it is not there at all: the keys
 * and nothing else. The page carries the labels and the ranges, so prose in the
 * file would be a second place to keep in step and one more thing to read; the
 * file is storage. An ini that exists is never rewritten, so this cannot undo a
 * setting, and the values written are the built-in ones - the plugin runs the
 * same way with the file deleted. */
static void SeedIni(void) {
    static const char kBody[] =
        "[Settings]\n"
        "enabled=0\n"
        "id=0x0154BBB495E1\n"
        "count=1\n"
        "cap=20\n"
        "distance=5\n"
        "hp=0.5\n";
    FILE *f;

    if (!g_ini[0]) return;
    if (GetFileAttributesA(g_ini) != INVALID_FILE_ATTRIBUTES) return;
    f = fopen(g_ini, "wb");
    if (!f) {
        Log("pm: no ini at %s and it cannot be written, so the built-in "
            "settings are used", g_ini);
        return;
    }
    fputs(kBody, f);
    fclose(f);
    Log("pm: no ini at %s, so one was written from the built-in settings",
        g_ini);
}

static void ReadSettings(void) {
    char buf[64];
    int v;

    /* An empty key means "keep the built-in value", not "no id". A missing ini,
     * an ini with no [Settings] section, and a line somebody blanked all have to
     * mean the same thing: what this plugin summons cannot depend on a file
     * being shipped, unpacked and readable. */
    IniStr("id", buf, (int)sizeof(buf));
    if (buf[0]) {
        g_id = (uint64_t)_strtoui64(buf, NULL, 16);
        g_idFromIni = 1;
    }
    IniStr("name", buf, (int)sizeof(buf));
    if (buf[0]) {
        strncpy(g_name, buf, sizeof(g_name) - 1);
        g_name[sizeof(g_name) - 1] = 0;
    }

    v = IniInt("count", g_count);
    g_count = v < 1 ? 1 : (v > PM_COUNT_MAX ? PM_COUNT_MAX : v);
    v = IniInt("cap", g_cap);
    g_cap = v < PM_CAP_MIN ? PM_CAP_MIN : (v > PM_CAP_MAX ? PM_CAP_MAX : v);
    v = IniInt("distance", (int)g_distM);
    g_distM = (float)v;
    if (g_distM < PM_DIST_MIN) g_distM = PM_DIST_MIN;
    if (g_distM > PM_DIST_MAX) g_distM = PM_DIST_MAX;
    g_hp = IniFloat("hp", 0.5f);
    if (g_hp < 0.1f) g_hp = 0.1f;
    if (g_hp > 1.0f) g_hp = 1.0f;
}

/* ---- what the page is doing -------------------------------------------- */

/* Seconds of cooldown left, 0 when a summon may go ahead. */
static int CdLeft(void) {
    ULONGLONG now = GetTickCount64();

    if (!g_cdUntil || now >= g_cdUntil) return 0;
    return (int)((g_cdUntil - now + 999) / 1000);
}

/* One line, exactly what the operator asked to see: summon CD | id | distance |
 * health | how many of what this page made are still up. */
static void RefreshStatus(void) {
    int cd = CdLeft();

    if (!g_menuReady) return;
    if (g_job)
        ShMenuStatusF(g_menu, "@pm.st.busy", cd, g_jobMade, g_count,
                      g_entCount, g_live);
    else
        ShMenuStatusF(g_menu, "@pm.st", cd, g_distM, g_hp * 100.0f,
                      g_entCount, g_live);
}

/* ---- health, applied once per entity ------------------------------------ */

/* Called from the frame hook, on the game thread, the moment a new entity
 * shows up. Once each: see the header for why this is not a loop that keeps
 * re-applying itself, and why an entity that arrived at full health is not
 * touched at all. */
static void ApplyToNew(void) {
    int i;

    if (!g_applied && !g_entCount) return;
    for (i = g_applied; i < g_entCount; i++) {
        uint32_t cur = 0, max = 0;
        uint64_t e = g_ent[i];

        if (g_hp < 0.999f && p_hpGet && p_hpSet) {
            if (!p_hpGet(e, &cur, &max) || !max) {
                Log("pm: entity %llX: its health could not be read, so it "
                    "arrives at whatever it has", (unsigned long long)e);
                continue;
            }
            {
                uint32_t want = (uint32_t)((float)max * g_hp);
                if (want < 1) want = 1;
                if (p_hpSet(e, want))
                    Log("pm: entity %llX arrives at %u of %u hp (%.1f)",
                        (unsigned long long)e, want, max, g_hp);
                else
                    Log("pm: entity %llX: health could not be set (%08X, %s)",
                        (unsigned long long)e, ShLastError(),
                        ShErrorString(ShLastError()));
            }
        }
    }
    g_applied = g_entCount;
}

/* Remember one entity if it is not already here. The job hands back the whole
 * list every time, so the same entity comes back on every poll. */
static void Remember(uint64_t e) {
    int i;
    if (!e) return;
    for (i = 0; i < g_entCount; i++) if (g_ent[i] == e) return;
    if (g_entCount < PM_ENT_MAX) {
        /* The handle in its slot first, then the count: this is the publication
         * the worker and the menu thread read. */
        g_ent[g_entCount] = e;
        InterlockedIncrement(&g_entCount);
    } else if (!g_entFull) {
        g_entFull = 1;
        Log("pm: %d entities is as many as this page keeps track of; anything "
            "made past that is not counted in the field figure or against the "
            "ceiling", PM_ENT_MAX);
    }
}

static void PollJob(void) {
    uint64_t got[PM_ENT_MAX];
    int n, done = 0, i;

    if (!g_job) return;
    n = p_poll(g_job, got, PM_ENT_MAX, &done);
    if (n < 0) {                     /* the job id is no longer live */
        g_job = 0;
        RefreshStatus();
        return;
    }
    for (i = 0; i < n; i++) Remember(got[i]);
    g_jobMade = n;
    ApplyToNew();
    if (done) {
        /* The success line, said at the moment it is true: a job being accepted
         * is not a summon, and this is the first point at which the batch has
         * actually produced anything. Two numbers, so it carries both what this
         * batch made and the running total this page has made. 0 is not a
         * success, and that case says so instead. */
        if (n > 0)
            Say("@pm.toast.ok", 0x66CC66u, n, g_entCount);
        else
            Say("@pm.toast.fail", 0xFFCC33u, 0, g_entCount);
        Log("pm: the batch is done: %d of the %d asked for", n, g_count);
        g_lastMade = n;
        if (!p_end(g_job))
            Log("pm: the finished job would not end; it will be freed before "
                "the next batch");
        g_job = 0;
    }
    RefreshStatus();
}

/* ---- the menu ----------------------------------------------------------- */

/* The conversion specifiers of a format string, in order and nothing else: "%d
 * of %d" and "%d 个 %d" come out the same, and "%%" comes out as nothing. What
 * has to match between a template and a translation is the arguments they read,
 * and for this plugin's two integers that is "two integer conversions, in
 * order". */
static int Conversions(const char *fmt, char *out, size_t cap) {
    size_t n = 0;

    if (cap) out[0] = 0;
    while (fmt && *fmt) {
        if (*fmt != '%') { fmt++; continue; }
        fmt++;
        if (*fmt == '%') { fmt++; continue; }          /* a literal percent */
        while (*fmt && strchr("-+ #0", *fmt)) fmt++;   /* flags   */
        while (*fmt >= '0' && *fmt <= '9') fmt++;      /* width   */
        if (*fmt == '.') {
            fmt++;
            while (*fmt >= '0' && *fmt <= '9') fmt++;  /* precision */
        }
        while (*fmt && strchr("hljztL", *fmt)) fmt++;  /* length  */
        if (!*fmt) break;
        if (n + 1 < cap) out[n++] = *fmt;              /* the conversion */
        fmt++;
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    return (int)n;
}

/* The en-US text this plugin declares for a key: a format string known at build
 * time, which is what a translation is checked against. */
static const char *EnTemplate(const char *key) {
    int i;

    for (i = 0; i < (int)(sizeof(kEn) / sizeof(kEn[0])); i++)
        if (kEn[i].id && strcmp(kEn[i].id, key) == 0) return kEn[i].text;
    return NULL;
}

/* One line on screen, in the operator's language, carrying two numbers (a key
 * with one placeholder just ignores the second): the framework's own toast, the
 * one GhostRevive uses. Silence when this dinput8 has no toast export - the log
 * line every caller writes next to this one is the record either way.
 *
 * The translation is the operator's text, not a format string. It used to be
 * handed to snprintf as one, which is undefined behaviour the first time a
 * lang.ini row has a conversion too many or one too few - and a lang.ini is
 * edited by hand. So the two are compared first: same conversions in the same
 * order, and the translation is formatted; anything else falls back to this
 * plugin's own template and says so, once per key, in the log. */
static void Say(const char *key, uint32_t rgb, int a, int b) {
    char line[192];
    char want[8], got[8];
    const char *en, *tr;

    if (!p_toast) return;
    tr = ShLangText(PM_OWNER, key);
    en = EnTemplate(key);
    Conversions(en, want, sizeof(want));
    Conversions(tr, got, sizeof(got));
    if (en && tr && strcmp(want, got) == 0) {
        snprintf(line, sizeof(line), tr, a, b);
    } else if (en) {
        if (tr)
            Log("pm: %s: the translation reads its arguments as \"%s\" and en-US "
                "as \"%s\", so the en-US text was used", key, got, want);
        snprintf(line, sizeof(line), en, a, b);
    } else {
        /* No template here for this key, so there is nothing to check it
         * against: the text goes out as it is. */
        snprintf(line, sizeof(line), "%s", tr ? tr : key);
    }
    p_toast(line, rgb, 2500u);
}

/* ---- writing the settings back ----------------------------------------- */

/* Every row on this page goes through MarkDirty, and the worker writes the file
 * once the changes have settled: a slider dragged across every one of its steps
 * is one write, not one per step. See the file head for why this plugin writes
 * its ini at all as of 2026-09-28. */
#define PM_SAVE_MS 400
static volatile LONG      g_dirty;
static volatile ULONGLONG g_dirtyAt;

static void MarkDirty(void) {
    g_dirtyAt = GetTickCount64();
    InterlockedExchange(&g_dirty, 1);
}

/* The whole [Settings] block, in the shape ReadSettings expects to read it back
 * - the distance is a whole number of metres and the health keeps its decimal
 * point, because that is what those two readers ask for. Keys this plugin does
 * not own are left alone: WritePrivateProfileString replaces the key it is
 * given and keeps the rest of the file. */
static void SaveIni(void) {
    char buf[64];

    if (!g_ini[0]) {
        Log("pm: no ini path, so the settings stay in memory only");
        return;
    }
    snprintf(buf, sizeof(buf), "%d",
             InterlockedCompareExchange(&g_on, 0, 0) ? 1 : 0);
    WritePrivateProfileStringA("Settings", "enabled", buf, g_ini);
    snprintf(buf, sizeof(buf), "0x%016llX", (unsigned long long)g_id);
    WritePrivateProfileStringA("Settings", "id", buf, g_ini);
    if (g_name[0])
        WritePrivateProfileStringA("Settings", "name", g_name, g_ini);
    snprintf(buf, sizeof(buf), "%d", g_count);
    WritePrivateProfileStringA("Settings", "count", buf, g_ini);
    snprintf(buf, sizeof(buf), "%d", g_cap);
    WritePrivateProfileStringA("Settings", "cap", buf, g_ini);
    snprintf(buf, sizeof(buf), "%d", (int)g_distM);
    WritePrivateProfileStringA("Settings", "distance", buf, g_ini);
    snprintf(buf, sizeof(buf), "%.1f", g_hp);
    WritePrivateProfileStringA("Settings", "hp", buf, g_ini);
    Log("pm: settings written to %s", g_ini);
}

/* The switch at the top of the page: summoning on or off, and the ini is
 * written to match (see the file head). */
static void OnEnabled(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    InterlockedExchange(&g_on, v ? 1 : 0);
    MarkDirty();
    Log("pm: summoning is %s; [Settings] enabled in the plugin ini is written "
        "to match, so the next session starts the way this one was left",
        v ? "on" : "off");
}

static void OnSummon(uint32_t m, uint32_t it, int v, void *u) {
    ShNpcSpawnRequest req;
    ShNpcSpawnLayout lay;
    int n, cd = CdLeft();

    (void)m; (void)it; (void)v; (void)u;
    /* The switch, first: with it off nothing else on this page has anything to
     * act on, and a refusal that names it is what says where to turn it back
     * on - the row that does is one above this one. */
    if (!InterlockedCompareExchange(&g_on, 0, 0)) {
        Say("@pm.toast.off", 0xFFCC33u, 0, 0);
        Log("pm: the summon row was pressed with the switch off ([Settings] "
            "enabled=0; the row at the top of the page turns it on)");
        return;
    }
    if (!g_id) {
        /* Unreachable while PM_DEF_ID is a real id - an empty key leaves the
         * built-in value standing - but kept as the guard it has always been,
         * and said on screen now as well: a row that does nothing when it is
         * pressed reads as a freeze, and the reports of this plugin "not
         * summoning" were all a silently refused press. */
        Say("@pm.toast.noid", 0xFFCC33u, 0, 0);
        Log("pm: no usable archetype id (built in %016llX, and the ini did not "
            "give one either), so there is nothing to summon",
            (unsigned long long)PM_DEF_ID);
        return;
    }
    /* Every refusal answers on screen as well as in the log. A row that does
     * nothing when it is pressed reads as a freeze, and one refusal looks like
     * any other: the toast says which, and how long. */
    if (cd > 0) {
        Say("@pm.toast.cd", 0xFFCC33u, cd, 0);
        Log("pm: the summons are on cooldown - %d s left of the %d s between "
            "batches", cd, (int)(PM_CD_MS / 1000));
        return;
    }
    if (g_job) {
        Say("@pm.toast.busy", 0xFFCC33u, g_jobMade, 0);
        Log("pm: a batch is still being made (%d so far); wait for it",
            g_jobMade);
        return;
    }
    /* The ceiling: a crowd that keeps growing is what freezes a game, not the
     * size of one batch. What is asked for is trimmed to the room left. */
    if (g_live >= g_cap) {
        Say("@pm.toast.cap", 0xFFCC33u, g_live, g_cap);
        Log("pm: %d are already on the field, which is the ceiling (%d) - zero "
            "their health, or raise the ceiling", g_live, g_cap);
        return;
    }
    n = g_count;
    if (n > g_cap - g_live) {
        n = g_cap - g_live;
        Log("pm: %d asked for, but only %d fit under the ceiling of %d",
            g_count, n, g_cap);
    }

    /* One policy, no rows: scattered placement, 40 degrees further round the
     * player than the last batch, and a spin step of 360 / n so the members of
     * this batch fan out instead of stacking on one heading. lay is zeroed
     * either way, because the log line below reads it. */
    memset(&lay, 0, sizeof(lay));
    if (p_layout) {
        lay.spread_step_deg = PM_SPREAD_DEG;
        lay.facing_mode = SH_NPC_FACING_MODE_SPIN;
        lay.facing_angle_deg = 360.0f / (float)n;
        if (!p_layout(&lay))
            Log("pm: the layout policy was refused; the batch goes straight "
                "ahead as the planner does by itself");
    }
    memset(&req, 0, sizeof(req));
    req.id = g_id;
    req.count = n;
    req.distance = g_distM;
    req.formation = SH_NPC_FORMATION_RANDOM;
    req.facing = SH_NPC_FACING_PLAYER;
    g_jobMade = 0;
    g_entFull = 0;
    /* No age until the job exists. The frame callback judges a batch that has not
     * moved for fifteen seconds by this clock, and it reads the clock the moment
     * it sees the job - publishing the job first let it judge a brand new batch
     * on the previous one's age and ask it to stop. */
    g_jobAt = 0;
    g_job = p_begin(&req);
    if (!g_job) {
        Log("pm: no free job (the framework keeps %d in flight); try again in "
            "a moment", SH_NPC_SPAWN_JOBS);
        return;
    }
    g_jobAt = GetTickCount64();
    g_cdUntil = g_jobAt + PM_CD_MS;
    Log("pm: asked for %d of id %016llX%s%s: scattered, %.0f m ahead, spin step "
        "%.0f deg, %.0f deg round, arrival health %.0f%%, next summon in %d s",
        n, (unsigned long long)g_id, g_name[0] ? " (" : "",
        g_name[0] ? g_name : "", req.distance, lay.facing_angle_deg,
        PM_SPREAD_DEG, g_hp * 100.0f, (int)(PM_CD_MS / 1000));
    RefreshStatus();
}

/* Zero the health of everything this page has made.
 *
 * This used to be the fallback for a despawn that did not work. The despawn row
 * is gone now: ShDespawn is a call this build accepts and ignores - its retire
 * address is the weakest pin in the framework, an 80% .pdata guess with no call
 * site to vote with (scripthook_npc.c's own words) - so the row that could not
 * work was removed rather than left in the menu. This one writes health, which
 * has worked since the first version of this plugin. They fall where they stand
 * rather than vanishing; the log says so, and checks a second later how many are
 * still up. */
static void OnKill(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)v; (void)u;
    if (!p_hpSet) {
        Log("pm: this dinput8 has no ShSetHealthEntity, so the health of what "
            "is out cannot be written");
        return;
    }
    if (!g_entCount) {
        Log("pm: this page has made nothing yet");
        return;
    }
    g_killAt = GetTickCount64();
    InterlockedExchange(&g_kill, 1);
    Log("pm: asked for the health of the %d this page made to be zeroed",
        g_entCount);
}

static void OnCount(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    g_count = v < 1 ? 1 : (v > PM_COUNT_MAX ? PM_COUNT_MAX : v);
    MarkDirty();
    Log("pm: %d per summon", g_count);
    RefreshStatus();
}

static void OnCap(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    g_cap = v < PM_CAP_MIN ? PM_CAP_MIN : (v > PM_CAP_MAX ? PM_CAP_MAX : v);
    MarkDirty();
    Log("pm: at most %d of them on the field at once", g_cap);
    RefreshStatus();
}

/* Both of these are ten-option lists whose labels carry the unit - "5 m …
 * 50 m" and "10% … 100%" - because a bare number on a row is a puzzle and the
 * option string is not. The value arriving here is the option index. */
static void OnDistance(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    if (v < 0) v = 0;
    if (v > 9) v = 9;
    g_distM = PM_DIST_MIN + PM_DIST_STEP * (float)v;
    MarkDirty();
    Log("pm: %.0f m ahead of the player", g_distM);
    RefreshStatus();
}

/* 0.1 .. 1.0, which is the fraction of its maximum an entity arrives with. */
static void OnHp(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    if (v < 0) v = 0;
    if (v > 9) v = 9;
    g_hp = (float)(v + 1) / 10.0f;
    MarkDirty();
    Log("pm: arrivals keep %.0f%% of their health%s", g_hp * 100.0f,
        g_hp > 0.999f ? " (full: nothing is written to their health)" : "");
    RefreshStatus();
}

/* ---- the frame hook ---------------------------------------------------- */

/* Is this entity up? Two things in one call: it has a health component at all
 * (0 for one that has left the world), and it is above zero (0 for one that has
 * been put down). "Up" is the second - that is what the field count and the
 * health-zero row's check both mean. */
static int AliveNow(uint64_t e) {
    uint32_t cur = 0, max = 0;
    if (!p_hpGet) return 0;
    return p_hpGet(e, &cur, &max) && max > 0 && cur > 0;
}

/* The one place a plugin may touch its menu from outside a menu callback, and
 * the only clock it has: the batch is polled here, the status line is brought up
 * to date when it or the cooldown changes, a batch that has stopped moving is cut
 * loose, how many are still up is counted once a second, and the health-zero
 * row's result is looked at. A handful of compares on a frame with nothing to
 * do. */
static void PmFrame(void *user) {
    ULONGLONG now = GetTickCount64();
    (void)user;

    if (g_job && now - g_lastPoll >= PM_POLL_MS) {
        g_lastPoll = now;
        PollJob();
    }

    /* A batch that has not moved for fifteen seconds is not going to move: it is
     * asked to stop, so the job can be freed and the next summon is not refused
     * for ever. The cancel row that used to do this by hand is gone. */
    if (g_job && g_jobAt && now - g_jobAt > 15000) {
        g_jobAt = now;                 /* ask again every fifteen seconds */
        Log("pm: this batch has not moved for 15 s, so it is being asked to "
            "stop; the poll frees it and the next summon can go ahead");
        p_end(g_job);
    }

    /* The status line's own clock: the cooldown ticks down and the field count
     * moves, so the line is rewritten when either does. The line at the top is
     * left alone - it states the rule once, and a summon pressed too early
     * answers with a toast instead, because two places counting down the same
     * thing is one too many. */
    {
        static int shownCd = -1, shownLive = -1, shownMade = -1;
        int cd = CdLeft();

        if (cd != shownCd || g_live != shownLive || g_entCount != shownMade) {
            shownCd = cd;
            shownLive = g_live;
            shownMade = g_entCount;
            RefreshStatus();
        }
    }

    /* How many of what this page made are still up, asked for four times a second
     * so a kill shows up while the operator is still looking at the line. The
     * counting itself is the worker's, not this callback's: it is one health read
     * per entity, and a health read reaches engine code that can block. All this
     * does is raise the flag; the worker ticks every PM_TICK_MS, so the figure
     * lands sooner than the four times a second it used to. */
    if (g_entCount > 0) {
        if (now - g_liveAt >= 250) {
            g_liveAt = now;
            InterlockedExchange(&g_liveWanted, 1);
        }
    } else {
        g_live = 0;
    }

    /* What the health-zero row did, said a second later rather than taken on
     * trust: health that still answers above zero is an entity still up. The
     * counting is the worker's; this only reports what it found, because a health
     * read is not this callback's work and a toast is not the worker's. */
    if (InterlockedExchange(&g_killDone, 0)) {
        Log("pm: after the health-zero row: %d of the %d this page made are "
            "still up", (int)g_killUp, (int)g_entCount);
        RefreshStatus();
    }
}

/* ---- the worker -------------------------------------------------------- */

/* The health write is an engine call that can block or wait on the game thread,
 * so it runs here and nowhere near a callback. */
static DWORD WINAPI PmWorker(LPVOID p) {
    (void)p;
    for (;;) {
        /* Rows changed on the page are written out here, once they have
         * settled - see MarkDirty/SaveIni. */
        if (InterlockedCompareExchange(&g_dirty, 0, 0) &&
            GetTickCount64() - g_dirtyAt >= PM_SAVE_MS) {
            InterlockedExchange(&g_dirty, 0);
            SaveIni();
        }

        /* The field figure, moved here from the frame callback: one health read
         * per entity is not work for the engine's frame. */
        if (InterlockedExchange(&g_liveWanted, 0)) {
            int i, up = 0;
            int n = (int)InterlockedCompareExchange(&g_entCount, 0, 0);

            for (i = 0; i < n; i++)
                if (g_ent[i] && AliveNow(g_ent[i])) up++;
            g_live = up;
        }

        /* The health-zero row, looked at again a second later: a read that still
         * answers above zero is an entity still up. The log line and the status
         * refresh are left to the frame callback, which is the only one of the two
         * allowed to touch the menu. */
        if (InterlockedCompareExchange(&g_killCheck, 0, 0) &&
            GetTickCount64() - g_killAt >= 1000) {
            int i, up = 0;
            int n = (int)InterlockedCompareExchange(&g_entCount, 0, 0);

            InterlockedExchange(&g_killCheck, 0);
            for (i = 0; i < n; i++)
                if (g_ent[i] && AliveNow(g_ent[i])) up++;
            g_killUp = up;
            InterlockedExchange(&g_killDone, 1);
        }

        if (InterlockedCompareExchange(&g_kill, 0, 1) == 1) {
            int i, asked = 0;

            for (i = 0; i < g_entCount; i++) {
                if (!g_ent[i]) continue;
                if (p_hpSet && p_hpSet(g_ent[i], 0)) asked++;
                else Log("pm: entity %llX refused the health write (%08X, %s)",
                         (unsigned long long)g_ent[i], ShLastError(),
                         ShErrorString(ShLastError()));
            }
            Log("pm: health 0 written to %d of the %d this page made; the frame "
                "callback reports in a second how many are still standing",
                asked, g_entCount);
            InterlockedExchange(&g_killCheck, 1);
            InterlockedExchange((volatile LONG *)&g_kill, 0);
        }
        Sleep(PM_TICK_MS);
    }
}

/* ---- start ------------------------------------------------------------- */

static DWORD WINAPI InitThread(LPVOID p) {
    /* The two option lists are built once into these, because an option string
     * has to outlive the menu. */
    static const char *distOpts[10], *hpOpts[10];
    static char distLabel[10][12], hpLabel[10][8];
    int i, distPick, hpPick;

    (void)p;
    while (!ShGetVersion()) Sleep(500);

    if (!Bind()) {
        LogInit("PredatorMelee.log");
        Log("pm: dinput8 has no NPC batch API; this build cannot use it");
        return 0;
    }

    LogInit("PredatorMelee.log");
    ShLangDeclare(PM_OWNER, "en-US", kEn, (int)(sizeof(kEn) / sizeof(kEn[0])));
    ShLangDeclare(PM_OWNER, "zh-CN", kZh, (int)(sizeof(kZh) / sizeof(kZh[0])));
    if (!ShPluginIniPath(PM_OWNER, g_ini, (int)sizeof(g_ini))) g_ini[0] = 0;
    /* The switch is a row on the page, so the page is built either way: what
     * enabled=0 means now is that the summon row refuses, not that this plugin
     * is invisible. It used to return here, which left the plugin unlisted and
     * with no way to turn it on from inside the game. */
    SeedIni();
    g_on = IniInt("enabled", 0) ? 1 : 0;
    ReadSettings();

    Log("pm: settings read from %s", g_ini[0] ? g_ini
        : "the plugin itself (no ini path was resolved)");
    Log("pm: id %016llX%s%s, %d per summon, %.0f m, hp %.0f%%, ceiling %d, "
        "cooldown %d s, summoning %s - the id is %s",
        (unsigned long long)g_id, g_name[0] ? " (" : "", g_name[0] ? g_name : "",
        g_count, g_distM, g_hp * 100.0f, g_cap, (int)(PM_CD_MS / 1000),
        g_on ? "on" : "off", g_idFromIni ? "from the ini" : "built in");
    if (!g_id)
        Log("pm: no usable archetype id, so the summon row will only say so");
    if (!p_layout)
        Log("pm: this dinput8 has no ShNpcSpawnSetLayout, so batches go "
            "straight ahead facing the player instead of scattering");
    if (!p_hpSet || !p_hpGet)
        Log("pm: this dinput8 cannot read or write entity health, so the "
            "arrival health row, the field count and the health-zero row do "
            "nothing");

    /* The two lists carry their unit in the label: "5 m … 50 m" and
     * "10% … 100%". Built here once, from the ini's values, because the option
     * strings must outlive the menu. */
    for (i = 0; i < 10; i++) {
        snprintf(distLabel[i], sizeof(distLabel[i]), "%.0f%s",
                 PM_DIST_MIN + PM_DIST_STEP * (float)i,
                 ShLangText(PM_OWNER, "@pm.unit.m"));
        distOpts[i] = distLabel[i];
        snprintf(hpLabel[i], sizeof(hpLabel[i]), "%d%s", (i + 1) * 10,
                 ShLangText(PM_OWNER, "@pm.unit.pct"));
        hpOpts[i] = hpLabel[i];
    }
    distPick = (int)((g_distM - PM_DIST_MIN) / PM_DIST_STEP + 0.5f);
    if (distPick < 0) distPick = 0;
    if (distPick > 9) distPick = 9;
    hpPick = (int)(g_hp * 10.0f + 0.5f) - 1;
    if (hpPick < 0) hpPick = 0;
    if (hpPick > 9) hpPick = 9;

    g_menu = ShMenuCreate("@pm.page");
    /* The line at the top, before the rows: what the summons are for, and the
     * one rule that goes with them. Written once - a summon pressed too early
     * answers with a toast instead, so this line and the status line do not both
     * count down the same thing. */
    ShMenuHint(g_menu, "@pm.hint");
    ShMenuToggle(g_menu, "@pm.enabled", g_on ? 1 : 0, OnEnabled, NULL);
    ShMenuAction(g_menu, "@pm.summon", OnSummon, NULL);
    ShMenuNumber(g_menu, "@pm.count", (float)g_count, 1.0f, (float)PM_COUNT_MAX,
                 1.0f, OnCount, NULL);
    ShMenuNumber(g_menu, "@pm.cap", (float)g_cap, (float)PM_CAP_MIN,
                 (float)PM_CAP_MAX, 1.0f, OnCap, NULL);
    ShMenuList(g_menu, "@pm.distance", distOpts, 10, distPick, OnDistance, NULL);
    ShMenuList(g_menu, "@pm.hp", hpOpts, 10, hpPick, OnHp, NULL);
    ShMenuAction(g_menu, "@pm.kill", OnKill, NULL);
    g_menuReady = 1;
    RefreshStatus();

    if (p_frameCb && p_frameCb(PmFrame, NULL))
        Log("pm: the batch is polled from the framework's frame hook");
    else
        Log("pm: this dinput8 has no frame hook, so the progress line will not "
            "move and the batch has to be watched in the log");

    CreateThread(NULL, 0, PmWorker, NULL, 0, NULL);
    Log("pm: page created");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
    }
    return TRUE;
}
