/* NpcProbe - a page for testing what this build can summon.
 *
 * The question it answers: which NPCs can this copy of the game produce, and
 * how does a wanted one get named? The framework already collects the
 * engine's own archetype registry at run time (ShNpcCount / ShNpcAt, 527
 * entries on the build this was written against) and groups each entry by the
 * kind the engine keeps with it, so the page below is built out of that and
 * nothing else. No id table is compiled in, which is the point: the ids a
 * third-party plugin wrote down belonged to the build it was made for, and on
 * this build not one of its 76 Special entries is in the registry at all.
 *
 * An id the registry does not hold cannot be summoned at all: ShSpawnNpc
 * resolves an id through that same catalogue and refuses the rest with "no
 * archetype block for the id". So the page's other way in - a list of ids the
 * operator writes in [Settings] ids - is not an escape hatch from the registry;
 * it is how a set of ids is walked and checked against it, one at a time. (The
 * 76 entries that list started with all turned out to be in the catalogue.)
 *
 * Two switches go with it, for looking at what a summon produces: a hostile NPC
 * walks up and ends the session otherwise, and a player who dies before the id
 * can be read has taught nothing. They are the framework's own facilities - the
 * health component's god and no-damage bytes (ShSetGodModePlayer) and the
 * detection factor behind ShSetVisibility - so this plugin writes no engine
 * memory itself. Both are topped up once a second while they are on, because a
 * respawn hands the player a fresh component and a switch that quietly stopped
 * working is worse than no switch.
 *
 * One file is shipped with it: npc-catalogue.txt, the ids one stated game build
 * held, read from that build's engine. The page never spawns from it and never
 * shows it - the engine is asked instead, every session - it is what the
 * engine's answer is checked against. See the section on it further down.
 *
 * Rules this follows, from the plugins that came before it:
 *   - nothing happens until [NpcProbe] enabled=1 (a probe changes nothing);
 *   - ShSpawnNpc blocks until the entity exists, so it is never called from a
 *     menu callback or the game thread: the callback only records a request,
 *     and a worker thread does the summoning;
 *   - every string a player reads goes through @np keys declared for en-US and
 *     zh-CN, so lang.ini only ever needs the rows it changes.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "scripthook.h"
#include "log.h"

#define NPCPROBE_TICK_MS 100
#define NPCPROBE_OWNER   "NpcProbe"

/* The plugin's text, both languages, compiled in. A lang.ini row overrides one
 * key in one language and never has to be written for the other. */
static const ShText kEn[] = {
    { "@np.page",       "NPC probe (catalogue)" },
    { "@np.group",      "Group" },
    { "@np.kind",       "Kind" },
    { "@np.kind.any",   "any kind" },
    { "@np.named",      "Only named entries" },
    { "@np.sub.cat",    "Catalogue" },
    { "@np.sub.list",   "Id list" },
    { "@np.sub.files",  "Names and files" },
    { "@np.dist",       "Summon distance" },
    { "@np.batch",      "Summon how many" },
    { "@np.autonote",   "Note every summon" },
    { "@np.dumpNames",  "Write the names to a file" },
    { "@np.item",       "Item" },
    { "@np.show",       "Show it (log)" },
    { "@np.summon",     "Summon it" },
    { "@np.remember",   "Append it to the names" },
    { "@np.litem",      "List item" },
    { "@np.summonList", "Summon list item" },
    { "@np.rememberList","Append the list item to the names" },
    { "@np.summonNext", "Summon next in list" },
    { "@np.summonId",   "Summon id from the ini" },
    { "@np.reload",     "Read the catalogue again" },
    { "@np.dump",       "Write the catalogue to a file" },
    { "@np.clear",      "Despawn everything summoned" },
    { "@np.god",        "Player invincible" },
    { "@np.ghost",      "Invisible to enemies" },
    { "@np.g.every",    "every group" },
    { "@np.g.ungrouped","ungrouped" },
    { "@np.g.sb",       "Santa Blanca" },
    { "@np.g.un",       "Unidad" },
    { "@np.g.rb",       "Rebels" },
    { "@np.g.cv",       "Civilians" },
    { "@np.g.sp",       "Special" },
    { "@np.st.both",    "%s: %d of %d | id %016llX kind %d %s | list %d of %d: %016llX" },
    { "@np.st.none",    "catalogue %d | %s: 0 | nothing picked%s" },
    { "@np.st.one",     "catalogue %d | %s: %d of %d | id %016llX kind %d %s%s" },
    { "@np.st.nolist",  " | no id list" },
    { "@np.st.named",   "· named only" }
};

static const ShText kZh[] = {
    { "@np.page",       "NPC 探针（按目录）" },
    { "@np.group",      "阵营" },
    { "@np.kind",       "kind" },
    { "@np.kind.any",   "任意 kind" },
    { "@np.named",      "只看有名册的条目" },
    { "@np.sub.cat",    "目录" },
    { "@np.sub.list",   "名单" },
    { "@np.sub.files",  "名册与文件" },
    { "@np.dist",       "召唤距离" },
    { "@np.batch",      "一次召唤几个" },
    { "@np.autonote",   "召唤时自动记入名册" },
    { "@np.dumpNames",  "把名册写成文件" },
    { "@np.item",       "第几项" },
    { "@np.show",       "记录选中项" },
    { "@np.summon",     "召唤选中项" },
    { "@np.remember",   "把选中项追加到名册" },
    { "@np.litem",      "名单第几项" },
    { "@np.summonList", "召唤名单项" },
    { "@np.rememberList","把名单项追加到名册" },
    { "@np.summonNext", "召唤并跳到下一项" },
    { "@np.summonId",   "召唤手填 id" },
    { "@np.reload",     "重新读取目录与名册" },
    { "@np.dump",       "把目录写成文件" },
    { "@np.clear",      "把召唤出来的都收掉" },
    { "@np.god",        "玩家无敌" },
    { "@np.ghost",      "隐身（敌人看不见）" },
    { "@np.g.every",    "全部阵营" },
    { "@np.g.ungrouped","未归类" },
    { "@np.g.sb",       "圣塔布兰卡" },
    { "@np.g.un",       "联合军" },
    { "@np.g.rb",       "反抗军" },
    { "@np.g.cv",       "平民" },
    { "@np.g.sp",       "特殊" },
    { "@np.st.both",    "%s：第 %d/%d 项 | id %016llX 类 %d %s | 名单 %d/%d：%016llX" },
    { "@np.st.none",    "目录 %d | %s：0 项 | 未选定%s" },
    { "@np.st.one",     "目录 %d | %s：第 %d/%d 项 | id %016llX 类 %d %s%s" },
    { "@np.st.nolist",  " | 未填名单" },
    { "@np.st.named",   "· 只看名册" }
};

/* The catalogue, snapshotted once: the API call walks the registry and can
 * take a moment on the first use, so it is not done from a menu callback. */
#define CAT_MAX 4096
static uint64_t g_catId[CAT_MAX];
static int      g_catKind[CAT_MAX];
static int      g_catGroup[CAT_MAX];
static int      g_catCount;

/* An id list, from [Settings] ids in this same file: the operator's own, and
 * the way a set of ids is tried in order - which is what the 76 entries of the
 * table this project once carried needed, since their meanings had to be
 * checked one at a time. (All 76 turned out to be in the registry: the
 * Predator, 154BBB495E1, is catalogue entry 252. An id outside the registry
 * cannot be summoned - ShSpawnNpc resolves through the registry, and says "no
 * archetype block for the id" when there is none.) No id table is compiled
 * into this source. */
#define LIST_MAX 256
static uint64_t g_listId[LIST_MAX];
static int      g_listCount;
static int      g_listPick;

/* The kinds this build uses, measured rather than assumed: nine of them, 0 to 7
 * and 10. The row offers 0 to 15 so a kind this build does not have can still
 * be looked for, and a number needs no translation.
 *
 * The labels are buffers the row points at, not literals: the catalogue arrives
 * after the row exists, and writing "3 (238)" into the buffer the row already
 * holds is the only way to show a count without rebuilding the menu. */
