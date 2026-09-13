/* Mode probe: find the play mode from the outside.
 *
 * The engine never exposes "which mode am I playing".  ShGetGameState
 * tells Playing from MenuOrLobby and nothing finer, so campaign,
 * Ghost Mode, Ghost War and the two DLCs all read the same.  This
 * plugin is the evidence step: it samples every candidate the
 * framework can already reach, writes one line per sample to
 * logs\ModeProbe.log, and changes nothing.
 *
 * Why a probe and not an API: a mode read has to be pinned to a
 * field, and no field is known.  The candidates are the GameFlow
 * machine and its seventeen sub objects (a per mode class hash would
 * show up here), the shell HybridMenu, the drawn scene set, the input
 * context, the PVP only entities the net identity table names, the
 * archives this session has read ([ARC], the framework's own ledger -
 * the exact form of "which files did this mode load"), and a raw window
 * over the machine, its state object and the shell - the mode int, if
 * one exists, has to be inside one of those. [DIG] hashes the object
 * graph one level down as well, because the flow machine turned out to
 * be identical in the Ghost War lobby and in the campaign menu, which
 * leaves the shell's own objects as the place a mode could hide.
 *
 * Read it by running one mode per session and comparing the sections:
 * every sample writes "[SIG]" with a fixed field order, so the same
 * column in campaign and in PvP can be put side by side.  The target
 * for now is the two player versus player modes - Ghost War (4v4) and
 * MERCENARIES - against a PvE control; every other mode counts as PvE
 * as far as the API is concerned.
 *
 * What the entity counts can and cannot say: PvE is up to four player
 * co-op, so several SH_KIND_PLAYER entities are NOT a PvP signal -
 * "humans" tells solo from multi human and nothing more.  The mode has
 * to come from the mode side instead: the GameFlow sub object class
 * hashes, the drawn scene set, the UI hash and the raw windows.  The
 * counts stay because one split would still settle it, and that is
 * worth watching for: if co-op classifies the other humans as
 * SH_KIND_TEAMMATE while PvP leaves them SH_KIND_PLAYER (or the other
 * way round), then the kind alone names the mode.
 *
 * The client restarts when a non campaign mode returns to the menu
 * (that is the engine's design - see ModeExitProbe.c), so the log is
 * opened for append and every line carries the process id: one file
 * holds several sessions and they have to be told apart.
 *
 * Nothing here is hooked and nothing is written to the game.  Press
 * the dump key (F8 by default) the moment a mode is known, so the log
 * has a marked sample to line up with it.  Removing the plugin folder
 * restores everything.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

#include "scripthook.h"

/* ---- late bound framework ---------------------------------- */

typedef int      (*GetVersion_t)(void);
typedef int      (*IsInGame_t)(void);
typedef int      (*GetGameState_t)(void);
typedef int      (*GameStateName_t)(char *, int);
typedef uint32_t (*GetGameStateHash_t)(void);
typedef uint32_t (*GetUiState_t)(void);
typedef uint64_t (*GameFlow_t)(void);
typedef uint64_t (*GameFlowObject_t)(int);
typedef uint64_t (*HybridMenu_t)(void);
typedef uint32_t (*ClassHash_t)(uint64_t);
typedef int      (*SceneCount_t)(void);
typedef uint64_t (*SceneAt_t)(int);
typedef int      (*SceneName_t)(uint64_t, char *, int);
typedef int      (*Scenes_t)(char *, int);
typedef int      (*InputContext_t)(void);
typedef int      (*GetPlayer_t)(ShPlayer *);
typedef int      (*FindEntities_t)(int, float, uint32_t, ShEntity *, int);
typedef int      (*ReadBytes_t)(uint64_t, void *, uint32_t);
typedef uint64_t (*ReadU64_t)(uint64_t, int *);
typedef int         (*ForgeReadCount_t)(void);
typedef const char *(*ForgeReadName_t)(int);
/* The engine's own widget tree, read only. */
typedef int      (*GameSceneCount_t)(void);
typedef uint64_t (*GameSceneAt_t)(int);
typedef int      (*GameSceneName_t)(uint64_t, char *, int);
typedef uint64_t (*SceneRoot_t)(uint64_t);
typedef int      (*WidgetChildCount_t)(uint64_t);
typedef uint64_t (*WidgetChildAt_t)(uint64_t, int);
typedef int      (*WidgetClass_t)(uint64_t, char *, int);
typedef int      (*WidgetGetS_t)(uint64_t, uint32_t, char *, int);
typedef int      (*WidgetGetU_t)(uint64_t, uint32_t, uint32_t *);
typedef int      (*WidgetGetF_t)(uint64_t, uint32_t, float *);
typedef int      (*WidgetGetV_t)(uint64_t, uint32_t, float *, int);
/* Ours, but needed as a bootstrap: see EnsureScene. */
typedef uint32_t (*UiSceneCreate_t)(const char *, int);
typedef int      (*UiSceneShow_t)(uint32_t, int);
typedef uint32_t (*MenuCreate_t)(const char *);
typedef int      (*MenuToggle_t)(uint32_t, const char *, int,
                                 ShMenuFn, void *);
typedef int      (*MenuAction_t)(uint32_t, const char *, ShMenuFn, void *);
typedef int      (*MenuStatus_t)(uint32_t, const char *);
typedef int      (*MenuHint_t)(uint32_t, const char *);

static GetVersion_t      pGetVersion;
static IsInGame_t        pIsInGame;
static GetGameState_t    pGetGameState;
static GameStateName_t   pGetGameStateName;
static GetGameStateHash_t pGetGameStateHash;
static GetUiState_t      pGetUiState;
static GameFlow_t        pGameFlow;
static GameFlowObject_t  pGameFlowObject;
static HybridMenu_t      pHybridMenu;
static ClassHash_t       pClassHash;
static Scenes_t          pScenes;
static InputContext_t    pInputContext;
static GetPlayer_t       pGetPlayer;
static FindEntities_t    pFindEntities;
static ReadBytes_t       pReadBytes;
static ReadU64_t         pReadU64;
static ForgeReadCount_t  pForgeReadCount;
static ForgeReadName_t   pForgeReadName;
static GameSceneCount_t  pGameSceneCount;
static GameSceneAt_t     pGameSceneAt;
static GameSceneName_t   pGameSceneName;
static SceneRoot_t       pSceneRoot;
static WidgetChildCount_t pWidgetChildCount;
static WidgetChildAt_t   pWidgetChildAt;
static WidgetClass_t     pWidgetClass;
static WidgetGetS_t      pWidgetGetS;
static WidgetGetU_t      pWidgetGetU;
static WidgetGetF_t      pWidgetGetF;
static WidgetGetV_t      pWidgetGetV;
static UiSceneCreate_t   pUiSceneCreate;
static UiSceneShow_t     pUiSceneShow;
static MenuCreate_t      pMenuCreate;
static MenuToggle_t      pMenuToggle;
static MenuAction_t      pMenuAction;
static MenuStatus_t      pMenuStatus;
static MenuHint_t        pMenuHint;

/* ---- settings ---------------------------------------------- */

