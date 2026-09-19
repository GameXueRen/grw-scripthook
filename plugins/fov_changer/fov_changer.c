/* Field of view, changed from the menu and held every
 * frame by the ScriptHook's camera override.
 *
 * Two things share the one channel: the override, which widens the
 * first and third person view, and No zoom on iron sights, which keeps
 * the hip's own fov while the iron sights are up. The second is
 * independent of the first - it needs neither the override switch nor
 * the fov row. Both, and the fov itself, are kept in fov_changer.ini
 * beside the .asi and restored on the next launch.
 */
/* Linked against the ScriptHook, so the API is called
 * directly. See src/README.md for how that works.
 */
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>

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
/* No zoom polls the fov faster than the menu's cadence: the sights come
 * up in a frame or two, not in half a second. */
#define NOZOOM_MS   16
/* Where an iron sight's fov sits. Measured live on the 2026-09 build,
 * the aim fov of every sight in the game: iron sights 0.69, 1x / 2.5x /
 * 3x 0.49, 2x 0.40 and 0.34, the 1x step of the dual lens 0.30, 3.5x
 * 0.28, 4x 0.25 and 0.24, 4.5x 0.23 and 0.19, 5x and 5.5x 0.20, 6x 0.17;
 * the hip is about 0.81. The iron sight is the only aim in this band -
 * the hip is above it and every magnified optic well below - so it is
 * recognised by the value alone. That matters: the aim state a plugin
 * can read is not dependable, and the whole feature would sit idle on a
 * frame where it read wrong.
 */
#define SIGHT_LO    0.60f
#define SIGHT_HI    0.75f

/* Config convention, the same every plugin follows: the .ini sits beside
 * the .asi and takes its base name, so fov_changer.asi pairs with
 * fov_changer.ini. The name comes from the module file rather than a
 * literal, so the pairing survives a rename.
 */
static HINSTANCE g_inst = NULL;
static char      g_name[64];
static char      g_iniPath[MAX_PATH];
/* A slider row fires on every change, and one save is three
 * read-modify-write passes of the whole file. Dragging one produced that
 * per tick, so the rows only mark the file dirty and the tick writes it
 * once the burst is over. */
static volatile LONG g_iniDirty = 0;

static uint32_t g_menu = 0;
static volatile LONG  g_on = 0;      /* read by the tick, written by the menu */
/* Set on unload. Only a flag: the release belongs to the tick thread, because
 * DllMain runs under the loader lock, where a framework call is forbidden. */
static volatile LONG  g_stop = 0;
static volatile float g_deg = 0.0f;
static volatile float g_defaultRad = 0.0f;
/* What the ini asked for, applied once the menu exists. */
static int g_initOverride = 0;
static int g_initNozoom = 0;
/* No zoom on iron sights: the toggle, the fov the hip last had, the value
 * the engine computed on the last frame, the value the render camera really
 * carries, and whether the last pass found the sights. The last three are
 * what the status line reports - engine against camera is what tells a
 * replacement that landed from one the engine wrote over.
 */
static volatile LONG  g_nozoom = 0;
static volatile float g_hipRad = 0.0f;
static volatile float g_engRad = 0.0f;
static volatile float g_camRad = 0.0f;
static volatile float g_engPrev = 0.0f;
static volatile LONG  g_aim = 0;

static int OverrideOn(void) {
    return InterlockedCompareExchange(&g_on, 0, 0) ? 1 : 0;
}

/* ---- settings -------------------------------------------- */

/* Resolve <gamedir>\plugins\<name>\<name>.ini from the plugin's own file
 * name, once - the framework knows the folders, this knows its own name. */
static void ResolveIniPath(HMODULE m) {
    char mod[MAX_PATH];
    const char *base, *dot;
    size_t len;

    g_name[0] = 0;
    g_iniPath[0] = 0;
    if (!m || !GetModuleFileNameA(m, mod, sizeof(mod))) return;
    base = strrchr(mod, '\\');
    base = base ? base + 1 : mod;
    dot = strrchr(base, '.');
    len = dot ? (size_t)(dot - base) : strlen(base);
    if (len == 0 || len >= sizeof(g_name)) return;
    memcpy(g_name, base, len);
    g_name[len] = 0;
    if (!ShPluginIniPath(g_name, g_iniPath, sizeof(g_iniPath)))
        g_iniPath[0] = 0;
}

