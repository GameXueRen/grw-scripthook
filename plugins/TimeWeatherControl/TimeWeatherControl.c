/* Time and weather, from the F4 menu.
 *
 * An independent plugin: every change goes through this framework's own
 * engine calls - ShSetTime, ShSetTimeSpeed, ShSetWeatherBlend,
 * ShReleaseWeather - so there is no hook here, no code patch and no memory
 * write of our own. The framework owns reaching the engine (the environment
 * object, the time manager, the queue onto the game thread); what this file
 * owns is the page, the settings, and the promise that turning the switch off
 * leaves the world the way the engine wanted it.
 *
 * The page, all of it one kind of row so one key is the whole interaction:
 *
 *   Apply on the fly     master switch, off by default - see below
 *   Weather              engine default, or the framework's six
 *   Hour (24h) / Minute  0..23 and 0..59
 *   Apply the set time   one action: ShSetTime(hour + minute / 60)
 *   Dawn / Day / Dusk / Night rate   the clock rate of each window
 *
 * Three things the settings ask for, and three different moments they are sent
 * at - which is the whole design of the loop below:
 *
 *   Time of day   written only when the player presses Enter on that one action
 *                 row - nothing else moves the world's clock, not the switch
 *                 going on, not a load, not the tick. Writing it every second
 *                 would pin the world at that hour, and a pinned world never
 *                 crosses a window, so four rates could never be more than one
 *                 of them; writing it anywhere else would move the clock for a
 *                 reason the player did not give.
 *   Clock rate    computed and written once a second from the hour the engine
 *                 reports: 05-07 dawn, 07-18 day, 18-20 dusk, 20-05 night,
 *                 night wrapping past midnight. Hard switching at the edges -
 *                 which is what the page this replaces did, and what its four
 *                 row labels say.
 *   Weather       written only when the engine's own type no longer matches
 *                 the row. That write is what arms the blend, so repeating it
 *                 every second would restart the transition every second.
 *
 * Off by default is deliberate: a plugin must not change the world because it
 * was installed. With the switch off nothing is called at all - the rows still
 * remember what was picked, and turning it on applies the rate and the weather
 * at once. The time of day is the one row that waits for its own key.
 *
 * The loop only touches the world in SH_STATE_INGAME or SH_STATE_PAUSED, so
 * menus and loads are left alone, and it does nothing at all while the mode
 * blacklist has this plugin blocked - the one hand back there is the callback's
 * job, once per flip.
 *
 * Config: plugins\TimeWeatherControl\TimeWeatherControl.ini, [Settings] - on,
 * hour, minute, dawn_speed, day_speed, dusk_speed, night_speed, weather. The
 * speeds are decimals: read them as text and strtod them, because
 * GetPrivateProfileInt turns 1.00 into 1, which would turn 1.00x into 0.01x.
 *
 * The plugin's own name is taken from its module path rather than written
 * twice (scripthook.h, plugin paths), so the ini, the log and the text owner
 * all follow the file name and a rename costs nothing.
 *
 * Text: kEn / kZh here, overridable by plugins\TimeWeatherControl\lang.ini.
 */

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scripthook.h"

/* What this plugin needs of the framework: nothing newer than the first
 * version of the plugin API, so any ScriptHook that carries the API at all can
 * load this (see SH_REQUIRES_API). Name the last thing you use, not the header
 * you happened to build against. */
SH_REQUIRES_API(1);
#include "log.h"

/* ---- the choices, as the rows show them --------------------------- */

/* The clock's own hour and minute. 24 and 60 list rows, which is what the
 * framework's option limit was raised to 64 for; the row wraps, so 23 steps
 * on to 0. The strings are built once at start up: numbers need no
 * translation, and the list has to outlive the menu. */
#define HOURS_N 24
#define MINS_N  60

static char        kHourOpts[HOURS_N][3];
static const char *kHourPtr[HOURS_N];
static char        kMinOpts[MINS_N][3];
static const char *kMinPtr[MINS_N];

