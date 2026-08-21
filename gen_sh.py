#!/usr/bin/env python3
"""Generate sh.py, ctypes bindings, from scripthook.h.

Run at build time: python3 gen_sh.py scripthook.h ../python/sh.py
Every SH_API function becomes sh.<Name> minus the Sh prefix,
and every simple typedef struct becomes a ctypes.Structure.
"""
import re
import sys

CTYPE = {
    "void": None,
    "int": "c_int",
    "float": "c_float",
    "double": "c_double",
    "char": "c_char",
    "uint8_t": "c_uint8",
    "uint16_t": "c_uint16",
    "uint32_t": "c_uint32",
    "uint64_t": "c_uint64",
    "int8_t": "c_int8",
    "int16_t": "c_int16",
    "int32_t": "c_int32",
    "int64_t": "c_int64",
    "size_t": "c_size_t",
    "uintptr_t": "c_uint64",
}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def ctype_of(decl, structs):
    decl = decl.strip()
    decl = re.sub(r"\bconst\b", "", decl).strip()
    stars = decl.count("*")
    base = decl.replace("*", "").strip()
    if base == "char" and stars == 1:
        return "c_char_p"
    if base == "void" and stars:
        return "c_void_p"
    if base in structs:
        inner = base[2:] if base.startswith("Sh") else base
        return ("POINTER(%s)" % inner) if stars else inner
    if base in CTYPE:
        t = CTYPE[base]
        if t is None:
            return "c_void_p" if stars else None
        return ("POINTER(%s)" % t) if stars else t
    # unknown types (function pointers, enums) pass as void*
    return "c_void_p" if stars else "c_int"


def parse_structs(text):
    out = {}
    pat = re.compile(r"typedef\s+struct\s*\{([^}]*)\}\s*(\w+)\s*;", re.S)
    for body, name in pat.findall(text):
        fields = []
        ok = True
        for line in body.split(";"):
            line = line.strip()
            if not line:
                continue
            # "float x, y, z" and "const char *name" both:
            # the last identifier of the first part is the
            # name, what precedes it is the type.
            parts = [s.strip() for s in line.split(",")]
            m = re.match(r"^(.*?)([A-Za-z_]\w*)"
                         r"(?:\s*\[\s*(\w+)\s*\])?$", parts[0])
            if not m:
                ok = False
                break
            decl, nm, cnt = m.groups()
            t = ctype_of(decl, out)
            if t is None:
                ok = False
                break
            fields.append((nm, t, cnt))
            for extra in parts[1:]:
                m2 = re.match(r"^(\*?)\s*([A-Za-z_]\w*)"
                              r"(?:\s*\[\s*(\w+)\s*\])?$", extra)
                if not m2:
                    ok = False
                    break
                star, nm2, cnt2 = m2.groups()
                t2 = ctype_of(decl + star, out)
                if t2 is None:
                    ok = False
                    break
                fields.append((nm2, t2, cnt2))
            if not ok:
                break
        if ok and fields:
            out[name] = fields
    return out


def parse_funcs(text, structs):
    out = []
    pat = re.compile(r"SH_API\s+([\w \t\*]+?[\w\*])\s*(Sh\w+)\s*\(([^;]*?)\)\s*;",
                     re.S)
    for ret, name, args in pat.findall(text):
        rt = ctype_of(ret, structs)
        ats = []
        args = " ".join(args.split())
        ok = True
        if args and args != "void":
            for a in args.split(","):
                a = a.strip()
                a = re.sub(r"\s*\w+$", "", a) if not a.endswith("*") else a
                t = ctype_of(a, structs)
                if t is None:
                    ok = False
                    break
                ats.append(t)
        if ok:
            out.append((name, rt, ats))
    return out


def parse_consts(text):
    """SH_ defines and anonymous enums, as plain ints.

    Without these a plugin has to hardcode 0x04 for
    SH_CAM_FOV, which is exactly the kind of magic number
    the header exists to prevent.
    """
    out = []
    seen = set()

    for name, val in re.findall(
            r"#define\s+(SH_[A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+|\d+)\b", text):
        if name in seen:
            continue
        seen.add(name)
        out.append((name[3:], int(val, 0)))

    for body in re.findall(r"enum\s+\w*\s*\{([^}]*)\}", text):
        nxt = 0
        for item in body.split(","):
            item = item.strip()
            if not item.startswith("SH_"):
                continue
            m = re.match(r"(SH_[A-Z0-9_]+)\s*(?:=\s*(0x[0-9A-Fa-f]+|\d+))?$",
                         item)
            if not m:
                continue
            name, val = m.groups()
            nxt = int(val, 0) if val else nxt
            if name not in seen:
                seen.add(name)
                out.append((name[3:], nxt))
            nxt += 1
    return out