#define KIND_RANGE 16
static char        kKindBuf[KIND_RANGE][24];
static const char *kKindOpt[KIND_RANGE];

/* How far from the player a summon lands, along the line the camera looks, and
 * how many copies of it arrive. Both are metres and a count, so neither needs
 * translating. */
static const float kDistM[] = { 1.5f, 3.0f, 5.0f, 10.0f, 20.0f };
static const char *const kDistLabel[] = { "1.5 m", "3 m", "5 m", "10 m", "20 m" };
#define DIST_N ((int)(sizeof(kDistM) / sizeof(kDistM[0])))
#define BATCH_MAX 10

/* What the page is looking at.
 *
 * The group row needs three kinds of answer, not two: every group, one of the
 * five, or the leftovers. The leftovers are entries whose group is -1, so they
 * cannot share a value with "every" - that mistake read as "every group is the
 * ungrouped ones" on the line and matched nothing in the list, since no entry
 * carries the group SH_NPC_GROUP_MAX. */
#define GRP_ANY       (-1)
#define GRP_UNGROUPED (-2)

static int g_filter = GRP_ANY;
static int g_kind = -1;        /* -1 = any kind, else the engine's own   */
static int g_namedOnly;        /* 1 = only the entries that have a name  */
static int g_distPick;         /* index into kDistM / kDistLabel         */
static int g_batch = 1;        /* how many copies one press summons      */
static int g_autoNote;         /* 1 = note every summon in [Names]       */
static int g_pick = 0;         /* index inside the filtered view         */
static int g_lastEntity;       /* 0 when nothing has been summoned       */

/* A summon request, handed to the worker. */
static volatile LONG g_reqId = 0;      /* low 32 bits  */
static volatile LONG g_reqIdHi = 0;    /* high 32 bits */
static volatile LONG g_request = 0;

/* The entities this page made, so it can take them away again: a walk through
 * the catalogue leaves the world full of bodies, and clearing them one at a
 * time is not something a menu can do. The list is read and written by the
 * worker alone - a menu callback only raises the flag - because ShDespawn, like
 * ShSpawnNpc, belongs nowhere near the game thread. */
#define SPAWNED_MAX 256
static uint64_t      g_spawned[SPAWNED_MAX];
static int           g_spawnedCount;
static int           g_spawnedFull;    /* the list being full is said once */
static unsigned long long g_spawnedTotal;
static volatile LONG g_clear;          /* 1 = the operator asked for them gone */

/* Set when something changed off the game thread - the catalogue arriving, in
 * practice - so that the page can be brought up to date on the game thread,
 * where the menu belongs. Nothing else touches the menu from the worker: the
 * framework's per frame hook is the one place a plugin may, and it is where
 * this is read. */
static volatile LONG g_uiStale;

/* And when the worker appended a name row of its own ("note every summon"), the
 * names are read back on the game thread too, so the row the operator just
 * added appears without anyone pressing anything. */
static volatile LONG g_namesStale;

/* The plugin's own config, plugins\NpcProbe\NpcProbe.ini, which is where a
 * plugin's settings belong. Everything below is [Settings] in that file. */
static char g_ini[MAX_PATH];

static int IniInt(const char *key, int def) {
    if (!g_ini[0]) return def;
    return GetPrivateProfileIntA("Settings", key, def, g_ini);
}

static void IniStr(const char *key, const char *def, char *out, int n) {
    out[0] = 0;
    if (g_ini[0]) GetPrivateProfileStringA("Settings", key, def, out, n, g_ini);
}

/* ---- the operator's own names ------------------------------------------- */

/* Who an archetype is, written by hand in this same file:
 *
 *   [Names]
 *   154BBB495E1=铁血战士
 *
 * One row per id, same file as the settings, read from the [Names] section
 * rather than [Settings]. Nothing here is compiled in and nothing is invented:
 * an id with no row simply has no name. Both "154BBB495E1" and "0x154BBB495E1"
 * are read, and the value is passed through exactly as written, so a name is in
 * whatever language the operator typed it in. */
#define NAME_MAX 512
#define NAME_LEN 80
static uint64_t g_nameId[NAME_MAX];
static char     g_nameText[NAME_MAX][NAME_LEN];
static int      g_nameCount;

/* Which catalogue entries have a name, cached: the filter asks this for every
 * entry on every menu key, and a name lookup is a scan of its own table. */
static unsigned char g_catNamed[CAT_MAX];
static void RefreshNamedFlags(void);

static void ReadNames(void) {
    static char keys[16384];
    static char val[NAME_LEN];
    char *p;

    g_nameCount = 0;
    if (!g_ini[0]) return;
    keys[0] = 0;
    GetPrivateProfileStringA("Names", NULL, "", keys, (int)sizeof(keys), g_ini);
    p = keys;
    while (*p && g_nameCount < NAME_MAX) {
        uint64_t id;
        size_t klen = strlen(p);

        /* A NULL key asks for every key in the section, one after another,
         * separated by NULs and ended by an empty string. */
        val[0] = 0;
        GetPrivateProfileStringA("Names", p, "", val, (int)sizeof(val), g_ini);
        id = (uint64_t)_strtoui64(p, NULL, 16);
        if (id && val[0]) {
            g_nameId[g_nameCount] = id;
            snprintf(g_nameText[g_nameCount], NAME_LEN, "%s", val);
            g_nameCount++;
        }
        p += klen + 1;
    }
    Log("npcprobe: [Names] carried %d name(s)", g_nameCount);
    RefreshNamedFlags();
}

/* The name for an id, or "" when there is none. */
static const char *NameOfId(uint64_t id) {
    int i;
    for (i = 0; i < g_nameCount; i++)
        if (g_nameId[i] == id) return g_nameText[i];
    return "";
}

static void RefreshNamedFlags(void) {
    int i;
    for (i = 0; i < g_catCount && i < CAT_MAX; i++)
        g_catNamed[i] = NameOfId(g_catId[i])[0] ? 1 : 0;
}

/* The file's own text, for the two things the profile API cannot do: see a row
 * whose value is still empty, and add one line without touching the rest.
 * Windows' ini writer rewrites the whole file and takes every comment in it
 * with it, and this file is the operator's - the settings, the list of ids and
 * all of the explanation live in it. So a line is appended and nothing else is
 * ever written. */
static char g_iniText[262144];

/* The file's text and the names table are touched from two threads once
 * "note every summon" is on: the worker appends, the menu reads. Everything
 * that goes near them takes this. It is initialised before either user starts
 * - the worker is created after the page - and is only ever held for a read or
 * a one line append, never across an engine call. */
static CRITICAL_SECTION g_fileLock;

static void IniLock(void)   { EnterCriticalSection(&g_fileLock); }
static void IniUnlock(void) { LeaveCriticalSection(&g_fileLock); }

static size_t ReadIniText(void) {
    FILE *fp;
    size_t n;
    if (!g_ini[0]) { g_iniText[0] = 0; return 0; }
    fp = fopen(g_ini, "rb");
    if (!fp) { g_iniText[0] = 0; return 0; }
    n = fread(g_iniText, 1, sizeof(g_iniText) - 1, fp);
    fclose(fp);
    g_iniText[n] = 0;
    return n;
}

/* 1 when the file already carries a row for this id, whether or not a name has
 * been written after the '=' yet. */
static int NameRowExists(uint64_t id) {
    char *p = g_iniText;

    for (;;) {
        char *nl = strchr(p, '\n');
        char *e, *k, key[32];
        size_t len, c;
        int whole = nl ? 1 : 0;
        char *end = nl ? nl : p + strlen(p);

        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p < end && *p != ';' && *p != '[') {
            e = p;
            while (e < end && *e != '=') e++;
            if (e < end) {
                k = p;
                if (e - k > 1 && k[0] == '0' && (k[1] == 'x' || k[1] == 'X'))
                    k += 2;
                len = (size_t)(e - k);
                while (len && (k[len - 1] == ' ' || k[len - 1] == '\t' ||
                               k[len - 1] == '\r')) len--;
                c = len < sizeof(key) - 1 ? len : sizeof(key) - 1;
                memcpy(key, k, c);
                key[c] = 0;
                if (key[0] && (uint64_t)_strtoui64(key, NULL, 16) == id) return 1;
            }
        }
        if (!whole) break;
        p = nl + 1;
    }
    return 0;
}