/* The four windows, in the order the page shows them. Night wraps past
 * midnight - from > to is the flag for that, not an error. */
typedef struct { const char *label; const char *name; float from, to; } Phase;

static const Phase kPhases[] = {
    { "@tw.dawn",  "@tw.p.dawn",   5.0f,  7.0f },
    { "@tw.day",   "@tw.p.day",    7.0f, 18.0f },
    { "@tw.dusk",  "@tw.p.dusk",  18.0f, 20.0f },
    { "@tw.night", "@tw.p.night", 20.0f,  5.0f }
};
#define PHASES_N ((int)(sizeof(kPhases) / sizeof(kPhases[0])))

/* The rate of each window: a list row, not a number row. A number row's
 * callback carries an int - the framework drops the fraction on the way to
 * the plugin - so a 0.25 step would arrive as whole numbers and the row would
 * lie about what is in force. A list row hands over the option's index
 * instead, which is exact - so the index is what gets stored, and the value it
 * stands for lives in the table beside it.
 *
 * The grid is in two segments because a list row carries at most 64 options
 * (scripthook_menu.c, OPTS): half steps to 10.00, where the player is tuning
 * against the engine's own rate, then whole steps to 50.00, where the only
 * question left is how long a whole day should take. 21 + 40 = 61, which
 * fits. */
#define SPEED_FINE_N      21             /* 0.00 .. 10.00, step 0.50 */
#define SPEED_COARSE_N    40             /* 11.00 .. 50.00, step 1.00 */
#define SPEED_N           (SPEED_FINE_N + SPEED_COARSE_N)
#define SPEED_FINE_STEP   0.50f
#define SPEED_COARSE_STEP 1.00f
/* The option the fallbacks land on, taken from the grid rather than counted by
 * hand so it cannot drift from it when a step changes. */
#define SPEED_1X          ((int)(1.0f / SPEED_FINE_STEP))

static char        kSpeedOpts[SPEED_N][8];
static const char *kSpeedPtr[SPEED_N];
static float       kSpeedVal[SPEED_N];
static const char *kSpeedKeys[PHASES_N] = { "dawn_speed", "day_speed",
                                            "dusk_speed", "night_speed" };

/* Weather: index 0 hands the weather back to the engine (the old page's
 * "engine default"), 1..6 are the framework's enum from SH_WEATHER_SUNNY up. */
static const char *kWeatherOpts[] = { "@tw.w.default", "@tw.w.sunny",
                                      "@tw.w.lclouds", "@tw.w.hclouds",
                                      "@tw.w.fog", "@tw.w.lrain",
                                      "@tw.w.hrain" };
#define WEATHER_N ((int)(sizeof(kWeatherOpts) / sizeof(kWeatherOpts[0])))

/* ---- state -------------------------------------------------------- */

static char     g_ini[MAX_PATH];
/* This plugin's own name, from its module path: the ini beside the .asi, the
 * log and the text owner are all keyed by it. */
static char     g_name[64];
static uint32_t g_menu;

/* The settings, shared by the menu callbacks (overlay thread) and the loop
 * below (ours), hence the interlocked access. A rate is stored as the index
 * of its option rather than as a float, which keeps that access an integer
 * one; the value it stands for is kSpeedVal[that index]. */
static volatile LONG g_on;
static volatile LONG g_hour;
static volatile LONG g_minute;
static volatile LONG g_speed[PHASES_N];
static volatile LONG g_weather;

/* One line per problem, not one per second. A refusal that repeats is the same
 * news, and the framework's own modules answer it the same way
 * (scripthook_weather.c, WhyOnce): the kind of refusal is what is remembered,
 * so a different one still gets its line, and the first success clears it - a
 * failure that shows up again after a working spell is news again. */
enum { WHY_TIME = 0, WHY_RATE, WHY_RATE_BACK, WHY_WEATHER, WHY_RELEASE,
       WHY_INI, WHY_KINDS };
