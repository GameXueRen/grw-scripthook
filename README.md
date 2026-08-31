# GRW ScriptHook

A ScriptHookV equivalent for **Ghost Recon Wildlands** (Definitive
Edition). It loads as a `dinput8.dll` proxy, exposes a plain C ABI,
and loads `.asi` plugins the same way ScriptHookV does.

Everything here was found by decompiling the game in Ghidra. The
engine has no scripting layer, no console and no exported hooks, so
each capability below is a specific function or field that had to be
located and verified in a running game.

Developed and tested under Proton on Linux, though nothing in it is
Wine specific.

## Documentation

The full API reference, generated from `scripthook.h`, lives at
<https://phialsbasement.github.io/grw-scripthook/api/>. It covers
every call, grouped the way the header is.

[docs/plugins.md](docs/plugins.md) is the plugin author's guide:
how loading works, which threads call you, and the rules that keep
a plugin from crashing the game. [docs/ui.md](docs/ui.md) covers
the native UI: scenes, widgets, properties, input and reloads.

## What works

| Capability | Notes |
| --- | --- |
| Vehicle spawning | 65 vehicles, catalogued and named by hand |
| Entity enumeration | Kind, position, health, components |
| Entity visibility | Hide or show anything, optionally held |
| Teleport | Verified 9.9km cross map, lands within 3m |
| Health | Read and write through the game's obfuscated storage |
| Ground queries | Uses the engine's own collision world |
| Camera | Position, orientation, roll, fov, the view matrices |
| OnHit events | Entity, position, normal, distance, shooter |
| OnFire events | Muzzle, direction, yaw and pitch, shooter |
| Native UI | The engine's own widgets, scenes per plugin, keys with focus |
| Menu | One shared root, plugins add submenus |
| Overlay | Slots that pack themselves, drawn by the game |
| Game state | Menu, loading, in game, transitions |

Three example mods are included:

- **hitfling** shoot a car, it launches into the air
- **tpgun** shoot anywhere, you arrive there
- **ui_sample** a window from the UI ABI alone: its own scene,
  rows, a highlight bar, keys through the input callback, a
  rebuild after a world reload (F7 toggles it)

## Building

**MinGW (Linux/Proton).** Requires a MinGW cross compiler. On
Arch that is `mingw-w64-gcc`; on Debian,
`gcc-mingw-w64-x86-64`.

```sh
make            # dinput8.dll and scripts/test_plugin/test_plugin.asi
make fling      # scripts/hitfling/hitfling.asi
make tpgun      # scripts/tpgun/tpgun.asi
make sample     # scripts/ui_sample/ui_sample.asi
make QUIET=1    # same, without the four benign warning families
make docs       # the API reference into docs/api (doxygen)
```

