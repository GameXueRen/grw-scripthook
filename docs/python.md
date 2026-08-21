# Writing a plugin in Python {#python}

A Python plugin is a `.py` file in the `plugins` folder beside
`GRW.exe`. `pyhost.asi` runs an embedded CPython inside the game
process and binds every call in `scripthook.h` through ctypes, so
a call costs roughly a microsecond and the plugin loop ticks 600
to 900 times a second.

Drop a file in and it loads within a second. Save an edit and it
reloads. Delete it and it unloads. The game keeps running, so
nothing here needs a restart.

## The four hooks

```python
import sh

def start():
    """Once, on a worker thread. It may block."""
    sh.log('%d npc archetypes' % sh.NpcCount())

def on_frame():
    """Every tick, on the host's driver thread. Keep it quick."""
    pass

def stop():
    """On reload or unload. Undo what start() did."""
    pass

def on_world_reload():
    """After a loading screen. Every entity handle is dead."""
    pass
```

All four are optional. A plugin that raises during import, in
`start()` or in `on_frame()` is logged to `pyhost.log` and
unhooked; it never takes the game down.

## Threads

Nothing your plugin writes runs on the game thread. The frame
hook signals a driver thread, and menu callbacks, hit events and
shot events are queued and delivered there too. So a handler may
block, sleep or spawn without stalling a frame.

Calls that must reach the game thread (spawning, placement)
queue themselves inside the library and apply on the frame path.
`start()` waits for the player to exist before it runs, so
`sh.NpcCount()` and `sh.GetPlayerPosition()` are ready.

## Names

Every `ShXxx` in the C API is `sh.Xxx` in Python, with the
structs bound as ctypes types:

```python
p = sh.Vec3()
if sh.GetPlayerPosition(p):
    e = sh.SpawnNpc(archetype_id, p)
    sh.QueueTransform(e, p, yaw, 0.0, 0.0)
    sh.Despawn(e)
```

Helpers the bindings add:

| call | meaning |
| ---- | ------- |
| `sh.log(msg)` | a line in `pyhost.log` |
| `sh.why()` | why the last call failed, as text |
| `sh.q(addr)`, `sh.f32(addr)` | guarded reads, `None` when unreadable |
| `sh.task(fn)` | run `fn` on a worker thread |
| `sh.every(seconds, fn)` | run `fn` on a timer |
| `sh.menu(title)` | your own row in the F4 menu |
| `sh.on_hit(fn)`, `sh.on_fire(fn)` | hit and shot events |

## Menus

```python
def start():
    m = sh.menu('My mod')
    m.action('Lightning', lambda _: sh.TriggerLightning())
    m.toggle('God mode', False, lambda on: sh.SetGodMode(1 if on else 0))
    m.number('Radius', 3.5, 1.0, 20.0, 0.5, set_radius)
    m.options('Mode', ['ring', 'line'], 0, set_mode)
    m.status('ready')

def stop():
    m.destroy()
```

Destroy your menu in `stop()`, otherwise a reload leaves the old
row behind next to the new one.

## Events

```python
def start():
    sh.on_hit(lambda hit: sh.log('hit %#x' % hit.entity))
    sh.on_fire(lambda shot: sh.log('shot from %#x' % shot.shooter))

def stop():
    sh.clear_events()
```

`hit` is an `ShHit` and `shot` an `ShShot`, the same structs the C
API uses.

## Installing

Copy `pyhost.asi` and the `python` folder next to `GRW.exe`
alongside `dinput8.dll`, then create a `plugins` folder. Linux and
Proton need nothing extra: the game is a Windows process, so the
bundled Windows CPython loads normally.

`plugins/example.py` in the release is a working plugin covering
the menu, an event and a spawn.
