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
| Menu | One shared root, plugins add submenus |
| Overlay | Slots that pack themselves, no Win32 in plugins |
| Game state | Menu, loading, in game, transitions |

Two example mods are included, each about ten lines of real logic:

- **hitfling** shoot a car, it launches into the air
- **tpgun** shoot anywhere, you arrive there

## Building

Requires a MinGW cross compiler. On Arch that is
`mingw-w64-gcc`; on Debian, `gcc-mingw-w64-x86-64`.

```sh
make            # dinput8.dll and test_plugin.asi
make fling      # hitfling.asi
make tpgun      # tpgun.asi
```

Output goes to `GAMEDIR`, set at the top of the Makefile, which
should be the folder containing `GRW.exe`.

## Installing

Drop `dinput8.dll` next to `GRW.exe`, along with any `.asi` plugins.
The proxy forwards every DirectInput8 export to the real system DLL,
so the game behaves normally with or without plugins present.

## Writing a mod

A plugin is a DLL named `.asi`. Resolve the API through
`GetProcAddress`, never by linking against the import library:
plugins are loaded from inside `dinput8`'s own `DllMain`, so a
static import on it deadlocks the Windows loader.

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
scripthook_api.c      player, teleport, entity placement, errors
scripthook_entity.c   enumeration, components, kinds
scripthook_health.c   the obfuscated health storage
scripthook_state.c    game flow state
scripthook_physics.c  ray hook, ground queries, game thread queue
scripthook_spawn.c    vehicle catalogue and spawning
scripthook_hit.c      OnHit and OnFire

test_plugin.c         a REPL on port 9999, every debugging command
hitfling.c tpgun.c    the example mods
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
