/* Where the mode selection turns into a mode load.
 *
 * The idea this probe tests: the main menu has one item per mode, so
 * clicking one has to end in a call that names the mode - a loader, a
 * session or a request with the choice as an argument. If that call can
 * be found, the mode reads off it directly and the whole "watch where
 * the mouse landed" approach becomes unnecessary.
 *
 * There is a way in. The engine's own objects carry a method table of
 * 32 byte entries - crc32 of the method name, the index, the function -
 * and the framework already reaches it (ShReflectMethods dumps one,
 * ShReflectMethod finds one by name, ShReflectCall calls one). Measured
 * against the five name hashes scripthook.h carries, the hash is plain
 * CRC-32: polynomial 0xEDB88320 reflected, init and final xor
 * 0xFFFFFFFF, ASCII, case sensitive. Four of the five match a bare
 * name exactly - Enter 0x78B1EF6A, Exit 0x343B2B30, Init 0x66464B4A,
 * Shutdown 0x6CD4BC94 - which is what makes the rest of this possible:
 * a method can be looked up by name without touching a disassembler.
 *
 * So this probe dumps the method tables of the front end - the GameFlow
 * machine, its seventeen sub objects and the HybridMenu shell every
 * menu page lives in - and then runs a dictionary over them: a few
 * thousand plausible method names, hashed and looked up. Whatever comes
 * back named is the vocabulary the engine uses around mode switching,
 * and a hit like SetMode or StartSession is the function to hook next.
 *
 * What is written to logs\ModeCallProbe.log, per object:
 *
 *   [OBJ]  slot, address, class hash, method count. The class hash is
 *          the same number ModeProbe prints per sub object, so the two
 *          logs can be lined up: this one names the class, that one
 *          says what the class does.
 *   [M]    every method: index, name hash, and the RVA of the function.
 *          Written whether or not a name was found, so an unnamed entry
 *          is still something that can be hooked by hash later.
 *   [NAME] the dictionary hits, and [HIT] the ones whose name looks
 *          like mode, session, load, enter, online, pvp, ghost and the
 *          like - the short list worth reading first.
 *   [SUM]  how many methods, how many named.
 *
 * Nothing is called, hooked or written. It would be safe to run in a
 * live match, but it is a menu time instrument: the tables are there
 * from the front end on, so the main menu is where to press the dump
 * key (F9 by default, or F4 -> "Mode call probe" -> Dump tables).
 *
 * Read the result like this: if a name in [HIT] looks like the mode
 * choice, hook that function (MinHook, as GhostWipeProbe does) and log
 * its arguments as the item is clicked. If no name lands, the tables
 * are still the map: the [M] lines say which functions exist on which
 * object, and the next step is to hook the plausible ones by hash and
 * see which fires on the click.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "third_party/minhook/include/MinHook.h"

/* The plugin binds every framework entry point by name, so only the
 * menu callback type has to be declared the same way. */
typedef void (*ShMenuFn)(uint32_t menu, uint32_t item, int value,
                         void *user);

typedef struct {
    uint32_t nameHash;
    int      index;
    uint64_t fn;
} ShMethod;

typedef uint64_t (*GameFlow_t)(void);
typedef uint64_t (*GameFlowObject_t)(int slot);
typedef uint64_t (*HybridMenu_t)(void);
typedef int      (*ReflectMethods_t)(uint64_t obj, ShMethod *out, int max);
typedef uint32_t (*ClassHash_t)(uint64_t obj);
typedef uint32_t (*ClassHashOf_t)(uint64_t obj);
typedef int      (*GetGameState_t)(void);
typedef int      (*StateName_t)(char *buf, int len);
typedef uint32_t (*MenuCreate_t)(const char *title);
typedef int      (*MenuAction_t)(uint32_t menu, const char *label,
                                 ShMenuFn fn, void *user);
typedef int      (*MenuHint_t)(uint32_t menu, const char *text);
typedef int      (*MenuStatus_t)(uint32_t menu, const char *text);

static GetGameState_t    pGetGameState;
static StateName_t       pGetGameStateName;
static GameFlow_t        pGameFlow;
static GameFlowObject_t  pGameFlowObject;
static HybridMenu_t      pHybridMenu;
static ReflectMethods_t  pReflectMethods;
static ClassHash_t       pClassHash;
static MenuCreate_t      pMenuCreate;
static MenuAction_t      pMenuAction;
static MenuHint_t        pMenuHint;
static MenuStatus_t      pMenuStatus;

static HINSTANCE g_inst;
static uint32_t  g_menu;
static FILE     *g_log;
static volatile LONG g_logBusy;
static uint64_t  g_base;

#define SLOT_MAX   17
#define METH_MAX   256
#define DICT_MAX   24000

/* ---- logging ------------------------------------------------------- */

static void LogOpen(void) {
    char path[MAX_PATH];
    char dir[MAX_PATH];
    char *slash;

    /* The main module is GRW.exe, so its folder is the game dir and
     * logs\ lands next to the other logs. The plugin's own folder is
     * the wrong place: it made plugins\<name>\logs once. */
    if (!GetModuleFileNameA(NULL, path, MAX_PATH)) return;
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    slash = strrchr(dir, '\\');
    if (!slash) return;
    slash[1] = 0;
    if (strlen(dir) + 24 >= sizeof(dir)) return;
    strcat(dir, "logs");
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof(path), "%s\\ModeCallProbe.log", dir);
    g_log = fopen(path, "a");
}

/* One line per event, stamped with the process id: a mode switch
 * restarts the client, so one file holds several processes. */
