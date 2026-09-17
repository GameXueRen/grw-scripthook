/* Time and weather control, the rewrite that replaced the third-party
 * Time&Weather.asi outright: its folder, its three ini and its switch in
 * scripthook.ini are gone from the tree and from the game folder, so this is
 * now the only time/weather control in the build.
 *
 * See docs/timeweather-reverse.md for the derivation. The short version of
 * why this rewrite is the easy one of the two: that plugin is already a
 * pure consumer of this framework's own weather API. Its import table has
 * no VirtualProtect and no VirtualAlloc, and every entry point it resolves
 * by name is a Sh* export - ShSetTime, ShSetTimeSpeed, ShSetWeatherBlend,
 * ShReleaseWeather, ShIsInGame and the menu calls. There is no mechanism to
 * recover, only an interface and a schedule to redo.
 *
 * So this file installs no hook, links no MinHook, and touches no engine
 * memory. Everything that changes the world goes through the framework, in
 * the plugin's own thread, and is handed back when it should not apply.
 *
 * Behaviour:
 *   - the clock rate is the setting's value straight through: a session
 *     with day_speed=2.0 reads back ShGetTimeSpeed() == 2.000, no scaling;
 *   - the day is cut into four windows - dawn 05:00-07:00, day 07:00-18:00,
 *     dusk 18:00-20:00, night 20:00-05:00 - and each runs on its own rate,
 *     switched at the boundary. The old plugin instead interpolated the two
 *     twilights between day and night off a constant table (5.0 5.5 6.5
 *     7.0 / 18.0 18.5 19.5 20.0 and a 0.5 factor); that ramp is why a
 *     change used to look like it crept into effect hour by hour, and it is
 *     gone. The four windows are a table, and the window a row covers is
 *     written on the row;
 *   - weather changes go through ShSetWeatherBlend (a transition, not a
 *     cut), and "Default" is ShReleaseWeather - hand it back;
 *   - its tick was 500 ms in two places and 250 ms in one; this one is
 *     250 ms, which covers the readout and the phase boundaries;
 *   - the status line is a readout, not a setting: HH:MM, the four phases
 *     in clock order with the one in force bracketed, and two rates - what
 *     the clock is doing read back from the game, and what the phase in
 *     force is set to. They differ only while a send is being refused;
 *   - outside a game nothing is written and the status line says so.
 *
 * Config, this plugin's own ini (plugins\TimeWeatherControl\TimeWeatherControl.ini):
 *
 *     [TimeWeather]
 *     enabled=1
 *     weather=default      default|sunny|light_clouds|heavy_clouds|fog|
 *                          light_rain|heavy_rain
 *     day_speed=1.00
 *     dusk_speed=1.00
 *     night_speed=1.00
 *     dawn_speed=1.00
 *     hour=12
 *     minute=0
 *
 * The keys are this plugin's own. day_speed and night_speed carry the names
 * the old plugin used for [DynamicTimeWeather] DaySpeed / NightSpeed, so an
 * existing ini keeps its two values; dusk_speed and dawn_speed are new and
 * default to 1.00.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "scripthook.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ---- state ------------------------------------------------------------ */

/* Off by default, like every other function in every plugin here: being
 * loaded is not the same as doing something. Switched off, the plugin
 * hands the clock and the weather straight back. */
static volatile LONG g_enabled = 0;
/* One rate per phase, x100 so Interlocked fits, indexed the way PhaseOf
 * numbers them: 0 day, 1 dusk, 2 night, 3 dawn. */
static volatile LONG g_speed[4] = { 100, 100, 100, 100 };

/* The four windows in the order the clock meets them - dawn, day, dusk,
 * night - which is the order the menu lists them in and the order the
 * status line reads. PhaseOf numbers them differently (0 day, 1 dusk,
 * 2 night, 3 dawn) because that is the schedule's own order; this array is
 * the bridge, and every player-facing list goes through it so the two
 * orders cannot drift apart. */
