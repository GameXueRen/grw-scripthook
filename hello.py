"""Demo python plugin: proves start() and on_frame().

start() runs on a worker thread when the world is up, so it
may call anything. on_frame() runs at frame cadence on the
host's driver thread; keep it cheap and never log from it
every frame.
"""
import sh

_frames = 0


def start():
    sh.log('hello.py started, %d npc archetypes' % sh.NpcCount())


def on_frame():
    global _frames
    _frames += 1
    if _frames % 600:
        return
    p = sh.Vec3()
    if sh.GetPlayerPosition(p):
        sh.log('hello.py frame %d at %.0f %.0f %.0f'
               % (_frames, p.x, p.y, p.z))