/* Which section the file ends in, so the appended line lands in [Names] and not
 * in whatever section happened to be last. A second [Names] reads like the
 * first - the profile API merges them - so appending one when the file no
 * longer ends in it is safe. */
static int AppendNameRow(uint64_t id) {
    FILE *fp;
    char *p = g_iniText;
    size_t n = strlen(g_iniText);
    int endsInNames = 0;

    /* Which section the file ends in: the last line that is a section header.
     * A comment is not a header, however many brackets the explanation in it
     * happens to use - and this file's comments name several sections. */
    for (;;) {
        char *nl = strchr(p, '\n');
        char *end = nl ? nl : p + strlen(p);
        char *q = p;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (q < end && *q == '[')
            endsInNames = _strnicmp(q, "[Names]", 7) == 0 ? 1 : 0;
        if (!nl) break;
        p = nl + 1;
    }

    fp = fopen(g_ini, "ab");
    if (!fp) return 0;
    if (n && g_iniText[n - 1] != '\n') fputc('\n', fp);
    if (!endsInNames) fprintf(fp, "\r\n[Names]\r\n");
    fprintf(fp, "%llX=\r\n", (unsigned long long)id);
    fclose(fp);
    return 1;
}

/* Put an id where a name belongs, and say so: the page can find the id, but
 * only the operator can say who it is, so the line is left to be filled in. */
static void RememberId(uint64_t id, const char *what) {
    if (!g_ini[0]) {
        Log("npcprobe: no plugin ini path, so there is nothing to append to");
        return;
    }
    if (!id) { Log("npcprobe: nothing selected, so there is nothing to note"); return; }
    IniLock();
    ReadIniText();
    if (NameRowExists(id)) {
        IniUnlock();
        Log("npcprobe: %s id %016llX already has a row in the names section%s%s",
            what, (unsigned long long)id,
            NameOfId(id)[0] ? " - it reads " : "", NameOfId(id));
        return;
    }
    if (!AppendNameRow(id)) {
        IniUnlock();
        Log("npcprobe: cannot append to %s", g_ini);
        return;
    }
    IniUnlock();
    Log("npcprobe: %s id %016llX appended to %s - write the name after the '='"
        " and press the reload row", what, (unsigned long long)id, g_ini);
    ReadNames();
}

/* The worker's version, for "note every summon": the same one line append, but
 * nothing that touches the menu or the names table - the table is the menu
 * thread's, and the row that reads it again is one press away. */
static void NoteIdQuiet(uint64_t id) {
    if (!g_ini[0] || !id) return;
    IniLock();
    ReadIniText();
    if (!NameRowExists(id)) {
        if (AppendNameRow(id)) {
            Log("npcprobe: id %016llX noted in the names - write the name after "
                "the '=' whenever it suits", (unsigned long long)id);
            InterlockedExchange(&g_namesStale, 1);
        }
        else
            Log("npcprobe: cannot append id %016llX to %s",
                (unsigned long long)id, g_ini);
    }
    IniUnlock();
}

/* ---- binding ------------------------------------------------------------ */

/* Late bound on purpose: a plugin that calls an export the running dinput8
 * does not have must say so rather than fail to load. */
typedef int      (*t_count)(void);
typedef const ShNpcArchetype *(*t_at)(int);
typedef int      (*t_groupOf)(const ShNpcArchetype *);
typedef const char *(*t_groupName)(int);
typedef uint64_t (*t_spawn)(uint64_t, const ShVec3 *);
typedef int      (*t_playerPos)(ShVec3 *);
typedef int      (*t_godPlayer)(int);
typedef int      (*t_setVisibility)(float);
typedef int      (*t_getVisibility)(float *);
typedef int      (*t_despawn)(uint64_t);
typedef int      (*t_camAngles)(float *, float *);
typedef int      (*t_frameCb)(void (*)(void *), void *);

static t_count     p_count;
static t_at        p_at;
static t_groupOf   p_groupOf;
static t_groupName p_groupName;
static t_spawn     p_spawn;
static t_playerPos p_playerPos;
static t_godPlayer p_godPlayer;
static t_setVisibility p_setVis;
static t_getVisibility p_getVis;
static t_despawn   p_despawn;
static t_camAngles p_camAngles;
static t_frameCb   p_frameCb;

static int Bind(void) {
    HMODULE m = GetModuleHandleA("dinput8.dll");
    if (!m) return 0;
    *(FARPROC *)&p_count     = GetProcAddress(m, "ShNpcCount");
    *(FARPROC *)&p_at        = GetProcAddress(m, "ShNpcAt");
    *(FARPROC *)&p_groupOf   = GetProcAddress(m, "ShNpcGroupOfArchetype");
    *(FARPROC *)&p_groupName = GetProcAddress(m, "ShNpcGroupName");
    *(FARPROC *)&p_spawn     = GetProcAddress(m, "ShSpawnNpc");
    *(FARPROC *)&p_playerPos = GetProcAddress(m, "ShGetPlayerPosition");
    /* The two switches are wanted, not required: a dinput8 without them still
     * gives the catalogue, and the page says so rather than failing to load. */
    *(FARPROC *)&p_godPlayer = GetProcAddress(m, "ShSetGodModePlayer");
    *(FARPROC *)&p_setVis    = GetProcAddress(m, "ShSetVisibility");
    *(FARPROC *)&p_getVis    = GetProcAddress(m, "ShGetVisibility");
    *(FARPROC *)&p_despawn   = GetProcAddress(m, "ShDespawn");
    *(FARPROC *)&p_camAngles = GetProcAddress(m, "ShCameraAngles");
    *(FARPROC *)&p_frameCb   = GetProcAddress(m, "ShRegisterFrameCallback");
    return p_count && p_at && p_groupOf && p_groupName && p_spawn;
}

/* ---- the catalogue ------------------------------------------------------ */

static int GroupAt(int i) {
    if (i < 0 || i >= g_catCount) return -1;
    return g_catGroup[i];
}

static const char *kGroupKey[SH_NPC_GROUP_MAX] = {
    "@np.g.sb", "@np.g.un", "@np.g.rb", "@np.g.cv", "@np.g.sp"
};

/* The framework names its five groups with English literals and keeps no
 * translation of its own for them, so the five names live here, in the list row
 * and wherever else a group is named. A group the framework does not know reads
 * as its own name rather than as a hole. */
static const char *GroupText(int g) {
    const char *own;
    if (g < 0) return ShLangText(NPCPROBE_OWNER, "@np.g.ungrouped");
    if (g >= SH_NPC_GROUP_MAX)
        return p_groupName ? p_groupName(g) : "?";
    own = p_groupName ? p_groupName(g) : "";
    if (own && own[0] && ShLangHas(NPCPROBE_OWNER, kGroupKey[g]))
        return ShLangText(NPCPROBE_OWNER, kGroupKey[g]);
    return own && own[0] ? own : "?";
}

/* How many are in the view the page is showing. Two filters, and they compose:
 * the group is the framework's coarse bucket, the kind is the engine's own
 * number. The kind row is the finer one, and the only one that can reach an
 * archetype the bucket misnames - the Predator's kind is 3, which the bucket
 * calls Santa Blanca. */
static int InView(int i) {
    if (g_filter == GRP_UNGROUPED) {
        if (g_catGroup[i] >= 0) return 0;
    } else if (g_filter >= 0 && g_catGroup[i] != g_filter) {
        return 0;
    }
    if (g_kind >= 0 && g_catKind[i] != g_kind) return 0;
    if (g_namedOnly && !g_catNamed[i]) return 0;
    return 1;
}

static int ViewCount(void) {
    int i, n = 0;
    for (i = 0; i < g_catCount; i++) if (InView(i)) n++;
    return n;
}

