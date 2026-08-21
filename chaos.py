"""Chaos mod: a random effect every so often, in Python.

A port of chaos.c. The wheel spins, an effect lands, timed
ones expire on their own. Everything here is hot reloadable,
so tuning an effect no longer means a rebuild and a restart.

The blackout effects draw with the engine's own UI panels
rather than a Win32 window, so they sit inside the game
image instead of over the top of it.
"""
import ctypes
import math
import os
import random
import time
import traceback

import sh

# ---- tunables, edit and save to see them apply ----

EVERY = 30.0          # seconds between rolls
HOLD = 20.0           # default seconds a timed effect runs
MAX_ACTIVE = 8
HIST_MAX = 6
FLASH_SECONDS = 4.0
SPIN_SECONDS = 2.0

WP_N, WP_R, WP_RATE = 12, 3.5, 1.2
FIDGET_MAX, FIDGET_RATE, FIDGET_SCAN = 24, 3.4, 0.4
TP_RANGE = 40.0

_user32 = ctypes.WinDLL('user32')


# ---- small helpers ----

def _pos():
    p = sh.Vec3()
    return p if sh.GetPlayerPosition(p) else None


def _root():
    p = sh.Player()
    if sh.GetPlayer(p):
        return p.root or p.entity
    return 0


def _transform(entity):
    """(pos, yaw, pitch, roll) or None."""
    v = sh.Vec3()
    y, pi, r = ctypes.c_float(), ctypes.c_float(), ctypes.c_float()
    if sh.GetEntityTransform(entity, v, ctypes.byref(y),
                             ctypes.byref(pi), ctypes.byref(r)):
        return v, y.value, pi.value, r.value
    return None


def _nearby(kind, radius, limit=64):
    buf = (sh.Entity * limit)()
    n = sh.FindEntities(kind, radius, 0, buf, limit)
    return [buf[i] for i in range(max(0, n))]


def _ammo_all(n):
    for slot in range(3):
        sh.SetAmmo(slot, n)


def _fov(f):
    o = sh.CameraOverride()
    o.apply = sh.CAM_FOV
    o.fov = f
    sh.CameraApply(o)


def _skew(x, y):
    o = sh.CameraOverride()
    o.apply = sh.CAM_SKEW
    o.skewX = x
    o.skewY = y
    sh.CameraApply(o)


_in_mask = 0


def _block(bits, on):
    global _in_mask
    _in_mask = (_in_mask | bits) if on else (_in_mask & ~bits)
    sh.BlockInput(_in_mask)


class _KI(ctypes.Structure):
    _fields_ = [('wVk', ctypes.c_ushort), ('wScan', ctypes.c_ushort),
                ('dwFlags', ctypes.c_uint32), ('time', ctypes.c_uint32),
                ('dwExtraInfo', ctypes.c_void_p)]


class _INPUT(ctypes.Structure):
    _fields_ = [('type', ctypes.c_uint32), ('ki', _KI),
                ('pad', ctypes.c_ubyte * 8)]


def _tap(vk, down):
    """One synthetic key, the way chaos.c does it."""
    e = _INPUT()
    e.type = 1
    e.ki.wVk = vk
    e.ki.dwFlags = 0 if down else 2
    e.ki.dwExtraInfo = ctypes.c_void_p(0xC4A05)
    _user32.SendInput(1, ctypes.byref(e), ctypes.sizeof(_INPUT))


def _nudge(dx, dy):
    _user32.mouse_event(0x0001, dx, dy, 0, 0xC4A05)


def _turn_target():
    """(entity, in_car). In a car the root is the car."""
    p = sh.Player()
    if not sh.GetPlayer(p):
        return 0, False
    if sh.IsInVehicle():
        root = ctypes.c_uint64(0)
        ok = sh.WalkToRoot(p.entity, ctypes.byref(root))
        if ok and root.value and root.value != p.entity:
            return root.value, True
    return p.entity, False


# ---- the blackout, drawn with engine panels ----