static volatile LONG g_why[WHY_KINDS];

static void LogRefused(int kind, const char *fmt, ...) {
    va_list ap;
    char line[192];

    if (InterlockedExchange(&g_why[kind], 1)) return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    Log("%s", line);
}

static void LogWorked(int kind) {
    InterlockedExchange(&g_why[kind], 0);
}

/* ---- text --------------------------------------------------------- */

static const ShText kEn[] = {
    { "@tw.page",       "Weather & Real-time Time Control" },
    { "@tw.hint",       "Set hour and minute first, then press Enter on "
                        "Apply the set time to make them take effect" },
    { "@tw.on",         "Enable" },
    { "@tw.weather",    "Weather" },
    { "@tw.hour",       "Hour (24h)" },
    { "@tw.minute",     "Minute" },
    { "@tw.apply",      "Apply the set time" },
    { "@tw.applied",    "time set" },
    { "@tw.apply.off",  "the switch is off - nothing was sent" },
    { "@tw.apply.fail", "the engine refused that change - see the plugin log" },
    { "@tw.dawn",       "Dawn speed (05:00-07:00)" },
    { "@tw.day",        "Day speed (07:00-18:00)" },
    { "@tw.dusk",       "Dusk speed (18:00-20:00)" },
    { "@tw.night",      "Night speed (20:00-05:00)" },
    { "@tw.p.dawn",     "Dawn" },
    { "@tw.p.day",      "Day" },
    { "@tw.p.dusk",     "Dusk" },
    { "@tw.p.night",    "Night" },
    { "@tw.status",     "%s  %s  rate: %.2f  weather: %s" },
    { "@tw.st.none",    "not applied" },
    { "@tw.off.ok",     "weather released, clock back to normal" },
    { "@tw.off.fail",   "the engine did not take it back - see the plugin log" },
    { "@tw.w.default",  "engine default" },
    { "@tw.w.sunny",    "sunny" },
    { "@tw.w.lclouds",  "light clouds" },
    { "@tw.w.hclouds",  "heavy clouds" },
    { "@tw.w.fog",      "fog" },
    { "@tw.w.lrain",    "light rain" },
    { "@tw.w.hrain",    "heavy rain" }
};

static const ShText kZh[] = {
    { "@tw.page",       "天气 & 时间实时控制" },
    { "@tw.hint",       "小时、分钟设置后，需回车“应用当前设置的时间”生效" },
    { "@tw.on",         "启用" },
    { "@tw.weather",    "天气" },
    { "@tw.hour",       "小时（24h）" },
    { "@tw.minute",     "分钟" },
    { "@tw.apply",      "应用当前设置的时间" },
    { "@tw.applied",    "已按设定时间跳转" },
    { "@tw.apply.off",  "开关未打开，未下发" },
    { "@tw.apply.fail", "引擎拒绝了这项改动 —— 见插件日志" },
    { "@tw.dawn",       "黎明时间流速（05:00-07:00）" },
    { "@tw.day",        "白天时间流速（07:00-18:00）" },
    { "@tw.dusk",       "黄昏时间流速（18:00-20:00）" },
    { "@tw.night",      "夜晚时间流速（20:00-05:00）" },
    { "@tw.p.dawn",     "黎明" },
    { "@tw.p.day",      "白天" },
    { "@tw.p.dusk",     "黄昏" },
    { "@tw.p.night",    "夜晚" },
    { "@tw.status",     "%s  %s  时间流速：%.2f  天气：%s" },
    { "@tw.st.none",    "未应用" },
    { "@tw.off.ok",     "天气已交还，时间流速恢复正常" },
    { "@tw.off.fail",   "引擎没有接受交还 —— 见插件日志" },
    { "@tw.w.default",  "默认动态" },
    { "@tw.w.sunny",    "晴天" },
    { "@tw.w.lclouds",  "阴云" },
    { "@tw.w.hclouds",  "多云" },
    { "@tw.w.fog",      "起雾" },
    { "@tw.w.lrain",    "下雨" },
    { "@tw.w.hrain",    "风暴" }
};