/* The view's index -> the catalogue's index. -1 when out of range. */
static int ViewToCat(int pick) {
    int i, n = 0;
    for (i = 0; i < g_catCount; i++) {
        if (!InView(i)) continue;
        if (n == pick) return i;
        n++;
    }
    return -1;
}

/* The catalogue's index for an id, or -1. */
static int CatIndexOf(uint64_t id) {
    int i;
    for (i = 0; i < g_catCount; i++)
        if (g_catId[i] == id) return i;
    return -1;
}

/* Read the registry, as many times as it takes.
 *
 * The engine keeps its archetypes private until the game is up, so the walk
 * behind ShNpcCount answers 0 while the title screen is still showing - and it
 * is not only this page that needs a non-empty answer: ShSpawnNpc resolves an
 * id through the framework's own copy of the catalogue, so a page that read the
 * registry once, early, and kept the empty result could only ever fail to
 * summon anything. That is exactly what happened the first time this page was
 * tried: 0 entries at load, 76 ids summoned, 76 refusals - "no archetype block
 * for the id", with the count printed beside it as 0. So the read is repeated
 * until it returns something, in the worker (a second at a time, silently) and
 * on the page's own row. */
static int CollectCatalogue(void) {
    int i, n;
    if (!p_count) return 0;
    n = p_count();
    if (n <= 0) return 0;
    if (n > CAT_MAX) n = CAT_MAX;
    for (i = 0; i < n; i++) {
        const ShNpcArchetype *a = p_at(i);
        g_catId[i] = a ? a->id : 0;
        g_catKind[i] = a ? a->kind : -1;
        g_catGroup[i] = a ? p_groupOf(a) : -1;
    }
    g_catCount = n;
    RefreshNamedFlags();
    return n;
}

static void LogCatalogue(void) {
    int i;
    Log("npcprobe: catalogue %d entries", g_catCount);
    for (i = 0; i < SH_NPC_GROUP_MAX; i++) {
        int n = 0, j;
        for (j = 0; j < g_catCount; j++) if (g_catGroup[j] == i) n++;
        Log("npcprobe: group %d (%s): %d", i, ShLangText(NPCPROBE_OWNER, kGroupKey[i]), n);
    }
    {
        int n = 0, j;
        for (j = 0; j < g_catCount; j++) if (g_catGroup[j] < 0) n++;
        Log("npcprobe: ungrouped: %d", n);
    }
    /* What the kind row can offer, so the row's value means something without
     * having to be tried: only the kinds this build uses are worth naming. */
    for (i = 0; i < KIND_RANGE; i++) {
        int n = 0, j;
        for (j = 0; j < g_catCount; j++) if (g_catKind[j] == i) n++;
        if (n) Log("npcprobe: kind %d: %d", i, n);
    }
}

/* The two rows that use it: the entry the page is showing, and the entry the
 * list is pointing at - the hunt goes through the list, so that is the one that
 * has to be noteable where the player already is. */
static void OnRemember(uint32_t m, uint32_t it, int v, void *u) {
    int cat = ViewToCat(g_pick);
    (void)m; (void)it; (void)v; (void)u;
    if (cat < 0) { Log("npcprobe: nothing picked"); return; }
    RememberId(g_catId[cat], "the selected entry");
}

static void OnRememberList(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)v; (void)u;
    if (g_listCount <= 0) {
        Log("npcprobe: [Settings] ids is empty - there is no list item to note");
        return;
    }
    RememberId(g_listId[g_listPick], "the list item");
}

/* ---- the page ----------------------------------------------------------- */

static uint32_t g_menu;        /* the plugin's own page  */
static uint32_t g_menuCat;     /* catalogue submenu      */
static uint32_t g_menuList;    /* id list submenu        */
static uint32_t g_menuFiles;   /* names and files        */

/* What the page is showing, in words: the group, then whichever of the two
 * narrower filters are on. Both the log and the status line use this, so a
 * picture of the view is never ambiguous. */
/* ---- the shipped record of one build ------------------------------------ */

/* plugins\NpcProbe\npc-catalogue.txt: the ids a stated game build held, read
 * from that build's engine and shipped beside the plugin as the record of it.
 *
 * The plugin does not spawn from it and does not show it. Spawning goes through
 * the engine's registry whatever a file says, and a list from another build
 * shown as if it were this one is exactly how the last static table went wrong.
 * What a record like this is good for is the check: the engine answers every
 * session, and this says whether that answer is still the one the file was read
 * from. A file that is never compared is a file nobody can trust. */
#define REF_MAX 4096
static uint64_t g_refId[REF_MAX];
static int      g_refCount;
static int      g_refSays;      /* 1 = the last comparison found a difference */

static void RefPath(char *out, int cap) {
    const char *slash = g_ini[0] ? strrchr(g_ini, '\\') : NULL;
    if (!slash) { out[0] = 0; return; }
    snprintf(out, cap, "%.*snpc-catalogue.txt", (int)(slash - g_ini + 1), g_ini);
}

static int ReadReference(void) {
    char path[MAX_PATH], line[512];
    FILE *fp;

    g_refCount = 0;
    RefPath(path, (int)sizeof(path));
    if (!path[0]) return 0;
    fp = fopen(path, "rb");
    if (!fp) return 0;
    while (fgets(line, (int)sizeof(line), fp) && g_refCount < REF_MAX) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == 0) continue;
        while (*p && *p != ' ' && *p != '\t') p++;      /* the index column */
        while (*p == ' ' || *p == '\t') p++;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        {
            uint64_t id = (uint64_t)_strtoui64(p, NULL, 16);
            if (id) g_refId[g_refCount++] = id;         /* stops at the space */
        }
    }
    fclose(fp);
    return g_refCount;
}

/* Read the file and say how it stands against the engine's answer. Called once
 * the catalogue is in hand - at load, when the registry finally answers, and
 * whenever the reload row is used. */
static void CompareReference(void) {
    int i, refOnly = 0, engineOnly = 0;

    g_refSays = 0;
    if (!ReadReference()) {
        Log("npcprobe: no shipped catalogue beside the plugin; the engine's own "
            "answer is all there is");
        return;
    }
    if (!g_catCount) {
        Log("npcprobe: the shipped catalogue holds %d ids, and the engine has "
            "not answered yet - nothing to compare it against", g_refCount);
        return;
    }
    for (i = 0; i < g_refCount; i++)
        if (CatIndexOf(g_refId[i]) < 0) refOnly++;
    for (i = 0; i < g_catCount; i++) {
        int j, found = 0;
        for (j = 0; j < g_refCount; j++)
            if (g_refId[j] == g_catId[i]) { found = 1; break; }
        if (!found) engineOnly++;
    }
    if (refOnly || engineOnly) {
        g_refSays = 1;
        Log("npcprobe: the shipped catalogue and this engine differ: %d id(s) in "
            "the file and not in the engine, %d in the engine and not in the file "
            "(file %d, engine %d) - the game is not the build the file was read "
            "from, so read that file as history", refOnly, engineOnly, g_refCount,
            g_catCount);
    } else {
        Log("npcprobe: the shipped catalogue agrees with this engine - %d ids, "
            "and neither side holds one the other does not", g_refCount);
    }
}

/* The kind row's labels, with the count this build actually has of each: the
 * row points at these buffers, so writing them in place is what shows a count
 * without rebuilding a menu that cannot be rebuilt in place. Only the kinds
 * in use are labelled; the rest keep their bare number, so a kind this build
 * does not have still reads as itself. */
static void RefreshKindLabels(void) {
    int i;

    for (i = 0; i < KIND_RANGE; i++) {
        int n = 0, j;
        for (j = 0; j < g_catCount; j++) if (g_catKind[j] == i) n++;
        if (n)
            snprintf(kKindBuf[i], sizeof(kKindBuf[i]), "%d (%d)", i, n);
        else
            snprintf(kKindBuf[i], sizeof(kKindBuf[i]), "%d", i);
        kKindOpt[i] = kKindBuf[i];
    }
}