static HINSTANCE g_inst;
static char      g_iniPath[MAX_PATH];
static uint32_t  g_menu;
static volatile int g_enabled = 1;
static int g_tickMs = 1000;
static int g_windowMs = 5000;        /* machine window cadence      */
static int g_slotMs = 30000;         /* sub object windows cadence  */
static int g_windowBytes = 768;      /* covers the +0x260 state ptr */
static int g_slotBytes = 256;
static int g_scanMs = 15000;         /* PVP entity scan cadence     */
static int g_scanRadius = 200;
static int g_hotkey = VK_F8;
/* The Ghost War item's place on the main menu, in the 1920 x 1080
 * reference space the UI API uses. Recorded once, held in the ini: the
 * engine's own menu tree cannot be read before the world loads, so the
 * item is identified by where it sits instead. -1 = not recorded yet. */
static int g_gwX = -1;
static int g_gwY = -1;
static int g_gwR = 70;               /* hit radius, same units      */
static int g_gwSet;
static int g_calHotkey = VK_F9;

/* The loaded file channel: the candidate archive names, the cadence
 * and the discover switch.  They live up here because LoadSettings
 * fills them long before the scan itself is reached. */
#define FILE_MAX_CAND 32
#define FILE_MAX_DISC 96

static char g_cand[FILE_MAX_CAND][64];
static int  g_candCount;
static int  g_discover;
static int  g_fileScanMs;            /* 0 = on a dump only */

/* The menu labels worth watching, from ui_find. Declared here because
 * both LoadSettings and the UI tree walk below want them. */
#define UI_FIND_MAX 4
#define UI_FIND_LEN 32

static char g_uiFind[UI_FIND_MAX][UI_FIND_LEN];
static int  g_uiFindCount;

static const int kSlots = 17;        /* GameFlow sub objects 0..16  */

/* ---- the log, append only ---------------------------------- */

static FILE *g_log;
static volatile LONG g_logBusy;

static void OpenLog(void) {
    char dir[MAX_PATH];
    char path[MAX_PATH];
    char *slash;
    size_t n;

    if (g_log) return;
    /* The main module is GRW.exe, so its folder is the game dir. */
    if (!GetModuleFileNameA(NULL, dir, MAX_PATH)) return;
    slash = strrchr(dir, '\\');
    if (!slash) return;
    slash[1] = 0;
    n = strlen(dir);
    if (n + 24 >= sizeof(dir)) return;
    strcpy(dir + n, "logs");
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof(path), "%s\\ModeProbe.log", dir);
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

/* ---- settings file ----------------------------------------- */

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

static int IniInt(const char *key, int def) {
    if (!g_iniPath[0]) return def;
    return GetPrivateProfileIntA("Settings", key, def, g_iniPath);
}

static void IniSaveInt(const char *key, int v) {
    char buf[24];

    if (!g_iniPath[0]) return;
    snprintf(buf, sizeof(buf), "%d", v);
    WritePrivateProfileStringA("Settings", key, buf, g_iniPath);
}

static void LoadSettings(void) {
    g_enabled    = IniInt("enabled", 1) ? 1 : 0;
    g_tickMs     = IniInt("tick_ms", 1000);
    g_windowMs   = IniInt("window_ms", 5000);
    g_slotMs     = IniInt("slot_window_ms", 30000);
    g_windowBytes = IniInt("window_bytes", 768);
    g_slotBytes  = IniInt("slot_bytes", 256);
    g_scanMs     = IniInt("scan_ms", 15000);
    g_scanRadius = IniInt("scan_radius", 200);
    g_fileScanMs = IniInt("file_scan_ms", 0);
    g_discover   = IniInt("discover_forge", 0) ? 1 : 0;
    g_hotkey     = IniInt("hotkey", VK_F8);
    g_calHotkey  = IniInt("cal_hotkey", VK_F9);
    g_gwX        = IniInt("gw_x", -1);
    g_gwY        = IniInt("gw_y", -1);
    g_gwR        = IniInt("gw_r", 70);
    g_gwSet      = (g_gwX >= 0 && g_gwY >= 0) ? 1 : 0;
    if (g_gwR < 5) g_gwR = 5;
    if (g_tickMs < 100) g_tickMs = 100;
    if (g_windowBytes < 16) g_windowBytes = 16;
    if (g_windowBytes > 4096) g_windowBytes = 4096;
    if (g_slotBytes < 16) g_slotBytes = 16;
    if (g_slotBytes > 2048) g_slotBytes = 2048;
    if (g_scanRadius < 1) g_scanRadius = 1;
}

/* ---- sampling ---------------------------------------------- */

/* The signature is the diff surface: fixed field order, hashes and
 * masks only, so the same column can be compared across modes. */
static void SampleSignature(int marked) {
    int st = pGetGameState ? pGetGameState() : -1;
    uint32_t hash = pGetGameStateHash ? pGetGameStateHash() : 0u;
    char stname[64] = "";
    uint32_t ui = pGetUiState ? pGetUiState() : 0u;
    int ctx = pInputContext ? pInputContext() : -1;
    ShPlayer pl;
    int hasPl = pGetPlayer ? pGetPlayer(&pl) : 0;
    uint64_t flow = pGameFlow ? pGameFlow() : 0;
    uint64_t hyb = pHybridMenu ? pHybridMenu() : 0;
    uint32_t hybh = (hyb && pClassHash) ? pClassHash(hyb) : 0u;
    char slots[256];
    int i, n = 0;

    if (pGetGameStateName) pGetGameStateName(stname, (int)sizeof(stname));
    for (i = 0; i < kSlots; i++) {
        uint64_t o = pGameFlowObject ? pGameFlowObject(i) : 0;
        uint32_t h = (o && pClassHash) ? pClassHash(o) : 0u;
        n += snprintf(slots + n, sizeof(slots) - (size_t)n,
                      "%s%08X", i ? "," : "", h);
        if (n >= (int)sizeof(slots) - 12) break;
    }
    PLog("[SIG]%s st=%d/%s hash=%08X ui=0x%04X ctx=%d ply=%d "
         "flow=%llX hyb=%llX/%08X slot=%s",
         marked ? " MARK" : "", st, stname[0] ? stname : "?",
         hash, ui, ctx, hasPl,
         (unsigned long long)flow, (unsigned long long)hyb, hybh, slots);
}

/* The drawn scene set is the other per mode signature, and it is a
 * long string: written when it changes, and always on a dump so that
 * a marked moment carries the whole set. */
static void SampleScenes(int force) {
    static char last[2048];
    char buf[2048];
    int n;

    if (!pScenes) return;
    n = pScenes(buf, (int)sizeof(buf));
    if (n <= 0) return;
    if (!force && strcmp(buf, last) == 0) return;
    strncpy(last, buf, sizeof(last) - 1);
    last[sizeof(last) - 1] = 0;
    PLog("[SCN]%s count=%d %s", force ? " MARK" : "", n, buf);
}

/* The read ledger, straight from the framework: the exact list of
 * .forge files this session has read. This is the file channel that
 * matters - a mode that mounts an archive of its own reads a name the
 * other modes never read, and nothing has to be scanned to see it. */
