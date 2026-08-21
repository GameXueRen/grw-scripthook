"""Conga line: six NPCs walk your exact path behind you.

Every frame the player position goes into a ring buffer and
each NPC is placed where the player stood N frames ago, so
the line snakes through doors and around corners exactly the
way you did. All python: spawning on a thread (SpawnNpc waits
on the frame pump, so it must never run inside on_frame),
placement on the frame hook.
"""
import math
import threading

import sh

COUNT = 6
SPACING = 45          # frames between dancers
HISTORY = SPACING * (COUNT + 1)

_trail = []
_ents = []


def _spawn_line():
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


def start():
    threading.Thread(target=_spawn_line, daemon=True).start()


def on_frame():
    p = sh.Vec3()
    if not sh.GetPlayerPosition(p):
        return
    _trail.append((p.x, p.y, p.z))
    if len(_trail) > HISTORY:
        del _trail[0]
    for i, e in enumerate(_ents):
        back = (i + 1) * SPACING
        if back >= len(_trail):
            continue
        x, y, z = _trail[-1 - back]
        # face along the path (our measured yaw convention)
        px, py, _ = _trail[-back] if back > 1 else _trail[-1]
        yaw = -(math.atan2(py - y, px - x) + math.pi / 2)
        at = sh.Vec3(x, y, z)
        sh.QueueTransform(e, at, yaw, 0.0, 0.0)
