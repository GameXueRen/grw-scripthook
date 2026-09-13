/* Plugin mode blacklist: a plugin says which conditions it must not run
 * in, the framework hides it from the menu and tells it when that happens.
 *
 * The rule, by the owner:
 *
 *   ShPluginBlacklist(SH_MODE_BLACKLIST_GHOST_WAR | ...)
 *                          blocked in exactly those;
 *   ShPluginBlacklist(SH_MODE_BLACKLIST_NONE)   (the value 0)
 *                          blocked nowhere - declaring "none" is the
 *                          only way to be unrestricted everywhere;
 *   never declared         blocked in Ghost War and Mercenaries. That is
 *                          every third party plugin by construction, and
 *                          every plugin of ours that has not been taught
 *                          otherwise; the built-in pages are not plugins
 *                          (their menu owner is "") and are never touched.
 *
 * A condition is one bit, and the bits are the same kind of value a plugin
 * declares, so "in force" and "declared" meet in one AND. There is one
 * source of conditions, the play mode:
 *
 *   Ghost War / Mercenaries / campaign / Ghost Mode / Guerrilla
 *                          from ShSelectedPlayMode(), the play mode
 *                          identification that hooks the game's mode
 *                          manager.
 *
 * The front end was a condition of its own once, read as "the main menu,
 * before anything has been chosen" from the game state plus the GameFlow
 * machine. It was dropped on 2026-09-13, after playing it: the GameFlow
 * reading latches - once a mode screen has been opened, its mode-screen
 * object (slot 16) never empties again, so a session that was correctly
 * the main menu at startup stopped being recognised as one after a round
 * trip through a campaign, and the plugins came back on in the menu. A
 * condition that is right only some of the time is worse than none, and
 * there is no second reading to fall back on: ShGetGameState puts the main
 * menu and every lobby in one bucket ("MenuOrLobby", the framework's own
 * name for it). Modes it is.
 *
 * A condition that cannot be read contributes nothing: a mode the manager
 * has not set (SH_PLAYMODE_NONE) leaves the mask alone. The mechanism is
 * cooperative, and a menu that vanishes with no condition to blame would
 * be worse than one that stays.
 *
 * What "blocked" does is two things, and no more:
 *
 *   1. the plugin's row in the F4 root menu is not drawn, cannot be
 *      selected and cannot be entered (scripthook_menu.c asks
 *      ShPluginHidden() for every row);
 *   2. the plugin is told, once per change, through ShPluginOnBlocked,
 *      and can ask any time with ShPluginAllowed.
 *
 * It cannot stop a plugin's code. Plugins run their own threads and carry
 * their own MinHook copy, and the framework has no registry that could
 * suspend them: what a plugin does about being blocked is up to the
 * plugin, and a plugin that ignores the callback keeps running. That
 * boundary is stated here, in scripthook.h and in docs/plugin-blacklist.md
 * rather than papered over.
 *
 * The caller is identified the way the menu layer already did it before
 * this module existed: the return address of an exported call sits in the
 * caller's own .asi, so GetModuleHandleExA(FROM_ADDRESS) names the plugin
 * folder. Framework code, the exe, and a plugin calling in through a
 * framework callback all resolve to "not a plugin", which is allowed.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "log.h"

/* The framework's single error channel lives in scripthook_api.c, and the
 * modules that use it declare it themselves - the same line
 * scripthook_blur.c, scripthook_ammo.c and the rest carry. */
extern void ShSetError(int err);

#ifdef _MSC_VER
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define SH_CALLER_ADDR() _ReturnAddress()
#else
#define SH_CALLER_ADDR() __builtin_return_address(0)
#endif

#define ENTRIES   64
#define NAME_CAP  48
#define TICK_MS   250

/* What a plugin that never declared anything is blocked in: the two PvP
 * modes, and nothing else. */
#define DEFAULT_BITS (SH_MODE_BLACKLIST_GHOST_WAR | \
                      SH_MODE_BLACKLIST_MERCENARIES)

typedef struct {
    char              name[NAME_CAP];   /* the plugin's folder name */
    uint32_t          bits;             /* declared; see declared */
    int               declared;         /* 1 once it called us */
    uint32_t          blocked;          /* what was announced last */
    ShPluginBlockedFn fn;
    void             *user;
} Entry;

static Entry            g_e[ENTRIES];
static int              g_n;
static volatile LONG    g_started;
static volatile LONG    g_lockState;
static CRITICAL_SECTION g_lock;

static uint32_t ActiveBits(void);

/* ---- the lock ---------------------------------------------------------
 * Initialised on first use, the way the other modules do it: a second
 * caller that arrives during the initialisation yields instead of
 * spinning on a lock that is not there yet. */
