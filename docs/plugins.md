# Writing a plugin {#mainpage}

A plugin is a DLL renamed to `.asi`, placed in its own folder
under `scripts\` next to `GRW.exe`:

```
<gamedir>/
├── dinput8.dll
├── scripthook.ini       main config, created on first launch
├── logs/                every log file, timestamps on each line
└── scripts/
    └── firstperson/
        ├── firstperson.asi
        └── firstperson.ini    the plugin's own config
```

The loader walks `scripts\` and loads `scripts\<name>\<name>.asi`
for every folder. A plugin's settings belong beside it as
`scripts\<name>\<name>.ini`; `ShPluginIniPath()` hands you that
path so the lookup is not the plugin's problem. If a plugin fails
to load, `logs/scripthook.log` records the reason.

`scripthook.ini` is the main config, parsed before any plugin
loads. The loader reads `[loader] load_plugins` and one
`[plugins] <name>` line per plugin to skip a plugin without
removing it. Future framework features will take their switches
from the same file. Plugins can read it too through
`ShConfigGetInt` / `ShConfigGetBool` / `ShConfigGetStr`.

## Build and link

The loader loads plugins from a worker thread rather than from
inside `DllMain`. `dinput8.dll` is therefore initialised before
plugin code runs, and a plugin can link `libscripthook.a` and
call the API directly.

```c
#include <windows.h>
#include "scripthook.h"

static void OnNoon(uint32_t m, uint32_t i, int v, void *u) {
    if (v) ShSetTime(12.0f);
}

static DWORD WINAPI Init(LPVOID p) {
    uint32_t menu = ShMenuCreate("High noon");
    ShMenuToggle(menu, "Always noon", 0, OnNoon, NULL);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD why, LPVOID res) {
    if (why == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        CreateThread(NULL, 0, Init, NULL, 0, NULL);
    }
    return TRUE;
}
```

```
x86_64-w64-mingw32-gcc -O2 -shared -o myplugin.asi myplugin.c -L. -lscripthook
```

The worker thread keeps setup off the loader lock. Menu
callbacks arrive on a thread the API owns, and calling the API
from a callback is safe.

`GetProcAddress` binding has one remaining use: optional
features. A plugin that must load on older ScriptHook versions
can resolve a newer export by name and skip the feature when
the result is NULL, instead of failing to load over a missing
import.

```c
typedef int (*SetBlur_t)(int);
static SetBlur_t g_setBlur;   /* NULL on 0.4.x */

*(FARPROC *)&g_setBlur = GetProcAddress(
    GetModuleHandleA("dinput8.dll"), "ShSetCameraBlur");
if (g_setBlur) g_setBlur(0);
```

## Rules that hold across the API

**Return values.** Every `int` function returns 1 on success and
0 on failure. On failure `ShLastError()` returns the reason as
an `ShError` and `ShErrorString()` returns its name.

**Threads.** Any API call is safe from any thread. Every engine
memory access inside the API is kernel mediated, so a pointer
freed during a call produces a failed call rather than a crash.
Event and menu callbacks run on threads the API owns, and
calling back into the API from one is fine.

**Engine calls.** Raw engine functions must run on the game
thread. Use `ShQueueCall()`. Calling an engine address from a
plugin thread deadlocks when the call takes an engine lock.

**Handles.** Entity and component handles come from the API's
enumerators. They can go stale at any time. The API revalidates
them internally, so a stale handle costs a failed call.

**Overrides.** The engine rebuilds the camera and stamps
visibility every frame, so overrides are reapplied each frame
until released. Camera ownership is per field. Release only the
fields you took; fields held by other plugins keep running.

**Units.** Metres and seconds. Camera angles are radians.
Struct fields that use degrees say so. World axes: x east,
y north, z up.

## Credits

Firejumper93 (GhostReconWildlandsVR, MIT) for the camera
structure, the rig transforms and the no-blur byte. AngelSoleil
and ClowdPanini's Last Rites table for head identification.
The GRW Environment table for weather. neburas for the Windows
spawn fix.

Source: https://github.com/PhialsBasement/grw-scripthook
