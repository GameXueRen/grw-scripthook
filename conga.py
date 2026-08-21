"""Conga line: six NPCs walk your exact path behind you.

The trail only extends forward once you have actually moved,
so nobody walks ahead of where you have been. Placement runs
every tick regardless, at a fixed arc length back along that
trail, so the line moves smoothly and settles when you stop.
"""
import math

import sh

COUNT = 6
STEP = 0.25           # metres before the trail extends
SPACING = 1.8         # metres between dancers
HISTORY = 400         # trail points kept

_trail = []
_ents = []
_menu = None
_running = True


def _on_running(value):
    global _running
    _running = bool(value)


def _on_spacing(value):
    global SPACING
    SPACING = value


def _on_respawn(_value):
    del _ents[:]
    del _trail[:]
    start()


def start():
    global _menu
    if _menu is None:
        _menu = sh.menu('Conga line')
        if _menu:
            _menu.toggle('Following', True, _on_running)
            _menu.number('Spacing', SPACING, 0.8, 6.0, 0.2,
                         _on_spacing)
            _menu.action('Respawn', _on_respawn)
    _spawn()


def stop():
    if _menu:
        _menu.destroy()


def _spawn():
    p = sh.Vec3()
    if not sh.GetPlayerPosition(p):
        sh.log('conga: no player')
        return
    picked = 0
    i = 0
    n = sh.NpcCount()
    while i < n and picked < COUNT:
        a = sh.NpcAt(i)
        i += 1
        if not a or a.contents.kind != 1:
            continue
        at = sh.Vec3(p.x - 2.0 * (picked + 1), p.y, p.z)
        e = sh.SpawnNpc(a.contents.id, at)
        if e:
            _ents.append(e)
            picked += 1
    sh.log('conga line: %d dancers' % len(_ents))


def _at_distance(back):
    """Point and heading `back` metres along the trail."""
    walked = 0.0
    i = len(_trail) - 1
    while i > 0:
        ax, ay, az = _trail[i]
        bx, by, bz = _trail[i - 1]
        seg = math.hypot(ax - bx, ay - by)
        if seg <= 0.0001:
            i -= 1
            continue
        if walked + seg >= back:
            t = (back - walked) / seg
            x = ax + (bx - ax) * t
            y = ay + (by - ay) * t
            z = az + (bz - az) * t
            return x, y, z, math.atan2(ay - by, ax - bx)
        walked += seg
        i -= 1
    if _trail:
        x, y, z = _trail[0]
        return x, y, z, 0.0
    return None


def on_frame():
    p = sh.Vec3()
    if _running is False:
        return
    if not sh.GetPlayerPosition(p):
        return
    here = (p.x, p.y, p.z)
    if _trail:
        lx, ly, _ = _trail[-1]
        if math.hypot(here[0] - lx, here[1] - ly) >= STEP:
            _trail.append(here)
    else:
        _trail.append(here)
    if len(_trail) > HISTORY:
        del _trail[0]

    for i, e in enumerate(_ents):
        spot = _at_distance((i + 1) * SPACING)
        if spot is None:
            continue
        x, y, z, heading = spot
        sh.QueueTransform(e, sh.Vec3(x, y, z),
                          -(heading + math.pi / 2), 0.0, 0.0)
