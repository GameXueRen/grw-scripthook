/* Vehicle spawner. A submenu of the shared root menu, so
 * navigation, scrolling and drawing belong to the API.
 */
/* Binds late by choice. Plugins may import the ScriptHook
 * directly instead, since the loader loads them from a
 * thread rather than from DllMain. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#include "scripthook.h"
#include "log.h"

/* What this plugin needs of the framework: nothing newer than the first
 * version of the plugin API, so any ScriptHook that carries the API at all can
 * load this (see SH_REQUIRES_API). Name the last thing you use, not the header
 * you happened to build against. */
SH_REQUIRES_API(1);

#define AHEAD  6.0f
#define LIFT   1.0f

typedef struct { uint32_t id; const char *name; } Vehicle;

typedef int (*Count_t)(void);
typedef const Vehicle *(*At_t)(int);
typedef uint64_t (*Spawn_t)(uint32_t, const ShVec3 *);
typedef int (*PlayerPos_t)(ShVec3 *);
typedef uint32_t (*MenuCreate_t)(const char *);
typedef int (*MenuAction_t)(uint32_t, const char *, ShMenuFn, void *);
typedef int (*MenuStatus_t)(uint32_t, const char *);
typedef int (*MenuStatusF_t)(uint32_t, const char *, ...);
typedef int (*MenuHint_t)(uint32_t, const char *);
typedef uint32_t (*MenuSub_t)(uint32_t, const char *);

/* This plugin had no log at all, which made the one failure it can have -
 * an export this dinput8 does not carry - look like a submenu that simply
 * never appeared. One file, opened on first use, through the framework's own
 * writer: it lands in logs\ with the other logs, it obeys [Settings] LogLevel
 * the way a plugin's log does - written at every level except none, because
 * the line a plugin's log is read for is usually the one about something not
 * working - and it is kept for the runs before this one instead of being wiped
 * at start up (log.h renames the previous run's file aside).
 *
 * LogInit opens a file, so one thread does it and the rest go straight to
 * Logv, which drops the line while it is still being opened. */
static volatile LONG g_logMade;

static void SpLog(const char *fmt, ...) {
    va_list ap;

    if (!g_logMade && InterlockedCompareExchange(&g_logMade, 1, 0) == 0)
        LogInitAlways("spawner.log");
    va_start(ap, fmt);
    Logv(fmt, ap);
    va_end(ap);
}

static Count_t      g_count;
static At_t         g_at;
static Spawn_t      g_spawn;
static PlayerPos_t  g_playerPos;
static MenuStatus_t g_status;
static MenuStatusF_t g_statusF;
static MenuHint_t   g_hint;

static uint32_t g_menu = 0;
static volatile int g_spawned = 0;

/* Where the vehicle lands: AHEAD metres the way the player is facing, not
 * AHEAD metres east. It used to be pos.x += AHEAD whatever direction that
 * was, so facing north put the vehicle out to your right - raised in the
 * audit as the one thing in the spawner that looked like a slip, and
 * settled on 2026-09-17: it should follow the facing.
 *
 * ShGetCamera answers with the pose matrix, and its forward row is the
 * direction the player is looking - the same vector in first and third
 * person, and the same one the aim uses. Optional at bind time: a dinput8
 * that does not carry it falls back to the old side offset rather than
 * dropping the vehicle on the player's head. */
typedef int (*Camera_t)(ShCamera *);
static Camera_t g_camera;

/* How far the framework's spec warm-up has got (ShSpawnWarmProgress).
 * Optional as well: a framework that does not carry it leaves the status
 * line saying "spawning...", which is what it said before. */
typedef int (*WarmProgress_t)(int *done, int *total);
static WarmProgress_t g_warmProgress;

/* Runs on the menu thread with the API's lock dropped, so a
 * plain API call here is exactly what the ABI expects.
 */
static void OnSpawn(uint32_t menu, uint32_t item, int value,
                    void *user) {
    const Vehicle *v = (const Vehicle *)user;
    ShVec3 pos;
    uint64_t ent;
    int aimed = 0;

    (void)item; (void)value;
    if (!v) return;
    if (!g_playerPos(&pos)) {
        g_status(menu, "@sp.noplayer");
        return;
    }
    /* Horizontal only: the forward row tilts with the view, and a vehicle
     * placed along a look down at the ground would go underground. The
     * lift below is what raises it. */
    if (g_camera) {
        ShCamera cam;

        memset(&cam, 0, sizeof(cam));
        if (g_camera(&cam)) {
            float fx = cam.forward.x, fy = cam.forward.y;
            float len = fx * fx + fy * fy;

            if (len > 1e-6f) {
                float k = AHEAD / (float)sqrt((double)len);

                pos.x += fx * k;
                pos.y += fy * k;
                aimed = 1;
            }
        }
    }
    if (!aimed) pos.x += AHEAD;      /* no camera to ask: the old offset */
    pos.z += LIFT;

    /* The framework walks the whole address space for the vehicle specs the
     * first time the world is playable, about fifteen seconds (measured on
     * this machine, 2026-09-17). A dispatch that lands inside that window
     * waits for it - which is fine, and used to look like nothing was
     * happening at all. The number moving is the whole difference between
     * waiting and being stuck, so this waits here rather than in there, and
     * says so while it does. */
    if (g_warmProgress) {
        int done = 0, total = 0;

        while (g_warmProgress(&done, &total)) {
            g_statusF(menu, "@sp.warming", done, total);
            Sleep(200);
        }
    }
    g_status(menu, "@sp.spawning");
    ent = g_spawn(v->id, &pos);
    if (ent) {
        g_spawned++;
        g_statusF(menu, "@sp.spawned", g_spawned);
    } else {
        g_status(menu, "@sp.nothing");
    }
}