class Blackout(object):
    """Full, portrait, letterbox or peephole, in game UI.

    Coordinates are fractions of the screen, so this needs no
    window handle and no Win32 regions.
    """

    def __init__(self):
        self.panels = []

    def _bar(self, x, y, w, h):
        p = sh.UiPanel(x, y, w, h, 0x000000, 1.0)
        if p:
            self.panels.append(p)

    def show(self, mode):
        self.clear()
        if mode == 'full':
            self._bar(0.0, 0.0, 1.0, 1.0)
        elif mode == 'portrait':
            # A 9:16 window centred, black either side.
            side = (1.0 - (9.0 / 16.0) * (9.0 / 16.0)) / 2.0
            self._bar(0.0, 0.0, side, 1.0)
            self._bar(1.0 - side, 0.0, side, 1.0)
        elif mode == 'letterbox':
            band = (1.0 - (9.0 / 21.0)) / 2.0
            self._bar(0.0, 0.0, 1.0, band)
            self._bar(0.0, 1.0 - band, 1.0, band)
        elif mode == 'peephole':
            # Four bars leave a hole in the middle.
            k = 0.35
            self._bar(0.0, 0.0, 1.0, k)
            self._bar(0.0, 1.0 - k, 1.0, k)
            self._bar(0.0, k, 0.30, 1.0 - 2 * k)
            self._bar(0.70, k, 0.30, 1.0 - 2 * k)

    def clear(self):
        for p in self.panels:
            sh.UiDestroy(p)
        self.panels = []


_black = Blackout()


# ---- instant effects ----

def fx_skydive():
    p = _pos()
    if p:
        p.z += 300.0
        sh.TeleportPlayer(p, None)


def fx_slam():
    p = _pos()
    if p:
        sh.TeleportPlayerToGround(p.x, p.y, 0.5)


def _drop_vehicles(count, height):
    p = _pos()
    n = sh.VehicleCount()
    if p is None or n <= 0:
        return
    for _ in range(count):
        v = sh.VehicleAt(random.randrange(n))
        if not v:
            continue
        at = sh.Vec3(p.x + random.uniform(-8, 8),
                     p.y + random.uniform(-8, 8),
                     p.z + height + random.uniform(0, 10))
        sh.SpawnVehicle(v.contents.id, at)


def fx_car_rain():
    _drop_vehicles(12, 60.0)


def fx_one_car():
    _drop_vehicles(1, 25.0)


def fx_heal():
    cur, mx = ctypes.c_uint32(), ctypes.c_uint32()
    if sh.GetHealthPlayer(ctypes.byref(cur), ctypes.byref(mx)):
        sh.SetHealthPlayer(mx.value)


def fx_one_hp():
    sh.SetHealthPlayer(1)


def fx_smite():
    for e in _nearby(sh.KIND_NPC, 120.0):
        sh.SetHealthEntity(e.entity, 0)


def fx_rich():
    sh.SetAllResources(9999)
    sh.SetSkillPoints(50)


def fx_broke():
    sh.SetAllResources(0)


def fx_dry():
    _ammo_all(0)


def fx_loaded():
    _ammo_all(999)


def fx_midnight():
    sh.SetTime(0.5)


def fx_noon():
    sh.SetTime(12.0)


def fx_dawn():
    sh.SetTime(6.0)


def fx_storm():
    sh.SetWeather(sh.WEATHER_RAIN_HEAVY)


def fx_clear():
    sh.SetWeather(sh.WEATHER_SUNNY)


def fx_fling():
    p = _pos()
    if p:
        p.z += 60.0
        sh.TeleportPlayer(p, None)


def fx_suicide():
    sh.KillPlayer()


def fx_180():
    t, _ = _turn_target()
    got = _transform(t) if t else None
    if got:
        pos, yaw, pitch, roll = got
        sh.PlaceEntityRot(t, pos, yaw + math.pi, pitch, roll)


def fx_balkan():
    t, in_car = _turn_target()
    if not t:
        return
    if in_car is False:
        fx_180()
        return
    got = _transform(t)
    if got:
        pos, yaw, pitch, _ = got
        pos.z += 0.6
        sh.PlaceEntityRot(t, pos, yaw, pitch, math.pi)


def fx_parking():
    """Six cars in a ring, rolled onto their roofs."""
    p = _pos()
    n = sh.VehicleCount()
    if p is None or n <= 0:
        return
    for i in range(6):
        v = sh.VehicleAt(random.randrange(n))
        if not v:
            continue
        a = i * 1.0472
        at = sh.Vec3(p.x + math.cos(a) * 7.0,
                     p.y + math.sin(a) * 7.0, p.z + 1.0)
        e = sh.SpawnVehicle(v.contents.id, at)
        if e:
            at.z += 0.6
            sh.PlaceEntityRot(e, at, a + 1.5708, 0.0, math.pi)


# ---- timed effects ----

def tick_god():
    cur, mx = ctypes.c_uint32(), ctypes.c_uint32()
    ok = sh.GetHealthPlayer(ctypes.byref(cur), ctypes.byref(mx))
    if ok and cur.value != mx.value:
        sh.SetHealthPlayer(mx.value)


