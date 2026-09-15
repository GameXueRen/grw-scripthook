/* Field of view, changed from the menu and held every
 * frame by the ScriptHook's camera override.
 */
/* Linked against the ScriptHook, so the API is called
 * directly. See src/README.md for how that works.
 */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#include "scripthook.h"

#define DEG2RAD     0.01745329252f
#define RAD2DEG     57.2957795f

/* The engine's own vertical fov is about 0.815 rad, which
 * is the fallback if we never manage to read it.
 */
#define FALLBACK    0.815f
#define DEG_MIN     30.0f
#define DEG_MAX     140.0f
#define DEG_STEP    2.0f
#define TICK_MS     500

static uint32_t g_menu = 0;
static volatile LONG  g_on = 0;      /* read by the tick, written by the menu */
/* Set on unload. Only a flag: the release belongs to the tick thread, because
 * DllMain runs under the loader lock, where a framework call is forbidden. */
static volatile LONG  g_stop = 0;
static volatile float g_deg = 0.0f;
static volatile float g_defaultRad = 0.0f;

static int OverrideOn(void) {
    return InterlockedCompareExchange(&g_on, 0, 0) ? 1 : 0;
}

/* Captured while the override is off, so it is the game's value rather than
 * one of ours read back. Both the tick and a menu switch can call this, and
 * the test-and-set was two separate steps: the flag makes exactly one of
 * them the learner. The two writes below are aligned 32-bit, so a reader
 * sees one value or the other - never a mix of two.
 */
static volatile LONG g_learned;

static void LearnDefault(void) {
    ShCamera c;

    if (OverrideOn() || InterlockedCompareExchange(&g_learned, 0, 0)) return;
    if (!ShIsInGame()) return;
    if (!ShGetCamera(&c)) return;
    if (c.fov > 0.05f && c.fov < 3.0f) {
        g_defaultRad = c.fov;
        if (g_deg <= 0.0f) g_deg = c.fov * RAD2DEG;
        InterlockedExchange(&g_learned, 1);
    }
}

static float DefaultRad(void) {
    return (g_defaultRad > 0.0f) ? g_defaultRad : FALLBACK;
}

/* One call is enough: the ScriptHook reapplies it inside
 * the engine's own frame until it is released.
 */
static int Push(void) {
    ShCameraOverride o;

    memset(&o, 0, sizeof(o));
    o.apply = SH_CAM_FOV;
    o.fov = g_deg * DEG2RAD;
    return ShCameraApply(&o);
}

/* ---- text ---------------------------------------------------------
 * The menu's own text, compiled in: with no lang.ini anywhere the menu
 * still reads in either language, and a lang.ini only has to carry what
 * it changes. The keys are stable IDs, so rewording a row never breaks
 * a translation - the reason they are not the English literals.
 */
static const ShText kEn[] = {
    { "@fov.page",       "Field of view" },
    { "@fov.override",   "Override" },
    { "@fov.vertical",   "Vertical fov" },
    { "@fov.reset",      "Back to the game default" },
    { "@fov.status.on",  "%.0f deg, game default %.0f" },
    { "@fov.status.off", "off, game is %.0f deg" },
    { "@fov.notready",   "the camera is not ready" }
};

static const ShText kZh[] = {
    { "@fov.page",       "延展视野范围" },
    { "@fov.override",   "覆盖游戏视野范围设置" },
    { "@fov.vertical",   "调整垂直视野范围（度）" },
    { "@fov.reset",      "恢复游戏默认视野范围" },
    { "@fov.status.on",  "当前视野范围 %.0f 度，游戏默认 %.0f" },
    { "@fov.status.off", "已关闭，当前视野范围 %.0f 度" },
    { "@fov.notready",   "游戏视野尚未就绪" }
};

static void FovText(void) {
    static int done;

    if (done) return;
    done = 1;
    ShLangDeclare("fov_changer", "en-US", kEn,
                  (int)(sizeof(kEn) / sizeof(kEn[0])));
    ShLangDeclare("fov_changer", "zh-CN", kZh,
                  (int)(sizeof(kZh) / sizeof(kZh[0])));
}

static void Report(void) {
    if (g_on)
        ShMenuStatusF(g_menu, "@fov.status.on",
                      g_deg, DefaultRad() * RAD2DEG);
    else
        ShMenuStatusF(g_menu, "@fov.status.off",
                      DefaultRad() * RAD2DEG);
}