static const int g_phaseOrder[4] = { 3, 0, 1, 2 };

/* Phase-indexed: the ini key each rate is stored under. */
static const char *g_speedKey[4] = { "day_speed", "dusk_speed",
                                     "night_speed", "dawn_speed" };

/* Menu order, entry for entry with g_phaseOrder: the row label for each
 * window. Long form, because the window is what the row is about - the
 * status line uses the short @tw.p.* names instead. */
static const char *g_speedId[4] = { "@tw.dawn", "@tw.day",
                                    "@tw.dusk", "@tw.night" };
static volatile LONG g_weather;             /* index into g_weatherOpts  */
static volatile LONG g_hour       = 12;
static volatile LONG g_minute;
static uint32_t      g_menu;
static HINSTANCE     g_inst;
static char          g_iniPath[MAX_PATH];

/* This plugin's owner name in the text layer: the folder it lives in, and
 * the name ShLangText resolves @tw.* against. */
#define TW_OWNER "TimeWeatherControl"

/* What is out there right now, so nothing is re-sent every tick. */
static volatile LONG g_sentRate;            /* x100, -1 = nothing sent  */
static volatile LONG g_sentWeather = -1;    /* -1 = nothing sent        */

/* ---- text -------------------------------------------------------------
 * Stable @tw IDs with the wording the old plugin's lang.ini carried, so a
 * session that had that plugin reads the same in Chinese and English. New
 * rows (enabled, the blocked line) are the only additions.
 */
static const ShText kEn[] = {
    { "@tw.page",     "Weather & Real-time Time Control" },
    { "@tw.enabled",  "Enable" },
    { "@tw.status",   "%s   %s   Rate %.2f / set %.2f" },
    { "@tw.on",       "On" },
    { "@tw.off",      "Off" },
    { "@tw.weather",  "Weather" },
    { "@tw.hour",     "Hour (24h)" },
    { "@tw.minute",   "Minute" },
    { "@tw.apply",    "Apply the set time" },
    { "@tw.dawn",     "Dawn Speed (05:00-07:00)" },
    { "@tw.day",      "Day Speed (07:00-18:00)" },
    { "@tw.dusk",     "Dusk Speed (18:00-20:00)" },
    { "@tw.night",    "Night Speed (20:00-05:00)" },
    { "@tw.w.default", "Default" },
    { "@tw.w.sunny",  "Sunny" },
    { "@tw.w.lclouds", "Light Clouds" },
    { "@tw.w.hclouds", "Heavy Clouds" },
    { "@tw.w.fog",    "Fog" },
    { "@tw.w.lrain",  "Light Rain" },
    { "@tw.w.hrain",  "Heavy Rain" },
    { "@tw.wait",     "Waiting for game..." },
    { "@tw.blocked",  "Blocked in this mode" },
    { "@tw.disabled", "Off" },
    /* The four phases, in the order the clock meets them in a day. The
     * status line brackets the one in force - it is drawn in one colour,
     * so bracketing is what marks it - and the names come from the text
     * layer, so a lang.ini row renames them like any other line. */
    { "@tw.p.day",    "Day" },
    { "@tw.p.dusk",   "Dusk" },
    { "@tw.p.night",  "Night" },
    { "@tw.p.dawn",   "Dawn" },
    { "@tw.hint",
      "Hour and minute take effect only on Enter over \"Apply the set time\"" }
};