static int IniInt(const char *key, int def) {
    if (!g_iniPath[0]) return def;
    return GetPrivateProfileIntA("Settings", key, def, g_iniPath);
}

/* The settings are optional: with no file, or a key missing, the built-in
 * defaults stand. */
static void LoadIni(void) {
    int v;

    if (!g_iniPath[0]) return;
    v = IniInt("fov", 0);
    if ((float)v >= DEG_MIN && (float)v <= DEG_MAX) g_deg = (float)v;
    g_initOverride = IniInt("override", 0) ? 1 : 0;
    g_initNozoom = IniInt("nozoom", 0) ? 1 : 0;
}

static void SaveIni(void) {
    char buf[64];

    if (!g_iniPath[0]) return;
    snprintf(buf, sizeof(buf), "%d", OverrideOn());
    WritePrivateProfileStringA("Settings", "override", buf, g_iniPath);
    snprintf(buf, sizeof(buf), "%d", (int)(g_deg + 0.5f));
    WritePrivateProfileStringA("Settings", "fov", buf, g_iniPath);
    snprintf(buf, sizeof(buf), "%d",
             InterlockedCompareExchange(&g_nozoom, 0, 0) ? 1 : 0);
    WritePrivateProfileStringA("Settings", "nozoom", buf, g_iniPath);
}

static void SaveIniSoon(void) { InterlockedExchange(&g_iniDirty, 1); }

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

/* What an aim has to keep: the player's own value when the override is on,
 * else the fov the hip had on the last wide frame.
 */
static float KeepRad(void) {
    if (OverrideOn()) return g_deg * DEG2RAD;
    return (g_hipRad > 0.0f) ? g_hipRad : DefaultRad();
}

/* One call is enough: the ScriptHook reapplies it inside
 * the engine's own frame until it is released.
 */
static int Push(void) {
    ShCameraOverride o;

    memset(&o, 0, sizeof(o));
    o.apply = SH_CAM_FOV;
    o.fov = KeepRad();
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
    { "@fov.nozoom",     "No zoom on iron sights" },
    { "@fov.reset",      "Back to the game default" },
    { "@fov.status.on",  "%.0f deg, game default %.0f" },
    { "@fov.status.off", "off, game is %.0f deg" },
    { "@fov.status.both.on",
      "fov %.0f deg (game %.0f)\niron sights: hold %.0f deg, engine %.2f, "
      "camera %.2f" },
    { "@fov.status.both.off",
      "fov %.0f deg (game %.0f)\nno iron sights, engine %.2f, camera %.2f" },
    { "@fov.notready",   "the camera is not ready" },
    { "@fov.hint",
      "Widens the first and third person camera fov; the aiming camera is "
      "not touched." }
};

static const ShText kZh[] = {
    { "@fov.page",       "延展视野范围" },
    { "@fov.override",   "覆盖游戏视野范围设置" },
    { "@fov.vertical",   "调整垂直视野范围（度）" },
    { "@fov.nozoom",     "机瞄开镜不缩放" },
    { "@fov.reset",      "恢复游戏默认视野范围" },
    { "@fov.status.on",  "当前视野范围 %.0f 度，游戏默认 %.0f" },
    { "@fov.status.off", "已关闭，当前视野范围 %.0f 度" },
    { "@fov.status.both.on",
      "视野范围 %.0f 度（游戏默认 %.0f）\n"
      "机瞄中：保持 %.0f 度，引擎 %.2f，相机 %.2f" },
    { "@fov.status.both.off",
      "视野范围 %.0f 度（游戏默认 %.0f）\n"
      "未在机瞄：引擎 %.2f，相机 %.2f" },
    { "@fov.notready",   "游戏视野尚未就绪" },
    { "@fov.hint",
      "延展第一/第三人称镜头的视野范围，不影响瞄准模式的镜头视野范围。" }
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
    float defDeg = DefaultRad() * RAD2DEG;
    if (InterlockedCompareExchange(&g_nozoom, 0, 0)) {
        /* The engine's own value and the camera's are both on the line
         * while the feature runs: equal means the replacement landed, and
         * a camera still on the engine's value means something wrote over
         * it after us. */
        if (InterlockedCompareExchange(&g_aim, 0, 0))
            ShMenuStatusF(g_menu, "@fov.status.both.on",
                          OverrideOn() ? g_deg : defDeg, defDeg,
                          KeepRad() * RAD2DEG, g_engRad, g_camRad);
        else
            ShMenuStatusF(g_menu, "@fov.status.both.off",
                          OverrideOn() ? g_deg : defDeg, defDeg,
                          g_engRad, g_camRad);
    } else if (g_on) {
        ShMenuStatusF(g_menu, "@fov.status.on",
                      g_deg, defDeg);
    } else {
        ShMenuStatusF(g_menu, "@fov.status.off",
                      defDeg);
    }
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
        /* The no-zoom feature is on the same channel; releasing the
         * override must not hand it back while the sights are held. */
        if (!InterlockedCompareExchange(&g_nozoom, 0, 0))
            ShCameraReleaseFields(SH_CAM_FOV);
    }
    SaveIniSoon();
    Report();
}

