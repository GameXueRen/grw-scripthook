/* GhostRevive - experimental: make a Ghost Mode death take the game's own
 * revive path instead of ending the run.
 *
 * ANSWERED, 2026-09-12: it cannot be done from outside the engine. This
 * header keeps the whole record so nobody digs the same hole twice; the
 * long form is in .codebuddy/plans/ghost-revive-findings.md.
 *
 * What the two deaths differ in is the squad, and nothing else. Measured
 * with this plugin's own probe, same crash both times, the only change
 * being the AI squadmates switch:
 *
 *   squad mates aboard   state 7 -> 5 (loading) -> 4, health reset to
 *                        the on foot maximum - a revive, play carries on
 *   no squad             state 7 -> 4 -> 7 -> 4, health pinned at zero,
 *                        then 5 -> 1 - back to the menu, slot gone
 *
 * Before the game over both are identical, so there is nothing to spot
 * early, and the branch is decided inside a flow no export reaches:
 *
 *   reload      ShTriggerGameOver(1) is accepted and ignored - the run
 *               still ends.
 *   fixhp       putting the health back works while in a game (the
 *               engine takes it, the reading goes to 3750/3750) and
 *               changes nothing: the game kills the player again 56 ms
 *               later. Health is not what it decides on. In the game
 *               over state the setter is refused outright, and
 *               ShGetHealthPlayer stops answering at all - which is why
 *               TryFixHp keeps a health maximum instead of reading one.
 *
 * Only two things were left untried, and both change what the player is
 * rather than which path the engine takes: cannotdie (floor at the
 * downed state - may just leave the player stuck there, and it is "not
 * dying", not "a revive") and prehp. Neither is the revive this was
 * after, so the experiment stops here.
 *
 * GhostNoWipe is the part that shipped: it does not touch the death flow
 * at all, it keeps the file. The save comes back on the next launch and
 * play goes on from the last checkpoint, which is everything the wipe
 * took away; the only difference from a revive is that the game has to
 * be restarted. Nothing here writes to a save file, and both plugins can
 * run at the same time.
 *
 * This plugin ships switched off (enabled=0 in its own ini): the menu is
 * still built, so it can be turned on to watch or to re-run an experiment,
 * but nothing is registered and nothing is logged while it is off.
 *
 * The methods stay because they are the record of what was tried, and
 * the probe is worth keeping for any future question about the death
 * flow:
 *
 *   off        do nothing. The control.
 *   probe      record only: engine state, UI flags, health, seat, squad,
 *              GameFlow object. Nothing is changed.
 *   noscene    the death screen appears -> exit that scene.
 *   reload     the death screen appears -> ask for the checkpoint reload.
 *   fixhp      the death screen appears -> put the health back to full.
 *   cannotdie  hold the player at the downed state so death cannot land.
 *   prehp      top the health up before it can reach zero.
 *
 * Everything is driven from a poll thread of this plugin's own. The
 * framework documents a per frame hook, but ships no export for one - the
 * name resolves to nothing - so that was not an option. What it does ship
 * is ShReflectCall, "call a method by name on the game thread and wait",
 * and both calls that change anything here go through it; the rest are
 * plain reads. The watching costs a few getters every 50 ms, and the log
 * only gets a line when a value changes, so a death is a line or two.
 *
 * One trap worth remembering: ShFindEntities cannot see the squad once
 * they are inside a vehicle with the player - at the death both cases
 * read zero. The only reading that tells the two apart is the one taken
 * as the player boards, which is what the "board" line records.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* From scripthook.h, kept as numbers because a plugin binds the
 * framework by name rather than by link. */
#define SH_STATE_MENU     1
#define SH_STATE_INGAME   4
#define SH_STATE_GAMEOVER 7

#define SH_UI_VEHICLE    0x0004u
#define SH_UI_GAMEOVER   0x0100u
#define SH_UI_LOADING    0x0200u
#define SH_UI_POPUP      0x0800u

#define SH_FLOW_GAMEOVER_SCENE 9

/* ---- modes ------------------------------------------------------------- */

enum {
    MODE_OFF = 0,
    MODE_PROBE,
    MODE_NOSCENE,
    MODE_RELOAD,
    MODE_FIXHP,
    MODE_CANNOTDIE,
    MODE_PREHP,
    MODE_COUNT
};