static void PLog(const char *fmt, ...) {
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

/* ---- the name hash -------------------------------------------------
 *
 * Plain CRC-32, the zlib parameters, over the ASCII name. Verified
 * against the four bare names the framework already carries (see the
 * header); a name the engine spells differently hashes to something
 * else, which is why a miss is a miss and not a bug.
 */
static uint32_t Crc32(const char *s) {
    uint32_t c = 0xFFFFFFFFu;
    int i;

    for (; *s; s++) {
        c ^= (uint32_t)(unsigned char)*s;
        for (i = 0; i < 8; i++)
            c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}

/* Declared here because both the dictionary and the dump reach for them
 * and the hook layer itself sits further down the file. */
static char g_iniPath[MAX_PATH];
static int  NameWanted(const char *name);
static void HookAdd(void *target, const char *tag, int idx, uint32_t hash,
                    const char *name);
static void InstallHooks(void);

/* ---- the dictionary ------------------------------------------------ */

static const char *const kVerbs[] = {
    "Enter", "Exit", "Init", "Shutdown", "Update", "Tick", "Start", "Stop",
    "Begin", "End", "Load", "Unload", "Launch", "Set", "Get", "Request",
    "Select", "Confirm", "Cancel", "Activate", "Deactivate", "Open", "Close",
    "Push", "Pop", "GoTo", "Goto", "Transition", "Change", "Switch", "Apply",
    "Reset", "Create", "Destroy", "EnterState", "Leave", "Join", "Host",
    "Find", "Resolve", "Prepare", "Commit", "Finish", "Abort", "Pause",
    "Resume", "Toggle", "Show", "Hide", "OnEnter", "OnExit", "SetNext",
    "SetCurrent", "Use", "Choose", "Pick", "LaunchMode", "Play", "StopMode",
    "On", "Do", "Make", "Build", "InitMode", "SetGame", "SetPlay",
    "SetSession", "SetOnline", "SetMission", "SetActivity", "SetWorld",
    "SetLevel", "LoadLevel", "LoadWorld", "StartGame", "StopGame",
    "OnSelect", "OnChoose", "OnPick", "OnConfirm", "OnActivate",
    "OnRequest", "OnLoad", "OnLaunch", "OnStart", "OnMode", "OnSession",
    "Mode", "Session", "State", "Match", "Lobby", "Game"
};

static const char *const kNouns[] = {
    "Mode", "GameMode", "PlayMode", "Session", "State", "GameState",
    "Flow", "Mission", "Activity", "Level", "Map", "World", "Menu",
    "Page", "Screen", "Context", "Request", "Target", "Next", "Current",
    "Selection", "Ghost", "GhostWar", "Mercenaries", "Pvp", "Pve",
    "Online", "Offline", "Lobby", "Match", "Game", "Network", "Player",
    "Host", "Server", "Team", "Faction", "Content", "Scene", "Database",
    "GameSession", "OnlineSession", "ModeSelection", "GameType",
    "GameModeType", "Playlist", "FrontEnd", "Shell", "Hub", "Campaign",
    "Coop", "CoOp", "Solo", "Multiplayer", "Client", "WorldMap",
    "MainMenu", "Boot", "Entry", "Start", "Load", "Loadout", "Profile",
    "Slot", "Save", "ActivityType", "MissionType", "GameWorld"
};

/* The words that make a hit worth looking at: a method whose name
 * holds one of these is either the mode switch or right next to it. */
static const char *const kHot[] = {
    "Mode", "Session", "Load", "Launch", "Enter", "Start", "Select",
    "Choose", "Pick", "Confirm", "Activate", "Request", "Transition",
    "Change", "Switch", "Online", "Pvp", "Pve", "Ghost", "Merc",
    "Match", "Lobby", "Activ", "Mission", "Content", "World", "Coop"
};

static char     g_dict[DICT_MAX][40];
static uint32_t g_dictHash[DICT_MAX];   /* hashed once, at build time:
                                         * a dump walks the tables many
                                         * times over and the hash of a
                                         * name never changes */
static int      g_dictN;

static void DictAdd(const char *s) {
    if (!s || !s[0]) return;
    if (strlen(s) >= sizeof(g_dict[0])) return;
    if (g_dictN >= DICT_MAX) return;
    strcpy(g_dict[g_dictN], s);
    g_dictHash[g_dictN] = Crc32(s);
    g_dictN++;
}

/* verbs x nouns, both orders, plus the bare words and whatever the ini
 * adds: a few thousand names is nothing to hash, and the engine's own
 * vocabulary is exactly these words in some order. */
/* Defined with the text tables below; the status line needs it first. */
static const char *T(const char *id);

static void DictBuild(void) {
    size_t i, j;
    char buf[40];
    char extra[512] = "";
    char *p;

    g_dictN = 0;
    for (i = 0; i < sizeof(kVerbs) / sizeof(kVerbs[0]); i++)
        DictAdd(kVerbs[i]);
    for (i = 0; i < sizeof(kNouns) / sizeof(kNouns[0]); i++)
        DictAdd(kNouns[i]);
    for (i = 0; i < sizeof(kVerbs) / sizeof(kVerbs[0]); i++) {
        for (j = 0; j < sizeof(kNouns) / sizeof(kNouns[0]); j++) {
            snprintf(buf, sizeof(buf), "%s%s", kVerbs[i], kNouns[j]);
            DictAdd(buf);
            snprintf(buf, sizeof(buf), "%s%s", kNouns[j], kVerbs[i]);
            DictAdd(buf);
            /* RequestMode / SetModeRequest: the middle word is how a
             * lot of this engine spells a request for something. */
            snprintf(buf, sizeof(buf), "%s%s%s", kVerbs[i], kNouns[j],
                     "Request");
            DictAdd(buf);
            snprintf(buf, sizeof(buf), "%s%s%s", "Set", kNouns[j],
                     "Type");
            DictAdd(buf);
        }
    }

    if (g_iniPath[0]) {
        GetPrivateProfileStringA("Settings", "names", "", extra,
                                 sizeof(extra), g_iniPath);
    }
    for (p = strtok(extra, ",; \t"); p; p = strtok(NULL, ",; \t"))
        DictAdd(p);
}

static int NameHasHotWord(const char *name) {
    size_t i;

    for (i = 0; i < sizeof(kHot) / sizeof(kHot[0]); i++) {
        size_t n = strlen(kHot[i]);
        const char *p;
        for (p = name; *p; p++) {
            size_t k;
            for (k = 0; k < n; k++) {
                char a = p[k], b = kHot[i][k];
                if (!a) break;
                if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
                if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
                if (a != b) break;
            }
            if (k == n) return 1;
        }
    }
    return 0;
}

/* ---- one object ---------------------------------------------------- */

static int  g_totalMeth, g_totalNamed, g_totalHot;

static void DumpObject(const char *tag, uint64_t obj) {
    ShMethod m[METH_MAX];
    int n, i, named = 0;

    if (!obj) {
        PLog("[OBJ] %-12s (null)", tag);
        return;
    }
    n = pReflectMethods ? pReflectMethods(obj, m, METH_MAX) : 0;
    PLog("[OBJ] %-12s obj=%llX class=%08X methods=%d", tag,
         (unsigned long long)obj,
         pClassHash ? pClassHash(obj) : 0u, n);
    g_totalMeth += n;
    for (i = 0; i < n; i++) {
        const char *name = "";
        int d;

        /* The reverse of the dictionary: the entry that hashes to this
         * method's name hash is the name, if the list holds it. A hit
         * is a real name - two names sharing a crc32 would be a
         * collision, not a misread. */
        for (d = 0; d < g_dictN; d++) {
            if (g_dictHash[d] == m[i].nameHash) {
                name = g_dict[d];
                break;
            }
        }
        if (name[0]) {
            named++;
            g_totalNamed++;
            PLog("[NAME] %-12s [%3d] %08X rva=%08llX %s", tag, m[i].index,
                 m[i].nameHash,
                 (unsigned long long)(m[i].fn - g_base), name);
            if (NameHasHotWord(name)) {
                g_totalHot++;
                PLog("[HIT ] %-12s [%3d] %08X rva=%08llX %s", tag,
                     m[i].index, m[i].nameHash,
                     (unsigned long long)(m[i].fn - g_base), name);
            }
            /* A named lifecycle method is a tripwire: it is armed and
             * every call it makes is logged with its stack. */
            if (NameWanted(name))
                HookAdd((void *)(uintptr_t)m[i].fn, tag, m[i].index,
                        m[i].nameHash, name);
        } else {
            PLog("[M   ] %-12s [%3d] %08X rva=%08llX", tag, m[i].index,
                 m[i].nameHash,
                 (unsigned long long)(m[i].fn - g_base));
        }
    }
    PLog("[SUM ] %-12s methods=%d named=%d", tag, n, named);
}

static void DumpAll(const char *why) {
    char stname[48] = "";
    int i, st;

    if (!pGameFlow) return;
    if (!g_log) LogOpen();
    if (pGetGameState) st = pGetGameState(); else st = -1;
    if (pGetGameStateName) pGetGameStateName(stname, (int)sizeof(stname));

    g_totalMeth = g_totalNamed = g_totalHot = 0;
    /* A line of its own, so the [CALL] timeline can be read against the
     * moments the player said something happened. */
    PLog("[MARK] %s state=%s(%d)", why, stname, st);
    PLog("---- method tables: %s ---- base=%llX state=%s(%d)",
         why, (unsigned long long)g_base, stname, st);

    DumpObject("gameflow", pGameFlow());
    for (i = 0; i < SLOT_MAX; i++) {
        char tag[16];
        snprintf(tag, sizeof(tag), "objs[%02d]", i);
        DumpObject(tag, pGameFlowObject ? pGameFlowObject(i) : 0);
    }
    if (pHybridMenu) DumpObject("hybridmenu", pHybridMenu());

    PLog("[SUM ] total methods=%d named=%d hot=%d (dictionary %d names)",
         g_totalMeth, g_totalNamed, g_totalHot, g_dictN);
    InstallHooks();
}

/* ---- the tripwire ---------------------------------------------------
 *
 * The dictionary named the scene interface - Init, Enter, Exit, Shutdown
 * - on every object it found, and nothing else: the mode choice is not
 * spelled in a way the list guesses. So the next step does not try to
 * name the loader. It hooks what is already named.
 *
 * Those four are lifecycle methods: they run when a scene is built,
 * entered, left or torn down, which is exactly what a mode switch does
 * to the front end. Hooking them costs nothing per frame (they are not
 * frame methods) and each call is a moment where the mode is already
 * decided - so the stack at that moment holds the code that decided it,
 * frame by frame, and that is the function to look at next.
 *
 * Every call is logged with the object it happened on, the first four
 * arguments - read as both a number and a string, since a mode may
 * arrive as a name - and a stack walk with each frame named
 * "GRW.exe+0x...". A per hook rate limit keeps a method that turns out
 * to run in a loop from flooding the log, and a re-entry guard keeps
 * the logging itself out of it, because the framework calls made here
 * could otherwise come back through one of these hooks.
 *
 * Only the four lifecycle names are armed, and only the ones the dump
 * actually found: a handful of functions on a handful of objects, not
 * the 312 methods the tables hold.
 */
#define HOOK_MAX   96
#define HOOK_STACK 14
#define CALL_MIN_MS 120

typedef struct {
    void       *target;
    void       *orig;
    char        tag[16];
    int         idx;
    uint32_t    hash;
    const char *name;
    DWORD       lastMs;
} TripHook;

/* ---- the call record, and why the detour does not log ---------------
 *
 * The detour used to format and write its own line. That is what took
 * the client down after a mode switch: the mode manager is called on a
 * thread whose stack is already deep, and a logging function puts a
 * kilobyte of locals plus a formatted write on top of that. Measured
 * twice - once inside the framework DLL, once inside this plugin - the
 * fault landed on the first sixteen byte store into that buffer
 * (movdqa [rbp+0x3E0], xmm0, the same instruction in both, because both
 * logging functions declare the same 1024 byte line buffer), with the
 * three mode sessions each dying within a few seconds of the call.
 *
 * So the detour only copies now: the slot, four register arguments, the
 * thread and tick, and the return addresses of a stack walk - into a
 * ring this plugin owns. Formatting, memory sniffing and the write all
 * happen on a thread of ours, which has stack to spare. What the game
 * thread pays is a few hundred bytes of frame and one short critical
 * section: nothing that allocates, formats, or touches a file.
 */
#define RING_MAX    128
#define RING_FRAMES 14

typedef struct {
    int      slot;
    DWORD    tick;
    DWORD    tid;
    uint64_t a[4];
    int      frames;
    void    *raw[RING_FRAMES];
} CallRec;

static CallRec          g_ring[RING_MAX];
static CRITICAL_SECTION g_ringLock;
static volatile LONG    g_ringHead;
static volatile LONG    g_ringTail;
static volatile LONG    g_ringDropped;
static volatile LONG    g_ringReady;

static TripHook g_hk[HOOK_MAX];
static int      g_hkCount;
static int      g_hkWant = 1;          /* ini: hook */
static int      g_stackWalk = 1;       /* ini: hook_stack */
static int      g_hkReady;
static char     g_hkNames[128] = "Init,Shutdown,Enter,Exit";

typedef USHORT (WINAPI *CaptureStack_t)(ULONG, ULONG, PVOID *, PULONG);
static CaptureStack_t g_captureStack;

static int SafeRead(uint64_t addr, void *out, size_t n) {
    SIZE_T got = 0;

    if (addr < 0x10000) return 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)addr,
                             out, n, &got) && got == n;
}