/* ---- text ---------------------------------------------------------
 * The plugin's own text, compiled in. lang.ini beside this source keeps
 * the catalogue labels - the framework's vehicle names are keys there -
 * and may override any of these rows. Keys are stable IDs, so rewording
 * one never breaks a translation.
 */
typedef struct { const char *key; const char *text; } TextRow;
typedef int (*LangDeclare_t)(const char *owner, const char *lang,
                             const TextRow *rows, int n);
static LangDeclare_t pLangDeclare;

static const TextRow kEn[] = {
    { "@sp.page",     "Vehicle dispatch" },
    { "@sp.hint",     "The first dispatch may take a moment." },
    { "@sp.cat.air",  "Aircraft" },
    { "@sp.cat.armor", "Armoured / military" },
    { "@sp.cat.suv",  "SUV, pickup, off-road" },
    { "@sp.cat.car",  "Cars" },
    { "@sp.cat.truck", "Trucks, buses, vans" },
    { "@sp.cat.bike", "Motorcycles" },
    { "@sp.cat.boat", "Boats" },
    { "@sp.cat.misc", "Special / unusable" },
    { "@sp.cat.misc.hint",
      "Alpaca and Monster freeze the game on entry." },
    { "@sp.noplayer", "no player position" },
    { "@sp.spawning", "spawning..." },
    { "@sp.spawned",  "spawned, %d this session" },
    { "@sp.warming",  "preparing, %d of %d specs" },
    { "@sp.nothing",  "nothing appeared" }
};

static const TextRow kZh[] = {
    { "@sp.page",     "载具派遣" },
    { "@sp.hint",     "首次载具派遣可能需要一点时间。" },
    { "@sp.cat.air",  "飞行器" },
    { "@sp.cat.armor", "装甲·军用" },
    { "@sp.cat.suv",  "SUV·皮卡·越野" },
    { "@sp.cat.car",  "轿车·跑车" },
    { "@sp.cat.truck", "卡车·巴士·厢车" },
    { "@sp.cat.bike", "摩托车" },
    { "@sp.cat.boat", "船" },
    { "@sp.cat.misc", "特殊·不可用" },
    { "@sp.cat.misc.hint", "Alpaca 与 Monster 进入后会卡死，勿点。" },
    { "@sp.noplayer", "无法获取玩家位置" },
    { "@sp.spawning", "正在生成……" },
    { "@sp.spawned",  "已生成，本次会话共 %d 辆" },
    { "@sp.warming",  "预热中 %d/%d" },
    { "@sp.nothing",  "没有出现" }
};

static void TextInit(void) {
    static int done;
    HMODULE m;

    if (done) return;
    m = GetModuleHandleA("dinput8.dll");
    if (!m) return;
    if (!pLangDeclare)
        *(FARPROC *)&pLangDeclare = GetProcAddress(m, "ShLangDeclare");
    if (!pLangDeclare) return;
    done = 1;
    pLangDeclare("spawner", "en-US", kEn,
                 (int)(sizeof(kEn) / sizeof(kEn[0])));
    pLangDeclare("spawner", "zh-CN", kZh,
                 (int)(sizeof(kZh) / sizeof(kZh[0])));
}

/* ---- the pages a vehicle can be filed under -------------------------
 * One page per kind, so a dispatch is a couple of steps instead of a scroll
 * through sixty-five rows. The catalogue carries nothing to file by: an id is
 * a hash and the names were written by eye, one spawn at a time (see
 * scripthook_spawn.c where they are listed), so this files by name - the same
 * thing the framework already does for "which vehicle am I sitting in"
 * (scripthook_api.c, VehicleClassFromName), just finer, and with nothing
 * added to the API for it.
 *
 * The order the word lists are asked in is part of the rule, and each list
 * sitting before another is there for a name that both would claim: "4x4
 * armed" is armour before it is off-road, "Trophy truck" and "Monster truck"
 * are off-road before they are trucks, the armoured ambulance is armour
 * before "ambulance" can have it, and the killdozer is a digger before it is
 * armoured - the one name that has to be asked about before the armour list
 * rather than after it.
 *
 * A name no word claims is filed with the last page and named in this
 * plugin's log rather than dropped: the catalogue grows without asking this
 * file, and a vehicle nobody can find is worse than one filed oddly.
 */