static const ShText kZh[] = {
    { "@tw.page",     "天气 & 时间实时控制" },
    { "@tw.enabled",  "启用" },
    { "@tw.status",   "%s   %s   时间流速：%.2f（设定 %.2f）" },
    { "@tw.on",       "开" },
    { "@tw.off",      "关" },
    { "@tw.weather",  "天气" },
    { "@tw.hour",     "小时（24 小时制）" },
    { "@tw.minute",   "分钟" },
    { "@tw.apply",    "应用当前设置的时间" },
    { "@tw.dawn",     "黎明时间流逝速度（05:00-07:00）" },
    { "@tw.day",      "白天时间流逝速度（07:00-18:00）" },
    { "@tw.dusk",     "黄昏时间流逝速度（18:00-20:00）" },
    { "@tw.night",    "夜晚时间流逝速度（20:00-05:00）" },
    { "@tw.w.default", "默认动态" },
    { "@tw.w.sunny",  "晴天" },
    { "@tw.w.lclouds", "阴云" },
    { "@tw.w.hclouds", "多云" },
    { "@tw.w.fog",    "起雾" },
    { "@tw.w.lrain",  "下雨" },
    { "@tw.w.hrain",  "风暴" },
    { "@tw.wait",     "等待进入游玩画面..." },
    { "@tw.blocked",  "当前模式已禁用" },
    { "@tw.disabled", "已关闭" },
    { "@tw.p.day",    "白天" },
    { "@tw.p.dusk",   "黄昏" },
    { "@tw.p.night",  "夜晚" },
    { "@tw.p.dawn",   "黎明" },
    { "@tw.hint",
      "小时、分钟设置后，需回车“应用当前设置的时间”生效" }
};

static void TwText(void) {
    static int done;

    if (done) return;
    done = 1;
    ShLangDeclare(TW_OWNER, "en-US", kEn,
                  (int)(sizeof(kEn) / sizeof(kEn[0])));
    ShLangDeclare(TW_OWNER, "zh-CN", kZh,
                  (int)(sizeof(kZh) / sizeof(kZh[0])));
}

/* The seven weather choices, in the old plugin's order, and the engine
 * value each maps to (-1 = hand it back). */
static const char *g_weatherOpts[] = {
    "@tw.w.default", "@tw.w.sunny", "@tw.w.lclouds", "@tw.w.hclouds",
    "@tw.w.fog", "@tw.w.lrain", "@tw.w.hrain"
};
static const int g_weatherValue[] = {
    -1, SH_WEATHER_SUNNY, SH_WEATHER_CLOUDS_LIGHT, SH_WEATHER_CLOUDS_HEAVY,
    SH_WEATHER_FOG, SH_WEATHER_RAIN_LIGHT, SH_WEATHER_RAIN_HEAVY
};
static const char *g_weatherIni[] = {
    "default", "sunny", "light_clouds", "heavy_clouds", "fog",
    "light_rain", "heavy_rain"
};
#define NWEATHER ((int)ARRAY_LEN(g_weatherOpts))

/* The on/off row's two options, so it reads like every other row in the
 * page rather than as a bracketed switch. Index 1 is on, which is what the
 * callback stores. */
static const char *g_onOffOpts[] = { "@tw.off", "@tw.on" };

/* The hour and minute rows are LISTS rather than number rows, because a
 * list WRAPS at both ends - left from 0 lands on 23 (or 59), right from
 * the last lands on 0 - while a number row clamps, and ShMenuSetValue is a
 * no-op on number rows, so a plugin cannot push one back from an edge. The
 * labels are plain numbers built once into static buffers: the row borrows
 * the pointers for as long as it lives, and a number needs no translation. */
static char        g_hourText[24][3];
static const char *g_hourOpts[24];
static char        g_minText[60][3];
static const char *g_minOpts[60];

static void BuildClockOpts(void) {
    int i;

    for (i = 0; i < 24; i++) {
        snprintf(g_hourText[i], sizeof(g_hourText[i]), "%d", i);
        g_hourOpts[i] = g_hourText[i];
    }
    for (i = 0; i < 60; i++) {
        snprintf(g_minText[i], sizeof(g_minText[i]), "%d", i);
        g_minOpts[i] = g_minText[i];
    }
}

/* ---- logging ---------------------------------------------------------- */

static FILE *g_log;
static LONG  g_logBusy;