static void OnFov(uint32_t menu, uint32_t item, int value,
                  void *user) {
    (void)menu; (void)item; (void)user;
    g_deg = (float)value;
    if (OverrideOn()) Push();
    SaveIniSoon();
    Report();
}

/* No zoom on iron sights. Turning it off hands the channel back at once
 * rather than on the next pass: with the override off this feature is the
 * only thing holding it, and the tick may be half a second away.
 */
static void OnNoZoom(uint32_t menu, uint32_t item, int value,
                     void *user) {
    (void)menu; (void)item; (void)user;
    InterlockedExchange(&g_nozoom, value ? 1 : 0);
    if (!value) {
        ShFovPin(0);
        if (!OverrideOn()) ShCameraReleaseFields(SH_CAM_FOV);
    }
    SaveIniSoon();
    Report();
}

static void OnReset(uint32_t menu, uint32_t item, int value,
                    void *user) {
    (void)menu; (void)item; (void)value; (void)user;
    g_deg = DefaultRad() * RAD2DEG;
    /* The number row shows the framework's copy, so it has to be told. */
    ShMenuSetValue(g_menu, "@fov.vertical", (int)(g_deg + 0.5f));
    if (OverrideOn()) Push();
    SaveIniSoon();
    Report();
}

/* A blocked (PvP) mode: the framework takes this page out of the menu, so
 * the override has to let go here - a fov left pushed in Ghost War is
 * exactly what the blacklist exists to prevent. */
static void OnBlocked(int allowed, int blocked, void *user) {
    (void)blocked; (void)user;

    if (!allowed) {
        ShFovPin(0);
        ShCameraReleaseFields(SH_CAM_FOV);
    } else if (OverrideOn()) Push();
}

/* Entering a session reinstalls the camera hook, so the
 * override is pushed again to survive the transition.
 */