static void Lock(void) {
    LONG s;

    for (;;) {
        s = InterlockedCompareExchange(&g_lockState, 0, 0);
        if (s == 1) { EnterCriticalSection(&g_lock); return; }
        if (s == 2) { Sleep(0); continue; }
        if (InterlockedCompareExchange(&g_lockState, 2, 0)) continue;
        InitializeCriticalSection(&g_lock);
        InterlockedExchange(&g_lockState, 1);
    }
}

static void Unlock(void) {
    LeaveCriticalSection(&g_lock);
}

/* ---- the registry (caller holds the lock) ---------------------------- */

static Entry *FindLocked(const char *name) {
    int i;

    for (i = 0; i < g_n; i++)
        if (!_stricmp(g_e[i].name, name)) return &g_e[i];
    return NULL;
}

static Entry *AddLocked(const char *name) {
    Entry *e;

    if (!name || !name[0] || g_n >= ENTRIES) return NULL;
    e = FindLocked(name);
    if (e) return e;
    e = &g_e[g_n++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, NAME_CAP - 1);
    return e;
}

/* What actually blocks this entry: what it declared, or the default. */
static uint32_t Effective(const Entry *e) {
    return e->declared ? e->bits : DEFAULT_BITS;
}

/* The bits blocking a plugin by name, whatever the registry holds. A name
 * it does not hold is an undeclared plugin, which is exactly the case the
 * default exists for. An empty name is framework code: never blocked. */
static uint32_t BlockedForName(const char *name) {
    uint32_t bits, mine = DEFAULT_BITS;
    int i;

    if (!name || !name[0]) return 0;
    bits = ActiveBits();
    if (!bits) return 0;
    Lock();
    for (i = 0; i < g_n; i++)
        if (!_stricmp(g_e[i].name, name)) { mine = Effective(&g_e[i]); break; }
    Unlock();
    return mine & bits;
}

/* ---- the conditions -------------------------------------------------- */

/* Everything switching plugins off right now: the play mode, and nothing
 * else. SH_PLAYMODE_NONE - the manager has not set a mode yet - is the
 * absence of a condition, not one. The how and why of the front end not
 * being one is at the top of this file. */
static uint32_t ActiveBits(void) {
    int mode = ShSelectedPlayMode();

    return mode == SH_PLAYMODE_NONE ? 0u : ShPlayModeBit(mode);
}

/* ---- the caller ------------------------------------------------------ */

void ShPluginOwnerFromAddress(void *address, char *out, int cap) {
    HMODULE m = NULL;
    char path[MAX_PATH];
    const char *name;
    size_t n;

    if (!out || cap < 1) return;
    out[0] = 0;
    if (!address) return;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)address, &m) || !m)
        return;
    path[0] = 0;
    GetModuleFileNameA(m, path, sizeof(path));
    name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    if (!_stricmp(name, "dinput8.dll") || !_stricmp(name, "GRW.exe"))
        return;                       /* built-in, not a plugin */
    n = strlen(name);
    if (n > 4 && !_stricmp(name + n - 4, ".asi")) n -= 4;
    else if (n > 4 && !_stricmp(name + n - 4, ".dll")) n -= 4;
    if (n >= (size_t)cap) n = (size_t)cap - 1;
    memcpy(out, name, n);
    out[n] = 0;
}

/* ---- naming a bit ---------------------------------------------------- */

SH_API const char *ShBlacklistName(uint32_t bit) {
    int m;

    for (m = SH_PLAYMODE_GHOST_WAR; m <= SH_PLAYMODE_GUERRILLA; m++)
        if (bit == ShPlayModeBit(m)) return ShPlayModeName(m);
    return "";
}

SH_API int ShBlockedText(uint32_t bits, char *buf, int cap) {
    int m, first = 1;

    if (!buf || cap < 1) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    buf[0] = 0;
    for (m = SH_PLAYMODE_GHOST_WAR; m <= SH_PLAYMODE_GUERRILLA; m++) {
        const char *n;
        size_t used, len;

        if (!(bits & ShPlayModeBit(m))) continue;
        n = ShPlayModeName(m);
        used = strlen(buf);
        len = strlen(n);
        if (used + len + 2 >= (size_t)cap) break;
        snprintf(buf + used, (size_t)cap - used, "%s%s", first ? "" : "+", n);
        first = 0;
    }
    return buf[0] ? 1 : 0;
}