static void ViewName(char *out, size_t cap) {
    size_t n;

    /* GroupText(-1) is the leftovers' name, so "every group" asks for its own
     * label rather than going through it. */
    if (g_filter == GRP_ANY)
        snprintf(out, cap, "%s", ShLangText(NPCPROBE_OWNER, "@np.g.every"));
    else
        snprintf(out, cap, "%s", GroupText(g_filter));
    n = strlen(out);
    if (g_kind >= 0 && n + 1 < cap)
        snprintf(out + n, cap - n, " kind %d", g_kind);
    n = strlen(out);
    if (g_namedOnly && n + 1 < cap)
        snprintf(out + n, cap - n, " %s",
                 ShLangText(NPCPROBE_OWNER, "@np.st.named"));
}

static void LogPage(void) {
    int cat = ViewToCat(g_pick);
    char view[96];

    ViewName(view, sizeof(view));

    if (cat < 0) {
        Log("npcprobe: view %s is empty (catalogue %d)", view, g_catCount);
        return;
    }
    Log("npcprobe: view %s item %d of %d -> catalogue %d id %016llX kind %d group %s%s%s",
        view, g_pick + 1, ViewCount(), cat,
        (unsigned long long)g_catId[cat], g_catKind[cat],
        GroupText(GroupAt(cat)),
        NameOfId(g_catId[cat])[0] ? " name " : "",
        NameOfId(g_catId[cat]));
}

static void RefreshStatus(void) {
    uint32_t menus[4];
    int menuCount = 0, mi;
    int cat = ViewToCat(g_pick);
    const char *listPart = g_listCount > 0 ? ""
                          : ShLangText(NPCPROBE_OWNER, "@np.st.nolist");
    char view[96];
    char who[NAME_LEN + 48];

    /* The page is four menus deep, and the framework draws the status line of
     * the page on screen and of no other - so the same line is written to every
     * one of them. It costs a format and answers wherever the operator is. */
    RefreshKindLabels();
    menus[menuCount++] = g_menu;
    if (g_menuCat)   menus[menuCount++] = g_menuCat;
    if (g_menuList)  menus[menuCount++] = g_menuList;
    if (g_menuFiles) menus[menuCount++] = g_menuFiles;

    /* The filters, in the one place the templates already print the view's
     * name, so what is on is visible whatever else is on the line. */
    ViewName(view, sizeof(view));

    /* The selected entry: the operator's own name for it when there is one,
     * then the framework's group. One of them is always there. */
    if (cat < 0) {
        snprintf(who, sizeof(who), "-");
    } else {
        const char *nm = NameOfId(g_catId[cat]);
        if (nm[0])
            snprintf(who, sizeof(who), "%s · %s", nm, GroupText(GroupAt(cat)));
        else
            snprintf(who, sizeof(who), "%s", GroupText(GroupAt(cat)));
    }

    for (mi = 0; mi < menuCount; mi++) {
        if (g_listCount > 0)
            ShMenuStatusF(menus[mi], "@np.st.both",
                          view, g_pick + 1, ViewCount(),
                          cat < 0 ? 0ULL : (unsigned long long)g_catId[cat],
                          cat < 0 ? -1 : g_catKind[cat], who,
                          g_listPick + 1, g_listCount,
                          (unsigned long long)g_listId[g_listPick]);
        else if (cat < 0)
            ShMenuStatusF(menus[mi], "@np.st.none", g_catCount, view, listPart);
        else
            ShMenuStatusF(menus[mi], "@np.st.one",
                          g_catCount, view, g_pick + 1, ViewCount(),
                          (unsigned long long)g_catId[cat], g_catKind[cat],
                          who, listPart);
    }
}

/* The framework's per frame hook, which runs on the game thread - the same
 * thread a menu callback runs on, and the only place a plugin may touch its
 * menu from outside one. It costs two compares a frame and does nothing at all
 * unless something behind the page changed. */
static void NpcProbeFrame(void *user) {
    (void)user;
    if (InterlockedCompareExchange(&g_namesStale, 0, 1) == 1) {
        ReadNames();
        InterlockedExchange(&g_uiStale, 1);
    }
    if (InterlockedCompareExchange(&g_uiStale, 0, 1) == 1)
        RefreshStatus();
}

/* Back to the first entry of a new view. The row is told as well: the row shows
 * a number the operator reads, and with the row counting from 1 and the index
 * from 0, a row left carrying an old number would point at an entry other than
 * the one the status line names. */
static void ResetPick(void) {
    g_pick = 0;
    if (g_menuCat) ShMenuSetValue(g_menuCat, "@np.item", 1);
}

static void OnFilter(uint32_t m, uint32_t it, int v, void *u) {
    if (v <= 0)
        g_filter = GRP_ANY;
    else if (v == SH_NPC_GROUP_MAX + 1)
        g_filter = GRP_UNGROUPED;
    else
        g_filter = v - 1;
    ResetPick();
    RefreshStatus();
    LogPage();
}

/* The engine's kind, the finer of the two filters. It is the only one that
 * reaches an archetype the group bucket misnames. */
static void OnKind(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    g_kind = v - 1;                 /* option 0 is "any kind" */
    ResetPick();
    RefreshStatus();
    LogPage();
}

/* Only the entries that carry a name: the operator's own shortlist, without
 * the 527 entries around it. */
static void OnNamedOnly(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    g_namedOnly = v ? 1 : 0;
    ResetPick();
    Log("npcprobe: named only %s (%d of %d entries carry a name)",
        g_namedOnly ? "on" : "off", g_nameCount, g_catCount);
    RefreshStatus();
    LogPage();
}

static void OnPick(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    g_pick = v - 1;                 /* the row counts from 1, the index from 0 */
    if (g_pick > ViewCount() - 1) g_pick = ViewCount() > 0 ? ViewCount() - 1 : 0;
    if (g_pick < 0) g_pick = 0;
    RefreshStatus();
    LogPage();
}

static void OnInfo(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)v; (void)u;
    LogPage();
}

/* Ask for a summon by catalogue index, or by the id in [NpcProbe] id. */
static void RequestSummon(uint64_t id) {
    if (!id) {
        Log("npcprobe: nothing to summon (id 0)");
        return;
    }
    if (InterlockedCompareExchange(&g_request, 1, 0) != 0) {
        Log("npcprobe: a summon is already in flight, id %016llX dropped",
            (unsigned long long)id);
        return;
    }
    InterlockedExchange(&g_reqId, (LONG)(id & 0xFFFFFFFFu));
    InterlockedExchange(&g_reqIdHi, (LONG)(id >> 32));
    Log("npcprobe: requested id %016llX", (unsigned long long)id);
}

static void OnSummon(uint32_t m, uint32_t it, int v, void *u) {
    int cat = ViewToCat(g_pick);
    (void)m; (void)it; (void)v; (void)u;
    if (cat < 0) { Log("npcprobe: nothing picked"); return; }
    RequestSummon(g_catId[cat]);
}

static void OnSummonByIni(uint32_t m, uint32_t it, int v, void *u) {
    char buf[64] = { 0 };
    (void)m; (void)it; (void)v; (void)u;
    IniStr("id", "", buf, (int)sizeof(buf));
    if (!buf[0]) {
        Log("npcprobe: [Settings] id is empty - put a hex id there to try one "
            "the catalogue does not hold");
        return;
    }
    RequestSummon((uint64_t)_strtoui64(buf, NULL, 16));
}

/* Read the registry again, on demand. The page may have been built before the
 * game was up, in which case its catalogue is empty and every summon through
 * ShSpawnNpc will fail until it is not: this row is what fixes that without a
 * restart. */
static void OnReload(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)v; (void)u;
    /* The names are read here too: [Names] is a text file the operator edits,
     * and a row that only re-read the registry would leave a name they just
     * wrote invisible until the next start. */
    ReadNames();
    if (CollectCatalogue() > 0) {
        Log("npcprobe: catalogue read again: %d entries", g_catCount);
        LogCatalogue();
        CompareReference();
        if (g_pick > ViewCount() - 1)
            g_pick = ViewCount() > 0 ? ViewCount() - 1 : 0;
        ShMenuSetValue(g_menuCat, "@np.item", g_pick + 1);
    } else {
        Log("npcprobe: the registry still answers 0 - it is only readable while "
            "a game is running");
    }
    RefreshStatus();
}

