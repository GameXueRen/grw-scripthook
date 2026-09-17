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

/* This plugin had no log at all, which made the one failure it can have -
 * an export this dinput8 does not carry - look like a submenu that simply
 * never appeared. One file, opened on first use, with the same directory
 * rule the other plugins use. */
static FILE *g_log;

/* This plugin's log is its own diagnostics, and the level's job here is
 * only to be able to turn all of it off: the file is written at every
 * level except none, unlike the framework's module logs, which need info.
 * The line that matters most in a plugin's log is usually the one about
 * something not working - exactly the line a quiet session would drop.
 * Bound on first use and optional - a framework that does not carry
 * ShLogLevel leaves the log ungated, which is what this did before. */
static int LogWanted(void) {
    typedef int (*LevelFn)(void);
    static LevelFn fn;
    static int tried;

    if (!tried) {
        HMODULE di;

        tried = 1;
        di = GetModuleHandleA("dinput8.dll");
        if (di) *(FARPROC *)&fn = GetProcAddress(di, "ShLogLevel");
    }
    return !fn || fn() > SH_LOG_NONE;
}

static void SpLog(const char *fmt, ...) {
    char path[MAX_PATH];
    char *s;
    va_list ap;
    SYSTEMTIME st;

    if (!g_log) {
        if (!LogWanted()) return;
        if (!GetModuleFileNameA(NULL, path, MAX_PATH)) return;
        s = strrchr(path, '\\');
        if (!s) return;
        s[1] = 0;
        if (strlen(path) + 5 < sizeof(path)) strcat(path, "logs");
        CreateDirectoryA(path, NULL);
        if (strlen(path) + 13 < sizeof(path)) strcat(path, "\\spawner.log");
        else return;
        g_log = fopen(path, "w");
    }
    if (!g_log) return;
    GetLocalTime(&st);
    va_start(ap, fmt);
    fprintf(g_log, "[%02u:%02u:%02u.%03u] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
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
    { "@sp.noplayer", "no player position" },
    { "@sp.spawning", "spawning..." },
    { "@sp.spawned",  "spawned, %d this session" },
    { "@sp.warming",  "preparing, %d of %d specs" },
    { "@sp.nothing",  "nothing appeared" }
};

static const TextRow kZh[] = {
    { "@sp.page",     "载具派遣" },
    { "@sp.hint",     "首次载具派遣可能需要一点时间。" },
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

static DWORD WINAPI BindThread(LPVOID p) {
    HMODULE m = NULL;
    MenuCreate_t menuCreate;
    MenuAction_t menuAction;
    int n, i;
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
    *(FARPROC *)&g_status = GetProcAddress(m, "ShMenuStatus");
    *(FARPROC *)&g_statusF = GetProcAddress(m, "ShMenuStatusF");
    *(FARPROC *)&g_hint = GetProcAddress(m, "ShMenuHint");
    if (!g_count || !g_at || !g_spawn || !g_playerPos ||
        !menuCreate || !menuAction || !g_status || !g_statusF || !g_hint) {
        /* Each of these is one GetProcAddress: a framework that does not
         * carry it means no submenu, and until now nothing said so. */
        SpLog("bind failed: count=%p at=%p spawn=%p playerPos=%p "
              "menuCreate=%p menuAction=%p status=%p statusF=%p hint=%p",
              (void *)g_count, (void *)g_at, (void *)g_spawn,
              (void *)g_playerPos, (void *)menuCreate, (void *)menuAction,
              (void *)g_status, (void *)g_statusF, (void *)g_hint);
        return 1;
    }

    /* The catalogue is static, so every vehicle becomes a
     * row once and the API scrolls them.
     */
    TextInit();
    g_menu = menuCreate("@sp.page");
    if (!g_menu) {
        SpLog("ShMenuCreate refused the page - no submenu this session");
        return 1;
    }
    g_hint(g_menu, "@sp.hint");
    n = g_count();
    for (i = 0; i < n; i++) {
        const Vehicle *v = g_at(i);
        if (v) menuAction(g_menu, v->name, OnSpawn, (void *)v);
    }
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
