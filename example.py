"""A python plugin, start to finish. Copy it and edit.

Drop this in the plugins folder beside GRW.exe and it loads
within a second, no restart. Edit and save it and it reloads
the same way.

  start()            once, on a worker thread, may block
  on_frame()         at frame cadence, keep it quick
  stop()             on reload or unload, clean up here
  on_world_reload()  after a loading screen, handles are dead
"""
import sh

_menu = None
_count = 0


def start():
    global _menu
    sh.log('example.py: %d npc archetypes available' % sh.NpcCount())

    _menu = sh.menu('Example')
    if _menu:
        _menu.action('Spawn one NPC', _spawn)
        _menu.action('Lightning', lambda _v: sh.TriggerLightning())
        _menu.toggle('God mode', False,
                     lambda v: sh.SetGodMode(1 if v else 0))
        _menu.status('ready')


def _spawn(_value):
    """Menu handlers run off the game thread, so they may block."""
    global _count
    p = sh.player_pos() if hasattr(sh, 'player_pos') else sh.Vec3()
    if not isinstance(p, sh.Vec3):
        return
    if not sh.GetPlayerPosition(p):
        return
    for i in range(sh.NpcCount()):
        a = sh.NpcAt(i)
        if a and a.contents.kind == 1:
            at = sh.Vec3(p.x + 3, p.y, p.z)
            e = sh.SpawnNpc(a.contents.id, at)
            _count += 1
            sh.log('spawned %#x (%d so far)' % (e, _count))
            if _menu:
                _menu.status('%d spawned' % _count)
            return


def on_frame():
    pass


def stop():
    if _menu:
        _menu.destroy()