/* Write the whole catalogue beside the plugin. The page shows one entry at a
 * time, and "which ids can this build summon" is a question about all of them
 * at once - but the answer still comes from the engine, at run time, and only
 * when this row is pressed: nothing is compiled in, and no id reaches the file
 * that the engine did not hand over. */
static void OnDump(uint32_t m, uint32_t it, int v, void *u) {
    static const char suffix[] = ".catalogue.txt";
    char path[MAX_PATH], *dot;
    FILE *fp;
    int i;

    (void)m; (void)it; (void)v; (void)u;
    if (g_catCount <= 0) {
        Log("npcprobe: the catalogue is empty, so there is nothing to write - "
            "read it again first");
        return;
    }
    if (!ShPluginIniPath(NPCPROBE_OWNER, path, (int)sizeof(path))) {
        Log("npcprobe: no path to write the catalogue to");
        return;
    }
    dot = strrchr(path, '.');
    if (dot && (size_t)(dot - path) + sizeof(suffix) <= sizeof(path))
        memcpy(dot, suffix, sizeof(suffix));
    fp = fopen(path, "wb");
    if (!fp) { Log("npcprobe: cannot write %s", path); return; }
    fprintf(fp, "# The NPC archetypes this build's engine holds, read at run "
                "time by NpcProbe.\n");
    fprintf(fp, "# index  id (hex)           kind  group         name\n");
    for (i = 0; i < g_catCount; i++)
        fprintf(fp, "%5d  0x%016llX  %4d  %-12s  %s\n", i,
                (unsigned long long)g_catId[i], g_catKind[i],
                GroupText(GroupAt(i)), NameOfId(g_catId[i]));
    fclose(fp);
    Log("npcprobe: catalogue written to %s (%d entries)", path, g_catCount);
}

/* The shortlist on its own, one line per name: what the operator has
 * identified, with the catalogue's index and kind beside it where the id is in
 * the catalogue. A name whose id the catalogue does not hold is written too and
 * marked - it is theirs, and leaving it out would misdescribe the file. */
static void OnDumpNames(uint32_t m, uint32_t it, int v, void *u) {
    static const char suffix[] = ".names.txt";
    char path[MAX_PATH], *dot;
    FILE *fp;
    int i;

    (void)m; (void)it; (void)v; (void)u;
    if (g_nameCount <= 0) {
        Log("npcprobe: the [Names] section is empty, so there is nothing to "
            "write");
        return;
    }
    if (!ShPluginIniPath(NPCPROBE_OWNER, path, (int)sizeof(path))) {
        Log("npcprobe: no path to write the names to");
        return;
    }
    dot = strrchr(path, '.');
    if (dot && (size_t)(dot - path) + sizeof(suffix) <= sizeof(path))
        memcpy(dot, suffix, sizeof(suffix));
    fp = fopen(path, "wb");
    if (!fp) { Log("npcprobe: cannot write %s", path); return; }
    fprintf(fp, "# NpcProbe's names: the archetypes this operator has "
                "identified.\n");
    fprintf(fp, "# id (hex)           index  kind  name\n");
    for (i = 0; i < g_nameCount; i++) {
        int ci = CatIndexOf(g_nameId[i]);
        if (ci >= 0)
            fprintf(fp, "0x%016llX  %5d  %4d  %s\n",
                    (unsigned long long)g_nameId[i], ci, g_catKind[ci],
                    g_nameText[i]);
        else
            fprintf(fp, "0x%016llX  %5s  %4s  %s\n",
                    (unsigned long long)g_nameId[i], "-", "-", g_nameText[i]);
    }
    fclose(fp);
    Log("npcprobe: the %d name(s) written to %s", g_nameCount, path);
}

/* The list, from [NpcProbe] ids: hex ids, separated by commas or spaces. */
static void ReadIdList(void) {
    static char buf[8192];
    char *p;

    g_listCount = 0;
    IniStr("ids", "", buf, (int)sizeof(buf));
    if (!buf[0]) return;
    p = buf;
    while (*p && g_listCount < LIST_MAX) {
        char *next;
        uint64_t id;
        while (*p == ' ' || *p == ',' || *p == ';' || *p == '\t') p++;
        if (!*p) break;
        next = p;
        while (*next && *next != ',' && *next != ';' && *next != ' ' &&
               *next != '\t') next++;
        if (*next) *next++ = 0;
        id = (uint64_t)_strtoui64(p, NULL, 16);
        if (id) g_listId[g_listCount++] = id;
        p = next;
    }
    Log("npcprobe: [NpcProbe] ids carried %d usable id(s)", g_listCount);
}

static void OnListPick(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    /* The row counts from 1, as the log line does and as anyone counting does.
     * It used to count from 0, which put the page and the log one apart - and
     * that is how a neighbouring entry came to be recorded as the right one
     * (0x537991063F instead of the Predator, 0x154BBB495E1). One numbering, and
     * the number in the page, the log and the ini can be compared directly. */
    g_listPick = v - 1;
    if (g_listPick < 0) g_listPick = 0;
    if (g_listPick > g_listCount - 1) g_listPick = g_listCount > 0 ? g_listCount - 1 : 0;
    RefreshStatus();
}

static void OnSummonList(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)v; (void)u;
    if (g_listCount <= 0) {
        Log("npcprobe: [NpcProbe] ids is empty - no list to summon from");
        return;
    }
    Log("npcprobe: list item %d of %d", g_listPick + 1, g_listCount);
    RequestSummon(g_listId[g_listPick]);
}

/* Step the list on and summon, so a hunt is two keys per candidate. */
static void OnSummonNext(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)v; (void)u;
    if (g_listCount <= 0) {
        Log("npcprobe: [NpcProbe] ids is empty - nothing to step through");
        return;
    }
    OnSummonList(m, it, v, u);
    g_listPick = (g_listPick + 1) % g_listCount;
    ShMenuSetValue(g_menuList, "@np.litem", g_listPick + 1);
    RefreshStatus();
}

/* ---- three settings for the summon itself ------------------------------- */

/* How far out, along the way the camera looks. The player's position is what
 * the height comes from, so this is a horizontal distance. */
static void OnDist(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    g_distPick = (v >= 0 && v < DIST_N) ? v : 0;
    Log("npcprobe: summons are placed %s from the player, along the camera",
        kDistLabel[g_distPick]);
}

/* How many copies one press asks for. They are spread sideways, not stacked. */
static void OnBatch(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    g_batch = v < 1 ? 1 : (v > BATCH_MAX ? BATCH_MAX : v);
    Log("npcprobe: %d per press", g_batch);
}

/* Note every id that gets summoned, so a hunt does not need a keypress per
 * candidate: the rows are appended blank and the names are written afterwards,
 * in one pass, in an editor. Off by default - it grows the file. */
static void OnAutoNote(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    g_autoNote = v ? 1 : 0;
    Log("npcprobe: noting every summon in the names section is %s",
        g_autoNote ? "on" : "off");
}

/* ---- invincible and invisible, for looking at a hostile summon ---------- */

/* Both are the framework's own facilities. ShSetGodModePlayer writes the
 * player health component's god and no-damage bytes; ShSetVisibility scales
 * how far enemies notice the player, 0 being not at all. Neither is written
 * by this plugin directly, and neither is invented: the switch only asks.
 *
 * A respawn builds a new component, so a switch left on would quietly stop
 * holding. The worker tops both up once a second while they are on - silently,
 * because a line a second would bury the log - and the switch itself is what
 * gets logged, once, when it is turned. */
static volatile LONG g_god;
static volatile LONG g_ghost;
static DWORD         g_topUp;    /* when either was last re-asked */

static void GodAsk(int on) {
    if (p_godPlayer) p_godPlayer(on ? 1 : 0);
}

static void GhostAsk(int on) {
    if (p_setVis) p_setVis(on ? 0.0f : 1.0f);
}