enum {
    CAT_AIR = 0,     /* helicopters and the light planes              */
    CAT_ARMOR,       /* APCs, MRAPs, the armed pickups                */
    CAT_SUV,         /* 4x4s, SUVs, pickups, the off-road trucks      */
    CAT_CAR,         /* sedans, hatchbacks, the sports cars           */
    CAT_TRUCK,       /* buses, vans and the lorries                   */
    CAT_BIKE,        /* motorcycles                                   */
    CAT_BOAT,        /* boats                                         */
    CAT_MISC,        /* the engineering vehicles - and the two that
                      * freeze the game if entered                    */
    CAT_COUNT
};

/* The page titles, in the order the pages are made: the air first, the
 * unusable last. Text keys, so a translation in this plugin's lang.ini
 * renames a page without touching code. */
static const char *const kCatKey[CAT_COUNT] = {
    "@sp.cat.air", "@sp.cat.armor", "@sp.cat.suv", "@sp.cat.car",
    "@sp.cat.truck", "@sp.cat.bike", "@sp.cat.boat", "@sp.cat.misc"
};

/* Case-insensitive substring, the way the framework's own name test works:
 * these names are hand-written and mixed-case ("HELICOPTER", "uh-60",
 * "KILLDOZER"), so nothing here can compare whole strings. */
static int NameHas(const char *name, const char *word) {
    size_t i, j, nl, wl;

    if (!name || !word) return 0;
    nl = strlen(name);
    wl = strlen(word);
    if (!wl || wl > nl) return 0;
    for (i = 0; i + wl <= nl; i++) {
        for (j = 0; j < wl; j++) {
            char a = name[i + j], b = word[j];

            if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
            if (a != b) break;
        }
        if (j == wl) return 1;
    }
    return 0;
}

static int HasWord(const char *name, const char *const *words, int n) {
    int i;

    for (i = 0; i < n; i++)
        if (NameHas(name, words[i])) return 1;
    return 0;
}
#define HAS(name, arr) HasWord((name), (arr), \
                               (int)(sizeof(arr) / sizeof((arr)[0])))

/* Which page a catalogue name belongs on, or -1 when no word claims it. */
static int CatOf(const char *name) {
    static const char *air[]   = { "helicopter", "gunship", "uh-60",
                                   "plane", "airplane", "cossna" };
    static const char *eng[]   = { "tractor", "digger", "killdozer" };
    static const char *armor[] = { "apc", "mrap", "amv", "armed",
                                   "armoured", "technical" };
    static const char *suv[]   = { "4x4", "suv", "buggy", "pickup",
                                   "trophy truck", "monster truck" };
    /* "sumitzu car," keeps its comma: the van below is a "Sumitzu Carry",
     * and "sumitzu car" is a prefix of it. */
    static const char *car[]   = { "sedan", "hatchback", "200gt", "90s",
                                   "paranero", "sumitzu car," };
    static const char *truck[] = { "minibus", "van", "tow truck",
                                   "oil truck", "boxcar", "barracks",
                                   "murder disposal", "advert truck",
                                   "comms truck", "ambulance" };
    static const char *bike[]  = { "bike" };
    static const char *boat[]  = { "boat", "dinghy", "yacht" };
    static const char *misc[]  = { "alpaca", "monster" };

    if (HAS(name, air))   return CAT_AIR;
    if (HAS(name, eng))   return CAT_MISC;
    if (HAS(name, armor)) return CAT_ARMOR;
    if (HAS(name, suv))   return CAT_SUV;
    if (HAS(name, car))   return CAT_CAR;
    if (HAS(name, truck)) return CAT_TRUCK;
    if (HAS(name, bike))  return CAT_BIKE;
    if (HAS(name, boat))  return CAT_BOAT;
    if (HAS(name, misc))  return CAT_MISC;
    return -1;
}

/* The pages, once they exist. One the API refuses stays 0 and its vehicles
 * go on the root page, so nothing becomes unreachable. */
static uint32_t g_cats[CAT_COUNT];