static void SampleArchives(int force) {
    static char last[1536];
    char buf[1536];
    int n, i;
    size_t at = 0;

    if (!pForgeReadCount || !pForgeReadName) return;
    n = pForgeReadCount();
    buf[0] = 0;
    for (i = 0; i < n; i++) {
        const char *nm = pForgeReadName(i);
        size_t len;

        if (!nm || !nm[0]) continue;
        len = strlen(nm);
        if (at + len + 2 >= sizeof(buf)) break;
        if (at) buf[at++] = ' ';
        memcpy(buf + at, nm, len);
        at += len;
        buf[at] = 0;
    }
    if (!force && strcmp(buf, last) == 0) return;
    strncpy(last, buf, sizeof(last) - 1);
    last[sizeof(last) - 1] = 0;
    PLog("[ARC]%s read=%d %s", force ? " MARK" : "", n,
         buf[0] ? buf : "(none)");
}

static void DumpWindow(const char *tag, uint64_t addr, int bytes) {
    int off;

    if (!addr) { PLog("[WIN] %s none", tag); return; }
    if (!pReadBytes) { PLog("[WIN] %s %llX (no reader)", tag,
                           (unsigned long long)addr); return; }
    PLog("[WIN] %s@%llX bytes=%d", tag, (unsigned long long)addr, bytes);
    for (off = 0; off < bytes; off += 16) {
        uint8_t b[16];
        char hex[64], asc[24];
        int i, hn = 0, an = 0;

        if (!pReadBytes(addr + (uint64_t)off, b, 16)) {
            PLog("  %04X: <unreadable>", off);
            continue;
        }
        for (i = 0; i < 16; i++) {
            hn += snprintf(hex + hn, sizeof(hex) - (size_t)hn, "%02X ", b[i]);
            an += snprintf(asc + an, sizeof(asc) - (size_t)an, "%c",
                           (b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.');
        }
        PLog("  %04X: %s %s", off, hex, asc);
    }
}

/* Every candidate window, on the slow cadence or when asked. */
static void DumpWindows(int includeSlots) {
    uint64_t flow = pGameFlow ? pGameFlow() : 0;
    uint64_t state = 0;
    int i, ok = 0;

    DumpWindow("flow", flow, g_windowBytes);
    if (flow && pReadU64)
        state = pReadU64(flow + 0x260, &ok);   /* the state object */
    if (ok) DumpWindow("state", state, g_windowBytes);
    /* The shell. Its class is the same in every mode, but it is the
     * object that knows which lobby it is drawing, so it earns a
     * window of its own. */
    if (pHybridMenu) DumpWindow("hybrid", pHybridMenu(), g_windowBytes);
    if (!includeSlots) return;
    for (i = 0; i < kSlots; i++) {
        uint64_t o = pGameFlowObject ? pGameFlowObject(i) : 0;
        char tag[16];
        if (!o) continue;
        snprintf(tag, sizeof(tag), "slot%02d", i);
        DumpWindow(tag, o, g_slotBytes);
    }
}

/* The PVP only entity names (PVPHostage, PVPPrisonCell) are a mode
 * signal in their own right, but ShFindEntities reads health for
 * every candidate it keeps, so this runs on its own thread and never
 * on the sampling one. */
static volatile LONG g_scanBusy;

static int InGame(void) {
    return pIsInGame && pIsInGame();
}

/* The set of entity classes present is a fingerprint of what kind of
 * world this is: a PvP map carries objectives a campaign map never has
 * (PVPHostage and friends), and a campaign map carries civilians and
 * mission props a PvP map never has. Written only when it changes, so
 * a mode transition shows up as one line. */
static void LogEntityClasses(const ShEntity *e, int n) {
    static char last[768];
    char list[768];
    int i, count = 0;
    size_t at = 0;

    list[0] = 0;
    for (i = 0; i < n; i++) {
        const char *nm = e[i].name;
        size_t len;

        if (!nm[0]) continue;
        if (strstr(list, nm)) continue;      /* crude; the names are distinct */
        len = strlen(nm);
        if (at + len + 2 >= sizeof(list)) break;
        if (at) list[at++] = ' ';
        memcpy(list + at, nm, len);
        at += len;
        list[at] = 0;
        count++;
    }
    if (strcmp(list, last) == 0) return;
    strncpy(last, list, sizeof(last) - 1);
    last[sizeof(last) - 1] = 0;
    PLog("[ENT] ent=%d classes=%d %s", n, count, list[0] ? list : "(none)");
}

static DWORD WINAPI ScanThread(LPVOID p) {
    ShEntity found[128];
    char names[256];
    int n, i, pvp = 0, players = 0, mates = 0;
    size_t at = 0;

    (void)p;
    names[0] = 0;
    if (!pFindEntities || !InGame()) {
        PLog("[PVP] skipped (not in game)");
        InterlockedExchange(&g_scanBusy, 0);
        return 0;
    }
    /* Other humans in the world.  This is deliberately NOT read as a
     * PvP signal: PvE is four player co-op too, so a count above one
     * only says the session is not solo.  It is sampled because one
     * split would still be decisive - whether co-op files the other
     * humans under SH_KIND_TEAMMATE while PvP keeps them under
     * SH_KIND_PLAYER.  A radius of 0 means the whole map. */
    n = pFindEntities(SH_KIND_PLAYER, 0.0f, 0, found, 128);
    players = n;
    n = pFindEntities(SH_KIND_TEAMMATE, (float)g_scanRadius, 0, found, 128);
    mates = n;
    n = pFindEntities(SH_KIND_ANY, (float)g_scanRadius, 0, found, 128);
    for (i = 0; i < n; i++) {
        if (!strstr(found[i].name, "PVP")) continue;
        pvp++;
        if (at + 24 < sizeof(names)) {
            at += (size_t)snprintf(names + at, sizeof(names) - at, "%s ",
                                   found[i].name);
        }
    }
    PLog("[PVP] humans=%d teammates=%d radius=%d ent=%d pvpNamed=%d %s",
         players, mates, g_scanRadius, n, pvp, at ? names : "");
    LogEntityClasses(found, n);
    InterlockedExchange(&g_scanBusy, 0);
    return 0;
}

static void ScanAsync(void) {
    if (InterlockedCompareExchange(&g_scanBusy, 1, 0) == 0) {
        if (!CreateThread(NULL, 0, ScanThread, NULL, 0, NULL))
            InterlockedExchange(&g_scanBusy, 0);
    }
}

/* ---- loaded file probe -------------------------------------
 *
 * What this can and cannot answer, measured rather than assumed:
 *
 *   - The engine READS its archives, it does not map them: 47 .forge
 *     opens and 646 reads in one session, and not one
 *     CreateFileMapping or MapViewOfFile on a .forge
 *     (docs/forge-mod-loader.md).  So a mapped file name never shows a
 *     forge.  [MAP] is kept because it is one cheap walk and would
 *     catch it if that ever changed.
 *   - The exact answer already exists in the framework: the forge I/O
 *     layer resolves every read handle to a path, and that is what the
 *     read ledger records (ShForgeReadCount/Name/Seen, the [ARC]
 *     line).  [ARC] is the channel to read as "which archives did this
 *     session load".
 *   - The name-in-memory scan below is the FALLBACK, for a name that is
 *     not in the ledger.  It is off by default for two reasons.  A full
 *     scan of a loaded session walks about 5 GB and took 21 seconds.
 *     And a name found this way only proves the string is somewhere in
 *     memory: the engine builds the path before it opens a file and
 *     caches archive catalogues, so a name can be present in a mode that
 *     never loaded it.  GhostRoom.forge is the example - it is the
 *     menu/lobby archive, not a PvP one, and it was found while sitting
 *     in the main menu.
 *
 * The candidates come from the ini (files=..., comma separated, empty
 * by default).  discover_forge=1 also reports every *.forge name the
 * scan runs into, which is how a mode specific archive can be found
 * without guessing at its name.
 */

static volatile LONG g_fileBusy;

/* kernel32 on Vista and up, declared here so neither build has to
 * link psapi. */
DWORD WINAPI K32GetMappedFileNameA(HANDLE, LPVOID, LPSTR, DWORD);

static int MemUsable(DWORD prot) {
    if (prot & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    switch (prot & 0xFFu) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return 1;
    }
    return 0;
}

static const unsigned char *MemFind(const unsigned char *p, size_t len,
                                    const void *needle, size_t nl) {
    const unsigned char *end = p + len;
    const unsigned char *n = (const unsigned char *)needle;

    if (!nl || len < nl) return 0;
    while ((size_t)(end - p) >= nl) {
        const unsigned char *q =
            memchr(p, n[0], (size_t)(end - p) - nl + 1);
        if (!q) return 0;
        if (memcmp(q, n, nl) == 0) return q;
        p = q + 1;
    }
    return 0;
}

static size_t ToWide(const char *s, wchar_t *out, int max) {
    int i;

    for (i = 0; s[i] && i < max - 1; i++) {
        out[i] = (wchar_t)(unsigned char)s[i];
    }
    out[i] = 0;
    return (size_t)i;
}

static int NameChar(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
           c == '\\' || c == '/';
}

static int EndsWithForge(const unsigned char *s, size_t n) {
    static const char ext[] = ".forge";
    int i;

    if (n < 6) return 0;
    for (i = 0; i < 6; i++) {
        unsigned char c = s[n - 6 + (size_t)i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if (c != (unsigned char)ext[i]) return 0;
    }
    return 1;
}

/* Space separated, deduplicated by substring: the names are
 * distinctive enough that this is cheaper than a real set. */
static void ListAdd(char *list, size_t cap, const char *name) {
    if (!name[0]) return;
    if (strstr(list, name)) return;
    if (strlen(list) + strlen(name) + 2 >= cap) return;
    if (list[0]) strcat(list, " ");
    strcat(list, name);
}

static void DiscAdd(char disc[][64], int *count, int max,
                    const unsigned char *s, size_t n) {
    char name[64];
    const unsigned char *b = s;
    size_t i;

    for (i = 0; i < n; i++) {
        if (s[i] == '\\' || s[i] == '/') b = s + i + 1;
    }
    n -= (size_t)(b - s);
    if (!EndsWithForge(b, n)) return;
    if (n > 63) n = 63;
    memcpy(name, b, n);
    name[n] = 0;
    for (i = 0; (int)i < *count; i++) {
        if (strcmp(disc[i], name) == 0) return;
    }
    if (*count >= max) return;
    strcpy(disc[*count], name);
    (*count)++;
}

/* One buffer's worth of the search.  The candidates are counted, the
 * first address of each is kept - that address is where the pointer
 * chain to the engine's own archive list can be picked up later - and
 * with discovery on, every *.forge base name around a hit is kept. */
static void ScanBuffer(const unsigned char *p, size_t len,
                       int *ascii, int *wide, uintptr_t *first,
                       char disc[][64], int *discCount) {
    const unsigned char *cur, *end;
    int i;

    for (i = 0; i < g_candCount; i++) {
        size_t nl = strlen(g_cand[i]);
        wchar_t w[64];
        size_t wl;

        if (!nl) continue;
        if (MemFind(p, len, g_cand[i], nl)) {
            ascii[i]++;
            if (!first[i]) first[i] = (uintptr_t)p;
        }
        wl = ToWide(g_cand[i], w, 64);
        if (wl && MemFind(p, len, w, wl * 2)) wide[i]++;
    }
    if (!g_discover) return;
    cur = p;
    end = p + len;
    while (cur < end && *discCount < FILE_MAX_DISC) {
        const unsigned char *q = MemFind(cur, (size_t)(end - cur),
                                         ".forge", 6);
        const unsigned char *s, *e;

        if (!q) break;
        s = q;
        e = q + 6;
        while (s > p && NameChar(s[-1]) && (size_t)(q - s) < 63) s--;
        while (e < end && NameChar(*e) && (size_t)(e - q) < 63) e++;
        if (s == q) break;                   /* nothing before ".forge" */
        DiscAdd(disc, discCount, FILE_MAX_DISC, s, (size_t)(e - s));
        cur = q + 6;
    }
}

/* One pass over the address space.  Called on its own thread: the
 * private memory of a loaded session is gigabytes, so this takes
 * about a second and must never sit on the sampling path.
 *
 * The read goes through ReadProcessMemory on our own process rather
 * than a plain dereference.  It is a little slower per byte, but a
 * page the engine frees mid scan makes it fail instead of faulting,
 * and a probe that can take the client down in a live match is not
 * worth the speed. */
#define FILE_CHUNK (1u << 20)

static uint8_t g_chunk[FILE_CHUNK];

static void FileScanOnce(void) {
    int hitAscii[FILE_MAX_CAND], hitWide[FILE_MAX_CAND];
    uintptr_t hitFirst[FILE_MAX_CAND];
    char disc[FILE_MAX_DISC][64];
    char discList[1024];
    char mapList[1024];
    char candList[512];
    static char lastMap[1024];
    SYSTEM_INFO si;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr;
    uint64_t scanned = 0, t0;
    int regions = 0, i, discCount = 0;

    memset(hitAscii, 0, sizeof(hitAscii));
    memset(hitWide, 0, sizeof(hitWide));
    memset(hitFirst, 0, sizeof(hitFirst));
    discList[0] = 0;
    mapList[0] = 0;
    candList[0] = 0;
    t0 = GetTickCount64();

    GetSystemInfo(&si);
    addr = (uintptr_t)si.lpMinimumApplicationAddress;
    while (addr < (uintptr_t)si.lpMaximumApplicationAddress) {
        if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) break;
        if (!mbi.RegionSize) break;
        if (mbi.State == MEM_COMMIT && MemUsable(mbi.Protect)) {
            if (mbi.Type == MEM_MAPPED) {
                char path[MAX_PATH];
                if (K32GetMappedFileNameA(GetCurrentProcess(),
                                          mbi.BaseAddress, path,
                                          MAX_PATH)) {
                    const char *b = strrchr(path, '\\');
                    b = b ? b + 1 : path;
                    if (strstr(b, ".forge")) ListAdd(mapList, sizeof(mapList), b);
                }
            } else if (mbi.Type == MEM_PRIVATE && mbi.RegionSize >= 4096) {
                size_t len = (size_t)mbi.RegionSize;
                size_t done = 0;

                while (done < len) {
                    size_t want = len - done;
                    uintptr_t src = (uintptr_t)mbi.BaseAddress + done;

                    if (want > FILE_CHUNK) want = FILE_CHUNK;
                    if (ReadProcessMemory(GetCurrentProcess(),
                                          (LPCVOID)src, g_chunk, want, NULL)) {
                        ScanBuffer(g_chunk, want, hitAscii, hitWide,
                                   hitFirst, disc, &discCount);
                    }
                    done += want;     /* a refused chunk is skipped, not fatal */
                }
                scanned += len;
                regions++;
            }
        }
        addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }

    for (i = 0; i < g_candCount; i++) {
        ListAdd(candList, sizeof(candList), g_cand[i]);
        PLog("[FILE] %s ascii=%d wide=%d first=%llX", g_cand[i],
             hitAscii[i], hitWide[i], (unsigned long long)hitFirst[i]);
    }
    if (g_discover) {
        for (i = 0; i < discCount; i++) ListAdd(discList, sizeof(discList), disc[i]);
        PLog("[FILE] forge names seen (%d): %s", discCount,
             discList[0] ? discList : "(none)");
    }
    PLog("[FILE] scan regions=%d mb=%llu ms=%llu cand=%s", regions,
         (unsigned long long)(scanned >> 20),
         (unsigned long long)(GetTickCount64() - t0),
         candList[0] ? candList : "(none)");
    if (mapList[0] || lastMap[0]) {
        if (strcmp(mapList, lastMap) != 0) {
            strncpy(lastMap, mapList, sizeof(lastMap) - 1);
            lastMap[sizeof(lastMap) - 1] = 0;
            PLog("[MAP] %s", mapList[0] ? mapList : "(none)");
        }
    }
}

static DWORD WINAPI FileScanThread(LPVOID p) {
    (void)p;
    FileScanOnce();
    InterlockedExchange(&g_fileBusy, 0);
    return 0;
}

static void FileScanAsync(void) {
    if (InterlockedCompareExchange(&g_fileBusy, 1, 0) == 0) {
        if (!CreateThread(NULL, 0, FileScanThread, NULL, 0, NULL))
            InterlockedExchange(&g_fileBusy, 0);
    }
}

static void LoadCandidates(void) {
    char buf[512];
    char *p;

    g_candCount = 0;
    buf[0] = 0;
    if (g_iniPath[0]) {
        GetPrivateProfileStringA("Settings", "files", "", buf,
                                 sizeof(buf), g_iniPath);
    }
    for (p = strtok(buf, ",; \t"); p && g_candCount < FILE_MAX_CAND;
         p = strtok(NULL, ",; \t")) {
        size_t n = strlen(p);
        if (n < 4 || n > 63) continue;
        memcpy(g_cand[g_candCount], p, n);
        g_cand[g_candCount][n] = 0;
        g_candCount++;
    }
    /* No candidates means no memory scan at all, and that is the
     * default: [ARC] answers "was this archive read" exactly and for
     * free, so the 5 GB walk is only ever run when someone asks for a
     * name the ledger does not have. */
}

/* The labels to watch in the game's own menus. The main menu names each
 * mode on its own button, so this is how the Ghost War item is found
 * without hardcoding where it sits on the screen. */
static void LoadUiFind(void) {
    char buf[256];
    char *p;

    g_uiFindCount = 0;
    buf[0] = 0;
    if (g_iniPath[0]) {
        GetPrivateProfileStringA("Settings", "ui_find", "Ghost,Mercenar", buf,
                                 sizeof(buf), g_iniPath);
    }
    for (p = strtok(buf, ",;"); p && g_uiFindCount < 4; p = strtok(NULL, ",;")) {
        while (*p == ' ' || *p == '\t') p++;
        if (!p[0] || strlen(p) >= sizeof(g_uiFind[0])) continue;
        strcpy(g_uiFind[g_uiFindCount++], p);
    }
}

/* ---- object graph digest -------------------------------------------
 *
 * The flow machine is identical in the Ghost War lobby and in the
 * campaign menu (measured), so the mode has to live in one of the
 * objects the shell holds rather than in the flow. Chasing that by hand
 * means dumping every candidate; this hashes them instead and writes
 * one line per root, so two marked dumps can be compared root by root
 * and only the root that really moved has to be looked at.
 *
 *   own  = hash of the root's own first 256 bytes (its fields)
 *   kids = hash over the objects its first pointer fields point at,
 *          which is what catches a mode kept one level down
 *
 * Written only on a marked dump: the digests follow heap churn, so on
 * the tick they would say nothing and fill the log.
 */
static uint32_t HashBytes(uint64_t addr, int len) {
    uint8_t buf[256];
    uint32_t h = 2166136261u;
    int i, n = len > (int)sizeof(buf) ? (int)sizeof(buf) : len;

    if (!pReadBytes || !addr || n <= 0) return 0;
    if (!pReadBytes(addr, buf, (uint32_t)n)) return 0;
    for (i = 0; i < n; i++) h = (h ^ buf[i]) * 16777619u;
    return h ? h : 1u;
}

static uint32_t HashKids(uint64_t obj, int words) {
    uint8_t buf[512];
    uint32_t h = 2166136261u;
    int i, used = 0;

    if (!pReadBytes || !obj) return 0;
    if (!pReadBytes(obj, buf, sizeof(buf))) return 0;
    for (i = 0; i + 8 <= (int)sizeof(buf) && used < words; i += 8) {
        uint64_t v;
        uint32_t ch;

        memcpy(&v, buf + i, 8);
        /* Anything outside these bounds is a number, not an object. */
        if (v < 0x10000u || v > 0x7FFFFFFFFFFFULL) continue;
        ch = HashBytes(v, 128);
        if (!ch) continue;
        h = (h ^ ch) * 16777619u;
        used++;
    }
    return h ? h : 1u;
}

static void SampleDigest(void) {
    static const char *kSlotTag[17] = {
        "s00", "s01", "s02", "s03", "s04", "s05", "s06", "s07", "s08",
        "s09", "s10", "s11", "s12", "s13", "s14", "s15", "s16"
    };
    uint64_t roots[20];
    const char *tag[20];
    char buf[1100];
    int nt = 0, i;
    size_t at = 0;

    if (!pReadBytes) return;

    if (pGameFlow) {
        uint64_t flow = pGameFlow();
        tag[nt] = "flow";
        roots[nt++] = flow;
        if (flow && pReadU64) {
            int ok = 0;
            uint64_t st = pReadU64(flow + 0x260, &ok);
            if (ok && st) { tag[nt] = "state"; roots[nt++] = st; }
        }
    }
    if (pHybridMenu) {
        uint64_t hy = pHybridMenu();
        if (hy) { tag[nt] = "hybrid"; roots[nt++] = hy; }
    }
    for (i = 0; i < kSlots && nt < 19; i++) {
        uint64_t o = pGameFlowObject ? pGameFlowObject(i) : 0;
        if (!o) continue;
        tag[nt] = kSlotTag[i];
        roots[nt++] = o;
    }

    for (i = 0; i < nt; i++) {
        at += (size_t)snprintf(buf + at, sizeof(buf) - at, "%s%s=%08X/%08X",
                               i ? " " : "", tag[i],
                               HashBytes(roots[i], 256),
                               HashKids(roots[i], 16));
        if (at >= sizeof(buf) - 24) break;
    }
    buf[sizeof(buf) - 1] = 0;
    PLog("[DIG] %s", buf);
}

/* ---- the game's own UI tree ----------------------------------------
 *
 * The main menu is where the mode is chosen, so what the menu itself
 * says about the item being used is the most direct evidence there is.
 * The framework can read the game's drawn scenes and their widget tree
 * (ShGameScene* / ShSceneRoot / ShWidget*), including each label's text,
 * its class and its geometry, so a button can be found by name instead
 * of by a hardcoded screen position - which would break with the
 * resolution, the UI scale and the next patch.
 *
 * This channel logs, on a marked dump, every text-bearing widget of the
 * drawn scenes (capped) and keeps hold of the ones whose text matches
 * ui_find. Those matched widgets are then polled twice a second and a
 * line is written whenever their state vector changes, which is what
 * shows a hover, a click or a confirm: mouse, pad and Enter all end up
 * as some property of the item moving.
 */
#define UI_MAX_MATCH  8
#define UI_MAX_LINES  140
#define UI_MAX_DEPTH  10

static uint64_t g_uiMatch[UI_MAX_MATCH];
static char     g_uiMatchText[UI_MAX_MATCH][80];
static char     g_uiMatchLast[UI_MAX_MATCH][256];
static int      g_uiLines;

static int UiMatchI(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    const char *p;

    if (!n) return 0;
    for (p = hay; *p; p++) {
        size_t i;
        for (i = 0; i < n; i++) {
            char c = p[i], d = needle[i];
            if (!c) return 0;
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (d >= 'A' && d <= 'Z') d += 'a' - 'A';
            if (c != d) break;
        }
        if (i == n) return 1;
    }
    return 0;
}

/* One property as text, trying the types in turn: the same call shape
 * covers floats, uints and the vec2/vec3 the engine's own ids use. */
static int UiPropText(uint64_t w, uint32_t prop, char *out, int cap) {
    uint32_t u = 0;
    float f = 0.0f, v[3] = { 0.0f, 0.0f, 0.0f };

    out[0] = 0;
    if (pWidgetGetU && pWidgetGetU(w, prop, &u))
        return snprintf(out, (size_t)cap, "u%X", u);
    if (pWidgetGetF && pWidgetGetF(w, prop, &f))
        return snprintf(out, (size_t)cap, "f%.5g", f);
    if (pWidgetGetV && pWidgetGetV(w, prop, v, 2))
        return snprintf(out, (size_t)cap, "v%.4g,%.4g", v[0], v[1]);
    if (pWidgetGetV && pWidgetGetV(w, prop, v, 3))
        return snprintf(out, (size_t)cap, "v3%.4g,%.4g,%.4g", v[0], v[1], v[2]);
    return 0;
}

/* The state vector of one widget: every property id the engine classes
 * actually carry, whatever type it turns out to be. */
static void UiStateVector(uint64_t w, char *out, int cap) {
    static const uint32_t props[] = { 0x01, 0x05, 0x06, 0x07, 0x09,
                                      0x0A, 0x0C, 0x0F, 0x35, 0x3E };
    size_t at = 0;
    int i;

    out[0] = 0;
    for (i = 0; i < (int)(sizeof(props) / sizeof(props[0])); i++) {
        char val[64];
        if (!UiPropText(w, props[i], val, sizeof(val))) continue;
        at += (size_t)snprintf(out + at, (size_t)cap - at, "%s%02X=%s",
                               at ? " " : "", props[i], val);
        if (at >= (size_t)cap - 40) break;
    }
}

static void UiRemember(uint64_t w, const char *text) {
    int i, free = -1;

    for (i = 0; i < UI_MAX_MATCH; i++) {
        if (g_uiMatch[i] == w) return;
        if (!g_uiMatch[i] && free < 0) free = i;
    }
    if (free < 0) return;
    g_uiMatch[free] = w;
    strncpy(g_uiMatchText[free], text, sizeof(g_uiMatchText[0]) - 1);
    g_uiMatchText[free][sizeof(g_uiMatchText[0]) - 1] = 0;
    g_uiMatchLast[free][0] = 0;
}

static void UiWalk(uint64_t w, int depth, const char *scene, float px,
                   float py) {
    char cls[64], text[256];
    float pos[3] = { 0.0f, 0.0f, 0.0f }, size[2] = { 0.0f, 0.0f };
    uint32_t vis = 0;
    int n, i;

    if (!w || depth > UI_MAX_DEPTH || g_uiLines > UI_MAX_LINES) return;

    cls[0] = 0;
    text[0] = 0;
    if (pWidgetClass) pWidgetClass(w, cls, sizeof(cls));
    if (pWidgetGetS) pWidgetGetS(w, 0x08 /* SH_P_TEXT */, text, sizeof(text));
    if (pWidgetGetV) pWidgetGetV(w, 0x01 /* SH_P_POSITION */, pos, 3);
    if (pWidgetGetV) pWidgetGetV(w, 0x0F /* SH_P_SIZE */, size, 2);
    if (pWidgetGetU) pWidgetGetU(w, 0x07 /* SH_P_VISIBLE */, &vis);

    if (text[0]) {
        PLog("[UITXT] %s cls=%s vis=%u at=(%.0f,%.0f) size=(%.0f,%.0f) \"%s\"",
             scene, cls, vis, px + pos[0], py + pos[1], size[0], size[1],
             text);
        g_uiLines++;
        for (i = 0; i < g_uiFindCount; i++) {
            if (UiMatchI(text, g_uiFind[i])) {
                PLog("[UIFIND] \"%s\" cls=%s vis=%u at=(%.0f,%.0f) "
                     "size=(%.0f,%.0f) match=%s",
                     text, cls, vis, px + pos[0], py + pos[1], size[0],
                     size[1], g_uiFind[i]);
                UiRemember(w, text);
                break;
            }
        }
    }

    n = pWidgetChildCount ? pWidgetChildCount(w) : 0;
    for (i = 0; i < n && g_uiLines <= UI_MAX_LINES; i++) {
        UiWalk(pWidgetChildAt ? pWidgetChildAt(w, i) : 0, depth + 1, scene,
               px + pos[0], py + pos[1]);
    }
}

/* Which scenes the game drew, and - for the ones worth it - the text of
 * every widget in them. Called on a marked dump only: walking the tree
 * is thousands of property reads, fine once, wasteful per tick. */
static void SampleGameUi(void) {
    char buf[600];
    int n, i;
    size_t at = 0;

    if (!pGameSceneCount || !pWidgetClass) return;
    n = pGameSceneCount();
    buf[0] = 0;
    for (i = 0; i < n && i < 10; i++) {
        uint64_t s = pGameSceneAt(i);
        char nm[64];
        nm[0] = 0;
        if (pGameSceneName) pGameSceneName(s, nm, sizeof(nm));
        at += (size_t)snprintf(buf + at, sizeof(buf) - at, "%s%s",
                               i ? "," : "", nm[0] ? nm : "(unnamed)");
        if (at >= sizeof(buf) - 40) break;
    }
    PLog("[UI] scenes=%d %s", n, buf[0] ? buf : "(none)");

    g_uiLines = 0;
    for (i = 0; i < n && i < 10 && g_uiLines <= UI_MAX_LINES; i++) {
        uint64_t s = pGameSceneAt(i);
        char nm[64];
        nm[0] = 0;
        if (pGameSceneName) pGameSceneName(s, nm, sizeof(nm));
        UiWalk(pSceneRoot ? pSceneRoot(s) : 0, 0, nm[0] ? nm : "?", 0.0f, 0.0f);
    }
}

/* The scene list the framework hands out is what it saw being drawn,
 * and that bookkeeping (the render hook) is only installed when one of
 * OUR phoenix scenes exists - so without this the main menu, which
 * draws no scene of ours, always reports zero scenes and the game's own
 * widget tree can never be reached. A hidden empty scene is enough to
 * install it; it draws nothing. Creation needs the engine's UI manager,
 * which may not be up at the very start, so it is retried once a
 * second until it takes.
 */
static uint32_t g_probeScene;
static int      g_sceneTried;

static void EnsureScene(void) {
    if (g_probeScene) return;
    if (!pUiSceneCreate) return;
    g_probeScene = pUiSceneCreate("ModeProbe", -1);
    if (g_probeScene) {
        if (pUiSceneShow) pUiSceneShow(g_probeScene, 0);
        PLog("scene: hidden scene %u created - now the drawn scenes show up",
             g_probeScene);
        return;
    }
    if (!g_sceneTried) {
        g_sceneTried = 1;
        PLog("scene: create failed for now (UI manager not up yet); retrying");
    }
}

/* The matched items, twice a second: a hover, a click or a confirm all
 * move some property of the item, and this is what makes that visible
 * without guessing which one. */
static void UiWatchTick(void) {
    int i;

    for (i = 0; i < UI_MAX_MATCH; i++) {
        char vec[256];
        if (!g_uiMatch[i]) continue;
        UiStateVector(g_uiMatch[i], vec, sizeof(vec));
        if (!vec[0]) {
            g_uiMatch[i] = 0;                 /* the widget is gone */
            continue;
        }
        if (strcmp(vec, g_uiMatchLast[i]) == 0) continue;
        strncpy(g_uiMatchLast[i], vec, sizeof(g_uiMatchLast[0]) - 1);
        g_uiMatchLast[i][sizeof(g_uiMatchLast[0]) - 1] = 0;
        PLog("[UIW] \"%s\" %s", g_uiMatchText[i], vec);
    }
}

/* One complete picture, for a marked moment. */
static void DumpAll(const char *why) {
    PLog("---- dump: %s ----", why);
    SampleSignature(1);
    SampleScenes(1);
    SampleArchives(1);
    SampleDigest();
    SampleGameUi();
    DumpWindows(1);
    ScanAsync();
    FileScanAsync();
}

/* ---- menu -------------------------------------------------- */

/* Defined with the hotkey thread below; the menu records a point too,
 * for a user who would rather use the menu than a key. */
static void PointerRef(int *x, int *y);
static void RecordGwPoint(int x, int y);

static void OnCalibrate(uint32_t menu, uint32_t item, int value,
                        void *user) {
    int x, y;

    (void)menu; (void)item; (void)value; (void)user;
    PointerRef(&x, &y);
    RecordGwPoint(x, y);
}

static void OnEnabled(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    g_enabled = value ? 1 : 0;
    IniSaveInt("enabled", g_enabled);
    PLog("menu: probe=%s", g_enabled ? "on" : "off");
}

static void OnDump(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)value; (void)user;
    DumpAll("menu");
}

static void OnFiles(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)value; (void)user;
    PLog("---- dump: files (menu) ----");
    FileScanAsync();
}

