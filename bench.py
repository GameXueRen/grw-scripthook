"""Measures what a python plugin actually costs.

start() times batches of the three call shapes. on_frame
counts ticks and reports the achieved rate, so "is it fast"
has a number rather than an impression.
"""
import time

import sh

_ticks = 0
_last = 0.0


def start():
    p = sh.Vec3()
    if not sh.GetPlayerPosition(p):
        sh.log('bench: no player')
        return

    t0 = time.perf_counter()
    for _ in range(10000):
        sh.GetPlayerPosition(p)
    us = (time.perf_counter() - t0) * 1e6 / 10000
    sh.log('bench GetPlayerPosition %.2f us per call' % us)

    pl = sh.Player()
    if not (sh.GetPlayer(pl) and pl.entity):
        sh.log('bench: no player entity')
        return

    t0 = time.perf_counter()
    for _ in range(10000):
        sh.q(pl.entity + 0x50)
    us = (time.perf_counter() - t0) * 1e6 / 10000
    sh.log('bench guarded memory read %.2f us per call' % us)

    if pl.entity:
        t0 = time.perf_counter()
        for _ in range(1000):
            sh.QueueTransform(pl.entity, p, 0.0, 0.0, 0.0)
        us = (time.perf_counter() - t0) * 1e6 / 1000
        sh.log('bench QueueTransform %.2f us per call' % us)


def on_frame():
    global _ticks, _last
    _ticks += 1
    now = time.perf_counter()
    if _last == 0.0:
        _last = now
        return
    if now - _last >= 2.0:
        sh.log('bench tick rate %.0f per second' % (_ticks / (now - _last)))
        _ticks = 0
        _last = now