static void OnToggle(uint32_t menu, uint32_t item, int value,
                     void *user) {
    (void)menu; (void)item; (void)user;

    if (value) {
        LearnDefault();
        if (g_deg <= 0.0f) g_deg = DefaultRad() * RAD2DEG;
        InterlockedExchange(&g_on, 1);
        if (!Push()) {
            InterlockedExchange(&g_on, 0);
            /* The framework's row already flipped to "on"; put it back, or
             * the menu claims an override that is not in force. */
            ShMenuSetValue(g_menu, "@fov.override", 0);
            ShMenuStatus(g_menu, "@fov.notready");
            return;
        }
    } else {
        InterlockedExchange(&g_on, 0);
        ShCameraReleaseFields(SH_CAM_FOV);
    }
    Report();
}

static void OnFov(uint32_t menu, uint32_t item, int value,
                  void *user) {
    (void)menu; (void)item; (void)user;
    g_deg = (float)value;
    if (OverrideOn()) Push();
    Report();
}

static void OnReset(uint32_t menu, uint32_t item, int value,
                    void *user) {
    (void)menu; (void)item; (void)value; (void)user;
    g_deg = DefaultRad() * RAD2DEG;
    /* The number row shows the framework's copy, so it has to be told. */
    ShMenuSetValue(g_menu, "@fov.vertical", (int)(g_deg + 0.5f));
    if (OverrideOn()) Push();
    Report();
}

/* A blocked (PvP) mode: the framework takes this page out of the menu, so
 * the override has to let go here - a fov left pushed in Ghost War is
 * exactly what the blacklist exists to prevent. */
static void OnBlocked(int allowed, int blocked, void *user) {
    (void)blocked; (void)user;

    if (!allowed) ShCameraReleaseFields(SH_CAM_FOV);
    else if (OverrideOn()) Push();
}

/* Entering a session reinstalls the camera hook, so the
 * override is pushed again to survive the transition.
 */
static DWORD WINAPI TickThread(LPVOID p) {
    int held = 0;      /* the override is in force right now */
    (void)p;

    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        int in = ShIsInGame();
        int allowed = ShPluginAllowed();

        LearnDefault();

        if (!allowed) {
            /* Blocked: the menu cannot switch it off, so it is let go
             * here and pushed again once the mode allows it. */
            if (held) { ShCameraReleaseFields(SH_CAM_FOV); held = 0; }
        } else if (OverrideOn() && in && !held) {
            if (Push()) held = 1;
        } else if (held && (!OverrideOn() || !in)) {
            held = 0;      /* off, or off the camera: the hold is over */
        }
        Sleep(TICK_MS);
    }

    /* Unloading: an override left pushed would be one with no owner left to
     * release it. */
    if (held) ShCameraReleaseFields(SH_CAM_FOV);
    return 0;
}

/* All initialization is here, not in DllMain: that runs under the loader
 * lock, where taking the framework's own locks and allocating is what the
 * plugin contract forbids. */
static DWORD WINAPI InitThread(LPVOID p) {
    (void)p;
    g_deg = FALLBACK * RAD2DEG;
    FovText();
    g_menu = ShMenuCreate("@fov.page");
    if (!g_menu) return 0;          /* nothing to drive without a page */
    ShMenuToggle(g_menu, "@fov.override", 0, OnToggle, NULL);
    ShMenuNumber(g_menu, "@fov.vertical", g_deg, DEG_MIN, DEG_MAX,
                 DEG_STEP, OnFov, NULL);
    ShMenuAction(g_menu, "@fov.reset", OnReset, NULL);
    Report();
    /* A view override is not something a PvP match wants; saying it out
     * loud is what makes the release in the tick deliberate rather than an
     * accident of the default. */
    ShPluginBlacklist(SH_MODE_BLACKLIST_GHOST_WAR |
                      SH_MODE_BLACKLIST_MERCENARIES);
    ShPluginOnBlocked(OnBlocked, NULL);
    {
        HANDLE h = CreateThread(NULL, 0, TickThread, NULL, 0, NULL);

        if (h) CloseHandle(h);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        {
            HANDLE h = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);

            if (h) CloseHandle(h);   /* never waited on */
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&g_stop, 1);
    }
    return TRUE;
}