def main():
    src, dst = sys.argv[1], sys.argv[2]
    text = strip_comments(open(src).read())
    structs = parse_structs(text)
    funcs = parse_funcs(text, structs)
    consts = parse_consts(text)

    w = open(dst, "w")
    w.write('"""ScriptHook bindings, generated by gen_sh.py. '
            'Edit the generator, never this file."""\n')
    w.write("import ctypes\nfrom ctypes import (POINTER, c_int, c_float,"
            " c_double, c_char, c_char_p,\n    c_void_p, c_uint8, c_uint16,"
            " c_uint32, c_uint64, c_int8, c_int16,\n    c_int32, c_int64,"
            " c_size_t)\n\n")
    w.write("_dll = ctypes.WinDLL('dinput8.dll')\n")
    w.write("_host = ctypes.WinDLL('pyhost.asi')\n")
    w.write("_host.PyHostGameDir.restype = ctypes.c_char_p\n\n")
    w.write("def log(msg):\n    _host.PyHostLog(str(msg).encode())\n\n")
    w.write("def status(msg):\n"
            "    \"\"\"The line under the Python mods menu.\"\"\"\n"
            "    _host.PyHostStatus(str(msg).encode())\n\n")
    w.write("def plugin_dir():\n"
            "    import os\n"
            "    return os.path.join(_host.PyHostGameDir().decode(),"
            " 'plugins')\n\n")
    w.write("def q(addr):\n"
            "    \"\"\"Guarded 8 byte read; None when unreadable.\"\"\"\n"
            "    ok = ctypes.c_int(0)\n"
            "    v = ReadU64(addr, ctypes.byref(ok))\n"
            "    return v if ok.value else None\n\n")
    w.write("def f32(addr):\n"
            "    \"\"\"Guarded float read; None when unreadable.\"\"\"\n"
            "    ok = ctypes.c_int(0)\n"
            "    v = ReadF32(addr, ctypes.byref(ok))\n"
            "    return v if ok.value else None\n\n")

    for name, fields in structs.items():
        py = name[2:] if name.startswith("Sh") else name
        w.write("class %s(ctypes.Structure):\n    _fields_ = [\n" % py)
        for nm, t, cnt in fields:
            if cnt:
                w.write("        ('%s', %s * %s),\n" % (nm, t, cnt))
            else:
                w.write("        ('%s', %s),\n" % (nm, t))
        w.write("    ]\n\n")

    for name, rt, ats in funcs:
        py = name[2:]
        w.write("%s = _dll.%s\n" % (py, name))
        w.write("%s.restype = %s\n" % (py, rt if rt else "None"))
        if ats:
            w.write("%s.argtypes = [%s]\n" % (py, ", ".join(ats)))
        w.write("\n")
    w.write("\n# Constants from the header, SH_ prefix dropped.\n")
    for name, val in consts:
        w.write("%s = %s\n" % (name, hex(val) if val > 9 else val))
    w.write(EPILOGUE)
    w.close()
    print("sh.py: %d structs, %d functions, %d constants"
          % (len(structs), len(funcs), len(consts)))


