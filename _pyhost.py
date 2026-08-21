"""The plugin runtime: load, hot reload, per frame dispatch.

pyhost.asi imports this once and calls tick() at frame
cadence. Everything about threads, reloading and error
containment lives here, so a plugin is just start(),
on_frame() and optionally stop().
"""
import importlib
import os
import sys
import time
import traceback

import sh

SCAN_SECONDS = 1.0

plugins = {}          # module name -> module
frame_fns = []        # (module name, callable)
_mtimes = {}
_next_scan = 0.0
_auto = True
_gen = 0


def _dir():
    return sh.plugin_dir()


def _names():
    try:
        found = os.listdir(_dir())
    except OSError:
        return []
    out = []
    for name in sorted(found):
        if name.endswith('.py') and name != 'sh.py':
            out.append(name[:-3])
    return out


def _path(mod):
    return os.path.join(_dir(), mod + '.py')


def _drop_frame_fn(mod):
    frame_fns[:] = [(n, f) for (n, f) in frame_fns if n != mod]


def _stop(mod):
    m = plugins.get(mod)
    _drop_frame_fn(mod)
    if m is None:
        return
    if hasattr(m, 'stop'):
        try:
            m.stop()
        except Exception:
            sh.log('stop FAILED %s\n%s' % (mod, traceback.format_exc()))


def _wait_for_world(seconds=30.0):
    """The player resolves a moment after the world loads."""
    end = time.perf_counter() + seconds
    p = sh.Player()
    while time.perf_counter() < end:
        if sh.GetPlayer(p) and p.entity:
            return True
        time.sleep(0.25)
    return False


def _start(mod, m):
    plugins[mod] = m
    if hasattr(m, 'on_frame'):
        frame_fns.append((mod, m.on_frame))
    if hasattr(m, 'start'):
        def go():
            if _wait_for_world():
                m.start()
            else:
                sh.log('start skipped, no player: ' + mod)
        sh.task(go)


def load(mod):
    try:
        importlib.invalidate_caches()
        if mod in sys.modules:
            m = importlib.reload(sys.modules[mod])
            sh.log('reloaded ' + mod)
        else:
            m = importlib.import_module(mod)
            sh.log('loaded ' + mod)
        _start(mod, m)
        return True
    except Exception:
        sh.log('FAILED %s\n%s' % (mod, traceback.format_exc()))
        plugins.pop(mod, None)
        _drop_frame_fn(mod)
        return False


def unload(mod):
    _stop(mod)
    plugins.pop(mod, None)
    sys.modules.pop(mod, None)
    _mtimes.pop(mod, None)
    sh.log('unloaded ' + mod)


def scan():
    """Load new files, reload edited ones, drop deleted ones."""
    seen = set()
    for mod in _names():
        seen.add(mod)
        try:
            stamp = os.path.getmtime(_path(mod))
        except OSError:
            continue
        if mod not in _mtimes:
            _mtimes[mod] = stamp
            load(mod)
        elif stamp != _mtimes[mod]:
            _mtimes[mod] = stamp
            _stop(mod)
            load(mod)
    for mod in list(_mtimes):
        if mod not in seen:
            unload(mod)


def fire(token, value):
    """Menu callback, bounced here off the game thread."""
    sh._fire(token, value)


def reload_all():
    """Menu action: stop everything and load it again."""
    for mod in list(plugins):
        _stop(mod)
        sys.modules.pop(mod, None)
    _mtimes.clear()
    scan()
    report()


def set_auto(on):
    """Menu toggle: watch the folder for edits."""
    global _auto
    _auto = bool(on)
    sh.log('watch files ' + ('on' if _auto else 'off'))


def report():
    """Menu action: what is loaded, into the log and status."""
    names = sorted(plugins)
    sh.log('python mods: %s' % (', '.join(names) or 'none'))
    for mod in names:
        m = plugins[mod]
        sh.log('  %s%s' % (mod, ' [frame]' if hasattr(m, 'on_frame') else ''))
    sh.status('%d python mods' % len(names))


def _check_world():
    """The UI generation changes when the world reloads.

    Every entity handle a plugin held is dead at that point,
    so plugins hear about it and the host drops its own
    event handlers.
    """
    global _gen
    gen = sh.UiGen()
    if gen == _gen:
        return
    first = _gen == 0
    _gen = gen
    if first:
        return
    sh.log('world reloaded, generation %d' % gen)
    for mod in list(plugins):
        m = plugins[mod]
        if hasattr(m, 'on_world_reload'):
            try:
                m.on_world_reload()
            except Exception:
                sh.log('on_world_reload FAILED %s\n%s'
                       % (mod, traceback.format_exc()))


def tick():
    global _next_scan
    _check_world()
    now = time.perf_counter()
    if now >= _next_scan:
        _next_scan = now + SCAN_SECONDS
        if _auto:
            try:
                scan()
            except Exception:
                sh.log('scan FAILED\n' + traceback.format_exc())
    sh._pump_events()
    for name, fn in list(frame_fns):
        try:
            fn()
        except Exception:
            sh.log('on_frame FAILED %s\n%s'
                   % (name, traceback.format_exc()))
            _drop_frame_fn(name)
