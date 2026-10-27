/* Who was busy when a frame was late. Internal - no SH_API, no plugin has
 * any business here. The overlay's Present hook owns the question (see
 * HookPresent in scripthook_ovl.cpp) and the framework's own threads, plus
 * the file interception layer, answer it.
 *
 * A thread pings once per loop iteration, from the one point every path of
 * that loop goes through: right after its Sleep where the Sleep is first
 * in the body, and the first statement of the body where the Sleep is
 * last. A ping later than the loop's own period means the thread did not
 * come around on time - it was blocked, or its work ran long - which is
 * what a frame stall has to be traced to. */
#ifndef SCRIPTHOOK_TICK_H
#define SCRIPTHOOK_TICK_H

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ShTickSlot {
    SH_TICK_STATE = 0,      /* scripthook_state.c      */
    SH_TICK_PLAYMODE,       /* scripthook_playmode.c   */
    SH_TICK_BLACKLIST,      /* scripthook_blacklist.c  */
    SH_TICK_COREFIX,        /* scripthook_corefix.c    */
    SH_TICK_HUD,            /* scripthook_hud.c        */
    SH_TICK_MENU,           /* scripthook_menu.c       */
    SH_TICK_MENUCALL,       /* scripthook_menu.c       */
    SH_TICK_HITPUMP,        /* scripthook_hit.c        */
    SH_TICK_CPULINE,        /* scripthook_modsettings.c*/
    SH_TICK_MAX
};

/* A monotonic microsecond clock. ShTickNow is the raw stamp; a caller that
 * wants to time its own work takes one, and asks how long ago it was.
 * Both are QPC, which is a few tens of nanoseconds. */
uint64_t ShTickNow(void);
int      ShTickUsSince(uint64_t then);

/* Once per loop iteration, after the Sleep. */
void ShTickPing(int slot);

/* One line naming the threads that were late, as "name age/lap ms (pN)"
 * with the period they should have kept - or "none". Always writes
 * something, so a caller can append it to a line of its own. */
int ShTickReport(char *buf, int n, DWORD now);

/* The rule-weighing pass of the interception layer, timed from a stamp
 * taken before it. Fed by scripthook_files.c; drained once a frame by the
 * Present hook, so the number it prints belongs to that window and not to
 * the session. */
void ShDecideFeed(uint64_t at);
int  ShDecideTake(void);

/* ---- per-entry API counters --------------------------------------------
 *
 * Opt-in ([loader] api_stats=1) and off by default, because the point of
 * them is to prove a change lowered a cost that is otherwise argued about
 * from the code. A call site pays one predictable branch when off; when on
 * it pays two QPC reads and three interlocked adds, and the numbers land on
 * the same 5s heartbeat the leak probe uses.
 *
 * `t0` is a stamp taken before the work, so the entry reports what the
 * caller waited for and not just what the function claims to cost.
 * ShApiDrain resets the window, so the caller passes the ms it covered and
 * the line is a rate, not a running total.
 */
enum ShApiSlot {
    SH_API_STATE = 0,     /* ShGetGameState                        */
    SH_API_PLAYER,        /* ShGetPlayer (public, non-scanning)    */
    SH_API_PLAYER_SCAN,   /* the internal scanning variant         */
    SH_API_WSET,          /* ShWidgetSetF / SetU / SetV            */
    SH_API_MENU_STATUS,   /* ShMenuStatusF                         */
    SH_API_UI_GET,        /* ShUiGetS                              */
    SH_API_TOAST,         /* ShToastEx                             */
    SH_API_SPAWN,         /* ShSpawnNpc                            */
    /* Counted *in addition to* SH_API_STATE, on the branch that answers from
     * the per-frame cache. state.hit / state is the hit rate: if it is not
     * close to "one miss per frame", the cache key is not doing what it
     * claims and the state cost is being paid on every call. */
    SH_API_STATE_HIT,     /* ShGetGameState answered from the cache */
    SH_API_WSET_REJ,      /* ShWidgetSet* refused: handle not live  */
    /* The two halves of the one computation a frame. state.hit already says
     * the cache works; these say what the remaining per-frame cost is made
     * of, so the next change is aimed at the half that is actually expensive
     * instead of at the pair. */
    SH_API_STATE_HASH,    /* StateHash(CurrentState()) - a game virtual call */
    SH_API_STATE_UI,      /* ShGetUiState() - the scene walk               */
    SH_API_MAX
};

/* 0 = off. Read by every instrumented entry point. */
extern volatile LONG ShApiStatsOn;

/* Record one call that started at `t0`. No-op when the counters are off. */
void ShApiNote(int slot, uint64_t t0);

/* One line for the heartbeat: "api: state 61/s 2.1/41us, ..." - calls per
 * second, then average and worst in microseconds over `ms`. Slots with no
 * calls in the window are left out. Returns what it wrote, 0 if nothing. */
int ShApiDrain(char *buf, int n, DWORD ms);

#ifdef __cplusplus
}
#endif

#endif