/* "GRW.exe+0x1A2B3C" for a frame inside a module, the raw value for
 * anything else: the game's own frames are the ones that matter. */
static void DescribeAddr(void *addr, char *out, size_t n) {
    HMODULE mod = NULL;
    char file[MAX_PATH];
    const char *base;
    uintptr_t off;

    out[0] = 0;
    if (!addr) { snprintf(out, n, "(null)"); return; }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) && mod) {
        if (!GetModuleFileNameA(mod, file, sizeof(file)))
            snprintf(file, sizeof(file), "?");
        base = strrchr(file, '\\');
        base = base ? base + 1 : file;
        off = (uintptr_t)addr - (uintptr_t)mod;
        snprintf(out, n, "%s+0x%llX", base, (unsigned long long)off);
        return;
    }
    snprintf(out, n, "0x%p", addr);
}

/* An argument, read both ways: a small number explains itself (a mode
 * id), and a pointer that holds a printable string is printed as one -
 * a mode handed over by name would show up here. */
static void SniffArg(const char *which, uint64_t v) {
    char s[48];
    wchar_t w[32];
    size_t i;

    if (v < 0x10000) {
        PLog("       %-4s = %llu (0x%llX)", which,
             (unsigned long long)v, (unsigned long long)v);
        return;
    }
    if (SafeRead(v, s, sizeof(s))) {
        for (i = 0; i < sizeof(s); i++) {
            if (!s[i]) break;
            if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7E) {
                i = 0;
                break;
            }
        }
        if (i >= 3 && i < sizeof(s)) {
            s[i] = 0;
            PLog("       %-4s = %llX \"%s\"", which,
                 (unsigned long long)v, s);
            return;
        }
    }
    if (SafeRead(v, w, sizeof(w))) {
        for (i = 0; i < 32; i++) {
            if (!w[i]) break;
            if (w[i] < 0x20 || w[i] > 0x7E) { i = 0; break; }
        }
        if (i >= 3 && i < 32) {
            char a[40];
            int k;
            for (k = 0; k < (int)i; k++) a[k] = (char)w[k];
            a[i] = 0;
            PLog("       %-4s = %llX L\"%s\"", which,
                 (unsigned long long)v, a);
            return;
        }
    }
    /* Not a string, but the bytes are worth having: a mode handed over
     * as a descriptor is usually a few small integers at the target of
     * one of these pointers, and four consecutive dwords read off a
     * dump name it faster than any amount of guessing. */
    {
        unsigned char b[32];
        char hex[3 * 32 + 4];
        int k, at = 0;

        if (SafeRead(v, b, sizeof(b))) {
            for (k = 0; k < 32; k++)
                at += snprintf(hex + at, sizeof(hex) - (size_t)at, "%02X ",
                               b[k]);
            PLog("       %-4s = %llX bytes: %s", which,
                 (unsigned long long)v, hex);
            return;
        }
    }
    PLog("       %-4s = %llX (unreadable)", which, (unsigned long long)v);
}