static void OnGod(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    InterlockedExchange(&g_god, v ? 1 : 0);
    if (!p_godPlayer) {
        Log("npcprobe: this dinput8 has no ShSetGodModePlayer - the switch "
            "does nothing");
        return;
    }
    if (p_godPlayer(v ? 1 : 0))
        Log("npcprobe: player invincible %s", v ? "on" : "off");
    else
        Log("npcprobe: player invincible could not be set (%08X, %s)",
            ShLastError(), ShErrorString(ShLastError()));
}

static void OnGhost(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    InterlockedExchange(&g_ghost, v ? 1 : 0);
    if (!p_setVis) {
        Log("npcprobe: this dinput8 has no ShSetVisibility - the switch does "
            "nothing");
        return;
    }
    if (p_setVis(v ? 0.0f : 1.0f))
        Log("npcprobe: invisible to enemies %s", v ? "on" : "off");
    else
        Log("npcprobe: invisibility could not be set (%08X, %s) - the build's "
            "instruction did not match, so nothing was patched",
            ShLastError(), ShErrorString(ShLastError()));
}

/* Ask for everything this page made to be taken away again. The work itself is
 * the worker's: ShDespawn blocks the way ShSpawnNpc does. */
static void OnClearSpawned(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)v; (void)u;
    if (!p_despawn) {
        Log("npcprobe: this dinput8 has no ShDespawn, so what was summoned stays "
            "where it is");
        return;
    }
    if (!g_spawnedCount) {
        Log("npcprobe: this page has made nothing this session, so there is "
            "nothing to clear");
        return;
    }
    InterlockedExchange(&g_clear, 1);
    Log("npcprobe: asked for the %d entities this page made to be despawned",
        g_spawnedCount);
}

/* ---- the worker --------------------------------------------------------- */

/* ShSpawnNpc blocks until the entity is there, so it runs here and nowhere
 * near a menu callback. A metre and a half in front of the player is close
 * enough to see and far enough not to be inside them. */
static DWORD WINAPI SpawnThread(LPVOID p) {
    (void)p;
    for (;;) {
        uint64_t id, ent;
        ShVec3 at;
        int ok, k;

        /* The two switches, re-asked once a second while they are on, so a
         * respawn does not quietly take them away - and the catalogue, asked
         * for again while it is still empty, because an empty catalogue is
         * what makes every summon fail. No menu call is made from here: the
         * status line picks the new count up on the next interaction. */
        if (GetTickCount() - g_topUp > 1000) {
            g_topUp = GetTickCount();
            if (InterlockedCompareExchange(&g_god, 0, 0)) GodAsk(1);
            if (InterlockedCompareExchange(&g_ghost, 0, 0)) GhostAsk(1);
            /* Asked for again until it answers, with no "in a game" guard: the
             * registry answers 0 until it can answer anything else, so asking
             * is harmless and asking early is the point. The guard held this
             * back for as long as the framework's own idea of being in a game
             * took to settle - which was well after the registry was ready. */
            if (!g_catCount && CollectCatalogue() > 0) {
                Log("npcprobe: catalogue %d entries (read once the registry "
                    "answered)", g_catCount);
                LogCatalogue();
                CompareReference();
                /* The page is the game thread's, so all this does is say it
                 * has something to pick up. */
                InterlockedExchange(&g_uiStale, 1);
            }
        }

        /* Asked for by the menu, done here: ShDespawn blocks the way
         * ShSpawnNpc does, so it is no more welcome on the game thread. */
        if (InterlockedCompareExchange(&g_clear, 0, 1) == 1) {
            int i, gone = 0;
            /* ShDespawn answers 1 only after it has read the entity back and
             * found it gone, so this is what actually left and not what was
             * asked for. On this build the retire call is the framework's
             * weakest pin, so a 0 here means nothing was removed at all. */
            for (i = 0; i < g_spawnedCount; i++)
                if (g_spawned[i] && p_despawn && p_despawn(g_spawned[i]))
                    gone++;
            Log("npcprobe: %d of the %d entities this page summoned are gone "
                "(if that is 0, the engine refused to retire them: see the "
                "framework's RETIRE note - use a drop or a reload instead)",
                gone, g_spawnedCount);
            g_spawnedCount = 0;
            g_spawnedFull = 0;
        }

        if (InterlockedCompareExchange(&g_request, 0, 1) != 1) {
            Sleep(NPCPROBE_TICK_MS);
            continue;
        }

        id = ((uint64_t)(ULONG)InterlockedCompareExchange(&g_reqIdHi, 0, 0) << 32) |
             (ULONG)InterlockedCompareExchange(&g_reqId, 0, 0);

        /* Where they go: g_distPick metres from the player along the line the
         * camera looks, so a hostile arrival does not stand inside them. The
         * player's own position keeps the height right - ShSpawnNpc stands an
         * entity where it is handed a position, and the camera's eye is at
         * head height, so using it would drop them from up there. yaw is
         * atan2(forward.x, forward.y) by the framework's own contract, so
         * forward is (sin yaw, cos yaw) and nothing here has to guess a sign. */
        {
            float yaw = 0.0f;
            int haveYaw = 0;
            const char *nm = NameOfId(id);   /* the operator's own name, or "" */

            memset(&at, 0, sizeof(at));
            ok = p_playerPos && p_playerPos(&at);
            if (!ok) {
                Log("npcprobe: no player position; summoning at the origin");
            } else {
                float want = (g_distPick >= 0 && g_distPick < DIST_N)
                                 ? kDistM[g_distPick] : kDistM[0];
                if (want > 0.01f && p_camAngles && p_camAngles(&yaw, NULL)) {
                    haveYaw = 1;
                    at.x += sinf(yaw) * want;
                    at.y += cosf(yaw) * want;
                }
                Log("npcprobe: player %.0f %.0f %.0f%s -> summoning %.1f m out "
                    "at %.0f %.0f %.0f", at.x, at.y, at.z,
                    haveYaw ? " (along the camera)" : " (no camera angle)",
                    want, at.x, at.y, at.z);
            }

            for (k = 0; k < g_batch; k++) {
                ShVec3 one = at;

                /* Side by side rather than inside one another, on the axis the
                 * camera's forward turns into a right vector. */
                if (k) {
                    one.x += (haveYaw ? cosf(yaw) : 1.0f) * 0.9f * (float)k;
                    one.y -= (haveYaw ? sinf(yaw) : 0.0f) * 0.9f * (float)k;
                }

                ent = p_spawn(id, &one);
                if (!ent) {
                    int ci = CatIndexOf(id);
                    if (!g_catCount)
                        Log("npcprobe: id %016llX%s%s -> nothing: the catalogue "
                            "is empty, and ShSpawnNpc resolves an id through it "
                            "- it is asked for again every second until the "
                            "registry answers",
                            (unsigned long long)id, nm[0] ? " " : "", nm);
                    else if (ci < 0)
                        Log("npcprobe: id %016llX%s%s -> nothing: it is not one "
                            "of the %d entries in the catalogue, so the engine "
                            "is never asked to resolve it",
                            (unsigned long long)id, nm[0] ? " " : "", nm,
                            g_catCount);
                    else
                        Log("npcprobe: id %016llX%s%s -> nothing, though it is "
                            "catalogue entry %d (kind %d) - see the npc log for "
                            "the step it stopped at", (unsigned long long)id,
                            nm[0] ? " " : "", nm, ci, g_catKind[ci]);
                    break;               /* one refusal is the whole story */
                }

                InterlockedExchange(&g_lastEntity, 1);
                g_spawnedTotal++;
                if (g_spawnedCount < SPAWNED_MAX)
                    g_spawned[g_spawnedCount++] = ent;
                else if (!g_spawnedFull) {
                    g_spawnedFull = 1;
                    Log("npcprobe: the list of what this page made holds %d; "
                        "what is made past that cannot be cleared from here",
                        SPAWNED_MAX);
                }
                Log("npcprobe: id %016llX%s%s -> entity %llX at %.0f %.0f %.0f%s",
                    (unsigned long long)id, nm[0] ? " " : "", nm,
                    (unsigned long long)ent, one.x, one.y, one.z,
                    g_batch > 1 ? " (one of several asked for)" : "");
            }

            /* Once per press, not once per copy: it is the id that is worth
             * noting, and the same line appended twice is only a mess. */
            if (g_autoNote) NoteIdQuiet(id);
        }
    }
}