static void TwLog(const char *fmt, ...) {
    va_list ap;
    char line[400];
    SYSTEMTIME st;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (!g_log) return;
    while (InterlockedExchange(&g_logBusy, 1)) Sleep(1);
    if (g_log) {
        GetLocalTime(&st);
        fprintf(g_log, "%02u:%02u:%02u.%03u  %s\n", st.wHour, st.wMinute,
                st.wSecond, st.wMilliseconds, line);
        fflush(g_log);
    }
    InterlockedExchange(&g_logBusy, 0);
}

/* ---- ini -------------------------------------------------------------- */

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

static int IniInt(const char *key, int def, int lo, int hi) {
    int v;

    if (!g_iniPath[0]) return def;
    v = (int)GetPrivateProfileIntA("TimeWeather", key, def, g_iniPath);
    return v < lo ? lo : (v > hi ? hi : v);
}

static void IniStr(const char *key, const char *def, char *out, int size) {
    out[0] = 0;
    if (!g_iniPath[0]) { snprintf(out, (size_t)size, "%s", def); return; }
    GetPrivateProfileStringA("TimeWeather", key, def, out, (DWORD)size,
                             g_iniPath);
}

/* The speeds are multipliers with two decimals, the menu's range is
 * 0.00-10.00, and they are kept as hundredths. They have to be READ as
 * decimals too: GetPrivateProfileInt reads "1.00" as 1, which is 0.01x once
 * divided - the bug that made a fresh install crawl at a hundredth of the
 * game's rate while the ini on disk said 1.00. */
static int IniSpeed(const char *key, int def) {
    char buf[32];
    double v;

    if (!g_iniPath[0]) return def;
    buf[0] = 0;
    GetPrivateProfileStringA("TimeWeather", key, "", buf, sizeof(buf),
                             g_iniPath);
    if (!buf[0]) return def;
    v = atof(buf);
    if (v < 0.0) v = 0.0;
    if (v > 10.0) v = 10.0;
    return (int)(v * 100.0 + 0.5);
}

static int WeatherFromIni(const char *name) {
    int i;

    for (i = 0; i < NWEATHER; i++)
        if (!_stricmp(name, g_weatherIni[i])) return i;
    return 0;
}

static void LoadConfig(void) {
    char w[32];
    int i;

    if (!g_iniPath[0]) return;
    InterlockedExchange(&g_enabled, IniInt("enabled", 0, 0, 1));
    for (i = 0; i < 4; i++)
        InterlockedExchange(&g_speed[i], IniSpeed(g_speedKey[i], 100));
    InterlockedExchange(&g_hour, IniInt("hour", 12, 0, 23));
    InterlockedExchange(&g_minute, IniInt("minute", 0, 0, 59));
    IniStr("weather", "default", w, sizeof(w));
    i = WeatherFromIni(w);
    InterlockedExchange(&g_weather, i);
    TwLog("ini: enabled=%ld day=%ld dusk=%ld night=%ld dawn=%ld weather=%s"
          " hour=%ld minute=%ld",
          (long)g_enabled, (long)g_speed[0], (long)g_speed[1],
          (long)g_speed[2], (long)g_speed[3], w, (long)g_hour,
          (long)g_minute);
}

/* Menu rows fire on every change and SaveConfig is a read-modify-write of
 * the whole file per call, so holding a stepper produced one per tick. The
 * callbacks mark the file dirty instead and the tick thread, which is
 * already running, writes it once the burst is over. */
static volatile LONG g_iniDirty;

static void SaveConfigSoon(void) { InterlockedExchange(&g_iniDirty, 1); }

static void SaveConfig(void) {
    char buf[32];
    int i;
    LONG w = InterlockedCompareExchange(&g_weather, 0, 0);

    if (!g_iniPath[0]) return;
    snprintf(buf, sizeof(buf), "%d", (int)InterlockedCompareExchange(
                 &g_enabled, 0, 0));
    WritePrivateProfileStringA("TimeWeather", "enabled", buf, g_iniPath);
    for (i = 0; i < 4; i++) {
        snprintf(buf, sizeof(buf), "%.2f",
                 (double)InterlockedCompareExchange(&g_speed[i], 0, 0)
                     / 100.0);
        WritePrivateProfileStringA("TimeWeather", g_speedKey[i], buf,
                                   g_iniPath);
    }
    WritePrivateProfileStringA("TimeWeather", "weather",
                               g_weatherIni[(w >= 0 && w < NWEATHER) ? w : 0],
                               g_iniPath);
}