static DWORD WINAPI TickThread(LPVOID p) {
    int held = 0;      /* the fov channel is ours right now */
    (void)p;

    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        int in = ShIsInGame();
        int allowed = ShPluginAllowed();
        int nz = InterlockedCompareExchange(&g_nozoom, 0, 0) != 0;
        ShCamera cam;
        float eng = ShFovEngine();
        int aim;

        LearnDefault();

        /* What the render camera really carries, beside the engine's own
         * value: the two together say whether a replacement landed. */
        if (ShGetCamera(&cam) && cam.fov > 0.05f && cam.fov < 3.0f)
            g_camRad = cam.fov;

        /* The engine's own value, taken before it is replaced: it is what
         * tells the sights apart from the hip and from a magnified optic. */
        if (eng > 0.05f && eng < 3.0f) g_engRad = eng;
        /* The hip's own fov, learned only from a value that is standing
         * still. A frame halfway through the pull to the sights sits inside
         * this band as well, and taking one as the hip is what left the
         * sights keeping a fov that was already narrowed: measured 0.76 rad
         * held against a hip of 0.81. */
        if (eng >= SIGHT_HI && eng < 1.2f &&
            fabsf(eng - g_engPrev) < 0.002f)
            g_hipRad = eng;
        g_engPrev = eng;

        /* An iron sight is the only aim that lands in this band: the hip
         * sits above it and every magnified optic below. Recognising it by
         * the value keeps the feature independent of the aim state, which
         * a plugin cannot depend on reading. */
        aim = nz && eng > SIGHT_LO && eng < SIGHT_HI;
        InterlockedExchange(&g_aim, aim);

        if (!allowed || !in) {
            /* Blocked, or off the camera: the menu cannot switch it off
             * from inside a blocked mode, so the hold is let go here and
             * taken again once the mode allows it. */
            if (held) { ShCameraReleaseFields(SH_CAM_FOV); held = 0; }
            ShFovPin(0);
        } else if (OverrideOn() || aim) {
            ShCameraOverride o;

            memset(&o, 0, sizeof(o));
            o.apply = SH_CAM_FOV;
            o.fov = KeepRad();
            if (ShCameraApply(&o)) {
                held = 1;
                /* The sights' value is inside the gameplay range, so the
                 * engine would take the override on its own; the pin is
                 * what holds it there whatever a frame computes. */
                ShFovPin(aim);
            } else {
                held = 0;
                ShFovPin(0);
            }
        } else {
            if (held) { ShCameraReleaseFields(SH_CAM_FOV); held = 0; }
            ShFovPin(0);
        }

        /* One write per burst of changes, never one per slider notch. */
        if (InterlockedExchange(&g_iniDirty, 0)) SaveIni();

        /* The status line is only refreshed while somebody can read it. */
        if (ShMenuIsOpen()) Report();
        Sleep(nz ? NOZOOM_MS : TICK_MS);
    }

    /* Unloading: an override left pushed would be one with no owner left to
     * release it. */
    if (held) ShCameraReleaseFields(SH_CAM_FOV);
    ShFovPin(0);
    return 0;
}

/* All initialization is here, not in DllMain: that runs under the loader
 * lock, where taking the framework's own locks and allocating is what the
 * plugin contract forbids. */
static DWORD WINAPI InitThread(LPVOID p) {
    (void)p;
    g_deg = FALLBACK * RAD2DEG;
    ResolveIniPath(g_inst);
    LoadIni();
    FovText();
    g_menu = ShMenuCreate("@fov.page");
    if (!g_menu) return 0;          /* nothing to drive without a page */
    ShMenuToggle(g_menu, "@fov.override", g_initOverride, OnToggle, NULL);
    ShMenuNumber(g_menu, "@fov.vertical", g_deg, DEG_MIN, DEG_MAX,
                 DEG_STEP, OnFov, NULL);
    ShMenuToggle(g_menu, "@fov.nozoom", g_initNozoom, OnNoZoom, NULL);
    ShMenuHint(g_menu, "@fov.hint");
    ShMenuAction(g_menu, "@fov.reset", OnReset, NULL);
    /* The rows above only set the menu's copy; the hold itself is taken
     * here. A push before the camera is up simply fails and the tick takes
     * it again once a session is running, so the ini's wish is not lost. */
    if (g_initOverride) {
        InterlockedExchange(&g_on, 1);
        Push();
    }
    if (g_initNozoom) InterlockedExchange(&g_nozoom, 1);
    Report();
    /* A view override is not something a PvP match wants; saying it out
     * loud is what makes the release in the tick deliberate rather than an
     * accident of the default. */
    /* Both calls are checked, and the status line is how this plugin has
     * always reported a refusal (it keeps no log of its own). A refused
     * blacklist means OnBlocked never runs, and that callback is the one
     * thing that gives the FOV back when a PvP match starts. */
    if (!ShPluginBlacklist(SH_MODE_BLACKLIST_GHOST_WAR |
                           SH_MODE_BLACKLIST_MERCENARIES))
        ShMenuStatus(g_menu, "fov: blacklist declaration refused");
    if (!ShPluginOnBlocked(OnBlocked, NULL))
        ShMenuStatus(g_menu, "fov: on-blocked registration refused");
    {
        HANDLE h = CreateThread(NULL, 0, TickThread, NULL, 0, NULL);

        if (h) CloseHandle(h);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = inst;
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