def on_ghost():
    sh.SetVisibility(0.0)


def on_seen():
    sh.SetVisibility(6.0)


def off_sight():
    sh.SetVisibility(1.0)


def on_first():
    sh.CameraFirstPerson(0.15, 0.0)


def on_far():
    sh.CameraOrbit(18.0, 9.0)


def on_close():
    sh.CameraOrbit(0.8, 0.2)


def on_worm():
    sh.CameraOrbit(2.5, -1.8)


def on_bird():
    sh.CameraOrbit(1.0, 28.0)


def off_cam():
    sh.CameraReleaseFields(sh.CAM_POS)


def on_fish():
    _fov(2.0)


def on_tunnel():
    _fov(0.35)


def off_fov():
    sh.CameraReleaseFields(sh.CAM_FOV)


def on_drunk():
    _skew(0.0, 0.0)


def tick_drunk():
    t = (time.time() % 6.0) * 1.047
    _skew(math.sin(t) * 0.25, math.cos(t * 0.7) * 0.15)


def off_skew():
    sh.CameraReleaseFields(sh.CAM_SKEW)


def on_vanish():
    r = _root()
    if r:
        sh.SetVisible(r, 0, 0, 1)


def off_vanish():
    r = _root()
    if r:
        sh.SetVisible(r, 0, 1, 0)


def on_fog():
    sh.SetWeather(sh.WEATHER_FOG)


def off_fog():
    sh.ReleaseWeather()


def on_night():
    sh.SetTime(1.0)


def off_night():
    sh.SetTime(11.0)


def on_jojo():
    sh.SetTimeSpeed(100.0)


def off_jojo():
    sh.SetTimeSpeed(1.0)


def on_blur():
    """Kill the close range body blur; 0 removes it."""
    sh.SetCameraBlur(0)


def off_blur():
    sh.SetCameraBlur(1)


def on_stare():
    _block(sh.INPUT_KEYS | sh.INPUT_LOOK, 1)


def off_stare():
    _block(sh.INPUT_KEYS | sh.INPUT_LOOK, 0)


def on_root():
    _block(sh.INPUT_MOVE, 1)


def off_root():
    _block(sh.INPUT_MOVE, 0)


def on_safety():
    _block(sh.INPUT_FIRE, 1)


def off_safety():
    _block(sh.INPUT_FIRE, 0)


def on_noaim():
    _block(sh.INPUT_AIM, 1)


def off_noaim():
    _block(sh.INPUT_AIM, 0)


def on_march():
    _tap(ord('W'), 1)


def off_march():
    _tap(ord('W'), 0)


def tick_twitch():
    _nudge(random.randrange(-20, 21), random.randrange(-10, 11))


def tick_shove():
    _nudge(random.randrange(-4, 5), 0)


def on_cctv():
    p = sh.Vec3()
    if sh.GetHeadPosition(p) or sh.GetPlayerPosition(p):
        sh.SetCamera(p)


def on_flatline():
    s = sh.GameFlowObject(sh.FLOW_GAMEOVER_SCENE)
    if s:
        sh.SceneEnter(s)


def off_flatline():
    s = sh.GameFlowObject(sh.FLOW_GAMEOVER_SCENE)
    if s:
        sh.SceneExit(s)


def _blind(mode):
    def go():
        _black.show(mode)
    return go


def off_blind():
    _black.clear()


_eye = {'next': 0.0, 'day': False}


def on_eyes():
    _eye['next'] = 0.0
    _eye['day'] = False


def tick_eyes():
    now = time.time()
    if now < _eye['next']:
        return
    _eye['day'] = not _eye['day']
    sh.SetTime(12.0 if _eye['day'] else 0.5)
    _eye['next'] = now + 0.26


_wx = {'next': 0.0, 'step': 0.0}


def on_dice():
    _wx['step'] = 0.12
    _wx['next'] = 0.0


def tick_dice():
    now = time.time()
    if now < _wx['next']:
        return
    sh.SetWeather(random.randrange(6))
    _wx['step'] += _wx['step'] / 3.0 + 0.04
    _wx['next'] = now + _wx['step']


_tp = {'next': 0.0, 'step': 0.0}


def on_tpspin():
    _tp['step'] = 0.26
    _tp['next'] = 0.0


def tick_tpspin():
    now = time.time()
    if now < _tp['next']:
        return
    p = _pos()
    if p:
        sh.TeleportPlayerToGround(
            p.x + random.uniform(-TP_RANGE, TP_RANGE),
            p.y + random.uniform(-TP_RANGE, TP_RANGE), 1.0)
    _tp['step'] += _tp['step'] / 3.0 + 0.06
    _tp['next'] = now + _tp['step']