static void HookSeen(int n, void *a, void *b, void *c, void *d) {
    TripHook *h = &g_hk[n];
    DWORD now = GetTickCount();
    CallRec *r;

    if (h->lastMs && now - h->lastMs < CALL_MIN_MS) return;
    h->lastMs = now;
    if (!g_ringReady) return;
    EnterCriticalSection(&g_ringLock);
    if (g_ringHead - g_ringTail >= RING_MAX) {
        g_ringDropped++;
        LeaveCriticalSection(&g_ringLock);
        return;
    }
    r = &g_ring[g_ringHead % RING_MAX];
    g_ringHead++;
    r->slot = n;
    r->tick = now;
    r->tid = GetCurrentThreadId();
    r->a[0] = (uint64_t)(uintptr_t)a;
    r->a[1] = (uint64_t)(uintptr_t)b;
    r->a[2] = (uint64_t)(uintptr_t)c;
    r->a[3] = (uint64_t)(uintptr_t)d;
    r->frames = (g_captureStack && g_stackWalk)
                ? (int)g_captureStack(0, RING_FRAMES, r->raw, NULL) : 0;
    LeaveCriticalSection(&g_ringLock);
}

/* The heavy half, on a thread of ours: format, sniff, write. */
static void ReportCall(const CallRec *r) {
    const TripHook *h = &g_hk[r->slot];
    char name[80];
    int i;

    PLog("[CALL] %-12s [%3d] %s hash=%08X rva=%08llX this=%llX tick=%lu "
         "tid=%lu",
         h->tag, h->idx, h->name, h->hash,
         (unsigned long long)((uintptr_t)h->target - g_base),
         (unsigned long long)r->a[0], (unsigned long)r->tick,
         (unsigned long)r->tid);
    SniffArg("a2", r->a[1]);
    SniffArg("a3", r->a[2]);
    SniffArg("a4", r->a[3]);
    for (i = 0; i < r->frames; i++) {
        DescribeAddr(r->raw[i], name, sizeof(name));
        PLog("       frame[%d] %s", i, name);
    }
}

static DWORD WINAPI DrainThread(LPVOID p) {
    (void)p;
    for (;;) {
        CallRec r;
        int have;

        Sleep(150);
        for (;;) {
            EnterCriticalSection(&g_ringLock);
            if (g_ringTail < g_ringHead) {
                r = g_ring[g_ringTail % RING_MAX];
                g_ringTail++;
                have = 1;
            } else {
                have = 0;
            }
            LeaveCriticalSection(&g_ringLock);
            if (!have) break;
            ReportCall(&r);
        }
        if (InterlockedExchange(&g_ringDropped, 0))
            PLog("[CALL] ring full - calls were dropped");
    }
    return 0;
}

/* One detour per hook slot: a generic detour cannot know which trampoline
 * to return through, and the slot has to be a constant for the compiler
 * to build it. 48 of them, generated, because a hand written list that
 * long is a place for a typo to hide. */
typedef void *(*Fn4)(void *, void *, void *, void *);

#define DET(n)                                                       \
    static void *Det##n(void *a, void *b, void *c, void *d) {        \
        void *r;                                                     \
        HookSeen(n, a, b, c, d);                                     \
        r = ((Fn4)g_hk[n].orig)(a, b, c, d);                         \
        return r;                                                    \
    }
DET(0) DET(1) DET(2) DET(3) DET(4) DET(5) DET(6) DET(7)
DET(8) DET(9) DET(10) DET(11) DET(12) DET(13) DET(14) DET(15)
DET(16) DET(17) DET(18) DET(19) DET(20) DET(21) DET(22) DET(23)
DET(24) DET(25) DET(26) DET(27) DET(28) DET(29) DET(30) DET(31)
DET(32) DET(33) DET(34) DET(35) DET(36) DET(37) DET(38) DET(39)
DET(40) DET(41) DET(42) DET(43) DET(44) DET(45) DET(46) DET(47)
DET(48) DET(49) DET(50) DET(51) DET(52) DET(53) DET(54) DET(55)
DET(56) DET(57) DET(58) DET(59) DET(60) DET(61) DET(62) DET(63)
DET(64) DET(65) DET(66) DET(67) DET(68) DET(69) DET(70) DET(71)
DET(72) DET(73) DET(74) DET(75) DET(76) DET(77) DET(78) DET(79)
DET(80) DET(81) DET(82) DET(83) DET(84) DET(85) DET(86) DET(87)
DET(88) DET(89) DET(90) DET(91) DET(92) DET(93) DET(94) DET(95)

#undef DET
#define DET(n) Det##n,
static void *const g_dets[] = {
    DET(0) DET(1) DET(2) DET(3) DET(4) DET(5) DET(6) DET(7)
    DET(8) DET(9) DET(10) DET(11) DET(12) DET(13) DET(14) DET(15)
    DET(16) DET(17) DET(18) DET(19) DET(20) DET(21) DET(22) DET(23)
    DET(24) DET(25) DET(26) DET(27) DET(28) DET(29) DET(30) DET(31)
    DET(32) DET(33) DET(34) DET(35) DET(36) DET(37) DET(38) DET(39)
    DET(40) DET(41) DET(42) DET(43) DET(44) DET(45) DET(46) DET(47)
    DET(48) DET(49) DET(50) DET(51) DET(52) DET(53) DET(54) DET(55)
    DET(56) DET(57) DET(58) DET(59) DET(60) DET(61) DET(62) DET(63)
    DET(64) DET(65) DET(66) DET(67) DET(68) DET(69) DET(70) DET(71)
    DET(72) DET(73) DET(74) DET(75) DET(76) DET(77) DET(78) DET(79)
    DET(80) DET(81) DET(82) DET(83) DET(84) DET(85) DET(86) DET(87)
    DET(88) DET(89) DET(90) DET(91) DET(92) DET(93) DET(94) DET(95)
};
#undef DET