/* ---- the schedule ------------------------------------------------------
 * Four windows, each on its own rate with nothing between them: a setting
 * takes effect at the boundary. The edges are the old plugin's own (its
 * constant table carries exactly these); what is gone is the interpolation
 * it put across them - that ramp is why a change used to arrive slowly.
 *
 * 0 day, 1 dusk, 2 night, 3 dawn - the order PhaseOf returns, the order
 * g_speed is indexed in, and the order g_speedKey names. The player sees
 * them in clock order instead; g_phaseOrder is the bridge.
 */
static int PhaseOf(float h) {
    if (h >= 7.0f && h <= 18.0f) return 0;
    if (h > 18.0f && h <= 20.0f) return 1;
    if (h >= 5.0f && h < 7.0f)  return 3;
    return 2;
}

/* The phase in force is the whole answer now: no fraction, no blending. */
static float WantRate(float hours) {
    return (float)InterlockedCompareExchange(&g_speed[PhaseOf(hours)], 0, 0)
               / 100.0f;
}

/* The four names as one line, the one in force bracketed. The status line
 * is drawn in a single colour, so bracketing is the only way to mark one;
 * the names come from the text layer, so a lang.ini row can rename them. */
static void PhaseText(float h, char *out, size_t cap) {
    /* Entry for entry with g_phaseOrder: short names, in the same clock
     * order the menu lists the windows in. */
    static const char *ids[4] = { "@tw.p.dawn", "@tw.p.day",
                                  "@tw.p.dusk", "@tw.p.night" };
    size_t used = 0;
    int act = PhaseOf(h), i;

    if (!cap) return;
    out[0] = 0;
    for (i = 0; i < 4 && used + 1 < cap; i++) {
        const char *nm = ShLangText(TW_OWNER, ids[i]);
        int on = (g_phaseOrder[i] == act);
        int w = snprintf(out + used, cap - used, "%s%s%s%s",
                         i ? " " : "", on ? "[" : "", nm, on ? "]" : "");

        if (w <= 0 || (size_t)w >= cap - used) break;   /* no room left */
        used += (size_t)w;
    }
}

/* ---- menu ------------------------------------------------------------- */

static void RefreshStatus(float hours, float rate, int inGame, int allowed) {
    char clock[16], phases[64];
    float live = 0.0f, shown = rate;
    int h, m;

    if (!g_menu) return;
    /* Only while this page is the one on screen - nobody can read the line
     * otherwise, and the tick runs four times a second. */
    if (!ShMenuIsShowing(g_menu)) return;
    if (!inGame) { ShMenuStatus(g_menu, "@tw.wait"); return; }
    if (!allowed) { ShMenuStatus(g_menu, "@tw.blocked"); return; }
    /* Switched off is not "waiting for a game": that was the old line's
     * answer here, and it is false while the player is standing in one. */
    if (!InterlockedCompareExchange(&g_enabled, 0, 0)) {
        ShMenuStatus(g_menu, "@tw.disabled");
        return;
    }
    h = (int)hours; if (h > 23) h = 23; if (h < 0) h = 0;
    m = (int)((hours - (float)h) * 60.0f); if (m > 59) m = 59; if (m < 0) m = 0;
    snprintf(clock, sizeof(clock), "%02d:%02d", h, m);
    PhaseText(hours, phases, sizeof(phases));
    /* Two numbers, because they answer two questions: what the clock is
     * doing (read back from the game, so a refused send shows as the
     * difference between them) and what the phase in force is set to. With
     * the windows cut rather than blended the two agree as soon as a send
     * lands, which is what makes a refusal visible instead of looking like
     * a slow ramp. */
    if (ShGetTimeSpeed(&live)) shown = live;
    ShMenuStatusF(g_menu, "@tw.status", clock, phases, (double)shown,
                  (double)rate);
}