/* ---- the plugin's own name ---------------------------------------- */

/* Taken from the module path rather than written twice, and the ini, the log
 * and the text owner all follow it: a plugin folder renamed in the package
 * keeps working without a line of code changing. */
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

/* ---- settings ----------------------------------------------------- */

static void BuildChoiceTables(void) {
    int i;

    for (i = 0; i < HOURS_N; i++) {
        snprintf(kHourOpts[i], sizeof(kHourOpts[i]), "%d", i);
        kHourPtr[i] = kHourOpts[i];
    }
    for (i = 0; i < MINS_N; i++) {
        snprintf(kMinOpts[i], sizeof(kMinOpts[i]), "%d", i);
        kMinPtr[i] = kMinOpts[i];
    }
    for (i = 0; i < SPEED_N; i++) {
        /* 0.00, 0.50 .. 10.00, then 11.00, 12.00 .. 50.00. */
        float last = (float)(SPEED_FINE_N - 1) * SPEED_FINE_STEP;
        float v = (i < SPEED_FINE_N)
                      ? (float)i * SPEED_FINE_STEP
                      : last + (float)(i - SPEED_FINE_N + 1) *
                                   SPEED_COARSE_STEP;

        kSpeedVal[i] = v;
        snprintf(kSpeedOpts[i], sizeof(kSpeedOpts[i]), "%.2f", (double)v);
        kSpeedPtr[i] = kSpeedOpts[i];
    }
}

/* A speed is a decimal. GetPrivateProfileInt would read 1.00 as 1 - the trap
 * the page this replaces hit, where the clock then crawled at 0.01x while the
 * file said 1.00 - so it is read as text and converted here. A value that does
 * not parse, or sits outside the row's own range, is not a speed of zero: it is
 * a typo, and 0.00 would stop the clock for that window while the file still
 * looked right, so it is called out and the engine's own rate is used instead.
 * Anything else lands on the nearest option - a hand written 2.30 becomes 2.25,
 * which is what the row can hold. */
static LONG IniSpeedIndex(const char *key) {
    char text[32];
    char *end;
    double v;
    int i, best = 0;

    GetPrivateProfileStringA("Settings", key, "1.00", text, sizeof(text),
                             g_ini);
    v = strtod(text, &end);
    while (*end == ' ' || *end == '\t') end++;
    if (end == text || *end != 0 || v < 0.0 ||
        v > (double)kSpeedVal[SPEED_N - 1]) {
        Log("twc: %s=\"%s\" is not a speed 0.00-%.2f - using 1.00x",
            key, text, (double)kSpeedVal[SPEED_N - 1]);
        return SPEED_1X;
    }
    for (i = 1; i < SPEED_N; i++) {
        float d = kSpeedVal[i] - (float)v;
        float b = kSpeedVal[best] - (float)v;

        if (d < 0.0f) d = -d;
        if (b < 0.0f) b = -b;
        if (d < b) best = i;
    }
    return (LONG)best;
}

static void LoadSettings(void) {
    LONG on, hour, minute, weather;
    int i;

    on      = (LONG)GetPrivateProfileIntA("Settings", "on", 0, g_ini);
    hour    = (LONG)GetPrivateProfileIntA("Settings", "hour", 12, g_ini);
    minute  = (LONG)GetPrivateProfileIntA("Settings", "minute", 0, g_ini);
    weather = (LONG)GetPrivateProfileIntA("Settings", "weather", 0, g_ini);

    if (hour < 0 || hour >= HOURS_N) hour = 12;
    if (minute < 0 || minute >= MINS_N) minute = 0;
    if (weather < 0 || weather >= WEATHER_N) weather = 0;

    InterlockedExchange(&g_on, on ? 1 : 0);
    InterlockedExchange(&g_hour, hour);
    InterlockedExchange(&g_minute, minute);
    InterlockedExchange(&g_weather, weather);
    for (i = 0; i < PHASES_N; i++)
        InterlockedExchange(&g_speed[i], IniSpeedIndex(kSpeedKeys[i]));
}

