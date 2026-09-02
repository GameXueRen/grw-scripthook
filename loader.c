#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "scripthook.h"

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
    GetSystemDirectoryA(sysdir, MAX_PATH);
    strcat(sysdir, "\\dinput8.dll");

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
extern void ShCrashStartup(void);
extern void ShCoreFixStartup(void);

/* Plugins live one folder each under scripts\, named after
 * the plugin:
 *
 *   scripts/<name>/<name>.asi
 *   scripts/<name>/<name>.ini
 *
 * The folder scan means a plugin's assets, config and log
 * stay together and nothing from the game root is touched.
 * scripts\ is created if this is a fresh install.
 */
static void LoadASIPlugins(void) {
    char scriptsDir[MAX_PATH], pat[MAX_PATH], full[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0, nSkipped = 0;

    if (!ShScriptsDir(scriptsDir, sizeof(scriptsDir))) {
        Log("cannot find the game directory");
        return;
    }
    /* Fresh install: make the layout the loader expects.
     * CreateDirectoryA dislikes the trailing backslash. */
    {
        char dir[MAX_PATH];
        size_t len = strlen(scriptsDir);
        if (len > 0 && scriptsDir[len - 1] == '\\') len--;
        memcpy(dir, scriptsDir, len);
        dir[len] = 0;
        CreateDirectoryA(dir, NULL);
    }

    if (!ShConfigGetBool("loader", "load_plugins", 1)) {
        Log("plugin loading disabled in scripthook.ini");
        return;
    }

    snprintf(pat, sizeof(pat), "%s*", scriptsDir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        Log("no plugin folders in %s", scriptsDir);
        return;
    }
    do {
        const char *name = fd.cFileName;

        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (name[0] == '.') continue;

        snprintf(full, sizeof(full), "%s%s\\%s.asi",
                 scriptsDir, name, name);
        if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) {
            Log("scripts\\%s: no %s.asi, skipping", name, name);
            nSkipped++;
            continue;
        }
        if (!ShConfigGetBool("plugins", name, 1)) {
            Log("plugin disabled in scripthook.ini: %s", name);
            nSkipped++;
            continue;
        }
        n++;
        Log("loading plugin: scripts\\%s\\%s.asi", name, name);
        HMODULE mod = LoadLibraryA(full);
        if (mod)
            Log("  loaded at %p", (void *)mod);
        else
            Log("  FAILED (error %lu)", GetLastError());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    Log("plugin scan done: %d loaded, %d skipped", n, nSkipped);
}

/* Plugins load here, not in DllMain. LoadLibrary blocks on
 * the loader lock until DllMain returns, so a plugin binds
 * against a fully initialised DLL and may import it. */
static DWORD WINAPI LoaderThread(LPVOID p) {
    (void)p;
    ShConfigInit();
    Log("config loaded from scripthook.ini");
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
        Log("GRW ScriptHook loader v0.1");
        Log("built " __DATE__ " " __TIME__);
        /* Armed first, so a crash during our own start up
         * is reported too.
         */
        ShCrashStartup();
        /* CPU core-count/affinity fix. Must run before the
         * engine reads the processor count, so it goes here,
         * on the attach path, not in the loader thread.
         * No-op unless scripthook.ini [loader] cpu_core_fix=1.
         */
        ShCoreFixStartup();
        LoadRealDinput8();
        /* Watch state before plugins, so the world is
         * resolved by the time any of them ask.
         */
        ShStateStartup();
        CreateThread(NULL, 0, LoaderThread, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH && g_logFile) {
        Log("unloading");
        LogClose();
        if (g_realDinput8) FreeLibrary(g_realDinput8);
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

SH_PROXY_EXPORT HRESULT WINAPI DllCanUnloadNow(void) {
    if (p_DllCanUnloadNow) return p_DllCanUnloadNow();
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