/* The speed rows are lists, not number rows. A number row's value reaches
 * its callback as an int (scripthook_menu.c casts it), so a 0.25 step would
 * arrive as 0 for "0.75" - the day clock would stop. Entries are 0.25 apart
 * and the callback gets the index, which maps straight to hundredths. */
#define SPEED_STEPS 41
static char        g_speedLabel[SPEED_STEPS][8];
static const char *g_speedOpts[SPEED_STEPS];

static void BuildSpeedOpts(void) {
    int i;

    if (g_speedOpts[0]) return;
    for (i = 0; i < SPEED_STEPS; i++) {
        snprintf(g_speedLabel[i], sizeof(g_speedLabel[i]), "%.2fx", i * 0.25);
        g_speedOpts[i] = g_speedLabel[i];
    }
}

static int SpeedIndex(LONG hundredths) {
    int i = (int)((hundredths + 12) / 25);      /* nearest 0.25 step */

    if (i < 0) i = 0;
    if (i >= SPEED_STEPS) i = SPEED_STEPS - 1;
    return i;
}

static void OnEnabled(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    InterlockedExchange(&g_enabled, value ? 1 : 0);
    InterlockedExchange(&g_sentRate, -1);
    InterlockedExchange(&g_sentWeather, -1);
    SaveConfigSoon();
    TwLog("menu: enabled=%d", value);
}

/* One callback for all four rows: the phase travels in the row's user
 * pointer, so a window is a table entry rather than a function. */
static void OnSpeed(uint32_t menu, uint32_t item, int value, void *user) {
    int phase = (int)(intptr_t)user;

    (void)menu; (void)item;
    if (phase < 0 || phase > 3) return;
    if (value < 0 || value >= SPEED_STEPS) return;
    InterlockedExchange(&g_speed[phase], (LONG)value * 25);
    InterlockedExchange(&g_sentRate, -1);
    SaveConfigSoon();
    TwLog("menu: %s=%d.%02d", g_speedKey[phase],
          (value * 25) / 100, (value * 25) % 100);
}

static void OnWeather(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    if (value < 0 || value >= NWEATHER) return;
    InterlockedExchange(&g_weather, value);
    InterlockedExchange(&g_sentWeather, -1);
    SaveConfigSoon();
    TwLog("menu: weather=%s", g_weatherIni[value]);
}

static void OnHour(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    InterlockedExchange(&g_hour, (LONG)value);
    SaveConfigSoon();
}

static void OnMinute(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    InterlockedExchange(&g_minute, (LONG)value);
    SaveConfigSoon();
}

static void OnApply(uint32_t menu, uint32_t item, int value, void *user) {
    LONG h = InterlockedCompareExchange(&g_hour, 0, 0);
    LONG m = InterlockedCompareExchange(&g_minute, 0, 0);
    float hours;

    (void)menu; (void)item; (void)user;
    /* A 24 hour clock, straight through: the row is 0-23 and there is no
     * AM/PM row left to combine with. The clock runs on from here - this
     * sets the time, it does not freeze it. */
    hours = (float)h + (float)m / 60.0f;
    if (ShSetTime(hours)) {
        InterlockedExchange(&g_sentRate, -1);   /* re-pick the speed now */
        TwLog("menu: set time to %.3f h", (double)hours);
    } else {
        TwLog("menu: ShSetTime refused (%d)", ShLastError());
    }
}