static void SaveInt(const char *key, LONG value) {
    char text[24];

    snprintf(text, sizeof(text), "%ld", (long)value);
    if (WritePrivateProfileStringA("Settings", key, text, g_ini))
        LogWorked(WHY_INI);
    else
        LogRefused(WHY_INI, "twc: could not write %s=%s to %s", key, text,
                   g_ini);
}

static void SaveSpeed(const char *key, LONG index) {
    char text[24];

    if (index < 0 || index >= SPEED_N) return;
    snprintf(text, sizeof(text), "%.2f", (double)kSpeedVal[index]);
    if (WritePrivateProfileStringA("Settings", key, text, g_ini))
        LogWorked(WHY_INI);
    else
        LogRefused(WHY_INI, "twc: could not write %s=%s to %s", key, text,
                   g_ini);
}

/* ---- applying ----------------------------------------------------- */

/* Say one thing to the world. Every call is the framework's, so a refusal is
 * a real answer (not in game, no environment yet, the engine said no) and is
 * worth a line - once, until that same call works again. */
static int ApplyTime(void) {
    int ok = ShSetTime((float)g_hour + (float)g_minute / 60.0f);

    if (ok) {
        LogWorked(WHY_TIME);
    } else {
        LogRefused(WHY_TIME, "twc: set time %02d:%02d refused (%08x)",
                   (int)g_hour, (int)g_minute, ShLastError());
    }
    return ok;
}

static int PhaseOf(float hours) {
    int i;

    for (i = 0; i < PHASES_N; i++) {
        float a = kPhases[i].from, b = kPhases[i].to;

        if (a < b) {
            if (hours >= a && hours < b) return i;
        } else {
            if (hours >= a || hours < b) return i;      /* night, past 0 */
        }
    }
    return 1;                     /* in a gap, which cannot happen: day */
}

/* The rate of the window the world is in right now. Reading the hour every
 * second is what makes the four rows one behaviour instead of four switches:
 * the player sets them once, and the world crosses them on its own. */
static void ApplyRate(void) {
    float hours, speed;
    int ph = ShGetTime(&hours) ? PhaseOf(hours) : 1;
    int idx = (int)g_speed[ph];

    if (idx < 0 || idx >= SPEED_N) idx = SPEED_1X;
    speed = kSpeedVal[idx];
    if (ShSetTimeSpeed(speed)) {
        LogWorked(WHY_RATE);
    } else {
        LogRefused(WHY_RATE, "twc: rate %.2f (%s) refused (%08x)",
                   (double)speed, kPhases[ph].label, ShLastError());
    }
}

/* Weather is a state, not a stream. The id write is what arms the blend
 * (scripthook_weather.c, ShSetWeatherBlend), so sending the same weather every
 * second would restart its ten second transition every second; what is sent
 * here is the difference between the engine's own type and the row. A session
 * load puts that type back, which is what brings the request back - the same
 * reason the rate is written every second. Index 0 is "engine default":
 * nothing is written for it, and the release that undoes an earlier blend
 * happens where the row is picked instead. */
static void ApplyWeather(int weather) {
    int have = -1;

    if (weather <= 0 || weather >= WEATHER_N) return;
    if (ShGetWeather(&have) && have == weather - 1) return;   /* already it */
    if (ShSetWeatherBlend(weather - 1, 10.0f)) {
        LogWorked(WHY_WEATHER);
    } else {
        LogRefused(WHY_WEATHER, "twc: set weather %s refused (%08x)",
                   kWeatherOpts[weather], ShLastError());
    }
}

/* Everything the settings ask for except the time of day, for the moment the
 * switch goes on: a switch the player just flipped should take effect at once.
 * The time is deliberately not part of it - the clock moves only when Enter is
 * pressed on the action row that says so, never as a side effect of another
 * row, a load, or the tick. */