_fid = {'ents': [], 'scan': 0.0}


def on_fidget():
    _fid['ents'] = []
    _fid['scan'] = 0.0


def _fidget_scan():
    now = time.time()
    have = set(e[0] for e in _fid['ents'])
    for ent in _nearby(sh.KIND_VEHICLE, 50.0, FIDGET_MAX):
        if len(_fid['ents']) >= FIDGET_MAX:
            break
        if ent.entity in have:
            continue
        got = _transform(ent.entity)
        if got:
            pos, yaw = got[0], got[1]
            _fid['ents'].append((ent.entity, yaw,
                                 sh.Vec3(pos.x, pos.y, pos.z), now))


def tick_fidget():
    now = time.time()
    if now - _fid['scan'] >= FIDGET_SCAN:
        _fid['scan'] = now
        _fidget_scan()
    for ent, yaw, pos, t0 in _fid['ents']:
        sh.QueueTransform(ent, pos, yaw + (now - t0) * FIDGET_RATE,
                          0.0, 0.0)


_wp = {'ents': [], 't0': 0.0}


def on_witness():
    """Twelve NPCs in a ring, spun around the player."""
    _wp['ents'] = []
    _wp['t0'] = time.time()
    p = _pos()
    if p is None:
        return
    for i in range(sh.NpcCount()):
        if len(_wp['ents']) >= WP_N:
            break
        a = sh.NpcAt(i)
        if not a or a.contents.kind != 1:
            continue
        ang = len(_wp['ents']) * (2 * math.pi / WP_N)
        at = sh.Vec3(p.x + math.cos(ang) * WP_R,
                     p.y + math.sin(ang) * WP_R, p.z)
        e = sh.SpawnNpc(a.contents.id, at)
        if e:
            _wp['ents'].append(e)


def tick_witness():
    p = _pos()
    if p is None or len(_wp['ents']) == 0:
        return
    t = time.time() - _wp['t0']
    for i, e in enumerate(_wp['ents']):
        a = i * (2 * math.pi / WP_N) + t * WP_RATE
        at = sh.Vec3(p.x + math.cos(a) * WP_R,
                     p.y + math.sin(a) * WP_R, p.z)
        # Yaw runs clockwise and zero yaw looks along -y.
        sh.QueueTransform(e, at, -(a + math.pi / 2), 0.0, 0.0)


def off_witness():
    for e in _wp['ents']:
        sh.Despawn(e)
    _wp['ents'] = []


# ---- motion: velocity and shoves ----

def _cars(radius=45.0, limit=24):
    return _nearby(sh.KIND_VEHICLE, radius, limit)


def _npcs(radius=60.0, limit=32):
    return _nearby(sh.KIND_NPC, radius, limit)


def fx_launch_cars():
    """Every car nearby leaves the ground at once."""
    for e in _cars():
        v = sh.Vec3(random.uniform(-6, 6), random.uniform(-6, 6),
                    random.uniform(18, 30))
        sh.AddVelocity(e.entity, v)


def fx_car_tornado():
    """Spin them where they stand, hard."""
    for e in _cars():
        sh.SetAngularVelocity(e.entity, sh.Vec3(0.0, 0.0,
                                                random.uniform(6, 14)))


def fx_shove_all():
    """One shove outward from the player, cars and people."""
    p = _pos()
    if p is None:
        return
    for e in _cars(50.0) + _npcs(50.0):
        got = _transform(e.entity)
        if got is None:
            continue
        at = got[0]
        dx, dy = at.x - p.x, at.y - p.y
        n = math.hypot(dx, dy) or 1.0
        sh.Shove(e.entity, sh.Vec3(dx / n, dy / n, 0.4), 25.0, None)


def fx_magnet():
    """The opposite: everything comes to you."""
    p = _pos()
    if p is None:
        return
    for e in _cars(70.0) + _npcs(70.0):
        got = _transform(e.entity)
        if got is None:
            continue
        at = got[0]
        dx, dy, dz = p.x - at.x, p.y - at.y, p.z - at.z
        n = math.sqrt(dx * dx + dy * dy + dz * dz) or 1.0
        sh.SetVelocity(e.entity, sh.Vec3(dx / n * 18.0, dy / n * 18.0,
                                         dz / n * 18.0 + 2.0))


def fx_freeze_all():
    """Everything nearby stops dead."""
    zero = sh.Vec3(0.0, 0.0, 0.0)
    for e in _cars(60.0) + _npcs(60.0):
        sh.SetVelocity(e.entity, zero)
        sh.SetAngularVelocity(e.entity, zero)