/* The same, translated and comma separated, for something a player reads. */
static void ConditionsText(uint32_t bits, char *out, int cap) {
    int m, first = 1;

    out[0] = 0;
    for (m = SH_PLAYMODE_GHOST_WAR; m <= SH_PLAYMODE_GUERRILLA; m++) {
        const char *n;
        size_t used, len;

        if (!(bits & ShPlayModeBit(m))) continue;
        n = ShLang(ShPlayModeName(m));
        used = strlen(out);
        len = strlen(n);
        if (used + len + 3 >= (size_t)cap) break;
        snprintf(out + used, (size_t)cap - used, "%s%s", first ? "" : ", ", n);
        first = 0;
    }
}

/* ---- the exports ----------------------------------------------------- */

SH_API int ShPluginBlacklist(uint32_t modes) {
    char me[NAME_CAP], text[128];
    Entry *e;

    ShPluginOwnerFromAddress(SH_CALLER_ADDR(), me, sizeof(me));
    if (!me[0]) {
        ShSetError(SH_ERR_BAD_ARG);   /* only a plugin can declare one */
        return 0;
    }
    Lock();
    e = AddLocked(me);
    if (!e) {
        Unlock();
        ShSetError(SH_ERR_REGISTRY_FULL);
        return 0;
    }
    e->bits = modes;
    e->declared = 1;
    Unlock();

    ShBlockedText(modes, text, (int)sizeof(text));
    Log("blacklist: %s declared %s", me, modes
        ? text
        : "no restriction - it runs in every mode");
    return 1;
}

SH_API uint32_t ShPluginBlacklistModes(void) {
    char me[NAME_CAP];
    uint32_t bits = 0;
    int i;

    ShPluginOwnerFromAddress(SH_CALLER_ADDR(), me, sizeof(me));
    if (!me[0]) return 0;
    Lock();
    for (i = 0; i < g_n; i++)
        if (!_stricmp(g_e[i].name, me)) { bits = Effective(&g_e[i]); break; }
    if (i >= g_n) bits = DEFAULT_BITS;   /* undeclared: the default */
    Unlock();
    return bits;
}

SH_API int ShPluginAllowed(void) {
    char me[NAME_CAP];

    ShPluginOwnerFromAddress(SH_CALLER_ADDR(), me, sizeof(me));
    return BlockedForName(me) ? 0 : 1;
}

SH_API int ShPluginBlockedBy(void) {
    char me[NAME_CAP];

    ShPluginOwnerFromAddress(SH_CALLER_ADDR(), me, sizeof(me));
    return (int)BlockedForName(me);
}

SH_API int ShPluginOnBlocked(ShPluginBlockedFn fn, void *user) {
    char me[NAME_CAP];
    Entry *e;

    ShPluginOwnerFromAddress(SH_CALLER_ADDR(), me, sizeof(me));
    if (!me[0]) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    Lock();
    e = AddLocked(me);
    if (!e) {
        Unlock();
        ShSetError(SH_ERR_REGISTRY_FULL);
        return 0;
    }
    e->fn = fn;
    e->user = user;
    Unlock();
    return 1;
}

SH_API int ShPluginBlacklistCount(void) {
    int n;

    Lock();
    n = g_n;
    Unlock();
    return n;
}