static void RefreshStatus(void) {
    char text[128];
    int st = pGetGameState ? pGetGameState() : -1;
    char stname[48] = "";
    uint32_t hash = pGetGameStateHash ? pGetGameStateHash() : 0u;

    if (!g_menu || !pMenuStatus) return;
    if (pGetGameStateName) pGetGameStateName(stname, (int)sizeof(stname));
    snprintf(text, sizeof(text), "%s (st %d), hash %08X",
             stname[0] ? stname : "?", st, hash);
    pMenuStatus(g_menu, text);
}

static void BuildMenu(void) {
    if (!pMenuCreate || !pMenuToggle) return;
    g_menu = pMenuCreate("Mode probe");
    if (!g_menu) return;
    pMenuToggle(g_menu, "Probe enabled", g_enabled, OnEnabled, NULL);
    pMenuAction(g_menu, "Dump now", OnDump, NULL);
    pMenuAction(g_menu, "Scan files now", OnFiles, NULL);
    pMenuAction(g_menu, "Record Ghost War item here", OnCalibrate, NULL);
    if (pMenuHint)
        pMenuHint(g_menu,
                  "Samples the GameFlow objects, the shell, the scene set, "
                  "the PVP entity names and the loaded archives into "
                  "logs\\ModeProbe.log, one line per sample. Play one mode "
                  "per session, then press the dump key (F8) to mark the "
                  "moment. Nothing is changed.");
    RefreshStatus();
}