_float = {'ents': []}


def on_float():
    _float['ents'] = [e.entity for e in _cars(60.0)]
    for e in _float['ents']:
        sh.SetEntityPhysics(e, 0)


def tick_float():
    """Held just off the ground, drifting upward slowly."""
    for e in _float['ents']:
        got = _transform(e)
        if got:
            pos = got[0]
            pos.z += 0.02
            sh.QueueTransform(e, pos, got[1], 0.0, 0.0)


def off_float():
    for e in _float['ents']:
        sh.SetEntityPhysics(e, 1)
    _float['ents'] = []


_jitter = {'ents': []}


def on_jitter():
    _jitter['ents'] = [e.entity for e in _cars(50.0)]


def tick_jitter():
    for e in _jitter['ents']:
        sh.AddVelocity(e, sh.Vec3(random.uniform(-3, 3),
                                  random.uniform(-3, 3),
                                  random.uniform(0, 4)))


def off_jitter():
    _jitter['ents'] = []


def fx_hop():
    """You, upward, without the fall damage."""
    r = _root()
    if r:
        sh.AddVelocity(r, sh.Vec3(0.0, 0.0, 14.0))


# ---- rides: the Domino attach primitive ----

_ride = {'ents': []}


def on_hat_car():
    """A car balanced on your head, physics off."""
    r = _root()
    p = _pos()
    if r == 0 or p is None:
        return
    cars = _cars(80.0, 4)
    if len(cars) == 0:
        return
    e = cars[0].entity
    sh.SetEntityPhysics(e, 0)
    at = sh.Vec3(p.x, p.y, p.z + 2.3)
    sh.PlaceEntityRot(e, at, 0.0, 0.0, 0.0)
    sh.AttachEntity(e, r, sh.Vec3(0.0, 0.0, 2.3))
    _ride['ents'] = [e]


def on_entourage():
    """Six NPCs orbiting, attached so they never lag."""
    r = _root()
    p = _pos()
    if r == 0 or p is None:
        return
    _ride['ents'] = []
    for i in range(sh.NpcCount()):
        if len(_ride['ents']) >= 6:
            break
        a = sh.NpcAt(i)
        if not a or a.contents.kind != 1:
            continue
        ang = len(_ride['ents']) * (2 * math.pi / 6)
        off = sh.Vec3(math.cos(ang) * 2.5, math.sin(ang) * 2.5, 0.0)
        e = sh.SpawnNpc(a.contents.id,
                        sh.Vec3(p.x + off.x, p.y + off.y, p.z))
        if e:
            sh.SetEntityPhysics(e, 0)
            sh.AttachEntity(e, r, off)
            _ride['ents'].append(e)


def off_ride():
    for e in _ride['ents']:
        sh.DetachEntity(e)
        sh.SetEntityPhysics(e, 1)
    _ride['ents'] = []


def off_entourage():
    for e in _ride['ents']:
        sh.DetachEntity(e)
        sh.Despawn(e)
    _ride['ents'] = []


# ---- weather and world, from the Domino work ----

def fx_lightning():
    """Three strikes, right now."""
    for _ in range(3):
        sh.TriggerLightning()


def on_thunderstorm():
    sh.SetWeatherBlend(sh.WEATHER_RAIN_HEAVY, 3.0)
    sh.SetLightningFrequency(1, 25.0)


def tick_thunderstorm():
    if random.random() < 0.02:
        sh.TriggerLightning()


def off_thunderstorm():
    sh.SetLightningFrequency(0, 0.0)
    sh.ReleaseWeather()


def on_shielded():
    """Explosions ignore a sphere around you."""
    p = _pos()
    if p:
        sh.ExplosionShield(p, 30.0)


def tick_shielded():
    p = _pos()
    if p:
        sh.ExplosionShield(p, 30.0)


def off_shielded():
    sh.ExplosionShield(None, 0.0)


def on_ghostmode():
    sh.SetGhostMode(1)


def off_ghostmode():
    sh.SetGhostMode(0)


def on_immortal():
    sh.SetGodModePlayer(1)


def off_immortal():
    sh.SetGodModePlayer(0)


def on_cannot_die():
    sh.SetCannotDiePlayer(1)


def off_cannot_die():
    sh.SetCannotDiePlayer(0)


def fx_paper_cut():
    """One point of damage, through the real damage path."""
    sh.DamagePlayer(1)


def fx_weather_creep():
    """The sky turns over half a minute, not instantly."""
    sh.SetWeatherBlend(random.randrange(6), 30.0)