**MSVC (Windows).** `build_msvc.ps1` builds the same outputs
with the Visual Studio Build Tools x64 toolchain. It locates
the game folder (a sibling of this repo's parent) or takes
`-Gamedir`, and `-Clean` removes the built files.

```powershell
pwsh ./build_msvc.ps1            # auto-detected game folder
pwsh ./build_msvc.ps1 -Gamedir "D:\Games\GRW"
pwsh ./build_msvc.ps1 -Clean
```

The source is shared between the two compilers: the engine
callbacks use `SH_MSABI`/`SH_ALIGNED` shims (GCC attributes,
MSVC no-ops/`__declspec(align)`), the guard pad is emitted by
inline asm on GCC and from `guard.asm` via ml64 on MSVC, and
the proxy entry points are `__declspec(dllexport)` on GCC and
listed in `proxy.def` on MSVC (where the SDK already declares
two of them).

Output goes to `GAMEDIR`, set at the top of the Makefile (or
auto-detected by `build_msvc.ps1`), which should be the folder
containing `GRW.exe`. Each plugin lands in
`scripts/<name>/<name>.asi`, the layout the loader scans for.

## Installing

Drop `dinput8.dll` next to `GRW.exe`. Plugins go into `scripts/`,
one folder each, named after the plugin:

```
Tom Clancy's Ghost Recon Wildlands/
├── GRW.exe
├── dinput8.dll
├── scripthook.ini     main config, created on first launch
├── logs/              every log file, timestamps on each line
└── scripts/
    └── firstperson/
        ├── firstperson.asi
        └── firstperson.ini    the plugin's own config
```

`scripthook.ini` controls the loader now (`[loader] load_plugins`,
`[plugins] <name>=0` to disable one) and will hold the switches
for the larger features. The proxy forwards every DirectInput8
export to the real system DLL, so the game behaves normally with
or without plugins present.

## Writing a mod

A plugin is a DLL named `.asi`. The loader runs plugins from a
worker thread rather than from `DllMain`, so a plugin can link
`libscripthook.a` and call the API directly, or resolve it through
`GetProcAddress` to also run on older loaders. The guide in
[docs/plugins.md](docs/plugins.md) walks through both.

The whole of the falling cars mod:

```c
static void OnHit(const ShHit *hit, void *user) {
    ShVec3 up = hit->pos;
    if (hit->kind != SH_KIND_VEHICLE) return;
    up.z += 90.0f;
    ShPlaceEntity(hit->root, &up, NULL);
}

while (!ShIsInGame() || !ShHitHookInstall()) Sleep(500);
ShOnHit(OnHit, NULL, 0);
```

Receivers are called on a worker thread the API owns, so any API
call is legal inside one. `hit->root` is already resolved for you,
because bullets usually strike a child part rather than the vehicle.

## The ABI

`scripthook.h` is the only header a mod needs. Every call returns 1
on success and 0 on failure, with `ShLastError` giving the reason.

```
player        ShGetPlayer ShGetPlayerPosition ShTeleportPlayer
              ShTeleportPlayerHops ShTeleportPlayerToGround
              ShIsInVehicle ShWalkToRoot

entities      ShFindEntities ShGetEntityKind ShKindName
              ShPlaceEntity ShGetComponents ShFindComponent
              ShSetEntityVisible ShEntityNodeCount

camera        ShGetCamera ShSetCamera ShCameraOrbit
              ShCameraFree ShCameraAngles ShCameraApply
              ShCameraMatrix ShCameraRelease

menu          ShMenuCreate ShMenuSub ShMenuAction
              ShMenuToggle ShMenuNumber ShMenuList
              ShMenuStatus ShMenuSetKey ShMenuOpen

overlay       ShHudCreate ShHudSet ShHudColour
              ShHudShow ShHudDestroy

vehicles      ShSpawnVehicle ShVehicleCount ShVehicleAt
              ShVehicleName

health        ShGetHealthPlayer ShSetHealthPlayer
              ShSetGodModePlayer ShSetCannotDiePlayer
              ShGetHealthEntity ShSetHealthEntity

events        ShHitHookInstall ShOnHit ShOffHit ShGetHits
              ShOnFire ShOffFire ShGetShots

physics       ShPhysicsReady ShGroundHeight ShGroundHeightFrom
              ShRayLog ShQueryRays ShRayFilterPlayer

engine        ShQueueCall ShQueueResult ShGetGameState ShIsInGame

ui            ShUiSceneCreate ShUiSceneSetOrder ShUiSceneShow
              ShUiSceneDestroy ShUiCreateIn ShUiReparent
              ShUiChildCount ShUiChildAt ShUiDestroy
              ShUiSetF ShUiSetU ShUiSetV ShUiSetS ShUiGetF
              ShUiGetU ShUiGetV ShUiGetS ShUiMeasure
              ShUiSetAutoSize ShUiBegin ShUiCommit
              ShUiCommitAsync ShUiSetReset ShUiSetInput
              ShUiFocus ShUiTextureCreate ShUiSetDefaultFont
```

### Events

`ShOnHit(fn, user, flags)` delivers one event per bullet, at the
impact that stopped it. Flags are opt in:

- `SH_EVT_MINE_ONLY` only your own shots
- `SH_EVT_NO_SELF` drop the graze on the firer's own body

A projectile is stepped every frame and its hit list accumulates, so
the API holds each bullet and reports its furthest hit once the
projectile stops being stepped. That costs about 120ms of latency
and is why acting on the first reported hit puts you on a fence post
instead of the target.

### Threads

Engine calls that take locks deadlock from any thread but the game
thread. `ShQueueCall` runs a call there and `ShQueueResult` collects
it, usually on the next frame. The API uses this internally, so
plugins rarely need it.

## Hazards

These are real, and each one cost a crash to find.

- Two vehicles freeze the game if you enter them: an alpaca that the
  engine classes as a vehicle, `0x40081214`, and an unused monster
  truck, `0x40BA6E9D`. They are safe to spawn, not to ride.
- Placing an entity outside the streamed region crashes the game.
  Collision only exists within roughly 1500m of the player.
- `ShPlaceEntity` carries riders on purpose, so moving a vehicle you
  are sitting in takes you with it.

## Addressing

Every engine address is stored as an RVA and resolved against the
module base at runtime, so ASLR is fine. `image.h` holds the helper.
The game currently loads at its preferred base under Proton, which
made pinned addresses easy to get away with and easy to get wrong.

## Layout

```
loader.c              dinput8 proxy, loads the real DLL and the .asi files
scripthook_config.c   game/scripts/logs paths, the main scripthook.ini
scripthook_api.c      player, teleport, entity placement, errors
scripthook_entity.c   enumeration, components, kinds, visibility
scripthook_health.c   the obfuscated health storage
scripthook_state.c    game flow state, pause detection
scripthook_physics.c  ray hook, ground queries, game thread queue
scripthook_spawn.c    vehicle catalogue and spawning
scripthook_hit.c      OnHit and OnFire
scripthook_camera.c   the camera hook, per field ownership
scripthook_head.c     the head bone, position and orientation
scripthook_fov.c      field of view, taken at its source
scripthook_blur.c     the hidden close range blur, one byte
scripthook_stat.c     the obfuscated stat storage
scripthook_resource.c resources and skill points
scripthook_stealth.c  detection visibility scale
scripthook_ammo.c     ammo by weapon slot
scripthook_weather.c  weather type and time of day
scripthook_reflect.c  method tables by name, scenes, GameFlow objects
scripthook_ui.c       native widgets, panels, labels, quads
scripthook_scene.c    the phoenix scene of our own that hosts them
scripthook_uiprop.c   widget properties by id from the engine's tables
scripthook_uiinput.c  keys and pointer for the focused scene
scripthook_dinput.c   the DirectInput keyboard wrapper, blocked keys
scripthook_input.c    cursor freeze and the modifier poll stub
scripthook_hud.c      overlay slots
scripthook_menu.c     the shared F4 menu
guard.c               landing pad for the spawn trampoline
guard.asm             the same pad for MSVC (ml64)

test_plugin.c         a REPL on port 9999, every debugging command
hitfling.c tpgun.c    the example mods
ui_sample.c           the native UI example
build_msvc.ps1        MSVC build script (see Building)
proxy.def             the proxy entry points for MSVC builds
```

`test_plugin.c` is large because it accumulated every experiment
used to find the rest. It is a research tool, not an example.

## Credits

The camera work stands on **Firejumper93's**
[GhostReconWildlandsVR](https://github.com/Firejumper93/GhostReconWildlandsVR),
MIT licensed and unusually well documented. Its notes gave us the
camera struct layout, the fact that `Camera+0x000` is the transform
the view builder actually consumes rather than one of the derived
matrices, and this engine's yaw and pitch convention. Their build log
also records the write to `+0x4A0` that quietly does nothing, which
is exactly the wrong turn we would have taken.

The addresses here are our own, since this build is newer than any in
their table, but the reverse engineering that made them meaningful is
theirs.

## Licence

GPL-3.0. See `LICENSE`.

You are free to use, modify and redistribute this, including
commercially. What you cannot do is take it, add features, and ship
that as a closed product: any derivative has to be released under
the GPL with its source available. Sell builds if you like, but the
improvements come back to everyone.
