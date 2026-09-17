/* Who was busy when a frame was late. See scripthook_tick.h for the API;
 * this is the storage and the report.
 *
 * Why it exists: "the game stutters" is the one report a mod cannot answer
 * from its own logs. The Present hook writes how long the gap between two
 * presents was, which says a frame was late but not who made it late. The
 * candidates are the game (streaming, a load, its own render work) and us,
 * and the way to tell them apart is to know what our own code was doing
 * while the frame was late:
 *
 *   - the render thread's share, timed by the Present hook itself;
 *   - each background thread, which pings once per iteration - a ping
 *     later than its period means it was blocked or its work ran long;
 *   - the file interception layer, fed per pass by scripthook_files.c: it
 *     is the hottest path the framework owns, and the one place where "the
 *     engine was streaming" and "our hooks slowed the stream down" look
 *     identical from outside.
 *
 * Cost: a ping is two GetTickCount reads and two stores per iteration, the
 * slowest cadence in the framework is one per second, and the fastest (the
 * menu's 40ms) is still nothing. Nothing here takes a lock - a reader can
 * catch a half-updated pair during a hitch, which is a diagnostic, not a
 * measurement instrument.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#include "scripthook_tick.h"

/* How far past its own period a thread has to be before it is worth
 * naming. A healthy menu thread is up to one period late by definition
 * (it sleeps, then works), so the slack has to clear the period itself -
 * 30ms does, against the 40ms period at 10x the resolution of the stall
 * being explained. */
#define TICK_SLACK 30

/* 32-bit milliseconds throughout, on purpose, and wrap-safe by
 * construction: every value here comes from the same counter, and ShTickReport
 * compares them as unsigned differences (now - ping), which is exactly what
 * survives the 48.9-day rollover. Widening to GetTickCount64 would mean
 * widening this struct, the report's `now` and the Present hook that
 * supplies it, in exchange for nothing the subtraction does not already
 * give. */
typedef struct {
    const char   *name;
    DWORD         period;   /* what a healthy iteration keeps, ms */
    volatile LONG ping;     /* GetTickCount of the last ping, 0 = never */
    volatile LONG prev;     /* the ping before that one */
} Tick;

static Tick g_t[SH_TICK_MAX] = {
    { "state",     100 },
    { "playmode",  200 },
    { "blacklist", 250 },
    { "corefix",   250 },
    { "hud",       120 },
    { "menu",       40 },
    { "menucall",   10 },
    { "hitpump",    10 },
    { "cpuline",  1000 }
};

static LARGE_INTEGER g_freq;
/* Raw counts. What the layer spent is converted to nanoseconds once, in
 * ShDecideTake - not once per call, which is what this used to do. */
static volatile LONG64 g_decideTicks;

uint64_t ShTickNow(void) {
    LARGE_INTEGER c;

    if (!g_freq.QuadPart) {
        LARGE_INTEGER f;

        QueryPerformanceFrequency(&f);
        g_freq = f;             /* the same value whoever gets there first */
    }
    QueryPerformanceCounter(&c);
    return (uint64_t)c.QuadPart;
}

int ShTickUsSince(uint64_t then) {
    LARGE_INTEGER c;
    uint64_t dt;

    QueryPerformanceCounter(&c);
    if (!g_freq.QuadPart || (uint64_t)c.QuadPart < then) return 0;
    dt = (uint64_t)c.QuadPart - then;
    return (int)(dt * (uint64_t)1000000 / (uint64_t)g_freq.QuadPart);
}

void ShTickPing(int slot) {
    Tick *t;
    LONG was;

    if (slot < 0 || slot >= SH_TICK_MAX) return;
    t = &g_t[slot];
    was = InterlockedCompareExchange(&t->ping, 0, 0);
    InterlockedExchange(&t->prev, was);
    InterlockedExchange(&t->ping, (LONG)GetTickCount());
}

int ShTickReport(char *buf, int n, DWORD now) {
    int i, w, late = 0;

    if (!buf || n < 2) return 0;
    buf[0] = 0;
    w = snprintf(buf, (size_t)n, " threads:");

    for (i = 0; i < SH_TICK_MAX; i++) {
        Tick *t = &g_t[i];
        DWORD ping, prev, age, lap;

        ping = (DWORD)InterlockedCompareExchange(&t->ping, 0, 0);
        if (!ping) continue;                    /* its thread is not up */
        prev = (DWORD)InterlockedCompareExchange(&t->prev, 0, 0);
        age = now - ping;                       /* since it last came round */
        lap = ping - prev;                      /* the iteration before that */

        if (age <= t->period + TICK_SLACK &&
            lap <= t->period + TICK_SLACK)
            continue;

        if (w < 0 || w > n - 24) break;         /* no room for another */
        w += snprintf(buf + w, (size_t)(n - w), "%s %s %u/%ums(p%u)",
                      late ? "," : "", t->name,
                      (unsigned)age, (unsigned)lap, (unsigned)t->period);
        late++;
    }
    if (!late && w < n)
        snprintf(buf + w, (size_t)(n - w), " none");
    return w;
}

void ShDecideFeed(uint64_t at) {
    LARGE_INTEGER c;

    QueryPerformanceCounter(&c);
    if (!g_freq.QuadPart || (uint64_t)c.QuadPart < at) return;
    /* Counts, and no division: this runs on the hottest path the framework
     * owns (tens of thousands of calls a second - see scripthook_files.c),
     * and turning each measurement into nanoseconds there cost as much as
     * the sample it measured. The unit only has to be consistent, and
     * ShDecideTake converts the whole window at once, once a frame, where
     * the number is actually read. */
    InterlockedExchangeAdd64(&g_decideTicks,
                             (LONG64)((uint64_t)c.QuadPart - at));
}

int ShDecideTake(void) {
    LONG64 ticks = InterlockedExchangeAdd64(&g_decideTicks, 0);

    if (ticks <= 0) return 0;
    /* Only what was just read is taken back, so a pass that lands in
     * between is still counted - in the next window. */
    InterlockedExchangeAdd64(&g_decideTicks, -ticks);
    if (!g_freq.QuadPart) return 0;
    /* Nanoseconds: one pass through the rules is well under a microsecond,
     * so a microsecond accumulator would round the whole layer to zero. */
    return (int)((uint64_t)ticks * 1000000000ull /
                 (uint64_t)g_freq.QuadPart / 1000ull);
}