/* ---- threads ----------------------------------------------- */

/* Pointer in the same 1920 x 1080 reference space the UI API uses, so
 * a recorded point survives a windowed or resized game. */
static void PointerRef(int *x, int *y) {
    HWND w = GetForegroundWindow();
    POINT p;
    RECT rc;

    GetCursorPos(&p);
    *x = p.x;
    *y = p.y;
    if (!w || !GetClientRect(w, &rc)) return;
    ScreenToClient(w, &p);
    if (rc.right > 0 && rc.bottom > 0) {
        *x = (int)((float)p.x * 1920.0f / (float)rc.right);
        *y = (int)((float)p.y * 1080.0f / (float)rc.bottom);
    }
}

static int InGwRegion(int x, int y) {
    int dx, dy;

    if (!g_gwSet) return 0;
    dx = x - g_gwX;
    dy = y - g_gwY;
    return (dx * dx + dy * dy) <= g_gwR * g_gwR;
}

static void RecordGwPoint(int x, int y) {
    char buf[24];

    g_gwX = x;
    g_gwY = y;
    g_gwSet = 1;
    if (g_iniPath[0]) {
        snprintf(buf, sizeof(buf), "%d", x);
        WritePrivateProfileStringA("Settings", "gw_x", buf, g_iniPath);
        snprintf(buf, sizeof(buf), "%d", y);
        WritePrivateProfileStringA("Settings", "gw_y", buf, g_iniPath);
    }
    PLog("calibrate: Ghost War item = (%d,%d) radius=%d, saved", x, y, g_gwR);
}