# Runtime conveniences, appended verbatim: the host runs
# start() on workers and on_frame() on the game thread, so
# every thread shaped thing lives here, never in a plugin.
EPILOGUE = '''
import threading as _threading
import time as _time

_in_frame = False

_host.PyHostMenu.restype = ctypes.c_uint32
_host.PyHostMenu.argtypes = [c_char_p]
_host.PyHostMenuSub.restype = ctypes.c_uint32
_host.PyHostMenuSub.argtypes = [ctypes.c_uint32, c_char_p]
_host.PyHostMenuAction.argtypes = [ctypes.c_uint32, c_char_p, c_int]
_host.PyHostMenuToggle.argtypes = [ctypes.c_uint32, c_char_p,
                                   c_int, c_int]
_host.PyHostMenuNumber.argtypes = [ctypes.c_uint32, c_char_p,
                                   c_float, c_float, c_float,
                                   c_float, c_int]
_host.PyHostMenuList.argtypes = [ctypes.c_uint32, c_char_p,
                                 c_char_p, c_int, c_int]
_host.PyHostMenuStatus.argtypes = [ctypes.c_uint32, c_char_p]
_host.PyHostMenuClear.argtypes = [ctypes.c_uint32]
_host.PyHostMenuDestroy.argtypes = [ctypes.c_uint32]

_handlers = {}
_next_token = 1


def _register(fn):
    global _next_token
    token = _next_token
    _next_token += 1
    _handlers[token] = fn
    return token


def _fire(token, value):
    fn = _handlers.get(token)
    if fn is None:
        return
    try:
        fn(value)
    except Exception:
        import traceback
        log('menu handler FAILED\\n' + traceback.format_exc())


class Menu(object):
    """A row in the F4 menu, owned by your plugin.

    Handlers run on the host's driver thread, so they may
    call anything, including calls that wait for a frame.
    """

    def __init__(self, handle):
        self.handle = handle

    def sub(self, label):
        return Menu(_host.PyHostMenuSub(self.handle,
                                        label.encode()))

    def action(self, label, fn):
        return _host.PyHostMenuAction(self.handle, label.encode(),
                                      _register(fn))

    def toggle(self, label, initial, fn):
        return _host.PyHostMenuToggle(self.handle, label.encode(),
                                      1 if initial else 0,
                                      _register(fn))

    def number(self, label, initial, lo, hi, step, fn):
        return _host.PyHostMenuNumber(self.handle, label.encode(),
                                      initial, lo, hi, step,
                                      _register(fn))

    def options(self, label, choices, initial, fn):
        csv = '|'.join(str(c) for c in choices)
        return _host.PyHostMenuList(self.handle, label.encode(),
                                    csv.encode(), initial,
                                    _register(fn))

    def status(self, text):
        return _host.PyHostMenuStatus(self.handle, str(text).encode())

    def clear(self):
        return _host.PyHostMenuClear(self.handle)

    def destroy(self):
        return _host.PyHostMenuDestroy(self.handle)


def menu(title):
    """Add your own row to the F4 menu."""
    h = _host.PyHostMenu(title.encode())
    return Menu(h) if h else None


def why():
    """Why the last call failed, as text."""
    code = LastError()
    text = ErrorString(code)
    if isinstance(text, bytes):
        text = text.decode('utf-8', 'replace')
    return '%s (%d)' % (text, code)


_host.PyHostWatchHits.argtypes = [c_int]
_host.PyHostWatchFire.argtypes = [c_int]
_host.PyHostNextHit.argtypes = [POINTER(Hit)]
_host.PyHostNextShot.argtypes = [POINTER(Shot)]

_hit_fns = []
_shot_fns = []


def on_hit(fn):
    """Call fn(hit) for every hit the engine reports."""
    if not _hit_fns:
        _host.PyHostWatchHits(1)
    _hit_fns.append(fn)
    return fn


def on_fire(fn):
    """Call fn(shot) for every shot the engine reports."""
    if not _shot_fns:
        _host.PyHostWatchFire(1)
    _shot_fns.append(fn)
    return fn


def clear_events():
    """Drop this plugin's event handlers, used on reload."""
    del _hit_fns[:]
    del _shot_fns[:]
    _ui_fns.clear()
    del _commit_fns[:]
    _host.PyHostWatchHits(0)
    _host.PyHostWatchFire(0)


_host.PyHostUiInput.argtypes = [ctypes.c_uint32, c_int]
_host.PyHostNextUiEvent.argtypes = [POINTER(ctypes.c_uint32),
                                    POINTER(UiEvent)]
_host.PyHostNextCommit.argtypes = [POINTER(c_int)]

_ui_fns = {}
_commit_fns = []


def on_ui_input(scene, fn, eat_keys=True):
    """Call fn(event) for input on a UI scene of yours."""
    _ui_fns.setdefault(scene, []).append(fn)
    return _host.PyHostUiInput(scene, 1 if eat_keys else 0)


def commit_async(fn=None):
    """Commit UI work from a worker; fn(ok) when it lands."""
    if fn is not None:
        _commit_fns.append(fn)
    return _host.PyHostCommitAsync()


def _pump_ui():
    import traceback
    scene = ctypes.c_uint32(0)
    ev = UiEvent()
    while _ui_fns and _host.PyHostNextUiEvent(ctypes.byref(scene),
                                              ctypes.byref(ev)):
        for fn in list(_ui_fns.get(scene.value, ())):
            try:
                fn(ev)
            except Exception:
                log('on_ui_input FAILED\\n' + traceback.format_exc())
    ok = c_int(0)
    while _commit_fns and _host.PyHostNextCommit(ctypes.byref(ok)):
        for fn in list(_commit_fns):
            try:
                fn(bool(ok.value))
            except Exception:
                log('commit_async FAILED\\n' + traceback.format_exc())


def _pump_events():
    import traceback
    _pump_ui()
    hit = Hit()
    while _hit_fns and _host.PyHostNextHit(ctypes.byref(hit)):
        for fn in list(_hit_fns):
            try:
                fn(hit)
            except Exception:
                log('on_hit FAILED\\n' + traceback.format_exc())
    shot = Shot()
    while _shot_fns and _host.PyHostNextShot(ctypes.byref(shot)):
        for fn in list(_shot_fns):
            try:
                fn(shot)
            except Exception:
                log('on_fire FAILED\\n' + traceback.format_exc())


def task(fn, *a, **k):
    """Run fn on a background thread; errors go to the log."""
    def run():
        try:
            fn(*a, **k)
        except Exception:
            import traceback
            log('task FAILED: ' + traceback.format_exc())
    t = _threading.Thread(target=run, daemon=True)
    t.start()
    return t

def every(seconds, fn):
    """Call fn repeatedly until it returns False."""
    def run():
        while True:
            try:
                if fn() is False:
                    return
            except Exception:
                import traceback
                log('every FAILED: ' + traceback.format_exc())
                return
            _time.sleep(seconds)
    return task(run)
'''


if __name__ == "__main__":
    main()