static const char *MODE_NAMES[MODE_COUNT] = {
    "off", "probe", "noscene", "reload", "fixhp", "cannotdie", "prehp"
};

static const char *MODE_HINTS[MODE_COUNT] = {
    "Nothing is changed: the control. The watch still records, so a "
    "death with the plugin idle can be compared against one with a "
    "method armed.",
    "Records the state, the UI flags, the health, the seat and the "
    "GameFlow object, and changes nothing.",
    "When the death screen appears, exits that scene so it stops being "
    "shown.",
    "When the death screen appears, asks for the game over path that "
    "reloads the checkpoint - the revive this plugin is after.",
    "When the death screen appears, puts the health back to full. A death "
    "that ends the run comes back with the health still on zero and dies "
    "again and again; a death that revives comes back with it refilled. "
    "This restores it and sees whether the game then takes the second "
    "path.",
    "Holds the player at the downed state so death cannot land at all.",
    "Tops the health up before it can reach zero."
};

/* ---- the framework, bound by name -------------------------------------- */

typedef uint32_t (*GetUiState_t)(void);
typedef int      (*GetGameState_t)(void);
typedef int      (*GetHealth_t)(uint32_t *cur, uint32_t *max);
typedef int      (*SetHealth_t)(uint32_t value);
typedef int      (*SetCannotDie_t)(int on);
typedef int      (*IsInVehicle_t)(void);
typedef int      (*IsInGame_t)(void);
typedef uint64_t (*GameFlow_t)(void);
typedef uint64_t (*GameFlowObject_t)(int slot);
typedef int      (*SceneExit_t)(uint64_t obj);
typedef int      (*TriggerGameOver_t)(int reason);
typedef uint32_t (*ToastEx_t)(const char *text, uint32_t rgb, uint32_t ms);

/* From scripthook.h: entity lookup by kind. SH_KIND_TEAMMATE is the one
 * that matters here - it is the only way to tell, from outside, whether
 * the game will treat a death as a revive or as the end of the run. */
typedef struct { float x, y, z; } ShVec3;

typedef struct {
    uint64_t entity;
    ShVec3   pos;
    float    distance;
    int      kind;
    uint32_t maxHealth;
    char     name[32];
} ShEntity;

#define SH_KIND_TEAMMATE 5
#define SH_FIND_UNNAMED  0x1u

typedef int (*FindEntities_t)(int kind, float radius, uint32_t flags,
                              ShEntity *out, int max);

static HINSTANCE g_inst;
static char      g_dirA[MAX_PATH];     /* the game's working directory */

static ToastEx_t         g_toastEx;
static FindEntities_t    g_findEntities;
static GetUiState_t      g_getUi;
static GetGameState_t    g_getState;
static GetHealth_t       g_getHealth;
static SetHealth_t       g_setHealth;
static SetCannotDie_t    g_setCannotDie;
static IsInVehicle_t     g_isInVehicle;
static IsInGame_t        g_isInGame;
static GameFlowObject_t  g_flowObject;
static SceneExit_t       g_sceneExit;
static TriggerGameOver_t g_triggerGameOver;

static volatile LONG g_enabled = 1;
static volatile LONG g_mode = MODE_OFF;

/* ---- logging ----------------------------------------------------------- */

static FILE *g_log;
static LONG  g_logBusy;