/* The dump key: edge detected, held bit only, the framework's own UI
 * input thread consumes the "pressed" bit first (see docs/ui.md).
 * The same thread watches the pointer and the left button, which is
 * what the Ghost War item is recognised by. */
static DWORD WINAPI HotkeyThread(LPVOID p) {
    int prev = 0, calPrev = 0, clickPrev = 0;
    uint64_t at = 0;

    (void)p;
    for (;;) {
        int x, y;

        Sleep(30);
        PointerRef(&x, &y);

        if (g_hotkey > 0) {
            int down = (GetAsyncKeyState(g_hotkey) & 0x8000) != 0;
            if (down && !prev && GetTickCount64() - at >= 500u) {
                at = GetTickCount64();
                DumpAll("hotkey");
            }
            prev = down;
        } else {
            prev = 0;
        }

        /* F9 over the item records where it is. Nothing else is needed
         * to identify it: the menu layout does not move on its own. */
        if (g_calHotkey > 0) {
            int down = (GetAsyncKeyState(g_calHotkey) & 0x8000) != 0;
            if (down && !calPrev) RecordGwPoint(x, y);
            calPrev = down;
        } else {
            calPrev = 0;
        }

        {
            int down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (down && !clickPrev) {
                PLog("[CLICK] at=(%d,%d) gw=%d", x, y, InGwRegion(x, y));
            }
            clickPrev = down;
        }
    }
    return 0;
}