static int NameWanted(const char *name) {
    char buf[128];
    char *p;

    strncpy(buf, g_hkNames, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (p = strtok(buf, ",; \t"); p; p = strtok(NULL, ",; \t")) {
        if (strcmp(p, name) == 0) return 1;
    }
    return 0;
}

static void HookAdd(void *target, const char *tag, int idx, uint32_t hash,
                    const char *name) {
    int i;

    if (!g_hkWant || g_hkReady) return;
    if (g_hkCount >= HOOK_MAX || !target) return;
    for (i = 0; i < g_hkCount; i++) {
        if (g_hk[i].target == target) return;    /* one hook per address */
    }
    g_hk[g_hkCount].target = target;
    g_hk[g_hkCount].orig = NULL;
    snprintf(g_hk[g_hkCount].tag, sizeof(g_hk[0].tag), "%s", tag);
    g_hk[g_hkCount].idx = idx;
    g_hk[g_hkCount].hash = hash;
    g_hk[g_hkCount].name = name;   /* a dictionary string: it is stable */
    g_hk[g_hkCount].lastMs = 0;
    g_hkCount++;
}

/* ---- hooks picked by address, from the ini --------------------------
 *
 * A dictionary cannot name what was never reflected. The mode manager
 * is a plain C++ class - GameModeManager - so nothing in the front end
 * object tables mentions it, and that is why 312 reflected methods and
 * a 24000 name dictionary came back with nothing but the scene
 * interface. A debugger session found it instead:
 *
 *   0x391E158  GameModeManager::SetCurrentGameMode(this, type, arg2)
 *   0x391E0D0  GameModeManager::CreateGameMode()
 *   0x391E120  GameModeManager::RemoveCurrentGameMode()
 *   0x391E1F0  GameModeManager::OnGameLoadingEnd()
 *
 * The first one is the answer to the question this whole probe exists
 * for: the mode arrives as an ordinary argument - a number, not a scene
 * set, an archive list or a pointer to compare.
 *
 * The addresses come from the ini rather than the source, so one found
 * in a debugger needs no rebuild:
 *
 *   hooks=391E158:SetCurrentGameMode,391E0D0:CreateGameMode,...
 *
 * each entry "RVA[:label]", hex, against the module base. Every one is
 * checked against the image before it is armed - an RVA from another
 * build would otherwise put a jump in the middle of something unrelated
 * and take the client down.
 *
 * dump_rva/dump_len dump a region the same way, for a table found by
 * hand. The mode name table (0x390CCF0, holding MP / TG / MERC / SP /
 * COOP) is read raw, as pointers to strings, and as runs of printable
 * bytes - which is how the numbers the hooks report get their names.
 */
static char     g_extraNames[HOOK_MAX][40];
static uint64_t g_imgEnd;

static void ImageRange(void) {
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)(uintptr_t)g_base;
    const IMAGE_NT_HEADERS *nt;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    nt = (const IMAGE_NT_HEADERS *)(uintptr_t)(g_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    g_imgEnd = g_base + nt->OptionalHeader.SizeOfImage;
}

static int InImage(uint64_t a) {
    return g_base && a >= g_base && (!g_imgEnd || a < g_imgEnd);
}

/* Why a hook cannot be placed is worth knowing before it fails: MinHook
 * reports one number for the whole patch and hides what Windows said.
 * This asks first - the page state, the bytes there, and a real
 * VirtualProtect on it - so the answer is in the log whether the hook
 * lands or not.
 *
 * The two readings that matter: a page that is not writable for a
 * reason (ERROR_ACCESS_DENIED, which is what a protection layer says)
 * is a different problem from an address that is not committed at all
 * (ERROR_INVALID_ADDRESS, which means the RVA is from another build),
 * and the bytes say whether a function starts here at all - a pointer
 * into the image instead means this is a table entry, and the thing to
 * hook is what it points at. */
static int ExecutableAt(uint64_t a) {
    MEMORY_BASIC_INFORMATION mbi;
    DWORD p;

    if (!a) return 0;
    if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    p = mbi.Protect & 0xFFu;
    return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

static void DiagTarget(uint64_t rva) {
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char b[32];
    char hex[3 * 32 + 2];
    uint64_t at = g_base + rva;
    DWORD old = 0, back = 0;
    int i, printable = 1;

    if (!VirtualQuery((LPCVOID)(uintptr_t)at, &mbi, sizeof(mbi))) {
        PLog("[DIAG] rva=%08llX VirtualQuery failed err=%lu",
             (unsigned long long)rva, GetLastError());
        return;
    }
    PLog("[DIAG] rva=%08llX region=%llX size=%llX state=%lX prot=%lX "
         "type=%lX",
         (unsigned long long)rva,
         (unsigned long long)(uintptr_t)mbi.BaseAddress,
         (unsigned long long)mbi.RegionSize, mbi.State, mbi.Protect,
         mbi.Type);
    if (!SafeRead(at, b, sizeof(b))) {
        PLog("[DIAG]  unreadable - the region is not committed here");
        return;
    }
    for (i = 0; i < 32; i++)
        snprintf(hex + i * 3, 4, "%02X ", b[i]);
    hex[96] = 0;
    PLog("[DIAG]  bytes %s", hex);
    for (i = 0; i < 32; i++) {
        if (b[i] < 0x20 || b[i] > 0x7E) { printable = 0; break; }
    }
    if (printable) PLog("[DIAG]  reads as text");
    else {
        uint64_t p;
        memcpy(&p, b, 8);
        if (InImage(p)) {
            MEMORY_BASIC_INFORMATION mi;
            unsigned char q[16];
            char qh[3 * 16 + 2];
            int k;

            PLog("[DIAG]  first qword is an image pointer %llX - so this "
                 "address is a variable holding a function, not a function",
                 (unsigned long long)p);
            if (VirtualQuery((LPCVOID)(uintptr_t)p, &mi, sizeof(mi)))
                PLog("[DIAG]   pointee state=%lX prot=%lX exec=%d "
                     "rva=%llX", mi.State, mi.Protect, ExecutableAt(p),
                     (unsigned long long)(p - g_base));
            if (SafeRead(p, q, sizeof(q))) {
                for (k = 0; k < 16; k++)
                    snprintf(qh + k * 3, 4, "%02X ", q[k]);
                qh[48] = 0;
                PLog("[DIAG]   pointee bytes %s", qh);
            }
        }
    }
    if (VirtualProtect((LPVOID)(uintptr_t)at, 16, PAGE_EXECUTE_READWRITE,
                       &old)) {
        VirtualProtect((LPVOID)(uintptr_t)at, 16, old, &back);
        PLog("[DIAG]  VirtualProtect RW ok (was %lX)", old);
    } else {
        PLog("[DIAG]  VirtualProtect RW FAILED err=%lu - this is why the "
             "hook cannot be placed", GetLastError());
    }
}

static int ExtraAdd(uint64_t rva, const char *label) {
    void *target;
    int i;

    if (g_hkReady || g_hkCount >= HOOK_MAX) return 0;
    if (!InImage(g_base + rva)) {
        PLog("[HOOK] hooks=%llX is outside GRW.exe (base=%llX size=%llX) - "
             "skipped",
             (unsigned long long)rva, (unsigned long long)g_base,
             (unsigned long long)(g_imgEnd - g_base));
        return 0;
    }
    DiagTarget(rva);
    /* A debugger read breakpoint lands on data, and an anti tamper layer
     * likes to keep a function's address in a variable rather than call
     * it directly. If the address is not executable but holds a pointer
     * to something that is, that pointer is the function to hook - and
     * following it is what makes an address from a read breakpoint
     * usable as is. */
    if (!ExecutableAt(g_base + rva)) {
        uint64_t p = 0;

        if (SafeRead(g_base + rva, &p, 8) && ExecutableAt(p)) {
            PLog("[HOOK] %s: rva=%08llX is data holding a function pointer "
                 "-> %08llX, arming that instead",
                 label, (unsigned long long)rva,
                 (unsigned long long)(p - g_base));
            target = (void *)(uintptr_t)p;
        } else {
            PLog("[HOOK] %s: rva=%08llX is not executable and holds no "
                 "function pointer - skipped", label,
                 (unsigned long long)rva);
            return 0;
        }
    } else {
        target = (void *)(uintptr_t)(g_base + rva);
    }
    for (i = 0; i < g_hkCount; i++) {
        if (g_hk[i].target == target) return 0;      /* already armed */
    }
    snprintf(g_extraNames[g_hkCount], sizeof(g_extraNames[0]), "%s", label);
    g_hk[g_hkCount].target = target;
    g_hk[g_hkCount].orig = NULL;
    snprintf(g_hk[g_hkCount].tag, sizeof(g_hk[0].tag), "mode");
    g_hk[g_hkCount].idx = -1;
    g_hk[g_hkCount].hash = (uint32_t)rva;
    g_hk[g_hkCount].name = g_extraNames[g_hkCount];
    g_hk[g_hkCount].lastMs = 0;
    g_hkCount++;
    return 1;
}

static void LoadExtraHooks(void) {
    char buf[1024];
    char *tok;

    if (!g_iniPath[0]) return;
    buf[0] = 0;
    GetPrivateProfileStringA("Settings", "hooks", "", buf, sizeof(buf),
                             g_iniPath);
    for (tok = strtok(buf, ",; \t"); tok; tok = strtok(NULL, ",; \t")) {
        char *colon = strchr(tok, ':');
        uint64_t rva;
        char label[40];

        if (colon) {
            size_t n = strlen(colon + 1);
            if (n >= sizeof(label)) n = sizeof(label) - 1;
            memcpy(label, colon + 1, n);
            label[n] = 0;
            *colon = 0;
        } else {
            snprintf(label, sizeof(label), "rva_%s", tok);
        }
        rva = (uint64_t)strtoull(tok, NULL, 16);
        if (rva) ExtraAdd(rva, label);
    }
}

static void DumpRegion(const char *tag, uint64_t rva, int len) {
    unsigned char b[256];
    char ascii[20];
    int i;

    if (len <= 0) return;
    if (len > (int)sizeof(b)) len = (int)sizeof(b);
    if (!InImage(g_base + rva) || !SafeRead(g_base + rva, b, (size_t)len)) {
        PLog("[DUMP] %s rva=%08llX unreadable (outside the image?)", tag,
             (unsigned long long)rva);
        return;
    }
    PLog("[DUMP] %s rva=%08llX len=%d", tag, (unsigned long long)rva, len);
    for (i = 0; i < len; i += 16) {
        char hex[3 * 16 + 2];
        int k, n = len - i > 16 ? 16 : len - i, at = 0;

        for (k = 0; k < n; k++)
            at += snprintf(hex + at, sizeof(hex) - (size_t)at, "%02X ",
                           b[i + k]);
        for (k = 0; k < n; k++) {
            unsigned char c = b[i + k];
            ascii[k] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        ascii[n] = 0;
        PLog("[DUMP]  +%03X  %-48s |%s|", i, hex, ascii);
    }
    /* Read as a table of pointers, each one followed to its string. */
    for (i = 0; i + 8 <= len; i += 8) {
        uint64_t p;
        char s[64];
        size_t k;
        int printable = 1;

        memcpy(&p, b + i, 8);
        if (!InImage(p)) continue;
        s[0] = 0;
        if (SafeRead(p, s, sizeof(s) - 1)) {
            s[sizeof(s) - 1] = 0;
            for (k = 0; k < sizeof(s) - 1 && s[k]; k++) {
                if ((unsigned char)s[k] < 0x20 ||
                    (unsigned char)s[k] > 0x7E) {
                    printable = 0;
                    break;
                }
            }
        } else {
            printable = 0;
        }
        /* Every image pointer in the region gets its RVA and whether it
         * points at executable code: a table of function pointers is
         * then readable by eye, and so is a table of data. */
        PLog("[DUMP]  ptr[+%02X] %llX rva=%08llX exec=%d%s%s", i,
             (unsigned long long)p, (unsigned long long)(p - g_base),
             ExecutableAt(p), printable ? " \"" : "",
             printable ? s : (printable ? "\"" : ""));
    }
    /* Read as packed short strings - what the mode names look like. */
    {
        int start = -1;

        for (i = 0; i <= len; i++) {
            int printable = i < len && b[i] >= 0x20 && b[i] < 0x7F;
            if (printable) {
                if (start < 0) start = i;
            } else if (start >= 0) {
                if (i - start >= 2 && i - start < 64) {
                    char t[64];
                    memcpy(t, b + start, (size_t)(i - start));
                    t[i - start] = 0;
                    PLog("[DUMP]  str[+%03X] \"%s\"", start, t);
                }
                start = -1;
            }
        }
    }
}

static void DumpExtraRegions(void) {
    char buf[32];
    uint64_t rva;
    int len;

    if (!g_iniPath[0]) return;
    /* The RVA is hex, which GetPrivateProfileInt would read as "390"
     * and stop at the C, so it is parsed here. */
    buf[0] = 0;
    GetPrivateProfileStringA("Settings", "dump_rva", "", buf, sizeof(buf),
                             g_iniPath);
    rva = (uint64_t)strtoull(buf, NULL, 16);
    len = GetPrivateProfileIntA("Settings", "dump_len", 0, g_iniPath);
    if (!rva || len <= 0) return;
    DumpRegion("table", rva, len);
}

static void InstallHooks(void) {
    int i, ok = 0;

    /* g_hkWant only gates the lifecycle tripwires - it is checked where
     * they are added. The ones picked by address are wanted by whoever
     * wrote them into the ini, so they arm either way. */
    if (g_hkReady || !g_hkCount) return;
    if (MH_Initialize() != MH_OK) {
        /* A second call on a later dump is not a failure: MinHook keeps
         * its state per DLL and reports already initialized. */
        MH_STATUS i = MH_Initialize();
        if (i != MH_ERROR_ALREADY_INITIALIZED) {
            PLog("[HOOK] minhook init failed (%d) - nothing armed", (int)i);
            g_hkReady = 1;          /* do not try again every dump */
            return;
        }
    }
    for (i = 0; i < g_hkCount; i++) {
        MH_STATUS s = MH_CreateHook(g_hk[i].target, g_dets[i],
                                    &g_hk[i].orig);
        if (s != MH_OK) {
            PLog("[HOOK] %s [%d] %s FAILED (%d)", g_hk[i].tag, g_hk[i].idx,
                 g_hk[i].name, (int)s);
            g_hk[i].target = NULL;
            continue;
        }
        s = MH_EnableHook(g_hk[i].target);
        if (s != MH_OK) {
            PLog("[HOOK] %s [%d] %s enable FAILED (%d)", g_hk[i].tag,
                 g_hk[i].idx, g_hk[i].name, (int)s);
            continue;
        }
        PLog("[HOOK] %s [%d] %s rva=%08llX armed", g_hk[i].tag, g_hk[i].idx,
             g_hk[i].name,
             (unsigned long long)((uintptr_t)g_hk[i].target - g_base));
        ok++;
    }
    PLog("[HOOK] %d of %d tripwires armed (names=%s)", ok, g_hkCount,
         g_hkNames);
    if (ok) g_hkReady = 1;
}

static void LoadHookSettings(void) {
    char buf[160];

    if (!g_iniPath[0]) return;
    g_hkWant = GetPrivateProfileIntA("Settings", "hook", 1, g_iniPath) ? 1 : 0;
    /* The stack walk is the only part of the recording with any cost
     * inside the game, so it can be turned off on its own. */
    g_stackWalk = GetPrivateProfileIntA("Settings", "hook_stack", 1, g_iniPath)
                  ? 1 : 0;
    buf[0] = 0;
    GetPrivateProfileStringA("Settings", "hook_names", "", buf, sizeof(buf),
                             g_iniPath);
    if (buf[0]) {
        strncpy(g_hkNames, buf, sizeof(g_hkNames) - 1);
        g_hkNames[sizeof(g_hkNames) - 1] = 0;
    }
}

/* ---- find the code behind a string ----------------------------------
 *
 * The mode manager is a plain C++ class, so nothing about it is
 * reflected (checked: the hashes of SetCurrentGameMode and its
 * neighbours match no method table entry, while the control names Enter
 * / Init / Shutdown match theirs exactly). Its methods are still in the
 * binary, and its log lines name them:
 *
 *   0x391E0D0  ...CreateGameMode()
 *   0x391E120  ...RemoveCurrentGameMode()
 *   0x391E158  ...SetCurrentGameMode(%u, %u)
 *   0x391E1F0  ...OnGameLoadingEnd()
 *
 * Those are read-only strings, which is why a hook on them cannot be
 * placed - and why a debugger read breakpoint on one is really asking
 * "which code reads this line". The answer is a reference scan: a
 * RIP-relative reference to the string is a lea or mov whose four byte
 * displacement resolves to exactly that address, and the instruction it
 * belongs to sits inside the method.
 *
 *   ref_rva=391E158,391E0D0,391E120,391E1F0
 *
 * An instruction is not a function, so the enclosing function has to
 * come from somewhere better than a guess about prologues: the image's
 * exception directory (.pdata) is a sorted table of every function's
 * [begin, end) in the image, and a binary search over it answers
 * exactly. What is logged per hit is the instruction's RVA, the bytes
 * there, and the function's begin and end - so the address to put in
 * hooks= is read off, not inferred.
 */
#define REF_CHUNK (1u << 20)
#define REF_MAXHITS 48

static uint64_t g_refWant[8];
static int      g_refCount;

static int FindFunction(uint64_t rva, uint64_t *begin, uint64_t *end) {
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)(uintptr_t)g_base;
    const IMAGE_NT_HEADERS *nt;
    const IMAGE_DATA_DIRECTORY *dir;
    uint32_t lo = 0, hi;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (const IMAGE_NT_HEADERS *)(uintptr_t)(g_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    if (nt->OptionalHeader.NumberOfRvaAndSizes <=
        IMAGE_DIRECTORY_ENTRY_EXCEPTION)
        return 0;
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!dir->VirtualAddress || dir->Size < 12) return 0;
    hi = dir->Size / 12;
    while (lo < hi) {
        struct { uint32_t begin, end, unwind; } e;
        uint32_t mid = lo + (hi - lo) / 2;

        if (!SafeRead(g_base + dir->VirtualAddress + (uint64_t)mid * 12,
                      &e, 12))
            return 0;
        if (rva < e.begin) hi = mid;
        else if (rva >= e.end) lo = mid + 1;
        else {
            if (begin) *begin = e.begin;
            if (end) *end = e.end;
            return 1;
        }
    }
    return 0;
}

static void RefHit(uint64_t insnRva, const unsigned char *bytes, int n,
                   uint64_t targetRva) {
    uint64_t fnB = 0, fnE = 0;
    char hex[3 * 16 + 2];
    int i;

    if (n > 16) n = 16;
    for (i = 0; i < n; i++)
        snprintf(hex + i * 3, 4, "%02X ", bytes[i]);
    hex[n * 3] = 0;
    if (FindFunction(insnRva, &fnB, &fnE))
        PLog("[REF ] string %08llX referenced at %08llX  function "
             "%08llX..%08llX (size %llu)  bytes %s",
             (unsigned long long)targetRva,
             (unsigned long long)insnRva,
             (unsigned long long)fnB, (unsigned long long)fnE,
             (unsigned long long)(fnE - fnB), hex);
    else
        PLog("[REF ] string %08llX referenced at %08llX  (not in .pdata)  "
             "bytes %s", (unsigned long long)targetRva,
             (unsigned long long)insnRva, hex);
}

static void ScanRefsTo(uint64_t targetRva, int *hits) {
    unsigned char *buf = (unsigned char *)malloc(REF_CHUNK);
    MEMORY_BASIC_INFORMATION mbi;
    uint64_t addr = g_base, target = g_base + targetRva;

    if (!buf) return;
    while (addr < g_imgEnd && *hits < REF_MAXHITS) {
        uint64_t base;
        size_t len, off;

        if (!VirtualQuery((LPCVOID)(uintptr_t)addr, &mbi, sizeof(mbi)))
            break;
        if (!mbi.RegionSize) break;
        base = (uint64_t)(uintptr_t)mbi.BaseAddress;
        len = (size_t)mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE &&
            ExecutableAt(base)) {
            for (off = 0; off + 8 < len && *hits < REF_MAXHITS;
                 off += REF_CHUNK) {
                size_t want = len - off, i;

                if (want > REF_CHUNK) want = REF_CHUNK;
                if (!SafeRead(base + off, buf, want)) continue;
                for (i = 0; i + 4 <= want; i++) {
                    int32_t d;
                    uint64_t after, insn = 0;
                    size_t insnOff;
                    int insnBytes;

                    memcpy(&d, buf + i, 4);
                    after = base + off + i + 4;
                    if ((uint64_t)((int64_t)after + d) != target) continue;
                    /* Is it really an addressing form? lea r64,[rip+d]
                     * is REX 8D modrm=mod00 rm=101, and mov r64,[rip+d]
                     * is the same shape with 8B. Both end in the disp,
                     * so the instruction starts one to three bytes
                     * before it. */
                    if (i >= 3 && (buf[i - 3] & 0xFE) == 0x48 &&
                        (buf[i - 2] == 0x8D || buf[i - 2] == 0x8B) &&
                        (buf[i - 1] & 0xC7) == 0x05)
                        insn = base + off + i - 3;
                    else if (i >= 2 &&
                             (buf[i - 2] == 0x8D || buf[i - 2] == 0x8B) &&
                             (buf[i - 1] & 0xC7) == 0x05)
                        insn = base + off + i - 2;
                    else
                        continue;
                    insnOff = (size_t)(insn - (base + off));
                    insnBytes = (insnOff + 16 <= want) ? 16
                                                       : (int)(want - insnOff);
                    RefHit(insn - g_base, buf + insnOff, insnBytes,
                           targetRva);
                    (*hits)++;
                    if (*hits >= REF_MAXHITS) break;
                }
            }
        }
        addr = base + mbi.RegionSize;
    }
    free(buf);
}

static void LoadRefTargets(void) {
    char buf[256];
    char *tok;

    g_refCount = 0;
    if (!g_iniPath[0]) return;
    buf[0] = 0;
    GetPrivateProfileStringA("Settings", "ref_rva", "", buf, sizeof(buf),
                             g_iniPath);
    for (tok = strtok(buf, ",; \t"); tok && g_refCount < 8;
         tok = strtok(NULL, ",; \t")) {
        uint64_t rva = (uint64_t)strtoull(tok, NULL, 16);

        if (rva && InImage(g_base + rva)) g_refWant[g_refCount++] = rva;
    }
}

static void ScanRefs(void) {
    int i, hits = 0;

    for (i = 0; i < g_refCount && hits < REF_MAXHITS; i++) {
        PLog("[REF ] scanning for references to rva %08llX",
             (unsigned long long)g_refWant[i]);
        ScanRefsTo(g_refWant[i], &hits);
    }
    if (g_refCount)
        PLog("[REF ] done: %d reference(s) to %d string(s)", hits,
             g_refCount);
}

/* ---- menu and hotkey ------------------------------------------------ */

static void OnDump(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)value; (void)user;
    DumpAll("menu");
}

