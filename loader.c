#include <windows.h>
#include <stdio.h>
#include "log.h"
#include "scripthook.h"

typedef HRESULT (WINAPI *DirectInput8Create_t)(
    HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
typedef HRESULT (WINAPI *DllCanUnloadNow_t)(void);
typedef HRESULT (WINAPI *DllGetClassObject_t)(REFCLSID, REFIID, LPVOID *);
typedef HRESULT (WINAPI *DllRegisterServer_t)(void);
typedef HRESULT (WINAPI *DllUnregisterServer_t)(void);

static HMODULE g_realDinput8 = NULL;

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

static void LoadASIPlugins(void) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("*.asi", &fd);
    if (h == INVALID_HANDLE_VALUE) {
        Log("no .asi plugins found");
        return;
    }
    do {
        Log("loading plugin: %s", fd.cFileName);
        HMODULE mod = LoadLibraryA(fd.cFileName);
        if (mod)
            Log("  loaded at %p", (void *)mod);
        else
            Log("  FAILED (error %lu)", GetLastError());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
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
        LoadRealDinput8();
        /* Watch state before plugins, so the world is
         * resolved by the time any of them ask.
         */
        ShStateStartup();
        LoadASIPlugins();
    } else if (reason == DLL_PROCESS_DETACH && g_logFile) {
        Log("unloading");
        LogClose();
        if (g_realDinput8) FreeLibrary(g_realDinput8);
    }
    return TRUE;
}

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(
    HINSTANCE inst, DWORD ver, REFIID iid, LPVOID *out, LPUNKNOWN outer)
{
    if (p_DirectInput8Create)
        return p_DirectInput8Create(inst, ver, iid, out, outer);
    return E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) {
    if (p_DllCanUnloadNow) return p_DllCanUnloadNow();
    return S_FALSE;
}

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(
    REFCLSID clsid, REFIID iid, LPVOID *out)
{
    if (p_DllGetClassObject)
        return p_DllGetClassObject(clsid, iid, out);
    return E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI DllRegisterServer(void) {
    if (p_DllRegisterServer) return p_DllRegisterServer();
    return E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI DllUnregisterServer(void) {
    if (p_DllUnregisterServer) return p_DllUnregisterServer();
    return E_FAIL;
}