def fx_hopscotch():
    """A long hop teleport, 300 ms per hop as the API asks."""
    p = _pos()
    if p is None:
        return
    at = sh.Vec3(p.x + random.uniform(-150, 150),
                 p.y + random.uniform(-150, 150), p.z + 40.0)
    sh.TeleportPlayerHops(at, None, 60.0, 300)


# ---- combat events ----

# One handler each, registered once in start(). Effects
# only flip a flag, so one ending leaves the others alone.
_evt = {'recoil': False, 'vampire': False, 'impact': False}


def _on_shot(_shot):
    """Every shot you fire kicks you off the ground."""
    if _evt['recoil'] is False:
        return
    r = _root()
    if r:
        sh.AddVelocity(r, sh.Vec3(random.uniform(-2, 2),
                                  random.uniform(-2, 2), 6.0))


def _on_hit(hit):
    if hit.byPlayer is False:
        return
    if _evt['vampire']:
        cur, mx = ctypes.c_uint32(), ctypes.c_uint32()
        if sh.GetHealthPlayer(ctypes.byref(cur), ctypes.byref(mx)):
            sh.SetHealthPlayer(min(mx.value, cur.value + 200))
    if _evt['impact'] and hit.root:
        sh.Shove(hit.root, sh.Vec3(0.0, 0.0, 1.0), 40.0, None)


def _flag(name, on):
    def go():
        _evt[name] = on
    return go


# ---- the effect table ----
# (name, timed, start, tick, stop, seconds; 0 means default)

EFFECTS = [
    ('Skydive', 0, fx_skydive, None, None, 0),
    ('Ground Slam', 0, fx_slam, None, None, 0),
    ("It's Raining Cars", 0, fx_car_rain, None, None, 0),
    ('Company Car', 0, fx_one_car, None, None, 0),
    ('Patched Up', 0, fx_heal, None, None, 0),
    ('One Hit Point', 0, fx_one_hp, None, None, 0),
    ('Smite the Cartel', 0, fx_smite, None, None, 0),
    ('Supply Drop', 0, fx_rich, None, None, 0),
    ('Bankrupt', 0, fx_broke, None, None, 0),
    ('Click Click', 0, fx_dry, None, None, 0),
    ('Full Mags', 0, fx_loaded, None, None, 0),
    ('Midnight', 0, fx_midnight, None, None, 0),
    ('High Noon', 0, fx_noon, None, None, 0),
    ('Sunrise', 0, fx_dawn, None, None, 0),
    ('Weather Roulette', 1, on_dice, tick_dice, None, 4),
    ('Downpour', 0, fx_storm, None, None, 0),
    ('Blue Skies', 0, fx_clear, None, None, 0),
    ('Teleport Roulette', 1, on_tpspin, tick_tpspin, None, 4),
    ('Yeet', 0, fx_fling, None, None, 0),
    ('Invincible', 1, None, tick_god, None, 0),
    ('Ghost', 1, on_ghost, None, off_sight, 0),
    ('Lighthouse', 1, on_seen, None, off_sight, 0),
    ('First Person', 1, on_first, None, off_cam, 0),
    ('Way Too Far', 1, on_far, None, off_cam, 0),
    ('Personal Space', 1, on_close, None, off_cam, 0),
    ('Fisheye', 1, on_fish, None, off_fov, 0),
    ('Tunnel Vision', 1, on_tunnel, None, off_fov, 0),
    ('Had a Few', 1, on_drunk, tick_drunk, off_skew, 0),
    ('Predator', 1, on_vanish, None, off_vanish, 0),
    ('Thick Fog', 1, on_fog, None, off_fog, 0),
    ('Night Shift', 1, on_night, None, off_night, 0),
    ('Jojo Moment', 1, on_jojo, None, off_jojo, 0),
    ('Crystal Clear', 1, on_blur, None, off_blur, 0),
    ('Suicide', 0, fx_suicide, None, None, 0),
    ('Flatline', 1, on_flatline, None, off_flatline, 8),
    ('Stop and Stare', 1, on_stare, None, off_stare, 6),
    ('Rooted', 1, on_root, None, off_root, 12),
    ('Safety On', 1, on_safety, None, off_safety, 20),
    ('No Peeking', 1, on_noaim, None, off_noaim, 20),
    ('Forward March', 1, on_march, None, off_march, 8),
    ('Nervous Twitch', 1, None, tick_twitch, None, 12),
    ('Gentle Shove', 1, None, tick_shove, None, 25),
    ('Security Camera', 1, on_cctv, None, off_cam, 12),
    ("Worm's Eye", 1, on_worm, None, off_cam, 20),
    ("Bird's Eye", 1, on_bird, None, off_cam, 20),
    ('Blinded', 1, _blind('full'), None, off_blind, 8),
    ('Anti-Portrait Mode', 1, _blind('portrait'), None, off_blind, 0),
    ('Cinematic', 1, _blind('letterbox'), None, off_blind, 0),
    ('Peephole', 1, _blind('peephole'), None, off_blind, 0),
    ('MY EYES!', 1, on_eyes, tick_eyes, None, 3),
    ("Can't Park There Mate", 0, fx_parking, None, None, 0),
    ('180', 0, fx_180, None, None, 0),
    ('Balkan Parking', 0, fx_balkan, None, None, 0),
    ('Fidget Spinner', 1, on_fidget, tick_fidget, None, 10),
    ('Witness Protection', 1, on_witness, tick_witness, off_witness, 20),

    # Motion, from the velocity and shove API.
    ('Liftoff', 0, fx_launch_cars, None, None, 0),
    ('Car Tornado', 0, fx_car_tornado, None, None, 0),
    ('Get Off Me', 0, fx_shove_all, None, None, 0),
    ('Magnetic', 0, fx_magnet, None, None, 0),
    ('Time Stop', 0, fx_freeze_all, None, None, 0),
    ('Bunny Hop', 0, fx_hop, None, None, 0),
    ('Anti Gravity', 1, on_float, tick_float, off_float, 15),
    ('Bad Suspension', 1, on_jitter, tick_jitter, off_jitter, 15),

    # Rides, from the Domino attach primitive.
    ('Hat Car', 1, on_hat_car, None, off_ride, 25),
    ('Entourage', 1, on_entourage, None, off_entourage, 25),

    # World, from the Domino weather and player work.
    ('Zeus', 0, fx_lightning, None, None, 0),
    ('Thunderstorm', 1, on_thunderstorm, tick_thunderstorm,
     off_thunderstorm, 30),
    ('Bomb Shelter', 1, on_shielded, tick_shielded, off_shielded, 25),
    ('Phase Shift', 1, on_ghostmode, None, off_ghostmode, 15),
    ('Immortal', 1, on_immortal, None, off_immortal, 20),
    ('Last Stand', 1, on_cannot_die, None, off_cannot_die, 25),
    ('Paper Cut', 0, fx_paper_cut, None, None, 0),
    ('Slow Front', 0, fx_weather_creep, None, None, 0),
    ('Hopscotch', 0, fx_hopscotch, None, None, 0),

    # Combat events, from the hit and shot hooks.
    ('Rocket Jump', 1, _flag('recoil', True), None,
     _flag('recoil', False), 20),
    ('Vampire', 1, _flag('vampire', True), None,
     _flag('vampire', False), 25),
    ('Hammer Blow', 1, _flag('impact', True), None,
     _flag('impact', False), 20),
]