static void RefreshStatus(void) {
    char text[160];

    if (!g_menu || !pMenuStatus) return;
    snprintf(text, sizeof(text),
             "%d %s, %d %s, %d %s - %s",
             g_totalMeth, T("@mc.status"), g_totalNamed, T("@mc.status"),
             g_totalHot, T("@mc.status"), T("@mc.n.dump"));
    pMenuStatus(g_menu, text);
}

/* ---- text ---------------------------------------------------------
 * The plugin's own text, compiled in: lang.ini beside this source only
 * has to carry what it changes, and the menu reads with or without it.
 * Keys are stable IDs. Late-bound, like the rest of this plugin.
 */
typedef struct { const char *key; const char *text; } TextRow;
typedef int (*LangDeclare_t)(const char *owner, const char *lang,
                             const TextRow *rows, int n);
typedef const char *(*LangText_t)(const char *owner, const char *key);
static LangDeclare_t pLangDeclare;
static LangText_t    pLangText;

static const TextRow kEn[] = {
    { "@mc.page", "Mode call probe" },
    { "@mc.dump", "Dump tables" },
    { "@mc.status", "methods / named / hot" },
    { "@mc.n.dump", "F9 dumps" },
    { "@mc.hint",
      "Writes the method tables of the GameFlow machine, its sub objects "
      "and the HybridMenu - with any names the dictionary resolves - to "
      "logs\\ModeCallProbe.log. Read only: nothing is called, hooked or "
      "changed." }
};

