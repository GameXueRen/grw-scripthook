"""Demo python plugin: proves start() and on_frame().

start() runs once when the world is up. on_frame() runs on
the game thread every frame; this one counts frames and once
a second logs the player position through the same C API the
compiled plugins use.
"""
import sh

_frames = 0


def start():
    sh.log('hello.py started, %d npc archetypes' % sh.NpcCount())


def on_frame():
    global _frames
    _frames += 1
    if _frames % 60:
        return
    p = sh.Vec3()
    if sh.GetPlayerPosition(p):
        sh.log('frame %d player %.1f %.1f %.1f'
               % (_frames, p.x, p.y, p.z))