static void BuildMenu(void) {
    LONG w = InterlockedCompareExchange(&g_weather, 0, 0);
    int i;

    TwText();
    BuildClockOpts();
    g_menu = ShMenuCreate("@tw.page");
    if (!g_menu) { TwLog("ShMenuCreate failed"); return; }
    /* Every row is a list or a number, so the page reads as one thing: the
     * switch is a list of off/on as well. */
    ShMenuList(g_menu, "@tw.enabled", g_onOffOpts, 2,
               (int)InterlockedCompareExchange(&g_enabled, 0, 0),
               OnEnabled, NULL);
    ShMenuList(g_menu, "@tw.weather", g_weatherOpts, NWEATHER,
               (int)((w >= 0 && w < NWEATHER) ? w : 0), OnWeather, NULL);
    /* Lists, so left/right cycles: right from 23 lands on 0, left from 0
     * lands on 23, and the same for the minutes. */
    ShMenuList(g_menu, "@tw.hour", g_hourOpts, 24,
               (int)InterlockedCompareExchange(&g_hour, 0, 0),
               OnHour, NULL);
    ShMenuList(g_menu, "@tw.minute", g_minOpts, 60,
               (int)InterlockedCompareExchange(&g_minute, 0, 0),
               OnMinute, NULL);
    ShMenuAction(g_menu, "@tw.apply", OnApply, NULL);
    /* The speed rows: 0.00 to 10.00 in 0.25 steps, 1.00 being the game's
     * own rate. Lists, so left/right cycles like every other row. */
    BuildSpeedOpts();
    /* Rows in the order the clock meets the windows, each driving the phase
     * it names: g_speedId and g_phaseOrder line up entry for entry, so the
     * label a player reads and the rate behind it cannot come apart. */
    for (i = 0; i < 4; i++) {
        int ph = g_phaseOrder[i];

        ShMenuList(g_menu, g_speedId[i], g_speedOpts, SPEED_STEPS,
                   SpeedIndex(InterlockedCompareExchange(&g_speed[ph], 0, 0)),
                   OnSpeed, (void *)(intptr_t)ph);
    }
    ShMenuHint(g_menu, "@tw.hint");
    TwLog("menu created");
}

/* Set on unload. Only a flag: the release belongs to the tick thread, the
 * only place a framework call is allowed - DllMain runs under the loader
 * lock. */
static volatile LONG g_stop = 0;

/* ---- the tick ---------------------------------------------------------
 * 250 ms: the old plugin's own fastest loop. Nothing is sent unless it
 * differs from what is out there, so a session that changes nothing costs
 * three reads per tick and one status line.
 */
static DWORD WINAPI TickThread(LPVOID p) {
    int gaveBack = 1;

    (void)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        float hours = 0.0f, rate = 0.0f;
        int inGame, allowed;

        Sleep(250);
        /* Whatever the menu marked dirty lands here, once per burst. */
        if (InterlockedExchange(&g_iniDirty, 0)) SaveConfig();
        inGame = ShIsInGame();
        allowed = inGame && ShPluginAllowed();

        if (!allowed) {
            /* Blocked, or no game yet: hand back once, write nothing after
             * that. Giving it back is the whole contract - a PvP mode must
             * not be left with a 2x clock. */
            if (!gaveBack) {
                ShReleaseWeather();
                ShSetTimeSpeed(1.0f);
                InterlockedExchange(&g_sentRate, -1);
                InterlockedExchange(&g_sentWeather, -1);
                gaveBack = 1;
                TwLog("handed back (in game %d, allowed %d)", inGame, allowed);
            }
            RefreshStatus(0.0f, 0.0f, inGame, allowed);
            continue;
        }
        gaveBack = 0;

        if (!InterlockedCompareExchange(&g_enabled, 0, 0)) {
            /* Switching the plugin off hands back what it took, exactly
             * like a blocked mode does: leaving a 2x clock and a pinned
             * weather behind would be the same as never switching off. */
            if (!gaveBack) {
                ShReleaseWeather();
                ShSetTimeSpeed(1.0f);
                InterlockedExchange(&g_sentRate, -1);
                InterlockedExchange(&g_sentWeather, -1);
                gaveBack = 1;
                TwLog("handed back (switch off)");
            }
            if (ShGetTime(&hours)) RefreshStatus(hours, 0.0f, inGame, allowed);
            continue;
        }
        gaveBack = 0;

        if (ShGetTime(&hours)) {
            rate = WantRate(hours);
            {
                LONG want = (LONG)(rate * 100.0f + 0.5f);
                LONG sent = InterlockedCompareExchange(&g_sentRate, 0, 0);

                if (sent != want) {
                    if (ShSetTimeSpeed(rate)) {
                        InterlockedExchange(&g_sentRate, want);
                        TwLog("rate %.2f at %.2f h (%s)",
                              (double)rate, (double)hours,
                              g_speedKey[PhaseOf(hours)]);
                    }
                }
            }
        }

        {
            LONG w = InterlockedCompareExchange(&g_weather, 0, 0);
            LONG sent = InterlockedCompareExchange(&g_sentWeather, 0, 0);

            if (sent != w) {
                if (w >= 0 && w < NWEATHER && g_weatherValue[w] >= 0) {
                    /* A transition, not a cut - the same call the old
                     * plugin used. */
                    if (ShSetWeatherBlend(g_weatherValue[w], 2.0f)) {
                        InterlockedExchange(&g_sentWeather, w);
                        TwLog("weather -> %s", g_weatherIni[w]);
                    }
                } else if (ShReleaseWeather()) {
                    InterlockedExchange(&g_sentWeather, w);
                    TwLog("weather released to ambient");
                }
            }
        }

        RefreshStatus(hours, rate, inGame, allowed);
    }

    /* Unloading: hand the clock and the weather back from here, the only
     * place that may - DllMain runs under the loader lock. */
    ShReleaseWeather();
    ShSetTimeSpeed(1.0f);
    return 0;
}