static const TextRow kZh[] = {
    { "@mc.page", "模式调用探测" },
    { "@mc.dump", "导出方法表" },
    { "@mc.status", "方法 / 具名 / 热点" },
    { "@mc.n.dump", "F9 导出" },
    { "@mc.hint",
      "把 GameFlow 状态机、其子对象与 HybridMenu 的方法表（含字典能解析出"
      "的名称）写到 logs\\ModeCallProbe.log。只读：不调用、不挂钩、不修改。" }
};

static void TextInit(void) {
    static int done;
    HMODULE mod;

    if (done) return;
    mod = GetModuleHandleA("dinput8.dll");
    if (!mod) return;
    if (!pLangDeclare)
        *(FARPROC *)&pLangDeclare = GetProcAddress(mod, "ShLangDeclare");
    if (!pLangText)
        *(FARPROC *)&pLangText = GetProcAddress(mod, "ShLangText");
    if (!pLangDeclare) return;
    done = 1;
    pLangDeclare("ModeCallProbe", "en-US", kEn,
                 (int)(sizeof(kEn) / sizeof(kEn[0])));
    pLangDeclare("ModeCallProbe", "zh-CN", kZh,
                 (int)(sizeof(kZh) / sizeof(kZh[0])));
}

/* One of our IDs as text: a value inside a status line is used exactly
 * as written, so it has to be resolved here. */
