#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "scripthook.h"
#include "plugin_scan.h"

/* The play mode module (scripthook_playmode.c): not a plugin facing
 * export beyond ShSelectedPlayMode and friends, so it is declared here. */
extern void ShPlayModeStart(void);
/* The overlay's switch (scripthook_ovl.cpp), for the same reason: it is the
 * loader's config that decides it, and the overlay thread waits for the
 * answer before doing anything. */
extern void ShOvlAllow(int on);

typedef HRESULT (WINAPI *DirectInput8Create_t)(
    HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
/* The sixth export of the system dinput8, and the one every proxy forgets.
 * Its prototype is deliberately not declared: it is either
 * "no arguments, returns a pointer to the standard joystick data format" or
 * "one out-parameter, returns an HRESULT", and the x64 ABI makes a
 * four-integer-argument, pointer-sized-return forward correct for both - the
 * extra arguments sit in registers the real function ignores, and the caller
 * only reads the part of the return the real function actually wrote. */
typedef void *(WINAPI *GetdfDIJoystick_t)(void *, void *, void *, void *);
typedef HRESULT (WINAPI *DllCanUnloadNow_t)(void);
typedef HRESULT (WINAPI *DllGetClassObject_t)(REFCLSID, REFIID, LPVOID *);
typedef HRESULT (WINAPI *DllRegisterServer_t)(void);
typedef HRESULT (WINAPI *DllUnregisterServer_t)(void);

static HMODULE g_realDinput8 = NULL;

/* The Windows SDK already declares DllCanUnloadNow and
 * DllGetClassObject, so dllexport on the definition is a
 * linkage mismatch under MSVC. Those builds export the five
 * proxy entry points through proxy.def instead; GCC keeps
 * the attribute. */
#ifdef _MSC_VER
#define SH_PROXY_EXPORT
#else
#define SH_PROXY_EXPORT __declspec(dllexport)
#endif

static DirectInput8Create_t   p_DirectInput8Create;
static GetdfDIJoystick_t      p_GetdfDIJoystick;
static DllCanUnloadNow_t      p_DllCanUnloadNow;
static DllGetClassObject_t    p_DllGetClassObject;
static DllRegisterServer_t    p_DllRegisterServer;
static DllUnregisterServer_t  p_DllUnregisterServer;

static void LoadRealDinput8(void) {
    char sysdir[MAX_PATH];
    UINT n = GetSystemDirectoryA(sysdir, MAX_PATH);
    /* The system dir fits easily, but never trust MAX_PATH math:
     * truncate instead of strcat past the buffer. */
    if (n == 0 || n > MAX_PATH - 14) {
        LogAlways("FATAL: system directory path too long");
        return;
    }
    memcpy(sysdir + n, "\\dinput8.dll", 13);

    g_realDinput8 = LoadLibraryA(sysdir);
    if (!g_realDinput8) {
        LogAlways("FATAL: could not load real dinput8.dll from %s", sysdir);
        return;
    }
    LogAlways("loaded real dinput8.dll from %s", sysdir);

    p_DirectInput8Create = (DirectInput8Create_t)
        GetProcAddress(g_realDinput8, "DirectInput8Create");
    p_GetdfDIJoystick = (GetdfDIJoystick_t)
        GetProcAddress(g_realDinput8, "GetdfDIJoystick");
    p_DllCanUnloadNow = (DllCanUnloadNow_t)
        GetProcAddress(g_realDinput8, "DllCanUnloadNow");
    p_DllGetClassObject = (DllGetClassObject_t)
        GetProcAddress(g_realDinput8, "DllGetClassObject");
    p_DllRegisterServer = (DllRegisterServer_t)
        GetProcAddress(g_realDinput8, "DllRegisterServer");
    p_DllUnregisterServer = (DllUnregisterServer_t)
        GetProcAddress(g_realDinput8, "DllUnregisterServer");
}

extern void ShStateStartup(void);
extern void ShNpcShutdown(void);
extern void ShCrashStartup(void);
extern void ShCoreFixStartup(void);
extern void ShCoreFixLateStartup(void);
extern void ShModSettingsStartup(void);
/* Temporary, until the mod loader replaces it: the evidence round that
 * records how the engine reads .forge. See scripthook_forgeprobe.c. */
extern void ShForgeProbeStartup(void);
/* Forge Mod Loader: reads [forgemod], scans mods\ and registers its
 * menu. The file I/O half installs from the loader thread too. */
extern void ShForgeStartup(void);
/* The shared hook layer: one MinHook state and one hook per target for
 * the whole DLL, which is what lets a plugin ask for a hook instead of
 * carrying MinHook of its own. It has no work of its own until a plugin
 * asks, and the lock it hands out has to exist before the first ask -
 * which can come from any thread at any time after this. */
extern void ShHookStartup(void);

/* Where plugins live, and what a plugin's name is, belongs to plugin_scan.h
 * now: the settings page and the plugin blacklist need the same answer and
 * used to walk plugins\ themselves, each with its own copy of the rule. The
 * scan below is the one that reads it; the other two ask for the result.
 *
 * A plugin loads unless a [plugins] line says otherwise: NO LINE MEANS ON, so
 * a plugin that was just added - dropped in by the player, or shipped by an
 * update - works without a trip to the menu first, which is the case that used
 * to cost the most (a plugin folder that is there and silently doing nothing
 * reads as a broken plugin). The scan writes nothing at all: an earlier version
 * wrote the missing line back as a 1, which made the section grow one line per
 * folder under plugins\ - a list of lines nobody had chosen, each of them
 * saying only what the absent line already said. A line in there is now
 * something a person put there: the Plugins page writes the 0 that keeps one
 * off, and a 1 it writes is a plugin switched back on. Nothing about loading
 * changed - only what the file holds at the end of a session.
 *
 * Deleting scripthook.ini is still the reset, and what it resets to is now
 * "everything in plugins\ loads" - what ruling a plugin out really takes is the
 * menu switch, or taking its folder away.
 */

/* ---- discovery: what is under plugins\ -------------------------------
 *
 * The one reader of plugins\. See plugin_scan.h for what the layout is, what
 * a plugin's name is, and why this is not in each of the three callers.
 *
 * The scan runs once and is kept: the table is filled inside the lock on the
 * first call and only read afterwards, so a reader takes no lock and the
 * pointer it is handed stays good for the life of the process. `info` is kept
 * too, not just recomputed for the caller that happened to scan - the loader
 * reads its counts for the "N skipped" line, and the blacklist gets there
 * first at start up, so a caller that only saw `count` would print a
 * different summary depending on which module asked first.
 */
static ShPluginEnt      g_pluginTable[SH_PLUGIN_MAX];
static ShPluginScanInfo g_pluginInfo;
static volatile LONG    g_pluginScanned;
static CRITICAL_SECTION g_pluginLock;
static volatile LONG    g_pluginLockReady;

/* The lock is made by whoever gets here first, as the shared hook layer's log
 * lock is: this runs on the start up path where two threads can arrive at the
 * same moment (the loader thread and the first thing that asks), and the
 * window is a few instructions wide. */
static void PluginLockInit(void) {
    static volatile LONG once;

    if (InterlockedCompareExchange(&once, 1, 0) == 0) {
        InitializeCriticalSection(&g_pluginLock);
        InterlockedExchange(&g_pluginLockReady, 1);
    }
    while (!g_pluginLockReady) Sleep(0);
}

/* What the log and the [plugins] section have always been built in: by name,
 * so two launches load in the same order and a menu row does not move because
 * the file system enumerated a folder differently.
 *
 * The comparison is strcmp, not _stricmp, and that is deliberate: this is the
 * order [plugins] lines and menu rows have been in since before the scan was
 * one function, and it puts capitals first. The name's *identity* is
 * case-insensitive (see the collision pass below) - the order it is read in
 * is not, so nothing about an existing install's menu moves.
 *
 * Ties, which only exist when two files carry the same name: the one nearer
 * to plugins\ first (the root beats a folder), then the path - case-
 * insensitively, as Windows compares paths, and then case-sensitively so that
 * two files differing only by case are still ordered by the path itself and
 * not by whichever the file system listed first.
 *
 * The depth is a field and not one strchr over `rel`: it is a fact the scan
 * knows when it takes the file, and reading it back out of a path - which can
 * be truncated for a folder with a very long name - would make the sort depend
 * on how much of the path fitted. */
static int PluginCmp(const ShPluginEnt *a, const ShPluginEnt *b) {
    int c = strcmp(a->name, b->name);

    if (c) return c;
    if (a->depth != b->depth) return a->depth - b->depth;  /* shallower first */
    c = _stricmp(a->asi, b->asi);
    if (c) return c;
    return strcmp(a->asi, b->asi);
}

static void PluginSort(ShPluginEnt *a, int n) {
    int i;

    for (i = 1; i < n; i++) {
        ShPluginEnt tmp = a[i];
        int j = i;

        while (j > 0 && PluginCmp(&a[j - 1], &tmp) > 0) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = tmp;
    }
}

/* One file, as an entry. `sub` is "" at the root of plugins\ and "<folder>\\"
 * one level in.
 *
 *   1  taken
 *   0  not a plugin at all: not an .asi, or a stem that cannot be a name
 *      (empty, as ".asi" alone would be, or longer than the table holds - a
 *      truncated name could not be the key of a [plugins] line)
 *  -1  it is a plugin, but a path built for it does not fit (see below)
 *
 * snprintf reports truncation only through its return, and the paths here are
 * the ones LoadLibraryA is handed - so a return that says "the buffer was too
 * small" has to be a failure, not a silent short path. The distinction
 * matters: before the layout allowed a plugin in a shared folder, a path that
 * did not fit came back truncated and the file was simply not found, so the
 * folder read as "no plugin in it". That answer was wrong but harmless; now
 * the caller is told, because a plugin that is announced and then does not
 * load is the hardest kind of report to act on. */
static int PluginTake(ShPluginEnt *e, const char *pluginsDir, const char *sub,
                      const char *file) {
    const char *dot = strrchr(file, '.');
    size_t stem;

    if (!dot || _stricmp(dot, ".asi") != 0) return 0;
    stem = (size_t)(dot - file);
    if (stem == 0 || stem >= sizeof(e->name)) return 0;

    memcpy(e->name, file, stem);
    e->name[stem] = 0;
    if (snprintf(e->dir, sizeof(e->dir), "%s%s", pluginsDir, sub) >=
            (int)sizeof(e->dir) ||
        snprintf(e->asi, sizeof(e->asi), "%s%s%s", pluginsDir, sub, file) >=
            (int)sizeof(e->asi) ||
        snprintf(e->rel, sizeof(e->rel), "%s%s", sub, file) >=
            (int)sizeof(e->rel))
        return -1;

    /* How many folders deep the file is, which is what decides a name
     * collision. A field rather than a strchr over `rel`, so it cannot be
     * lost to a long folder name - see PluginCmp. */
    e->depth = sub[0] ? 1 : 0;
    return 1;
}

/* One candidate, offered to the raw table and counted. The counting is here
 * because all three outcomes (not a plugin, does not fit, table full) have to
 * come out of the same place for the numbers in ShPluginScanInfo to add up:
 * found = count + dupes + overflow. */
static void PluginOffer(ShPluginEnt *table, int *raw, const char *pluginsDir,
                        const char *sub, const char *file) {
    ShPluginEnt e;
    int taken = PluginTake(&e, pluginsDir, sub, file);

    if (taken == 0) return;
    g_pluginInfo.found++;
    if (taken < 0) {
        Log("plugins\\%s%s: the path is too long for this build, not loaded",
            sub, file);
        g_pluginInfo.overflow++;
        return;
    }
    if (*raw >= SH_PLUGIN_MAX) {
        Log("plugins\\%s%s: scan table full (%d), not loaded",
            sub, file, SH_PLUGIN_MAX);
        g_pluginInfo.overflow++;
        return;
    }
    table[(*raw)++] = e;
}

int ShPluginScan(const ShPluginEnt **ents, ShPluginScanInfo *info) {
    char pluginsDir[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATAA fd, kidFd;
    HANDLE h, kidH;
    int raw = 0, i;

    if (info) memset(info, 0, sizeof(*info));
    if (!ents) return 0;

    PluginLockInit();
    EnterCriticalSection(&g_pluginLock);

    if (g_pluginScanned) {
        LeaveCriticalSection(&g_pluginLock);
        if (info) *info = g_pluginInfo;
        *ents = g_pluginTable;
        return g_pluginInfo.count;
    }

    if (!ShPluginsDir(pluginsDir, sizeof(pluginsDir))) {
        LogAlways("cannot find the game directory");
        goto done;
    }

    /* Fresh install: make the folder the player is told to fill, rather than
     * reporting that there is nowhere to put a plugin. It used to be made by
     * the loader just before the scan; the scan is the thing that needs it
     * existing, and the blacklist gets here first, so it is made here. */
    {
        char dir[MAX_PATH];
        size_t len = strlen(pluginsDir);

        if (len > 0 && pluginsDir[len - 1] == '\\') len--;
        memcpy(dir, pluginsDir, len);
        dir[len] = 0;
        CreateDirectoryA(dir, NULL);
    }

    if (snprintf(pat, sizeof(pat), "%s*", pluginsDir) >= (int)sizeof(pat))
        goto done;

    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        LogAlways("no plugins in %s", pluginsDir);
        goto done;
    }

    /* One walk of plugins\. A file is a candidate; a folder is looked into
     * once, and its own files are the candidates one level in. Nothing deeper
     * is opened - plugins\a\b\x.asi is not looked for at all.
     *
     * The inner listing has its own handle and buffer, so the outer walk keeps
     * its place in `fd` while that runs. */
    do {
        char kid[MAX_PATH], kidPat[MAX_PATH];

        if (fd.cFileName[0] == '.') continue;

        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            PluginOffer(g_pluginTable, &raw, pluginsDir, "", fd.cFileName);
            continue;
        }

        if (snprintf(kid, sizeof(kid), "%s\\", fd.cFileName) >=
            (int)sizeof(kid))
            continue;
        if (snprintf(kidPat, sizeof(kidPat), "%s%s*", pluginsDir, kid) >=
            (int)sizeof(kidPat))
            continue;

        kidH = FindFirstFileA(kidPat, &kidFd);
        if (kidH == INVALID_HANDLE_VALUE) continue;
        do {
            if (kidFd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (kidFd.cFileName[0] == '.') continue;
            PluginOffer(g_pluginTable, &raw, pluginsDir, kid, kidFd.cFileName);
        } while (FindNextFileA(kidH, &kidFd));
        FindClose(kidH);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    PluginSort(g_pluginTable, raw);

    /* Settle the names, and in the order the sort produced - so the winner of
     * a collision is the shallower path, then the one that sorts first, and
     * the table that comes out is still in name order.
     *
     * The comparison is _stricmp - the identity is case-insensitive, because
     * it is a [plugins] key, a menu row and a lang owner, and Windows would
     * not keep two files apart on case either. That is also why this looks at
     * every entry that was kept rather than only the one before it: the sort
     * above is case-sensitive, so "Foo" and "foo" are not necessarily
     * neighbours ("FooBar" sorts between them). The table is at most 64 long,
     * so the whole search costs nothing.
     *
     * Compacting in place: `count` never passes `i`, because it only advances
     * on the entries that are kept. */
    for (i = 0; i < raw; i++) {
        ShPluginEnt *cand = &g_pluginTable[i];
        const ShPluginEnt *owner = NULL;
        int k;

        for (k = 0; k < g_pluginInfo.count; k++)
            if (_stricmp(g_pluginTable[k].name, cand->name) == 0) {
                owner = &g_pluginTable[k];
                break;
            }
        if (owner) {
            Log("plugins\\%s: REFUSED - the name \"%s\" is already "
                "plugins\\%s. Two plugins cannot share a name: the [plugins] "
                "switch, the menu row and the lang owner are one slot each",
                cand->rel, cand->name, owner->rel);
            g_pluginInfo.dupes++;
            continue;
        }
        g_pluginTable[g_pluginInfo.count++] = *cand;
    }

done:
    g_pluginScanned = 1;
    LeaveCriticalSection(&g_pluginLock);
    if (info) *info = g_pluginInfo;
    *ents = g_pluginTable;
    return g_pluginInfo.count;
}

const ShPluginEnt *ShPluginFind(const char *name) {
    const ShPluginEnt *ents;
    int n, i;

    if (!name || !name[0]) return NULL;
    n = ShPluginScan(&ents, NULL);
    for (i = 0; i < n; i++)
        if (_stricmp(ents[i].name, name) == 0) return &ents[i];
    return NULL;
}

/* ---- the API a plugin asks for --------------------------------------
 *
 * SH_REQUIRES_API (scripthook.h) is published as an exported number, and it is
 * read here with the image mapped but NOT initialised: with
 * DONT_RESOLVE_DLL_REFERENCES the loader maps the file and leaves its exports
 * readable without calling DllMain - no threads, nothing of the plugin has run
 * yet, which is the only way to refuse one before it starts. The probe is
 * unmapped again immediately, so the real LoadLibrary below is a fresh load
 * that does start it.
 *
 * A plugin that declares nothing reads 0 here and is loaded as it always was:
 * silence means "no requirement", never "whatever is newest". A plugin that is
 * switched off in scripthook.ini never reaches this at all - the check sits
 * after that gate, so a plugin the player turned off is not probed, not
 * counted against anything and not announced.
 */
static uint32_t PluginNeedsApi(const char *path) {
    HMODULE mod;
    uint32_t *p;
    uint32_t need = 0;

    mod = LoadLibraryExA(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!mod) return 0;
    p = (uint32_t *)GetProcAddress(mod, "ShRequiresApi");
    if (p) need = *p;
    FreeLibrary(mod);
    return need;
}

/* The refusals, kept until something can be seen: plugins load long before the
 * overlay draws, and a toast lives for seconds, so a line put up here would
 * have expired before the first frame of anything was on screen. */
#define API_REFUSED_MAX 8

static struct {
    char     name[64];
    uint32_t need;
} g_refusedApi[API_REFUSED_MAX];
static volatile LONG g_refusedApiN;

static void RefusedApiAdd(const char *name, uint32_t need) {
    LONG i = InterlockedIncrement(&g_refusedApiN) - 1;

    if (i < 0 || i >= API_REFUSED_MAX) return;
    snprintf(g_refusedApi[i].name, sizeof(g_refusedApi[i].name), "%s", name);
    g_refusedApi[i].need = need;
}

/* Said once the screen can show it: ShDrawReady is the overlay saying it is
 * attached and drawing, and before that a toast is a line nobody sees. The
 * wait is bounded, so a build with no overlay leaves no thread sleeping. */
static DWORD WINAPI RefusedApiNotice(LPVOID p) {
    LONG n, i;
    int waited;

    (void)p;
    for (waited = 0; waited < 240 && !ShDrawReady(); waited++) Sleep(500);
    n = InterlockedCompareExchange(&g_refusedApiN, 0, 0);
    if (n > API_REFUSED_MAX) n = API_REFUSED_MAX;
    for (i = 0; i < n; i++) {
        const char *en = ShTextEnUS(NULL, "@toast.api.refused");
        const char *tr = ShLang("@toast.api.refused");
        char asi[80], line[256];

        snprintf(asi, sizeof(asi), "%s.asi", g_refusedApi[i].name);
        ShTextFormat(line, sizeof(line), en ? en : tr, tr, asi,
                     (int)g_refusedApi[i].need, ShGetVersion());
        ShToastEx(line, 0xFFD24Au, 8000);
    }
    return 0;
}

static void LoadASIPlugins(void) {
    const ShPluginEnt *ents;
    ShPluginScanInfo   info;
    uint32_t need;
    int count, n = 0, nSkipped = 0, nDefaultOn = 0, i;

    if (!ShConfigGetBool("loader", "load_plugins", 1)) {
        LogAlways("plugin loading disabled in scripthook.ini");
        return;
    }

    /* What is installed is not this function's question any more - one scan
     * answers it for the loader, the settings page and the blacklist alike
     * (plugin_scan.h). The entries arrive sorted by name, so the log and the
     * [plugins] section come out in the same order on every launch, which is
     * what the sort here used to be for. */
    count = ShPluginScan(&ents, &info);

    /* A plugin over the ceiling, and one whose name another file already has,
     * are skipped before anything is decided - the scan counted and logged
     * them on the way past. */
    nSkipped = info.overflow + info.dupes;

    /* Decide, and write nothing. -1 is "no line at all" (ShConfigGetInt
     * cannot tell that apart from a 0, which is why this asks for an int),
     * and a plugin with no line is ON: a plugin that was just added works
     * without a trip to the menu first, and the ini is left as the player
     * wrote it. A 0 put there by the Plugins page is what keeps one off from
     * then on; the default is counted so the scan's summary says how many
     * plugins are on that way, without a line each to say it. */
    for (i = 0; i < count; i++) {
        const char *name = ents[i].name;
        int on = ShConfigGetInt("plugins", name, -1);

        if (on < 0) {
            on = 1;
            nDefaultOn++;
        }
        if (!on) {
            Log("plugin disabled in scripthook.ini: %s", name);
            nSkipped++;
            continue;
        }

        /* What this plugin asks of the framework, read without running any of
         * it. Nothing declared is 0, and 0 is loaded as it always was. */
        need = PluginNeedsApi(ents[i].asi);
        if (need > (uint32_t)ShGetVersion()) {
            Log("plugins\\%s: REFUSED - it needs API %u and this framework is "
                "%u (SH_API_VERSION); load a newer ScriptHook, or an older "
                "build of this plugin", name, (unsigned)need,
                (unsigned)ShGetVersion());
            RefusedApiAdd(name, need);
            nSkipped++;
            continue;
        }

        n++;
        Log("loading plugin: plugins\\%s", ents[i].rel);
        {
            HMODULE mod = LoadLibraryA(ents[i].asi);
            if (mod)
                Log("  loaded at %p", (void *)mod);
            else
                Log("  FAILED (error %lu)", GetLastError());
        }
    }
    /* One toast per refused plugin, put up once the screen can show it. The
     * log already carries the reason; this is so the player who switched a
     * plugin on is told why nothing came of it. */
    if (InterlockedCompareExchange(&g_refusedApiN, 0, 0) > 0) {
        HANDLE t = CreateThread(NULL, 0, RefusedApiNotice, NULL, 0, NULL);

        if (t) CloseHandle(t);
    }
    LogAlways("plugin scan done: %d loaded, %d skipped, %d on by default "
              "(no [plugins] line, nothing written)", n, nSkipped, nDefaultOn);
}

/* Which game builds this framework has been run against, and what was
 * verified on each.
 *
 * Every engine entry point in this framework is a plain offset from the
 * module base (image.h), so a game update that moves code turns those
 * constants into something else - and some of them are called rather than
 * read, which is the failure that has nothing to report for itself. The two
 * numbers are how a build names itself, read straight out of the PE header
 * of the loaded module: the COFF TimeDateStamp and SizeOfImage.
 *
 * It is a report, not a gate. Which sites still hold is what the byte
 * comparisons in each module decide - scripthook_physics.c refuses to patch
 * a site that does not open as pinned and says so in its own log - so
 * refusing to run on an unlisted build would take a working install down
 * with it. What this line buys is the opposite: on a build nobody has
 * verified yet it is the first line of the log that explains the others, and
 * it carries the numbers to add here.
 *
 * The first entry is the build the RVAs were taken against, recorded by a
 * third-party plugin that carried the same gate (docs\opticacamo-reverse.md,
 * section on its build check). Every entry after it is a build this
 * framework has actually run on, carrying the sites that answered for
 * themselves - never a claim about the ones that cannot.
 */
typedef struct {
    uint32_t stamp;
    uint32_t image;
    const char *note;
} KnownBuild;

static const KnownBuild kKnownBuilds[] = {
    { 0x6A7C5143u, 0x18B09000u, "the build the engine RVAs were taken "
                                "against" },
    /* The shipped build moved on in 2026-09: a smaller image, a new stamp.
     * The sites that check themselves were read out of the log on it on
     * 2026-09-21 - the ray hook site, and the cast it calls. Nothing here
     * says the rest of the pinned RVAs hold on this build; only a run, and
     * only the ones that can answer for themselves, say that. */
    { 0x6A99768Au, 0x185BA000u, "2026-09-21: the physics ray site and its "
                                "cast were read back byte for byte" },
};

/* The build's two numbers, read out of the PE header of the loaded module
 * the first time they are asked for and kept: nothing under us can change a
 * header. 0 when there is no header to read. */
static int GameBuildNums(uint32_t *stamp, uint32_t *size) {
    static int      state;      /* 0 = not read, 1 = read, -1 = no header */
    static uint32_t gStamp, gSize;

    if (!state) {
        const IMAGE_DOS_HEADER *dos =
            (const IMAGE_DOS_HEADER *)(uintptr_t)GetModuleHandleA(NULL);
        const IMAGE_NT_HEADERS *nt;

        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) {
            state = -1;
        } else {
            nt = (const IMAGE_NT_HEADERS *)
                ((const uint8_t *)dos + (uint32_t)dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) {
                state = -1;
            } else {
                gStamp = nt->FileHeader.TimeDateStamp;
                gSize  = nt->OptionalHeader.SizeOfImage;
                state  = 1;
            }
        }
    }
    if (state < 0) return 0;
    *stamp = gStamp;
    *size  = gSize;
    return 1;
}

/* The index+1 of the entry these numbers are, or 0 for a build this
 * framework has not run on. */
static int BuildIsKnown(uint32_t stamp, uint32_t size) {
    int i;

    for (i = 0; i < (int)(sizeof(kKnownBuilds) / sizeof(kKnownBuilds[0])); i++)
        if (stamp == kKnownBuilds[i].stamp && size == kKnownBuilds[i].image)
            return i + 1;
    return 0;
}

static void ReportGameBuild(void) {
    uint32_t stamp, size;
    int i;

    if (!GameBuildNums(&stamp, &size)) {
        LogAlways("game build: no PE header - not the game module?");
        return;
    }
    i = BuildIsKnown(stamp, size);
    if (i)
        LogAlways("game build: %08X / %08X - %s", stamp, size,
                  kKnownBuilds[i - 1].note);
    else
        LogAlways("game build: %08X / %08X - NOT a build this framework has "
                  "run on (%08X / %08X is the one the RVAs were taken "
                  "against). A site that moved says so in its own module log; "
                  "these are the numbers to add here.",
                  stamp, size, kKnownBuilds[0].stamp, kKnownBuilds[0].image);
}

/* The game build for the About page: the two numbers as text, and whether
 * they are a build this framework has run on. 1 = listed, 0 = not listed,
 * -1 = nothing to report (no PE header, or no room for the string). */
int ShGameBuildText(char *buf, int cap) {
    uint32_t stamp, size;

    if (!buf || cap < 24) return -1;
    if (!GameBuildNums(&stamp, &size)) return -1;
    snprintf(buf, (size_t)cap, "%08X / %08X", stamp, size);
    return BuildIsKnown(stamp, size) ? 1 : 0;
}

/* Plugins load here, not in DllMain. LoadLibrary blocks on
 * the loader lock until DllMain returns, so a plugin binds
 * against a fully initialised DLL and may import it. */
static DWORD WINAPI LoaderThread(LPVOID p) {
    (void)p;
    ShConfigInit();
    LogAlways("config loaded from scripthook.ini");
    /* Before any hook layer installs: this is the line that explains the
     * rest of the log on a build nobody has verified. */
    ReportGameBuild();
    /* Which menu language this session runs in, and - on the one run that
     * picks it - where it came from. The config layer cannot write this
     * itself: a line logged while it is loading would ask it for the log
     * level, which is one of the settings it is loading. logs\scripthook.log
     * is the file a report arrives with, whatever [Settings] LogLevel says,
     * so the answer belongs here, beside the version and the plugin count. */
    {
        char note[200];
        if (ShLangPickLine(note, sizeof(note)))
            LogAlways("menu language: %s (%s)", ShLangGet(), note);
        else
            LogAlways("menu language: %s ([Settings] Language=)",
                      ShLangGet());
    }
    /* The overlay's own switch, read here because the overlay thread starts
     * from a static initialiser and waits for this answer: with
     * [loader] overlay=0 it does nothing at all - no factory capture, no
     * window scan, no Present hook, no subclass. It is the one switch that
     * takes the overlay out of the equation on its own, which is what
     * 2026-09-17's field round had no way to do. */
    ShOvlAllow(ShConfigGetBool("loader", "overlay", 1));
    /* One line saying how much this session's logs\ holds, so a folder
     * with only two files in it explains itself. */
    LogAlways("log level: %s - %s", LogLevelName(ShLogLevel()),
              ShLogLevel() >= LOG_INFO
                  ? "every module logs"
                  : "module logs off, plugin logs on");
    /* Watch state before plugins, so the world is resolved by the time any
     * of them ask. Here rather than in DllMain, where creating a thread
     * can deadlock against the loader lock. */
    ShStateStartup();
    /* Forge Mod Loader, evidence round: watch how the engine reads
     * .forge. Off unless [forgemod] probe=1, because both this and the
     * loader's own I/O hooks want ReadFile and MinHook keeps one hook
     * per target. Nothing is blocked, nothing is changed. */
    if (ShConfigGetBool("forgemod", "probe", 0))
        ShForgeProbeStartup();
    /* The CPU trims' stage thread: it puts the play dial in force once the
     * world is up (the processor-0 combinations among them) and keeps
     * track of the stage for the public CPU API. Started here rather than
     * in DllMain, where creating a thread can deadlock against the loader
     * lock, and it is not conditional: with every dial left alone it still
     * reports the stage.
     */
    ShCoreFixLateStartup();
    /* ScriptHook settings must be up before the plugin scan: it owns the
     * very switches that gate the plugins, so it has to exist even
     * when load_plugins=0 (otherwise nothing could turn them back on
     * from inside the game). Its root row carries weight 0, so it
     * sorts to the top no matter when the plugins register theirs. */
    ShModSettingsStartup();
    /* Forge Mod Loader: scan mods\ and build the per-archive overrides
     * before the plugins load, so the menu is up with the rest. */
    ShForgeStartup();
    /* The shared hook layer, before the plugins load: a plugin's first
     * ShHookAdd can come from any thread, and the registry lock has to
     * exist by then. The layer itself installs nothing until asked. */
    ShHookStartup();
    /* Which play mode the session is in: hooks the game's own mode
     * manager, so it has to be up before anything asks, and it is what
     * the plugin blacklist decides from. */
    ShPlayModeStart();
    /* The plugin blacklist: scans plugins\ and comes up before the
     * plugins load, so a plugin's declaration has somewhere to go. */
    ShBlacklistStartup();
    LoadASIPlugins();
    return 0;
}

static BOOL IsGRW(void) {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    return (_stricmp(name, "GRW.exe") == 0);
}

/* ---- the framework's own start up --------------------------------------
 *
 * Where this runs is not a detail; it is the difference between a session
 * that comes up and one that does not, and the two field reports say the
 * same thing from opposite sides.
 *
 * It used to run on the attach path, in DllMain, and that is where it is
 * again. It was moved off the attach path on 2026-09-17 to keep two
 * documented-unsafe things out of DllMain:
 *
 *   LoadLibraryA(system32\dinput8.dll) - a nested module load while the
 *   host's own import resolution still holds the loader lock, which can
 *   deadlock against another thread inside a module's init.
 *
 *   CreateThread(LoaderThread) - the new thread can start before the loader
 *   lock is released, then block on it, and everything it sets up waits
 *   behind that.
 *
 * The fix for both is not "later", it is "not inside DllMain": the thread is
 * created here, on the attach path, and the thread is what loads the real
 * dinput8, reads the config, installs the hook layers and loads the plugins.
 * A LoadLibrary on our own thread is an ordinary load under the loader lock,
 * not a nested one under our own DllMain, so the deadlock case is gone.
 *
 * "Later" - starting on the host's first call into our exports - was the
 * other answer, and it was wrong in a way only a machine can show: on the
 * development machine the host's first DirectInput call arrives about six
 * seconds in, by which time the engine's renderer and its threads are
 * already running. Every install then lands on a live process - 18 file
 * hooks, the factory vtable patch, playmode's and fpx's patches of the
 * game's own code, eight plugin DLLs - and every session black-screened at
 * the front end, while the same machine, game build and config ran the
 * 18:52 build (which installs from the attach path) fine.
 *
 * The exports still wait: the thread publishes "ready" only once the real
 * dinput8 answers, so a host thread that gets in first waits for it instead
 * of being handed a null pointer.
 */
static volatile LONG g_framework;   /* 0 = nothing, 1 = thread up, 2 = ready */

/* The thread that does the loading. Loading the real dinput8 here rather
 * than on the attach path is the half that removes the nested module load. */
static DWORD WINAPI FrameworkThread(LPVOID p) {
    (void)p;
    LoadRealDinput8();
    /* Published here and not before: an export that ran while this was in
     * flight is waiting on this word. */
    InterlockedExchange(&g_framework, 2);
    LoaderThread(NULL);
    return 0;
}

/* Called from DllMain. Never waits: the thread it creates cannot get past
 * its first LoadLibrary until this DllMain returns, so waiting here would be
 * a deadlock with ourselves. */
static void ShFrameworkStartEarly(void) {
    HANDLE h;

    if (InterlockedCompareExchange(&g_framework, 0, 0) != 0) return;
    if (InterlockedCompareExchange(&g_framework, 1, 0) != 0) return;
    h = CreateThread(NULL, 0, FrameworkThread, NULL, 0, NULL);
    if (h) {
        CloseHandle(h);      /* never waited on: prompt close */
        return;
    }
    /* No thread available. This is the one case that has to touch the loader
     * lock from the attach path - and a load that might nest is still better
     * than every export forwarding to a null pointer for the session. */
    LogAlways("loader: could not start the load thread - loading inline");
    FrameworkThread(NULL);
}

/* Called from every export. Waits for the real dinput8 to be resolved, so a
 * host thread that arrives first is served rather than short changed. */
static void ShFrameworkStart(void) {
    int i;

    ShFrameworkStartEarly();
    for (i = 0; i < 30000; i++) {           /* 30 s, then say so */
        if (InterlockedCompareExchange(&g_framework, 0, 0) == 2) return;
        Sleep(1);
    }
    LogAlways("loader: the real dinput8 did not answer in 30 s - input "
              "forwarding will fail");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        if (!IsGRW()) return TRUE;
        LogInit("scripthook.log");
        /* Which process and thread is calling, and nothing else - none of
         * these three calls may touch the loader, the heap or the
         * environment, since this runs behind the loader lock.
         *
         * The pid is what a start up report gets read for, because a second
         * start up in the same log is not a second load: the session of
         * 2026-09-25 01:38 carried the whole sequence twice, four seconds
         * apart, out of the launcher's process and the game's - a different
         * pid each time, where a second image of this dll would have to show
         * the same one. The module base rides on the banner for the same
         * question and is the weaker half of it: two processes started the
         * same way were measured to get the SAME base (00007FFB31560000
         * both), so the address cannot tell them apart on its own. The
         * thread id is what says which thread inside the process got here. */
        LogAlways("loader: attach pid=%lu tid=%lu",
                  (unsigned long)GetCurrentProcessId(),
                  (unsigned long)GetCurrentThreadId());
        /* The two lines a report is always asked for, so they are written
         * whatever [Settings] LogLevel says - including none. */
        LogAlways("GRW ScriptHook " SH_VERSION " at %p", (void *)inst);
        LogAlways("built " __DATE__ " " __TIME__);

        /* Armed first, so a crash during our own start up
         * is reported too.
         */
        ShCrashStartup();
        /* CPU core-count/affinity fix. Must run before the
         * engine reads the processor count, so it goes here,
         * on the attach path, not in the loader thread.
         * Everything it does comes from the [loader] cpu_boot /
         * cpu_window / cpu_play / cpu_cores and cpu_prio_* dials;
         * with all of them left alone it installs no hook and
         * touches not one scheduling API.
         */
        ShCoreFixStartup();
        /* The framework's thread, started here so every install is in place
         * before the host's own threads reach the code they patch - the file
         * hooks, the factory vtable patch, playmode's and fpx's patches of
         * the game's own code, the plugin DLLs. DllMain itself only creates
         * the thread: the real dinput8 load, the config, the hook layers and
         * the plugins all happen on it, which is what keeps a nested module
         * load out of the attach path. See the note above
         * ShFrameworkStartEarly for why this cannot be deferred to the host's
         * first call into our exports. */
        ShFrameworkStartEarly();
    } else if (reason == DLL_PROCESS_DETACH) {
        /* Handed back, not left to the OS: the pump event is the one kernel
         * object the framework holds that outlives its users. */
        ShNpcShutdown();
        if (!g_logFile) return TRUE;
        Log("unloading");
        LogClose();
        /* No FreeLibrary(g_realDinput8) here: this also runs on
         * process exit under the loader lock, where freeing a
         * system dll the process still references can deadlock.
         * The OS unmaps everything anyway. */
    }
    return TRUE;
}