static void ApplySwitchOn(void) {
    ApplyRate();
    if (g_weather <= 0) {
        if (ShReleaseWeather()) {
            LogWorked(WHY_RELEASE);
        } else {
            LogRefused(WHY_RELEASE, "twc: release weather refused (%08x)",
                       ShLastError());
        }
    } else {
        ApplyWeather((int)g_weather);
    }
}

/* Give the world back: the weather blend is released and the clock rate goes
 * back to the engine's own, whatever the rows still say. The chosen time of
 * day is deliberately left alone - there is no "engine's own time" to
 * restore, and jumping the clock on the way out would be worse than leaving
 * it. Reached from the switch going off and from the blacklist callback. */
static int HandBack(void) {
    int ok = 1;

    if (ShReleaseWeather()) {
        LogWorked(WHY_RELEASE);
    } else {
        LogRefused(WHY_RELEASE, "twc: release weather refused (%08x)",
                   ShLastError());
        ok = 0;
    }
    if (ShSetTimeSpeed(1.0f)) {
        LogWorked(WHY_RATE_BACK);
    } else {
        LogRefused(WHY_RATE_BACK, "twc: restore clock rate refused (%08x)",
                   ShLastError());
        ok = 0;
    }
    if (ok) Log("twc: world handed back");
    return ok;
}

/* Blocked by the mode blacklist (nothing is declared here, so the framework's
 * own default - Ghost War and Mercenaries - is what this is about): hand the
 * world back, once, rather than leave a forced weather and rate behind in a
 * session this plugin is not allowed to touch. The callback only fires when
 * the answer flips, and the two calls it makes are direct writes with no game
 * thread queue, so the thread it arrives on does not matter. */
static void OnBlocked(int allowed, int blocked, void *user) {
    (void)user;

    if (allowed) {
        /* Back in an allowed mode: the loop re-applies on its next second
         * while the switch is still on - nothing to do here. */
        Log("twc: allowed again (was blocked by %d)", blocked);
        return;
    }
    Log("twc: blocked by %d - handing the world back", blocked);
    HandBack();
}

/* ---- the page ----------------------------------------------------- */

/* One callback for every row: each stores its pick, writes it to the ini, and
 * applies it if the switch is on. `user` carries which row it is, the way the
 * rest of the tree passes a small tag through a menu callback. */
enum { ROW_ON = 1, ROW_WEATHER, ROW_HOUR, ROW_MINUTE, ROW_APPLY, ROW_SPEED };

static void OnRow(uint32_t menu, uint32_t item, int value, void *user) {
    int which = (int)(intptr_t)user;

    (void)menu; (void)item;

    switch (which) {
    case ROW_ON:
        InterlockedExchange(&g_on, value ? 1 : 0);
        SaveInt("on", value ? 1 : 0);
        if (value) {
            ApplySwitchOn();
        } else {
            /* The lines the action row that used to sit here wrote: with it
             * gone, this is where a failure to take the world back is said. */
            ShMenuStatus(g_menu, HandBack() ? "@tw.off.ok" : "@tw.off.fail");
        }
        break;

    case ROW_WEATHER:
        if (value < 0 || value >= WEATHER_N) break;
        InterlockedExchange(&g_weather, value);
        SaveInt("weather", value);
        if (!g_on) break;
        if (value == 0) {
            /* "Engine default" picked while the switch is on: whatever was
             * set has to go back to the engine. */
            if (ShReleaseWeather()) {
                LogWorked(WHY_RELEASE);
            } else {
                LogRefused(WHY_RELEASE, "twc: release weather refused (%08x)",
                           ShLastError());
            }
        } else {
            ApplyWeather(value);
        }
        break;

    case ROW_HOUR:
    case ROW_MINUTE:
        /* Remembered, not applied: the action row under them is what sends
         * the pair to the engine, which is what its own label says. */
        if (which == ROW_HOUR) {
            if (value < 0 || value >= HOURS_N) break;
            InterlockedExchange(&g_hour, value);
            SaveInt("hour", value);
        } else {
            if (value < 0 || value >= MINS_N) break;
            InterlockedExchange(&g_minute, value);
            SaveInt("minute", value);
        }
        break;

    case ROW_APPLY:
        if (!g_on) {
            ShMenuStatus(g_menu, "@tw.apply.off");
            break;
        }
        ShMenuStatus(g_menu, ApplyTime() ? "@tw.applied" : "@tw.apply.fail");
        break;

    default: {
        int ph = which - ROW_SPEED;

        if (ph < 0 || ph >= PHASES_N) break;
        if (value < 0 || value >= SPEED_N) break;
        InterlockedExchange(&g_speed[ph], value);
        SaveSpeed(kSpeedKeys[ph], value);
        if (g_on) ApplyRate();
        break;
    }
    }
}