static void GuardLog(const char *fmt, ...) {
    va_list ap;
    char line[1024];
    SYSTEMTIME st;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (!g_log) return;

    while (InterlockedExchange(&g_logBusy, 1)) Sleep(1);
    if (g_log) {
        GetLocalTime(&st);
        fprintf(g_log, "%02u:%02u:%02u.%03u [%lu] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                (unsigned long)GetCurrentProcessId(), line);
        fflush(g_log);
    }
    InterlockedExchange(&g_logBusy, 0);
}

static FILE *OpenLogAt(const char *dir, const char *name) {
    char path[MAX_PATH];
    int len = (int)strlen(dir);

    if (len + 32 >= (int)sizeof(path)) return NULL;
    lstrcpynA(path, dir, sizeof(path));
    strcpy(path + len, "logs");
    CreateDirectoryA(path, NULL);
    strcat(path, "\\");
    strcat(path, name);
    return fopen(path, "a");
}

/* ---- plugin ini -------------------------------------------------------- */

static char g_iniPath[MAX_PATH];

static void ResolveIniPath(void) {
    char mod[MAX_PATH];
    const char *dot;
    size_t n;

    g_iniPath[0] = 0;
    if (!g_inst || !GetModuleFileNameA(g_inst, mod, sizeof(mod))) return;
    dot = strrchr(mod, '.');
    n = dot ? (size_t)(dot - mod) : strlen(mod);
    if (n >= sizeof(g_iniPath)) n = sizeof(g_iniPath) - 1;
    memcpy(g_iniPath, mod, n);
    g_iniPath[n] = 0;
    strncat(g_iniPath, ".ini", sizeof(g_iniPath) - n - 1);
}

static int Enabled(void) {
    return InterlockedCompareExchange(&g_enabled, 0, 0) ? 1 : 0;
}

static int Mode(void) {
    return (int)InterlockedCompareExchange(&g_mode, 0, 0);
}

static void LoadConfig(void) {
    int on = 1;
    int mode = MODE_OFF;

    if (g_iniPath[0]) {
        on = GetPrivateProfileIntA("Settings", "enabled", 1, g_iniPath);
        mode = GetPrivateProfileIntA("Settings", "mode", MODE_OFF, g_iniPath);
    }
    if (mode < MODE_OFF || mode >= MODE_COUNT) mode = MODE_OFF;
    InterlockedExchange(&g_enabled, on ? 1 : 0);
    InterlockedExchange(&g_mode, mode);
}

static void SaveIni(void) {
    char buf[16];

    if (!g_iniPath[0]) return;
    snprintf(buf, sizeof(buf), "%d", Enabled());
    WritePrivateProfileStringA("Settings", "enabled", buf, g_iniPath);
    snprintf(buf, sizeof(buf), "%d", Mode());
    WritePrivateProfileStringA("Settings", "mode", buf, g_iniPath);
}

/* ---- menu -------------------------------------------------------------- */

typedef void (*MenuFn)(uint32_t menu, uint32_t item, int value, void *user);

static void OnEnable(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    InterlockedExchange(&g_enabled, value ? 1 : 0);
    GuardLog("menu: revive=%s", Enabled() ? "on" : "off");
    SaveIni();
}

/* The mode cycles, because the point of this build is to compare them
 * against one another without a recompile between each. */
static void OnCycleMode(uint32_t menu, uint32_t item, int value, void *user) {
    int next = (Mode() + 1) % MODE_COUNT;

    (void)menu; (void)item; (void)value; (void)user;
    InterlockedExchange(&g_mode, next);
    GuardLog("menu: mode=%s (%d) - %s", MODE_NAMES[next], next,
             MODE_HINTS[next]);
    SaveIni();

    /* The menu label cannot be rewritten from here, so the mode the
     * switch landed on goes up on screen instead - otherwise there is no
     * way to tell one from the next. */
    if (g_toastEx) {
        char line[96];
        snprintf(line, sizeof(line), "Ghost revive: %s", MODE_NAMES[next]);
        g_toastEx(line, 0xFFCC33u, 2500u);
    }
}

static void BuildMenu(HMODULE m) {
    uint32_t (*menuCreate)(const char *) = NULL;
    int (*menuToggle)(uint32_t, const char *, int, MenuFn, void *) = NULL;
    int (*menuAction)(uint32_t, const char *, MenuFn, void *) = NULL;
    int (*menuHint)(uint32_t, const char *) = NULL;

    *(FARPROC *)&menuCreate = GetProcAddress(m, "ShMenuCreate");
    *(FARPROC *)&menuToggle = GetProcAddress(m, "ShMenuToggle");
    *(FARPROC *)&menuAction = GetProcAddress(m, "ShMenuAction");
    *(FARPROC *)&menuHint   = GetProcAddress(m, "ShMenuHint");
    if (!menuCreate || !menuToggle) return;

    {
        uint32_t menu = menuCreate("Ghost revive");

        menuToggle(menu, "Watch and try to revive", Enabled(), OnEnable, NULL);
        if (menuAction)
            menuAction(menu, "Switch the method", OnCycleMode, NULL);
        if (menuHint)
            menuHint(menu,
                     "Experimental. A Ghost Mode death that ends the run "
                     "goes through a different path from one that does not; "
                     "this tries to send every death down the reviving one. "
                     "The method cycles: off, probe, noscene, reload, "
                     "fixhp, cannotdie, prehp.");
    }
    GuardLog("menu created");
}

/* ---- what the engine is doing, watched once a frame -------------------- */

/* One reading of everything that could tell the two deaths apart. Held
 * as plain integers so a frame's comparison costs nothing. */
/* How far out the squad is looked for. Generous on purpose: the game's
 * own check is what decides the branch, and a teammate lagging behind
 * still counts as being there as far as the player is concerned. */
#define SQUAD_RADIUS 120.0f
#define SQUAD_MAX    8

typedef struct {
    int      state;
    uint32_t ui;
    uint32_t hp;
    uint32_t hpMax;
    int      hpOk;
    int      inVehicle;
    int      inGame;
    int      team;        /* teammates within SQUAD_RADIUS, -1 unknown */
    uint64_t flow;
} Reading;

static Reading g_prev;

/* The flow object is the expensive one to fetch, so it is sampled on a
 * slower clock than the rest; a checkpoint reload rebuilds it, and that
 * rebuild is not something a tenth of a second would hide. */
#define FLOW_SAMPLE_MS 500u

static ULONGLONG g_lastFlowSample;

static void TakeReading(Reading *r, int withFlow) {
    ULONGLONG now = GetTickCount64();

    memset(r, 0, sizeof(*r));
    if (g_getState) r->state = g_getState();
    if (g_getUi) r->ui = g_getUi();
    if (g_getHealth) r->hpOk = g_getHealth(&r->hp, &r->hpMax);
    if (g_isInVehicle) r->inVehicle = g_isInVehicle();
    if (g_isInGame) r->inGame = g_isInGame();

    /* The number that decides everything: the game revives when the squad
     * is there and ends the run when it is not. */
    r->team = -1;
    if (g_findEntities) {
        ShEntity found[SQUAD_MAX];
        r->team = g_findEntities(SH_KIND_TEAMMATE, SQUAD_RADIUS, 0,
                                 found, SQUAD_MAX);
    }

    if (withFlow && g_flowObject && now - g_lastFlowSample >= FLOW_SAMPLE_MS) {
        g_lastFlowSample = now;
        r->flow = g_flowObject(SH_FLOW_GAMEOVER_SCENE);
    } else {
        r->flow = g_prev.flow;
    }
}

/* Only the differences reach the log: a death is a handful of lines, and
 * a quiet frame is none at all. */
static void ReportReading(const Reading *now, const Reading *was) {
    if (now->state != was->state)
        GuardLog("state   %d -> %d", was->state, now->state);
    if (now->ui != was->ui)
        GuardLog("ui      0x%04lX -> 0x%04lX%s%s",
                 (unsigned long)was->ui, (unsigned long)now->ui,
                 (now->ui & SH_UI_GAMEOVER) ? "  [GAMEOVER]" : "",
                 (now->ui & SH_UI_VEHICLE) ? "  [VEHICLE]" : "");
    if (now->inGame != was->inGame)
        GuardLog("ingame  %d -> %d", was->inGame, now->inGame);
    if (now->inVehicle != was->inVehicle)
        GuardLog("seat    %s -> %s", was->inVehicle ? "vehicle" : "on foot",
                 now->inVehicle ? "vehicle" : "on foot");
    if (now->hpOk && (!was->hpOk || now->hp != was->hp || now->hpMax != was->hpMax))
        GuardLog("health  %lu/%lu", (unsigned long)now->hp,
                 (unsigned long)now->hpMax);
    if (now->team != was->team)
        GuardLog("squad   %d -> %d teammate(s) within %.0f m",
                 was->team, now->team, (double)SQUAD_RADIUS);
    if (now->flow != was->flow)
        GuardLog("flow    %p -> %p", (void *)(uintptr_t)was->flow,
                 (void *)(uintptr_t)now->flow);
}

/* ---- the strategies ---------------------------------------------------- */

/* One action per death: the death screen stays up for a while, and a
 * strategy that fires every frame while it is up is a strategy that
 * fights the engine. The latch is cleared once the player is in a game
 * again with health. */
static int g_acted;

/* Held once and left held: the engine keeps the flag on the player's own
 * component, so there is no reason to rewrite it every frame. Let go
 * when the mode is moved away from this one - pinning a player at the
 * downed state is not something to leave behind by accident. */
static int g_cannotDieOn;

/* Read the moment the player boards, because that is the only moment it
 * means anything. Once under way the lookup stops seeing the squad - the
 * ones who came along are inside with the player - so the same number
 * reads zero later either way: measured on 2026-09-11, the boarding that
 * ended in a revive read 3, and the one that ended the run read 0, yet
 * by the time the death card was up both read 0.
 *
 * Recorded, not acted on. It is kept in the log so the next death can be
 * told apart after the fact, which is what the probe is for. */
static int g_teamOnEntry = -1;

static void TryNoScene(const Reading *now) {
    uint64_t scene;

    if (!(now->ui & SH_UI_GAMEOVER) || !g_flowObject || !g_sceneExit) return;
    scene = g_flowObject(SH_FLOW_GAMEOVER_SCENE);
    if (!scene) return;

    GuardLog("noscene: exiting the death scene %p",
             (void *)(uintptr_t)scene);
    if (g_sceneExit(scene))
        GuardLog("noscene: exit accepted");
    else
        GuardLog("noscene: exit refused");
}

static void TryReload(const Reading *now) {
    if (!(now->ui & SH_UI_GAMEOVER) || !g_triggerGameOver) return;

    GuardLog("reload: asking for the game over path that reloads a "
             "checkpoint (reason 1)");
    if (g_triggerGameOver(1))
        GuardLog("reload: accepted");
    else
        GuardLog("reload: refused");
}

static void TryCannotDie(const Reading *now, int active) {
    if (!g_setCannotDie) return;

    if (!active) {
        if (g_cannotDieOn) {
            g_setCannotDie(0);
            g_cannotDieOn = 0;
            GuardLog("cannotdie: released");
        }
        return;
    }
    if (g_cannotDieOn || !now->inGame) return;

    g_cannotDieOn = 1;
    GuardLog("cannotdie: holding the player at the downed state");
    g_setCannotDie(1);
}

/* Put the health back whenever the death screen is up with it on zero.
 *
 * The two deaths differ in exactly this. The one that revives comes back
 * with the health refilled, to the on foot maximum; the one that ends
 * the run comes back still on zero and is killed again on the spot - the
 * log shows the state bouncing between game over and in game about once
 * a second, health never leaving zero, until it gives up and goes to the
 * menu. If that refill is what the game waits for before it reloads,
 * doing it here turns the second death into the first. A death that
 * already revives on its own is untouched: its health is full by the
 * time this would run.
 *
 * Not latched like the scene calls, because the engine writes its own
 * value back over this one and it can take a few goes; rate limited so
 * the poll does not become a spin. */
#define FIXHP_MS 200u

static ULONGLONG g_lastFixHp;

/* Set when a death card is seen, cleared once the player is back on their
 * feet with health. The card is not enough on its own: measured, the run
 * ending death drops back to in game five seconds before the reload with
 * the GAMEOVER bit already off and the health still on zero - the very
 * stretch worth repairing. Watching for the card and then following the
 * zero for as long as it lasts is what covers it.
 *
 * It also keeps this off an ordinary knock down: going down on foot puts
 * the health on zero well before anything decides, and topping it up
 * then would be refusing the death rather than letting it play out. Only
 * a death the game has already put a card up for is followed. */
static int g_deathSeen;

/* The last health maximum that could be read while playing. The engine
 * stops answering once the death card is up, so this is what a repair
 * goes on - see TryFixHp. */
static uint32_t g_hpMaxLast;

/* One line the first time the method is reached in a game, with every
 * value it will decide on - the quickest way to tell a method that is not
 * running from one that is running and cannot act. */
static int g_fixHpArmed;

static void TryFixHp(const Reading *now) {
    uint32_t max;
    ULONGLONG nowMs;

    if (!g_setHealth) return;

    /* The last reading that worked, kept because the engine stops
     * answering on the frame the death card goes up: measured, it reads
     * 3750/3750 while playing and fails the instant the state changes to
     * game over. Without a kept value there would be nothing to put
     * back, which is why an earlier attempt at this looked like the
     * method did nothing at all. */
    if (now->hpOk && now->hpMax) g_hpMaxLast = now->hpMax;

    if ((now->ui & SH_UI_GAMEOVER) || now->state == SH_STATE_GAMEOVER) {
        if (!g_deathSeen)
            GuardLog("fixhp: the death card is up - hpOk=%d hp=%lu/%lu "
                     "kept max=%lu", now->hpOk, (unsigned long)now->hp,
                     (unsigned long)now->hpMax,
                     (unsigned long)g_hpMaxLast);
        g_deathSeen = 1;
    } else if (now->inGame && now->hpOk && now->hp > 0) {
        g_deathSeen = 0;
        return;
    }

    if (!g_deathSeen) return;

    /* Goes on the kept value, not on a fresh read: by now there is none.
     * A player at full health a moment before the card is put back to
     * exactly that. */
    max = g_hpMaxLast;
    if (!max) return;
    if (now->hpOk && now->hp >= max) return;

    nowMs = GetTickCount64();
    if (nowMs - g_lastFixHp < FIXHP_MS) return;
    g_lastFixHp = nowMs;

    GuardLog("fixhp: putting the health back to %lu (hpOk=%d hp=%lu)",
             (unsigned long)max, now->hpOk, (unsigned long)now->hp);
    if (g_setHealth(max))
        GuardLog("fixhp: accepted");
    else
        GuardLog("fixhp: refused (err=%lu)", GetLastError());
}

static void TryPreHp(const Reading *now) {
    if (!g_setHealth || !now->hpOk || !now->hpMax) return;
    if (!now->inGame) return;
    if (now->hp == 0) return;               /* too late, do not fight it */
    if (now->hp > now->hpMax / 8) return;   /* not close enough yet */

    GuardLog("prehp: %lu/%lu is low - topping the health up",
             (unsigned long)now->hp, (unsigned long)now->hpMax);
    g_setHealth(now->hpMax);
}

static void Act(const Reading *now) {
    int mode = Mode();
    int deathUp = (now->ui & SH_UI_GAMEOVER) != 0;

    /* The one reading worth keeping: how many were there at the moment
     * of boarding. See g_teamOnEntry. */
    if (now->inVehicle && !g_prev.inVehicle && now->team >= 0) {
        g_teamOnEntry = now->team;
        GuardLog("board   %d teammate(s) at the wheel - the run %s",
                 g_teamOnEntry,
                 g_teamOnEntry > 0 ? "should still revive"
                                   : "is the one that ends");
    }

    /* A player back in the world with health is a player whose death is
     * over: the latch opens again for the next one. */
    if (now->inGame && now->state == SH_STATE_INGAME && now->hpOk &&
        now->hp > 0)
        g_acted = 0;

    TryCannotDie(now, mode == MODE_CANNOTDIE);

    if (mode == MODE_PREHP) {
        TryPreHp(now);
    } else if (mode == MODE_FIXHP) {
        /* Reported once the player is actually in the world, not at
         * startup: before that there is no player to read a health from
         * and the reading is empty, which says nothing. */
        if (!g_fixHpArmed && now->inGame) {
            g_fixHpArmed = 1;
            GuardLog("fixhp: method reached in game - sethealth=%p hpOk=%d "
                     "hp=%lu/%lu ui=0x%04lX state=%d",
                     (void *)g_setHealth, now->hpOk, (unsigned long)now->hp,
                     (unsigned long)now->hpMax, (unsigned long)now->ui,
                     now->state);
        }
        TryFixHp(now);
    } else if (deathUp && !g_acted) {
        if (mode == MODE_NOSCENE) {
            TryNoScene(now);
            g_acted = 1;
        } else if (mode == MODE_RELOAD) {
            TryReload(now);
            g_acted = 1;
        }
    }
}

/* The watching runs on a thread of its own. The framework declares a per
 * frame hook, but ships no export for one - the name resolves to nothing
 * - so this is driven the way GhostNoWipe drives its state sampling.
 *
 * That is safe here because everything it calls is either a plain read,
 * or a reflected call: ShSceneExit and ShTriggerGameOver both go through
 * ShReflectCall, which the framework documents as "call a method by name
 * on the game thread and wait". The queueing onto the game thread is
 * done for us, which is what makes them callable from anywhere. */
#define POLL_MS 50

static DWORD WINAPI PollThread(LPVOID p) {
    (void)p;
    GuardLog("poll: thread up (every %u ms)", (unsigned)POLL_MS);
    for (;;) {
        if (Enabled()) {
            Reading now;

            TakeReading(&now, 1);
            ReportReading(&now, &g_prev);
            Act(&now);
            g_prev = now;
        }
        Sleep(POLL_MS);
    }
    return 0;
}

/* ---- startup ----------------------------------------------------------- */

static void BindApi(HMODULE di) {
    *(FARPROC *)&g_toastEx = GetProcAddress(di, "ShToastEx");
    *(FARPROC *)&g_findEntities = GetProcAddress(di, "ShFindEntities");
    *(FARPROC *)&g_getUi = GetProcAddress(di, "ShGetUiState");
    *(FARPROC *)&g_getState = GetProcAddress(di, "ShGetGameState");
    *(FARPROC *)&g_getHealth = GetProcAddress(di, "ShGetHealthPlayer");
    *(FARPROC *)&g_setHealth = GetProcAddress(di, "ShSetHealthPlayer");
    *(FARPROC *)&g_setCannotDie = GetProcAddress(di, "ShSetCannotDiePlayer");
    *(FARPROC *)&g_isInVehicle = GetProcAddress(di, "ShIsInVehicle");
    *(FARPROC *)&g_isInGame = GetProcAddress(di, "ShIsInGame");
    *(FARPROC *)&g_flowObject = GetProcAddress(di, "ShGameFlowObject");
    *(FARPROC *)&g_sceneExit = GetProcAddress(di, "ShSceneExit");
    *(FARPROC *)&g_triggerGameOver = GetProcAddress(di, "ShTriggerGameOver");
}

static DWORD WINAPI InitThread(LPVOID p) {
    char mod[MAX_PATH];
    char *slash;

    (void)p;
    g_dirA[0] = 0;
    if (GetModuleFileNameA(NULL, mod, sizeof(mod))) {
        slash = strrchr(mod, '\\');
        if (slash) {
            slash[1] = 0;
            lstrcpynA(g_dirA, mod, sizeof(g_dirA));
        }
    }
    if (g_dirA[0]) g_log = OpenLogAt(g_dirA, "GhostRevive.log");

    ResolveIniPath();

    ResolveIniPath();
    LoadConfig();

    GuardLog("--- GhostRevive: trying to make a death revive ---");
    GuardLog("build " __DATE__ " " __TIME__);
    GuardLog("config: revive=%s mode=%s ini=%s", Enabled() ? "on" : "off",
             MODE_NAMES[Mode()], g_iniPath[0] ? g_iniPath : "(none)");

    {
        HMODULE di = GetModuleHandleA("dinput8.dll");
        if (di) {
            BindApi(di);
            BuildMenu(di);
        } else {
            GuardLog("install: dinput8.dll is not loaded - the framework "
                     "cannot be reached");
        }
    }

    GuardLog("api: ui=%p state=%p health=%p sethealth=%p cannotdie=%p "
             "vehicle=%p ingame=%p flowobj=%p sceneexit=%p gameover=%p "
             "toast=%p find=%p",
             (void *)g_getUi, (void *)g_getState, (void *)g_getHealth,
             (void *)g_setHealth, (void *)g_setCannotDie,
             (void *)g_isInVehicle, (void *)g_isInGame,
             (void *)g_flowObject, (void *)g_sceneExit,
             (void *)g_triggerGameOver, (void *)g_toastEx,
             (void *)g_findEntities);

    if (!g_getUi || !g_getState)
        GuardLog("install: the state getters are missing - without them "
                 "there is nothing to watch");

    /* The watching runs whenever the plugin is loaded rather than only
     * when a strategy is armed: the probe is the point of this build,
     * and it has to be recording before the death, not after. */
    if (Enabled()) {
        CreateThread(NULL, 0, PollThread, NULL, 0, NULL);
        GuardLog("install: watching started, mode=%s", MODE_NAMES[Mode()]);
    } else {
        GuardLog("revive is off: nothing is watched, the game behaves as "
                 "if this plugin were not installed");
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = inst;
        DisableThreadLibraryCalls(inst);
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
    }
    return TRUE;
}
