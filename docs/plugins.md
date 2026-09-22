# Writing a plugin {#mainpage}

A plugin is a DLL renamed to `.asi`, placed in its own folder
under `plugins\` next to `GRW.exe`:

```
<gamedir>/
├── dinput8.dll
├── scripthook.ini       main config, created on first launch
├── lang.ini             optional, and yours to write: text overrides
│                        (the build never creates this; see below)
├── logs/                every log file, timestamps on each line
└── plugins/
    └── firstperson/
        ├── firstperson.asi
        ├── firstperson.ini    the plugin's own config
        └── lang.ini           the plugin's text, one section per language
```

The loader walks `plugins\` and loads `plugins\<name>\<name>.asi`
for every folder. A plugin's settings belong beside it as
`plugins\<name>\<name>.ini`; `ShPluginIniPath()` hands you that
path so the lookup is not the plugin's problem. If a plugin fails
to load, `logs/scripthook.log` records the reason.

`lang.ini` beside it holds the plugin's text: page titles, row
labels, hints, status templates. One section per language
(`[zh-CN]`, `[en-US]`), each key either a stable ID (`@fp.page`) or
the English string itself - the second form is what translates a
plugin whose source you do not have. The framework only ever reads
it, so a translation can be edited in place, and `lang.ini` and the
config can be deleted independently. This repo keeps that same
layout under its own `plugins\<name>\`, so a plugin's source, its
config and its text travel together. See docs/i18n-refactor.md.

A plugin's own text is **compiled in** (English and Chinese) once its
source is in this repo, so its `lang.ini` is an override: delete it and
the menu still reads both languages. An `.asi` that ships without source
is the exception - its text exists only in that file, which is how the
framework translates it at all. It is seeded once by the build and never
overwritten. The same goes for `<gamedir>\lang.ini`: it is the way to
change a line of the framework's own text, to add a language, or to
translate such a plugin from one place, and **the build does not create
it** - nothing is there on a fresh install, and the compiled-in text
carries both languages. `lang.example.ini` in this repository is that
file's reference, to be copied out by hand.

`scripthook.ini` is the main config, parsed before any plugin
loads. The loader reads `[loader] load_plugins` and one
`[plugins] <name>` line per plugin, and **a plugin with no line
there is loaded** - the same for a plugin this mod ships and for a
third-party `.asi` dropped into `plugins\`. That is the case that
used to cost the most: a folder that is there and silently doing
nothing reads as a broken plugin. The first scan writes the line a
folder was missing (`=1`), so the list ends up naming every plugin
it found; switching one off in the mod menu's Plugin switches page
writes a `0` there, and that line is what keeps it off. Both take
effect on the next launch. Deleting `scripthook.ini` is still the
reset, and what it resets to is "everything in `plugins\` loads" -
so ruling a plugin out means the menu switch, or taking its folder
away. The file also holds the `[loader]` CPU dials and the
language, so those come back as defaults too. Future framework
features will take their switches from the same file.

What deleting it does **not** touch is the text: the compiled-in tables
(the framework's in `scripthook_text.c`, each plugin's own) carry English
and Chinese on their own, and any `lang.ini` - `plugins\<name>\lang.ini`
or a `<gamedir>\lang.ini` you wrote yourself - only overrides that. The
framework never writes one, so the menu keeps reading, in both languages,
whichever of those files exist.
Plugins can read it too through `ShConfigGetInt` /
`ShConfigGetBool` / `ShConfigGetStr`.

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
x86_64-w64-mingw32-gcc -O2 -shared -I. \
    -o plugins/myplugin/myplugin.asi plugins/myplugin/myplugin.c \
    -L. -lscripthook
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

**What you need of the API.** A plugin that calls something the API
only grew later declares it once, outside every function:

```c
SH_REQUIRES_API(2);   /* I call ShGetAmmoObject */
```

Name the last thing you use, not the header you happened to build
against: a plugin that only calls older entry points stays loadable
on older frameworks, and the loader refuses on "needs more than this
one offers" and nothing else. The declaration is read out of the file
before the plugin's code runs at all, and a plugin that needs more is
refused - a line in `logs\scripthook.log`, and a toast once the
overlay is up. A plugin that writes nothing declares nothing and
loads exactly as it always did: silence is "no requirement", never
"whatever is newest", and one that is switched off in
`scripthook.ini` is not even read. The framework's own number is on
the About page; `SH_API_VERSION` in `scripthook.h` carries the note
for each version that changed it, and it only ever goes up by one.

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

**Hooks.** A plugin does not install one. Anything that patches code
or writes engine memory is the framework's, one owner, with an API in
front of it: that is what keeps two plugins off the same byte, and what
keeps a session that goes wrong diagnosable. A plugin that needs an
effect the API does not have is the reason the API grows a call for it -
`ShSetAmmoScale` is the most recent one, written because a plugin wanted
the magazine capacity and only a hook over the engine's own computation
can reach it (docs/ammocapacity-reverse.md - a write-up kept out of the
repository). When the effect is a rule
over someone else's calls - a file that must look missing, a read that
must be answered differently - a plugin registers with the layer that
already owns those calls instead (@ref files).

Five plugin sources still carry MinHook of their own, from before this
rule: `skipintro`, `ModeExitProbe`, `ModeCallProbe`, `AllLanguages` and
`micfix`. Each is a convergence item, not a licence. `micfix` is the most
recent one and the only one aimed outside the game: it hooks
`ole32!CoCreateInstance` and patches the vtable slots of the DirectShow and
MMDevice objects that call returns, so the recording-device names the game
walks through both doors come back ASCII
(`plugins/micfix/micfix.c` says why, and what would
move it into the framework: a second consumer of the device list).

## Credits

Firejumper93 (GhostReconWildlandsVR, MIT) for the camera
structure, the rig transforms and the no-blur byte. AngelSoleil
and ClowdPanini's Last Rites table for head identification.
The GRW Environment table for weather. neburas for the Windows
spawn fix.

This repository used to carry rewrites of four third-party plugins -
AmmoCapacity, Time&Weather, OpticalCamo and NPCSpawner, all by the same
author, and all read from their shipped binaries. At his request they were
removed from the tree, the packages and the development machine on 2026-09-20,
so his own versions, updated for TU25, are the ones to use; the README says
where to find them.

What they needed from the framework is still here, because it was always ours:
the ammo-capacity hook, the visibility factor, the time and weather API, and
the NPC spawn and formation calls. An independent plugin can use any of them -
which is also why the framework no longer carries the per-archetype id tables
those routines used to be pointed at.

Source: https://github.com/PhialsBasement/grw-scripthook