extern void ShWrapDirectInput(void *di);

SH_PROXY_EXPORT HRESULT WINAPI DirectInput8Create(
    HINSTANCE inst, DWORD ver, REFIID iid, LPVOID *out, LPUNKNOWN outer)
{
    HRESULT hr;
    ShFrameworkStart();          /* the real dinput8, and the loader thread */
    if (!p_DirectInput8Create) return E_FAIL;
    hr = p_DirectInput8Create(inst, ver, iid, out, outer);
    /* A milestone: this line is what proves the host got as far as starting
     * its input, which is the first thing a "black screen" report is asked
     * about. */
    LogAlways("DirectInput8Create hr %08lx", (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out) ShWrapDirectInput(*out);
    return hr;
}

/* Never ours to answer. Forwarding this to the real dinput8 asks it about a
 * module it does not know exists, and if it says S_OK the host is free to
 * unload us - while the corefix detours, the dinput vtable patches and three
 * threads that never exit all still live in this image. The next input or
 * file call would be a jump into unmapped memory. This module's lifetime is
 * the game's lifetime, so the answer is always "still in use". */
SH_PROXY_EXPORT HRESULT WINAPI DllCanUnloadNow(void) {
    return S_FALSE;
}

/* The sixth export of the system dinput8, and the one a proxy must not drop.
 *
 * Callers reach it through the standard joystick data format - a module that
 * uses c_dfDIJoystick / c_dfDIJoystick2 from dinput8.lib resolves this
 * export's name, either at load or on first use - so a proxy that does not
 * carry it turns every such caller into a failed resolution. That failure is
 * silent from the outside: the caller's module either refuses to load
 * (ERROR_PROC_NOT_FOUND) or is handed a null and takes its own error path,
 * and the host stops somewhere in its input init with no CPU use and nothing
 * in any log, which is the shape of the 2026-09-17 field report.
 *
 * The prototype is not declared on purpose: this export is reached as
 * "no arguments, returns the format pointer" by some callers and as
 * "one out-parameter, returns an HRESULT" by others, and on x64 a
 * four-integer-argument, pointer-sized-return forward is correct for both.
 */
SH_PROXY_EXPORT void *WINAPI GetdfDIJoystick(void *a1, void *a2, void *a3,
                                             void *a4)
{
    ShFrameworkStart();
    if (!p_GetdfDIJoystick) return NULL;
    return p_GetdfDIJoystick(a1, a2, a3, a4);
}

SH_PROXY_EXPORT HRESULT WINAPI DllGetClassObject(
    REFCLSID clsid, REFIID iid, LPVOID *out)
{
    ShFrameworkStart();          /* the CoCreateInstance route in */
    if (p_DllGetClassObject)
        return p_DllGetClassObject(clsid, iid, out);
    return E_FAIL;
}

SH_PROXY_EXPORT HRESULT WINAPI DllRegisterServer(void) {
    if (p_DllRegisterServer) return p_DllRegisterServer();
    return E_FAIL;
}

SH_PROXY_EXPORT HRESULT WINAPI DllUnregisterServer(void) {
    if (p_DllUnregisterServer) return p_DllUnregisterServer();
    return E_FAIL;
}