# ---- the wheel ----

_state = {
    'on': False,
    'active': [],        # [idx, expires_at]
    'next_roll': 0.0,
    'spin_until': 0.0,
    'spin_next': 0.0,
    'spin_step': 0.0,
    'spin_idx': 0,
    'call': '',
    'call_until': 0.0,
    'history': [],
    'every': EVERY,
    'hold': HOLD,
    'fire': None,
}

_hud = 0
_menu = None


def _log_roll(name, timed):
    """On disk as well as on screen: a crash takes the HUD."""
    try:
        path = os.path.join(os.path.dirname(sh.plugin_dir()), 'chaos.log')
        with open(path, 'a') as f:
            f.write('%s  %-22s %s\n'
                    % (time.strftime('%H:%M:%S'), name,
                       'timed' if timed else 'instant'))
    except OSError:
        pass


def _is_active(idx):
    return any(a[0] == idx for a in _state['active'])


def _fire(idx):
    name, timed, start, _t, _s, secs = EFFECTS[idx]
    if _is_active(idx):
        return
    if timed and len(_state['active']) >= MAX_ACTIVE:
        return
    if start:
        try:
            start()
        except Exception:
            sh.log('effect %s FAILED\n%s' % (name, traceback.format_exc()))
            return
    now = time.time()
    if timed:
        hold = float(secs) if secs > 0 else _state['hold']
        _state['active'].append([idx, now + hold])
        _state['call'] = '>>  %s  <<   %.0fs' % (name, hold)
    else:
        _state['call'] = '>>  %s  <<' % name
    _state['call_until'] = now + FLASH_SECONDS
    _state['history'].insert(0, ('* ' if timed else '  ') + name)
    del _state['history'][HIST_MAX:]
    _log_roll(name, timed)