static DWORD WINAPI BindThread(LPVOID p) {
    HMODULE m = NULL;
    MenuCreate_t menuCreate;
    MenuAction_t menuAction;
    MenuSub_t menuSub;
    int n, i, c;
    int counts[CAT_COUNT] = {0};
    (void)p;

    while (!m) {
        m = GetModuleHandleA("dinput8.dll");
        if (!m) Sleep(500);
    }
    *(FARPROC *)&g_count = GetProcAddress(m, "ShVehicleCount");
    *(FARPROC *)&g_at = GetProcAddress(m, "ShVehicleAt");
    *(FARPROC *)&g_spawn = GetProcAddress(m, "ShSpawnVehicle");
    *(FARPROC *)&g_playerPos = GetProcAddress(m,
                                              "ShGetPlayerPosition");
    /* Optional: the facing the vehicle is placed along. Absent means the
     * old east offset, which is a worse landing but not a failure. */
    *(FARPROC *)&g_camera = GetProcAddress(m, "ShGetCamera");
    /* Optional: how far along the framework's spec warm-up is. */
    *(FARPROC *)&g_warmProgress = GetProcAddress(m,
                                                 "ShSpawnWarmProgress");
    *(FARPROC *)&menuCreate = GetProcAddress(m, "ShMenuCreate");
    *(FARPROC *)&menuAction = GetProcAddress(m, "ShMenuAction");
    *(FARPROC *)&menuSub = GetProcAddress(m, "ShMenuSub");
    *(FARPROC *)&g_status = GetProcAddress(m, "ShMenuStatus");
    *(FARPROC *)&g_statusF = GetProcAddress(m, "ShMenuStatusF");
    *(FARPROC *)&g_hint = GetProcAddress(m, "ShMenuHint");
    if (!g_count || !g_at || !g_spawn || !g_playerPos ||
        !menuCreate || !menuAction || !menuSub || !g_status || !g_statusF ||
        !g_hint) {
        /* Each of these is one GetProcAddress: a framework that does not
         * carry it means no submenu, and until now nothing said so. */
        SpLog("bind failed: count=%p at=%p spawn=%p playerPos=%p "
              "menuCreate=%p menuAction=%p menuSub=%p status=%p statusF=%p "
              "hint=%p",
              (void *)g_count, (void *)g_at, (void *)g_spawn,
              (void *)g_playerPos, (void *)menuCreate, (void *)menuAction,
              (void *)menuSub, (void *)g_status, (void *)g_statusF,
              (void *)g_hint);
        return 1;
    }

    /* The catalogue is static, so every vehicle becomes a row once and the
     * API scrolls them. One page per kind instead of one page of sixty-five
     * rows: the pages are made up front, in the order a vehicle is usually
     * wanted (the air first, the unusable last), and the catalogue is walked
     * once, filing every name into one of them.
     */
    TextInit();
    g_menu = menuCreate("@sp.page");
    if (!g_menu) {
        SpLog("ShMenuCreate refused the page - no submenu this session");
        return 1;
    }
    g_hint(g_menu, "@sp.hint");
    for (c = 0; c < CAT_COUNT; c++) {
        g_cats[c] = menuSub(g_menu, kCatKey[c]);
        if (!g_cats[c])
            SpLog("ShMenuSub refused %s - those vehicles stay on the root page",
                  kCatKey[c]);
    }
    n = g_count();
    for (i = 0; i < n; i++) {
        const Vehicle *v = g_at(i);
        uint32_t page;
        int cat;

        if (!v) continue;
        cat = CatOf(v->name);
        if (cat < 0) {
            /* Nothing claimed it: filed with the last page, and named here,
             * so a vehicle the word lists have not caught up with is one log
             * line away from being found. */
            SpLog("no category matched \"%s\" - filed with %s",
                  v->name, kCatKey[CAT_MISC]);
            cat = CAT_MISC;
        }
        counts[cat]++;
        page = g_cats[cat] ? g_cats[cat] : g_menu;
        menuAction(page, v->name, OnSpawn, (void *)v);
    }
    /* What went where, once per session: this is what a report about a
     * vehicle sitting on the wrong page is read against. */
    SpLog("filed %d vehicles: air %d, armor %d, suv %d, car %d, truck %d, "
          "bike %d, boat %d, special %d",
          n, counts[CAT_AIR], counts[CAT_ARMOR], counts[CAT_SUV],
          counts[CAT_CAR], counts[CAT_TRUCK], counts[CAT_BIKE],
          counts[CAT_BOAT], counts[CAT_MISC]);
    /* The last page holds two entries that freeze the game if entered (the
     * framework's own note on those two catalogue rows), so the page says so
     * before the rows do. */
    if (g_cats[CAT_MISC]) g_hint(g_cats[CAT_MISC], "@sp.cat.misc.hint");
    /* The spec cache warm-up is owned by the framework: the state
     * hook calls ShSpawnOnEnterPlaying when the world is playable. */
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        {
            HANDLE h = CreateThread(NULL, 0, BindThread, NULL, 0, NULL);

            if (h) CloseHandle(h);   /* never waited on */
        }
    }
    return TRUE;
}