static DWORD WINAPI TickThread(LPVOID p) {
    uint64_t lastWin = 0, lastSlot = 0, lastScan = 0, lastFile = 0;

    (void)p;
    for (;;) {
        uint64_t now;

        Sleep((DWORD)g_tickMs);
        if (!g_enabled) continue;
        now = GetTickCount64();

        SampleSignature(0);
        SampleScenes(0);
        SampleArchives(0);
        EnsureScene();
        UiWatchTick();

        if (g_windowMs > 0 && now - lastWin >= (uint64_t)g_windowMs) {
            lastWin = now;
            DumpWindows(0);
        }
        if (g_slotMs > 0 && now - lastSlot >= (uint64_t)g_slotMs) {
            lastSlot = now;
            DumpWindows(1);
        }
        if (g_scanMs > 0 && now - lastScan >= (uint64_t)g_scanMs) {
            lastScan = now;
            ScanAsync();
        }
        if (g_fileScanMs > 0 && now - lastFile >= (uint64_t)g_fileScanMs) {
            lastFile = now;
            FileScanAsync();
        }
        RefreshStatus();
    }
    return 0;
}

static DWORD WINAPI InitThread(LPVOID p) {
    HMODULE m = NULL;
    (void)p;

    OpenLog();
    ResolveIniPath();

    PLog("--- ModeProbe: the play mode, from the outside ---");
    PLog("build " __DATE__ " " __TIME__);
    PLog("start: cmdline=%s", GetCommandLineA());
    PLog("ini=%s", g_iniPath[0] ? g_iniPath : "(none)");

    while (!m) {
        m = GetModuleHandleA("dinput8.dll");
        if (!m) Sleep(500);
    }
    *(FARPROC *)&pGetVersion       = GetProcAddress(m, "ShGetVersion");
    *(FARPROC *)&pIsInGame         = GetProcAddress(m, "ShIsInGame");
    *(FARPROC *)&pGetGameState     = GetProcAddress(m, "ShGetGameState");
    *(FARPROC *)&pGetGameStateName = GetProcAddress(m, "ShGetGameStateName");
    /* Declared nowhere in scripthook.h, but exported: the framework
     * itself reaches it the same way. */
    *(FARPROC *)&pGetGameStateHash =
        GetProcAddress(m, "ShGetGameStateHash");
    *(FARPROC *)&pGetUiState       = GetProcAddress(m, "ShGetUiState");
    *(FARPROC *)&pGameFlow         = GetProcAddress(m, "ShGameFlow");
    *(FARPROC *)&pGameFlowObject   = GetProcAddress(m, "ShGameFlowObject");
    *(FARPROC *)&pHybridMenu       = GetProcAddress(m, "ShHybridMenu");
    *(FARPROC *)&pClassHash        = GetProcAddress(m, "ShReflectClassHash");
    *(FARPROC *)&pScenes           = GetProcAddress(m, "ShGameScenes");
    *(FARPROC *)&pInputContext     = GetProcAddress(m, "ShInputContext");
    *(FARPROC *)&pGetPlayer        = GetProcAddress(m, "ShGetPlayer");
    *(FARPROC *)&pFindEntities     = GetProcAddress(m, "ShFindEntities");
    *(FARPROC *)&pReadBytes        = GetProcAddress(m, "ShReadBytes");
    *(FARPROC *)&pReadU64          = GetProcAddress(m, "ShReadU64");
    *(FARPROC *)&pForgeReadCount   = GetProcAddress(m, "ShForgeReadCount");
    *(FARPROC *)&pForgeReadName    = GetProcAddress(m, "ShForgeReadName");
    *(FARPROC *)&pGameSceneCount   = GetProcAddress(m, "ShGameSceneCount");
    *(FARPROC *)&pGameSceneAt      = GetProcAddress(m, "ShGameSceneAt");
    *(FARPROC *)&pGameSceneName    = GetProcAddress(m, "ShGameSceneName");
    *(FARPROC *)&pSceneRoot        = GetProcAddress(m, "ShSceneRoot");
    *(FARPROC *)&pWidgetChildCount = GetProcAddress(m, "ShWidgetChildCount");
    *(FARPROC *)&pWidgetChildAt    = GetProcAddress(m, "ShWidgetChildAt");
    *(FARPROC *)&pWidgetClass      = GetProcAddress(m, "ShWidgetClass");
    *(FARPROC *)&pWidgetGetS       = GetProcAddress(m, "ShWidgetGetS");
    *(FARPROC *)&pWidgetGetU       = GetProcAddress(m, "ShWidgetGetU");
    *(FARPROC *)&pWidgetGetF       = GetProcAddress(m, "ShWidgetGetF");
    *(FARPROC *)&pWidgetGetV       = GetProcAddress(m, "ShWidgetGetV");
    *(FARPROC *)&pUiSceneCreate    = GetProcAddress(m, "ShUiSceneCreate");
    *(FARPROC *)&pUiSceneShow      = GetProcAddress(m, "ShUiSceneShow");
    *(FARPROC *)&pMenuCreate       = GetProcAddress(m, "ShMenuCreate");
    *(FARPROC *)&pMenuToggle       = GetProcAddress(m, "ShMenuToggle");
    *(FARPROC *)&pMenuAction       = GetProcAddress(m, "ShMenuAction");
    *(FARPROC *)&pMenuStatus       = GetProcAddress(m, "ShMenuStatus");
    *(FARPROC *)&pMenuHint         = GetProcAddress(m, "ShMenuHint");

    if (!pGetGameState || !pGameFlow || !pClassHash || !pReadBytes) {
        PLog("bind failed: state=%p flow=%p class=%p read=%p - giving up",
             (void *)pGetGameState, (void *)pGameFlow,
             (void *)pClassHash, (void *)pReadBytes);
        return 1;
    }
    while (!pGetVersion || !pGetVersion()) Sleep(500);

    LoadSettings();
    LoadCandidates();
    PLog("config: enabled=%d tick=%d window=%d/%dB slot=%d/%dB "
         "scan=%d/%dm file=%d discover=%d hotkey=%d",
         g_enabled, g_tickMs, g_windowMs, g_windowBytes,
         g_slotMs, g_slotBytes, g_scanMs, g_scanRadius, g_fileScanMs,
         g_discover, g_hotkey);
    if (g_candCount) {
        int ci;
        for (ci = 0; ci < g_candCount; ci++)
            PLog("watch file: %s", g_cand[ci]);
    }
    LoadUiFind();
    if (g_uiFindCount) {
        int k;
        for (k = 0; k < g_uiFindCount; k++)
            PLog("watch ui label: %s", g_uiFind[k]);
    }
    PLog("bound: state=%p name=%p hash=%p ui=%p flow=%p slot=%p "
         "hybrid=%p class=%p scenes=%p ctx=%p player=%p find=%p "
         "read=%p menu=%p",
         (void *)pGetGameState, (void *)pGetGameStateName,
         (void *)pGetGameStateHash, (void *)pGetUiState,
         (void *)pGameFlow, (void *)pGameFlowObject,
         (void *)pHybridMenu, (void *)pClassHash, (void *)pScenes,
         (void *)pInputContext, (void *)pGetPlayer,
         (void *)pFindEntities, (void *)pReadBytes, (void *)pMenuCreate);

    BuildMenu();
    DumpAll("startup");

    CreateThread(NULL, 0, TickThread, NULL, 0, NULL);
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
