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

#ifdef __cplusplus
}
#endif

#endif