/* ---- startup ---------------------------------------------------------- */

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

static void OpenLog(void) {
    char dir[MAX_PATH], logs[MAX_PATH], path[MAX_PATH];
    char *slash;

    if (!GetModuleFileNameA(NULL, dir, MAX_PATH)) return;
    slash = strrchr(dir, '\\');
    if (!slash) return;
    slash[1] = 0;
    if (!LogWanted()) return;
    /* Built with snprintf: the old fixed addend was four bytes short of
     * what "\\TimeWeatherControl.log" needs, so a long game path ran off
     * the end of the buffer. */
    if (snprintf(logs, sizeof(logs), "%slogs", dir) < 0) return;
    CreateDirectoryA(logs, NULL);
    if (snprintf(path, sizeof(path), "%s\\TimeWeatherControl.log", logs) < 0)
        return;
    /* "w", not "a": the framework's own logs are per session, and a
     * diagnostic that only ever grows is a file that grows on the player's
     * disk forever. */
    g_log = fopen(path, "w");
}

static DWORD WINAPI InitThread(LPVOID p) {
    (void)p;
    OpenLog();
    TwLog("--- TimeWeatherControl plugin ---");
    ResolveIniPath();
    LoadConfig();
    BuildMenu();

    /* Control over the clock and the weather is not something a PvP match
     * wants, so declare both PvP modes: the framework takes the page out
     * of the menu there and ShPluginAllowed turns false, which the tick
     * answers by handing everything back. */
    if (ShPluginBlacklist(SH_MODE_BLACKLIST_GHOST_WAR |
                          SH_MODE_BLACKLIST_MERCENARIES))
        TwLog("blacklist: Ghost War and Mercenaries declared");
    else
        TwLog("blacklist: declaration refused (%d)", ShLastError());

    {
        HANDLE h = CreateThread(NULL, 0, TickThread, NULL, 0, NULL);

        if (h) CloseHandle(h);   /* never waited on */
    }
    TwLog("ready: enabled=%ld day=%ld dusk=%ld night=%ld dawn=%ld weather=%s",
          (long)g_enabled, (long)g_speed[0] / 100, (long)g_speed[1] / 100,
          (long)g_speed[2] / 100, (long)g_speed[3] / 100,
          g_weatherIni[InterlockedCompareExchange(&g_weather, 0, 0)]);
    RefreshStatus(0.0f, 0.0f, 0, 1);
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