static void BuildMenu(void) {
    int i;

    ShMenuHint(g_menu, "@tw.hint");
    ShMenuToggle(g_menu, "@tw.on", (int)g_on, OnRow, (void *)(intptr_t)ROW_ON);
    ShMenuList(g_menu, "@tw.weather", kWeatherOpts, WEATHER_N, (int)g_weather,
               OnRow, (void *)(intptr_t)ROW_WEATHER);
    ShMenuList(g_menu, "@tw.hour", kHourPtr, HOURS_N, (int)g_hour, OnRow,
               (void *)(intptr_t)ROW_HOUR);
    ShMenuList(g_menu, "@tw.minute", kMinPtr, MINS_N, (int)g_minute, OnRow,
               (void *)(intptr_t)ROW_MINUTE);
    ShMenuAction(g_menu, "@tw.apply", OnRow, (void *)(intptr_t)ROW_APPLY);
    for (i = 0; i < PHASES_N; i++)
        ShMenuList(g_menu, kPhases[i].label, kSpeedPtr, SPEED_N,
                   (int)g_speed[i], OnRow,
                   (void *)(intptr_t)(ROW_SPEED + i));
    ShMenuStatus(g_menu, "@tw.st.none");
}

/* The status line names what the ENGINE is doing: the rate and the weather are
 * read back from it rather than from the rows, so a row and the world drifting
 * apart shows up here. Only while the page is the one on screen - the framework
 * shows the last line written when a page comes up, so computing it while
 * nobody can see it is wasted work. */
static void RefreshStatus(void) {
    char time[16];
    float hours = 0.0f, speed = 0.0f;
    int weather = -1, ph = 1, mins;
    const char *label;

    if (!ShMenuIsShowing(g_menu)) return;
    if (!g_on) {
        ShMenuStatus(g_menu, "@tw.st.none");
        return;
    }

    if (ShGetTime(&hours)) {
        ph = PhaseOf(hours);
        /* Rounded to the minute: truncating shows 10:00 for a world at
         * 10:00:59, and this line is read against the engine's own clock. */
        mins = (int)(hours * 60.0f + 0.5f);
        snprintf(time, sizeof(time), "%02d:%02d", (mins / 60) % 24, mins % 60);
    } else {
        snprintf(time, sizeof(time), "--:--");
    }

    if (!ShGetTimeSpeed(&speed))
        speed = kSpeedVal[g_speed[ph]];

    if (!ShGetWeather(&weather) || weather < 0 || weather >= 6)
        label = ShLangText(g_name, "@tw.w.default");
    else
        label = ShLangText(g_name, kWeatherOpts[weather + 1]);

    ShMenuStatusF(g_menu, "@tw.status", time,
                  ShLangText(g_name, kPhases[ph].name), (double)speed, label);
}

/* ---- start up ----------------------------------------------------- */