def _run_stop(idx):
    stop = EFFECTS[idx][4]
    if stop:
        try:
            stop()
        except Exception:
            sh.log('stop %s FAILED\n%s'
                   % (EFFECTS[idx][0], traceback.format_exc()))


def _stop_all():
    global _in_mask
    for idx, _ in _state['active']:
        _run_stop(idx)
    _state['active'] = []
    # Never leave input blocked, the screen black, an event
    # armed or a spawned thing attached to the player.
    _in_mask = 0
    sh.BlockInput(0)
    _black.clear()
    for k in _evt:
        _evt[k] = False


def _draw():
    now = time.time()
    lines = []
    if _state['spin_until'] > now:
        lines.append('>>  %s  <<' % EFFECTS[_state['spin_idx']][0])
    elif _state['call_until'] > now:
        lines.append(_state['call'])
    for idx, until in _state['active']:
        lines.append('%-22s %2.0fs'
                     % (EFFECTS[idx][0], max(0.0, until - now)))
    if len(lines) == 0:
        lines.append('next in %2.0fs'
                     % max(0.0, _state['next_roll'] - now))
    lines.extend(_state['history'][:3])
    if _hud:
        sh.HudSet(_hud, '\n'.join(lines))


# ---- plugin hooks ----

def start():
    global _hud, _menu

    _hud = sh.HudCreate('chaos', sh.HUD_TOPRIGHT, 0)
    if _hud:
        sh.HudColour(_hud, 0xFFCC33)

    sh.on_fire(_on_shot)
    sh.on_hit(_on_hit)

    _menu = sh.menu('Chaos')
    if _menu:
        _menu.toggle('Enabled', False, _menu_enable)
        _menu.number('Every (s)', EVERY, 5.0, 300.0, 5.0, _menu_every)
        _menu.number('Hold (s)', HOLD, 5.0, 120.0, 5.0, _menu_hold)
        _menu.action('Roll now', _menu_now)
        _menu.action('Clear active', _menu_clear)
        picks = _menu.sub('Fire one')
        if picks:
            for i, eff in enumerate(EFFECTS):
                picks.action(eff[0], _picker(i))
    sh.log('chaos.py ready, %d effects' % len(EFFECTS))


def _menu_enable(v):
    _state['on'] = bool(v)
    if _state['on']:
        _state['next_roll'] = time.time() + _state['every']
    else:
        _stop_all()


def _menu_every(v):
    _state['every'] = float(v)


def _menu_hold(v):
    _state['hold'] = float(v)


def _menu_now(_v):
    _state['next_roll'] = 0.0
    _state['on'] = True


def _menu_clear(_v):
    _stop_all()


def _picker(idx):
    def go(_v):
        _state['fire'] = idx
    return go


def on_frame():
    now = time.time()

    if _state['fire'] is not None:
        idx = _state['fire']
        _state['fire'] = None
        _fire(idx)

    # Timed effects that ran out.
    for entry in list(_state['active']):
        if now >= entry[1]:
            _run_stop(entry[0])
            _state['active'].remove(entry)

    # Whatever is still running gets its tick.
    for idx, _ in _state['active']:
        tick = EFFECTS[idx][3]
        if tick:
            try:
                tick()
            except Exception:
                sh.log('tick %s FAILED\n%s'
                       % (EFFECTS[idx][0], traceback.format_exc()))

    if _state['on'] and sh.IsInGame():
        if _state['spin_until'] > now:
            if now >= _state['spin_next']:
                _state['spin_idx'] = random.randrange(len(EFFECTS))
                _state['spin_step'] += _state['spin_step'] / 4.0 + 0.02
                _state['spin_next'] = now + _state['spin_step']
        elif _state['spin_step'] > 0.0:
            _state['spin_step'] = 0.0
            _fire(_state['spin_idx'])
            _state['next_roll'] = now + _state['every']
        elif now >= _state['next_roll']:
            _state['spin_until'] = now + SPIN_SECONDS
            _state['spin_step'] = 0.03
            _state['spin_next'] = now
            _state['spin_idx'] = random.randrange(len(EFFECTS))

    _draw()


def stop():
    _stop_all()
    sh.clear_events()
    if _menu:
        _menu.destroy()
    if _hud:
        sh.HudSet(_hud, '')


def on_world_reload():
    """Spawned NPCs and cars are gone; drop the handles."""
    _wp['ents'] = []
    _fid['ents'] = []
