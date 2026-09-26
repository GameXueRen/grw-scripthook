#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "scripthook.h"

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

/* Plugins live one folder each under plugins\, named after
 * the plugin:
 *
 *   plugins/<name>/<name>.asi
 *   plugins/<name>/<name>.ini
 *
 * The folder scan means a plugin's assets, config and log
 * stay together and nothing from the game root is touched.
 * plugins\ is created if this is a fresh install.
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
#define PLUGIN_SCAN_MAX 64

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
    char pluginsDir[MAX_PATH], pat[MAX_PATH], full[MAX_PATH];
    static char names[PLUGIN_SCAN_MAX][MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    uint32_t need;
    int n = 0, nSkipped = 0, nDefaultOn = 0, count = 0, i, j;

    if (!ShPluginsDir(pluginsDir, sizeof(pluginsDir))) {
        LogAlways("cannot find the game directory");
        return;
    }
    /* Fresh install: make the layout the loader expects.
     * CreateDirectoryA dislikes the trailing backslash. */
    {
        char dir[MAX_PATH];
        size_t len = strlen(pluginsDir);
        if (len > 0 && pluginsDir[len - 1] == '\\') len--;
        memcpy(dir, pluginsDir, len);
        dir[len] = 0;
        CreateDirectoryA(dir, NULL);
    }

    if (!ShConfigGetBool("loader", "load_plugins", 1)) {
        LogAlways("plugin loading disabled in scripthook.ini");
        return;
    }

    snprintf(pat, sizeof(pat), "%s*", pluginsDir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        LogAlways("no plugin folders in %s", pluginsDir);
        return;
    }

    /* Phase one: the folders that actually hold a <name>.asi, sorted,
     * so the log and the lines written back to the ini come out in the
     * same order on every launch. */
    do {
        const char *name = fd.cFileName;

        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (name[0] == '.') continue;

        snprintf(full, sizeof(full), "%s%s\\%s.asi",
                 pluginsDir, name, name);
        if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) {
            Log("plugins\\%s: no %s.asi, skipping", name, name);
            nSkipped++;
            continue;
        }
        if (count >= PLUGIN_SCAN_MAX) {
            Log("plugins\\%s: scan table full (%d), not loaded",
                name, PLUGIN_SCAN_MAX);
            nSkipped++;
            continue;
        }
        snprintf(names[count], sizeof(names[count]), "%s", name);
        count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    for (i = 1; i < count; i++) {
        char tmp[MAX_PATH];
        j = i;
        while (j > 0 && strcmp(names[j - 1], names[j]) > 0) {
            memcpy(tmp, names[j - 1], sizeof(tmp));
            memcpy(names[j - 1], names[j], sizeof(tmp));
            memcpy(names[j], tmp, sizeof(tmp));
            j--;
        }
    }

    /* Phase two: decide, and write nothing. -1 is "no line at all"
     * (ShConfigGetInt cannot tell that apart from a 0, which is why this asks
     * for an int), and a folder with no line is ON: a plugin that was just
     * added works without a trip to the menu first, and the ini is left as the
     * player wrote it. A 0 put there by the Plugins page is what keeps one off
     * from then on; the default is counted so the scan's summary says how many
     * plugins are on that way, without a line each to say it. */
    for (i = 0; i < count; i++) {
        const char *name = names[i];
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

        snprintf(full, sizeof(full), "%s%s\\%s.asi",
                 pluginsDir, name, name);

        /* What this plugin asks of the framework, read without running any of
         * it. Nothing declared is 0, and 0 is loaded as it always was. */
        need = PluginNeedsApi(full);
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
        Log("loading plugin: plugins\\%s\\%s.asi", name, name);
        {
            HMODULE mod = LoadLibraryA(full);
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