SH_API int ShPluginBlacklistAt(int i, char *name, int cap, uint32_t *modes,
                               int *blockedBy) {
    uint32_t bits = ActiveBits();
    Entry *e;

    if (i < 0) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    Lock();
    if (i >= g_n) {
        Unlock();
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    e = &g_e[i];
    if (name && cap > 0) {
        strncpy(name, e->name, (size_t)cap - 1);
        name[cap - 1] = 0;
    }
    if (modes) *modes = Effective(e);
    if (blockedBy) *blockedBy = (int)(Effective(e) & bits);
    Unlock();
    return 1;
}

/* A line for the Mod settings page: who is switched off by what is in
 * force, and by which conditions. Localised here, so the page only has to
 * place the text; empty when there is nothing to say. */
SH_API int ShPluginBlacklistNotice(char *buf, int cap) {
    char names[44], why[64];
    size_t used = 0;
    uint32_t bits;
    int i, listed = 0, more = 0;

    if (!buf || cap < 1) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    buf[0] = 0;
    bits = ActiveBits();
    if (!bits) return 0;                          /* nothing to blame */
    names[0] = 0;
    Lock();
    for (i = 0; i < g_n; i++) {
        size_t l;

        if (!(Effective(&g_e[i]) & bits)) continue;
        l = strlen(g_e[i].name);
        if (listed >= 3 || used + l + 3 >= sizeof(names)) { more++; continue; }
        if (used) { names[used++] = ','; names[used++] = ' '; }
        memcpy(names + used, g_e[i].name, l);
        used += l;
        names[used] = 0;
        listed++;
    }
    Unlock();
    if (!listed && !more) return 0;
    if (more)
        snprintf(names + strlen(names), sizeof(names) - strlen(names),
                 " (+%d)", more);
    ConditionsText(bits, why, (int)sizeof(why));
    /* Kept short on purpose: the Plugins page has one hint line, and the
     * "changes need a restart" note shares it. */
    snprintf(buf, (size_t)cap, "%s (%s): %s", ShLang("Off now"), why, names);
    return 1;
}

int ShPluginHidden(const char *owner) {
    /* "" is a built-in page: never hidden. Anything else is a plugin,
     * declared or not - and "not declared" is the default above. */
    return BlockedForName(owner) != 0;
}

/* ---- the watcher -----------------------------------------------------
 * Nothing has to run for the menu or for a query: both compute from the
 * conditions on the spot. This thread exists for the two things that need a
 * moment in time - the change log and the plugin callbacks - and it fires a
 * callback only when the answer actually flipped. Callbacks are handed out
 * after the lock is dropped, so a plugin that calls back into the framework
 * from its own callback cannot deadlock against us. */
typedef struct {
    ShPluginBlockedFn fn;
    void             *user;
    char              name[NAME_CAP];
    uint32_t          blocked;      /* the bits, 0 = allowed now */
} Fired;

static DWORD WINAPI WatchThread(LPVOID p) {
    Fired fire[ENTRIES];
    uint32_t lastBits = 0xFFFFFFFFu;    /* nothing announced yet */
    int first = 1;

    (void)p;
    for (;;) {
        uint32_t bits;
        char text[128];
        int i, n = 0, conditionsChanged = 0;

        Sleep(TICK_MS);
        bits = ActiveBits();
        Lock();
        if (bits != lastBits) {
            lastBits = bits;
            conditionsChanged = 1;
        }
        for (i = 0; i < g_n; i++) {
            Entry *e = &g_e[i];
            uint32_t blocked = Effective(e) & bits;

            if (blocked == e->blocked) continue;
            e->blocked = blocked;
            fire[n].fn = e->fn;
            fire[n].user = e->user;
            fire[n].blocked = blocked;
            strncpy(fire[n].name, e->name, NAME_CAP - 1);
            fire[n].name[NAME_CAP - 1] = 0;
            n++;
        }
        Unlock();

        if (conditionsChanged) {
            /* Logged on its own, with the raw evidence, so a change in the
             * conditions is visible even when it blocks nobody: the game
             * state comes along, because "the mode is set but the state is
             * still the front end" and "no mode yet" are the two readings
             * the modes cannot spell out. */
            char state[32] = "";
            const char *why;

            ShGetGameStateName(state, (int)sizeof(state));
            ShBlockedText(bits, text, (int)sizeof(text));
            why = bits ? text : "nothing (everything is allowed)";
            Log("blacklist: in force now: %s (state %s)", why,
                state[0] ? state : "?");
            if (first) {
                first = 0;
                Log("blacklist: %d plugin(s) seen; one that does not declare "
                    "is blocked in Ghost War and Mercenaries", g_n);
            }
        }

        for (i = 0; i < n; i++) {
            ShBlockedText(fire[i].blocked, text, (int)sizeof(text));
            if (fire[i].blocked)
                Log("blacklist: %s is off now (%s)", fire[i].name, text);
            else
                Log("blacklist: %s is back on", fire[i].name);
            if (fire[i].fn)
                fire[i].fn(fire[i].blocked ? 0 : 1, (int)fire[i].blocked,
                           fire[i].user);
        }
    }
    return 0;
}

/* Called by the loader after the play mode module and before the plugins
 * load, so a plugin's declaration has somewhere to go. */
void ShBlacklistStartup(void) {
    char dir[MAX_PATH], pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0;

    if (InterlockedExchange(&g_started, 1)) return;

    /* The log first: Log drops everything written before LogInit, and the
     * scan line below is the one worth keeping. */
    LogInit("scripthook_blacklist.log");

    Lock();
    if (ShPluginsDir(dir, sizeof(dir))) {
        size_t l = strlen(dir);

        while (l > 0 && dir[l - 1] == '\\') dir[--l] = 0;
        snprintf(pattern, sizeof(pattern), "%s\\*", dir);
        h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char asi[MAX_PATH];

                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    continue;
                if (fd.cFileName[0] == '.') continue;
                snprintf(asi, sizeof(asi), "%s\\%s\\%s.asi", dir,
                         fd.cFileName, fd.cFileName);
                if (GetFileAttributesA(asi) == INVALID_FILE_ATTRIBUTES)
                    continue;
                if (!AddLocked(fd.cFileName)) break;
                n++;
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    Unlock();

    Log("blacklist: registry up with %d plugin(s) from plugins\\", n);
    if (!CreateThread(NULL, 0, WatchThread, NULL, 0, NULL))
        Log("blacklist: watcher thread failed to start");
}