static const char *T(const char *id) {
    const char *t;

    if (!id || id[0] != '@') return id;
    if (!pLangText) TextInit();
    if (!pLangText) return id;
    t = pLangText("ModeCallProbe", id);
    return (t && t[0]) ? t : id;
}

static void BuildMenu(void) {
    if (!pMenuCreate || !pMenuAction) return;
    TextInit();
    g_menu = pMenuCreate("@mc.page");
    if (!g_menu) return;
    pMenuAction(g_menu, "@mc.dump", OnDump, NULL);
    if (pMenuHint)
        pMenuHint(g_menu, "@mc.hint");
}

#define DUMP_KEY VK_F9

static DWORD WINAPI HotkeyThread(LPVOID arg) {
    int prev = 0;

    (void)arg;
    for (;;) {
        int down = (GetAsyncKeyState(DUMP_KEY) & 0x8000) != 0;
        if (down && !prev) DumpAll("hotkey");
        prev = down;
        Sleep(30);
    }
    return 0;
}

/* ---- startup -------------------------------------------------------- */

static FARPROC Bind(HMODULE m, const char *name) {
    FARPROC p = GetProcAddress(m, name);
    if (!p) PLog("bind: %s MISSING", name);
    return p;
}

static void BindStackWalk(void) {
    static const char *mods[] = { "ntdll.dll", "kernelbase.dll",
                                  "kernel32.dll" };
    static const char *fns[]  = { "RtlCaptureStackBackTrace",
                                  "CaptureStackBackTrace" };
    int i, j;

    for (i = 0; i < (int)(sizeof(mods) / sizeof(mods[0])) && !g_captureStack;
         i++) {
        HMODULE m = GetModuleHandleA(mods[i]);
        if (!m) continue;
        for (j = 0; j < (int)(sizeof(fns) / sizeof(fns[0])); j++) {
            g_captureStack = (CaptureStack_t)GetProcAddress(m, fns[j]);
            if (g_captureStack) return;
        }
    }
}

static DWORD WINAPI InitThread(LPVOID arg) {
    HMODULE m;
    char path[MAX_PATH];
    int waited = 0;

    (void)arg;
    /* The ini sits beside the .asi, so this module's own path is the
     * right one for it (the log, by contrast, goes to the game dir). */
    if (GetModuleFileNameA(g_inst, path, MAX_PATH)) {
        char *slash = strrchr(path, '\\');
        if (slash) {
            slash[1] = 0;
            snprintf(g_iniPath, sizeof(g_iniPath), "%sModeCallProbe.ini",
                     path);
        }
    }
    g_base = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);

    /* The loader is a dinput8 proxy, so the framework is this module's
     * neighbour: wait for it the way the other probes do. */
    for (;;) {
        m = GetModuleHandleA("dinput8.dll");
        if (m && GetProcAddress(m, "ShGetGameState")) break;
        if (waited > 30000) return 0;
        Sleep(200);
        waited += 200;
    }

    LogOpen();
    PLog("ModeCallProbe start base=%llX", (unsigned long long)g_base);

    pGetGameState     = (GetGameState_t)Bind(m, "ShGetGameState");
    pGetGameStateName = (StateName_t)Bind(m, "ShGetGameStateName");
    pGameFlow         = (GameFlow_t)Bind(m, "ShGameFlow");
    pGameFlowObject   = (GameFlowObject_t)Bind(m, "ShGameFlowObject");
    pHybridMenu       = (HybridMenu_t)Bind(m, "ShHybridMenu");
    pReflectMethods   = (ReflectMethods_t)Bind(m, "ShReflectMethods");
    pClassHash        = (ClassHash_t)Bind(m, "ShReflectClassHash");
    pMenuCreate       = (MenuCreate_t)Bind(m, "ShMenuCreate");
    pMenuAction       = (MenuAction_t)Bind(m, "ShMenuAction");
    pMenuHint         = (MenuHint_t)Bind(m, "ShMenuHint");
    pMenuStatus       = (MenuStatus_t)Bind(m, "ShMenuStatus");

    /* The ring and its writer come up before anything is armed, so a
     * hook that fires the moment it is installed has somewhere to go. */
    InitializeCriticalSection(&g_ringLock);
    InterlockedExchange(&g_ringReady, 1);
    CreateThread(NULL, 0, DrainThread, NULL, 0, NULL);

    LoadHookSettings();
    BindStackWalk();
    ImageRange();
    PLog("image: base=%llX size=%llX", (unsigned long long)g_base,
         (unsigned long long)(g_imgEnd - g_base));
    /* The addresses a debugger session found, and any table to read, go
     * in before the first dump so that the dump's InstallHooks call
     * arms them together with the tripwires. */
    LoadExtraHooks();
    DumpExtraRegions();
    /* Which code reads the mode manager's log lines: the methods are
     * found by what references them, since nothing reflects the class.
     * Read only, and it names the enclosing function for each hit. */
    LoadRefTargets();
    ScanRefs();
    DictBuild();
    PLog("dictionary: %d names, ini=%s", g_dictN,
         g_iniPath[0] ? g_iniPath : "(none)");
    PLog("tripwires: hook=%d names=%s extra=%d", g_hkWant, g_hkNames,
         g_hkCount);

    if (!pGameFlow || !pReflectMethods) {
        PLog("the reflection exports are missing - nothing to dump");
        return 0;
    }

    BuildMenu();
    Sleep(6000);            /* let the front end come up */
    DumpAll("startup");
    RefreshStatus();
    CreateThread(NULL, 0, HotkeyThread, NULL, 0, NULL);
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
