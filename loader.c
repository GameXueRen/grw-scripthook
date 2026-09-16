#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "scripthook.h"

/* The play mode module (scripthook_playmode.c): not a plugin facing
 * export beyond ShSelectedPlayMode and friends, so it is declared here. */
extern void ShPlayModeStart(void);

typedef HRESULT (WINAPI *DirectInput8Create_t)(
    HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
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
        Log("FATAL: system directory path too long");
        return;
    }
    memcpy(sysdir + n, "\\dinput8.dll", 13);

    g_realDinput8 = LoadLibraryA(sysdir);
    if (!g_realDinput8) {
        Log("FATAL: could not load real dinput8.dll from %s", sysdir);
        return;
    }
    Log("loaded real dinput8.dll from %s", sysdir);

    p_DirectInput8Create = (DirectInput8Create_t)
        GetProcAddress(g_realDinput8, "DirectInput8Create");
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
 * A plugin loads only when the one thing that can say so says so: a
 * [plugins] line reading 1 - written by hand, or by the mod menu's
 * Plugins page. No line means off, and that goes for a plugin this
 * mod ships and for a third-party .asi dropped in here alike: the
 * scan writes the line it found missing (0), so the ini ends up
 * listing every folder it saw, and deleting that file is the reset -
 * a fresh default with every plugin off, which is what a player who
 * has just broken something wants to be able to do.
 */
#define PLUGIN_SCAN_MAX 64

static void LoadASIPlugins(void) {
    char pluginsDir[MAX_PATH], pat[MAX_PATH], full[MAX_PATH];
    static char names[PLUGIN_SCAN_MAX][MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0, nSkipped = 0, nAdded = 0, count = 0, i, j;

    if (!ShPluginsDir(pluginsDir, sizeof(pluginsDir))) {
        Log("cannot find the game directory");
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
        Log("plugin loading disabled in scripthook.ini");
        return;
    }

    snprintf(pat, sizeof(pat), "%s*", pluginsDir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        Log("no plugin folders in %s", pluginsDir);
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

    /* Phase two: decide, and give a folder with no line of its own the
     * line it is missing. -1 is "no line at all" (ShConfigGetBool
     * cannot tell that apart from a 0, which is why this asks for an
     * int), and the value is always 0: nothing is loaded until a line
     * - or the menu - says 1. A write that fails is logged and changes
     * nothing: the decision is made either way. */
    for (i = 0; i < count; i++) {
        const char *name = names[i];
        int on = ShConfigGetInt("plugins", name, -1);

        if (on < 0) {
            on = 0;
            if (ShConfigSetInt("plugins", name, 0)) {
                Log("plugins\\%s: no [plugins] line, wrote %s=0 (off by "
                    "default; switch it on in the mod menu)", name, name);
                nAdded++;
            } else {
                Log("plugins\\%s: no [plugins] line, off by default "
                    "(writing it back failed)", name);
            }
        }
        if (!on) {
            Log("plugin disabled in scripthook.ini: %s", name);
            nSkipped++;
            continue;
        }

        snprintf(full, sizeof(full), "%s%s\\%s.asi",
                 pluginsDir, name, name);
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
    Log("plugin scan done: %d loaded, %d skipped, %d line(s) written back",
        n, nSkipped, nAdded);
}

/* Plugins load here, not in DllMain. LoadLibrary blocks on
 * the loader lock until DllMain returns, so a plugin binds
 * against a fully initialised DLL and may import it. */
static DWORD WINAPI LoaderThread(LPVOID p) {
    (void)p;
    ShConfigInit();
    Log("config loaded from scripthook.ini");
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

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        if (!IsGRW()) return TRUE;
        LogInit("scripthook.log");
        Log("GRW ScriptHook " SH_VERSION);
        Log("built " __DATE__ " " __TIME__);
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
        LoadRealDinput8();
        /* State watching moved into LoaderThread: starting a thread here
         * is the one thing this file warns about (the loader lock), and
         * the watch only has to be up before the plugins load. */
        {
            HANDLE h = CreateThread(NULL, 0, LoaderThread, NULL, 0, NULL);

            if (h) CloseHandle(h);   /* never waited on: prompt close */
            else
                /* Nothing else can explain a session that came up with no
                 * plugins, no config and no hooks: the thread that reads
                 * them never started. */
                Log("loader: could not start the load thread - nothing will "
                    "be loaded this session");
        }
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
    if (!p_DirectInput8Create) return E_FAIL;
    hr = p_DirectInput8Create(inst, ver, iid, out, outer);
    Log("DirectInput8Create hr %08lx", (unsigned long)hr);
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

SH_PROXY_EXPORT HRESULT WINAPI DllGetClassObject(
    REFCLSID clsid, REFIID iid, LPVOID *out)
{
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