/* ---- start -------------------------------------------------------------- */

static DWORD WINAPI InitThread(LPVOID p) {
    static const char *filterOpts[SH_NPC_GROUP_MAX + 2];
    static const char *kindOpts[KIND_RANGE + 1];
    static const char *distOpts[DIST_N];
    int i;

    (void)p;
    while (!ShGetVersion()) Sleep(500);

    if (!Bind()) {
        LogInit("NpcProbe.log");
        Log("npcprobe: dinput8 has no NPC API; this build cannot use it");
        return 0;
    }

    LogInit("NpcProbe.log");
    /* Before either user of the ini's text exists: the menu and, later, the
     * worker that "note every summon" sends to it. */
    InitializeCriticalSection(&g_fileLock);
    ShLangDeclare(NPCPROBE_OWNER, "en-US", kEn, (int)(sizeof(kEn) / sizeof(kEn[0])));
    ShLangDeclare(NPCPROBE_OWNER, "zh-CN", kZh, (int)(sizeof(kZh) / sizeof(kZh[0])));
    if (!ShPluginIniPath("NpcProbe", g_ini, (int)sizeof(g_ini))) g_ini[0] = 0;
    if (!IniInt("enabled", 0)) {
        Log("npcprobe: [Settings] enabled=0 in %s, the page is not created",
            g_ini[0] ? g_ini : "the plugin ini (not found)");
        return 0;
    }

    /* The catalogue, if the engine will give it now. It usually will not: a
     * plugin is loaded while the title screen is still up, and the registry is
     * only readable in a running game. So an empty answer here is expected and
     * is not the end of it - the worker asks again every second, and the page
     * has a row that asks on the spot. */
    if (CollectCatalogue() > 0) {
        LogCatalogue();
        CompareReference();
    } else {
        Log("npcprobe: the registry answers 0 entries at load; it will be asked "
            "again once a game is running");
    }

    /* The list row shows resolved text: an option is a plain string and the
     * framework does not translate those, so each one is asked for here. */
    filterOpts[0] = ShLangText(NPCPROBE_OWNER, "@np.g.every");
    for (i = 0; i < SH_NPC_GROUP_MAX; i++) filterOpts[i + 1] = GroupText(i);
    filterOpts[SH_NPC_GROUP_MAX + 1] =
        ShLangText(NPCPROBE_OWNER, "@np.g.ungrouped");
    kindOpts[0] = ShLangText(NPCPROBE_OWNER, "@np.kind.any");
    for (i = 0; i < KIND_RANGE; i++) kindOpts[i + 1] = kKindOpt[i];
    /* Metres, so the labels need no translation and cannot be wrong in either
     * language. */
    for (i = 0; i < DIST_N; i++) distOpts[i] = kDistLabel[i];

    ReadIdList();
    ReadNames();

    /* An empty list while the file does carry an ids= line is the one mistake
     * this file invites: a [section] header ends the section before it, so an
     * ids= written below [Names] belongs to [Names] and [Settings] has none.
     * Naming the line turns a silent "nothing to step through" - which is how
     * the list was lost once already - into something to act on. */
    if (g_listCount == 0) {
        char *p = g_iniText;
        IniLock();
        ReadIniText();
        p = g_iniText;
        for (;;) {
            char *nl = strchr(p, '\n');
            char *end = nl ? nl : p + strlen(p);
            char *q = p;
            while (q < end && (*q == ' ' || *q == '\t')) q++;
            if (q + 4 < end && _strnicmp(q, "ids=", 4) == 0 &&
                q[4] != ' ' && q[4] != '\t') {
                Log("npcprobe: [Settings] ids is empty, but the file has an "
                    "ids= line elsewhere - a [section] header ends the one "
                    "before it, so the list has to sit under [Settings]; the "
                    "list is off until it does");
                break;
            }
            if (!nl) break;
            p = nl + 1;
        }
        IniUnlock();
    }

    g_menu = ShMenuCreate("@np.page");
    /* Three submenus, so the page is not one wall of seventeen rows: what to
     * look at, the operator's own set of ids, and the names and files. The
     * settings that change the summon itself stay on the page itself. The
     * framework draws the status line of the page on screen and of no other,
     * so RefreshStatus writes the same line to all four. */
    g_menuCat   = ShMenuSub(g_menu, "@np.sub.cat");
    g_menuList  = ShMenuSub(g_menu, "@np.sub.list");
    g_menuFiles = ShMenuSub(g_menu, "@np.sub.files");

    ShMenuList(g_menuCat, "@np.group", filterOpts, SH_NPC_GROUP_MAX + 2, 0,
               OnFilter, NULL);
    /* The engine's own kind, beside the bucket. The two compose: pick a group
     * and a kind and the list holds what is in both. */
    ShMenuList(g_menuCat, "@np.kind", kindOpts, KIND_RANGE + 1, 0, OnKind, NULL);
    /* The operator's own shortlist, once there is one to shortlist. */
    ShMenuToggle(g_menuCat, "@np.named", 0, OnNamedOnly, NULL);
    /* The row is given the whole catalogue's width, not the count read so far:
     * the read can arrive later, and a row's range cannot be widened after it
     * exists. It counts from 1 (1…4096), as the log line and the status line do
     * - one numbering on the page, in the log and in the ini. OnPick and the
     * status line keep the number honest. */
    ShMenuNumber(g_menuCat, "@np.item", 1, 1, (float)CAT_MAX, 1,
                 OnPick, NULL);
    ShMenuAction(g_menuCat, "@np.show", OnInfo, NULL);
    ShMenuAction(g_menuCat, "@np.summon", OnSummon, NULL);
    ShMenuAction(g_menuCat, "@np.remember", OnRemember, NULL);

    /* One-based, so the row, the log line and the ini all carry the same
     * number - they used to differ by a digit, which is how a neighbouring
     * entry got written down as the Predator. */
    ShMenuNumber(g_menuList, "@np.litem", 1, 1,
                 (float)(g_listCount > 0 ? g_listCount : 1), 1,
                 OnListPick, NULL);
    ShMenuAction(g_menuList, "@np.summonList", OnSummonList, NULL);
    ShMenuAction(g_menuList, "@np.rememberList", OnRememberList, NULL);
    ShMenuAction(g_menuList, "@np.summonNext", OnSummonNext, NULL);
    ShMenuAction(g_menuList, "@np.summonId", OnSummonByIni, NULL);

    ShMenuAction(g_menuFiles, "@np.reload", OnReload, NULL);
    ShMenuAction(g_menuFiles, "@np.dumpNames", OnDumpNames, NULL);
    ShMenuAction(g_menuFiles, "@np.dump", OnDump, NULL);
    ShMenuAction(g_menuFiles, "@np.clear", OnClearSpawned, NULL);

    ShMenuList(g_menu, "@np.dist", distOpts, DIST_N, 0, OnDist, NULL);
    ShMenuNumber(g_menu, "@np.batch", 1, 1, (float)BATCH_MAX, 1, OnBatch, NULL);
    ShMenuToggle(g_menu, "@np.autonote", 0, OnAutoNote, NULL);
    ShMenuToggle(g_menu, "@np.god", 0, OnGod, NULL);
    ShMenuToggle(g_menu, "@np.ghost", 0, OnGhost, NULL);
    RefreshStatus();

    /* Registered before the worker exists, so the page cannot be refreshed
     * before it has been built. */
    if (p_frameCb && p_frameCb(NpcProbeFrame, NULL))
        Log("npcprobe: the page refreshes itself when the catalogue arrives");
    else
        Log("npcprobe: this dinput8 has no per frame hook, so the count on the "
            "page moves only when a row is used");

    CreateThread(NULL, 0, SpawnThread, NULL, 0, NULL);
    Log("npcprobe: page created");
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