static DWORD WINAPI PluginThread(LPVOID param) {
    char logFile[80];

    /* The name first: the log file is named after it, and the framework's
     * DLL is not up yet when this thread starts. */
    NameFromModule((HINSTANCE)param);
    if (!g_name[0]) strcpy(g_name, "TimeWeatherControl");

    snprintf(logFile, sizeof(logFile), "%s.log", g_name);
    LogInitAlways(logFile);

    /* The framework's DLL first: the loader starts plugins from a thread so
     * that this wait is allowed, and every Sh* call below needs it. */
    while (!GetModuleHandleA("dinput8.dll")) Sleep(500);

    BuildChoiceTables();

    if (!ShPluginIniPath(g_name, g_ini, sizeof(g_ini)))
        g_ini[0] = 0;
    LoadSettings();

    /* Nothing is declared to the mode blacklist: the framework's default -
     * blocked in Ghost War and Mercenaries - is the right answer for a plugin
     * that changes the world for everyone in the session. What is added is
     * the hand back when that default blocks this plugin - see OnBlocked. */

    ShLangDeclare(g_name, "en-US", kEn, (int)(sizeof(kEn) / sizeof(kEn[0])));
    ShLangDeclare(g_name, "zh-CN", kZh, (int)(sizeof(kZh) / sizeof(kZh[0])));

    g_menu = ShMenuCreate("@tw.page");
    if (!g_menu) {
        Log("twc: no menu page (%08x) - the plugin does nothing",
            ShLastError());
        return 0;
    }
    BuildMenu();
    if (!ShPluginOnBlocked(OnBlocked, NULL))
        Log("twc: could not subscribe to the blacklist (%08x) - a blocked "
            "session would keep the forced weather and rate", ShLastError());
    Log("twc: page up, switch is %s", g_on ? "on" : "off");

    {
        const DWORD settle = 3000;   /* ms: see the readiness note below */
        int inWorld = 0;
        int applied = 0;
        DWORD readyAt = 0;

        for (;;) {
            int in, ready;
            float hours;

            Sleep(1000);
            /* Only in the world: the engine resets the clock and the weather
             * while a session loads, so writing during one is work thrown
             * away - and that is true of the first seconds after the game
             * state turns, which is exactly where the wish in the ini used to
             * be lost. */
            in = ShGetGameState() == SH_STATE_INGAME ||
                 ShGetGameState() == SH_STATE_PAUSED;
            /* The world answering for its own clock is what says the session
             * has settled enough to write into; a few seconds more on top,
             * because the engine is still putting the world up. */
            ready = in && ShGetTime(&hours);

            if (!in) {
                inWorld = 0;
                applied = 0;
                readyAt = 0;
            } else {
                if (!inWorld) { inWorld = 1; readyAt = 0; }
                if (!applied && ready && !readyAt) readyAt = GetTickCount();
                if (!applied && readyAt &&
                    (DWORD)(GetTickCount() - readyAt) >= settle) {
                    /* A world entered with the switch already saved on: the
                     * menu callback only runs when a row moves, so this is
                     * the one place the wish in the ini can be taken up. The
                     * world is handed back first - the blend record a
                     * previous session armed is not one this session should
                     * write through - and the settings are then applied, the
                     * same sequence a manual off and on walks. */
                    if (g_on) {
                        HandBack();
                        ApplySwitchOn();
                        Log("twc: applied after the world settled");
                    }
                    applied = 1;
                }
            }

            if (!g_on || !in) continue;
            /* Blocked: nothing is written, and the world was already handed
             * back by the callback when the answer flipped. */
            if (!ShPluginAllowed()) continue;
            /* The rate and the weather, never the time: the world's hour moves
             * only when Enter is pressed on the action row that says so. See
             * the header. */
            ApplyRate();
            ApplyWeather((int)g_weather);
            RefreshStatus();
        }
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    HANDLE h;

    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    DisableThreadLibraryCalls(inst);
    h = CreateThread(NULL, 0, PluginThread, (LPVOID)inst, 0, NULL);
    if (h) CloseHandle(h);
    return TRUE;
}
