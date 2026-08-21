/* Chaos mod. A random effect fires on a timer, in the
 * spirit of the GTA V one. Instant effects happen once,
 * timed ones hold for a while and then let go. */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "scripthook.h"

#define TICK_MS      33
#define SPIN_MS      2200
#define SPIN_FIRST   40
#define BAR_CELLS    24
#define MAX_ACTIVE   8
#define FLASH_MS     4000

/* Teleports stay inside the streamed region: collision
 * exists within about 1500m and placing outside it is a
 * documented crash. */
#define TP_RANGE     350.0f

#define HIST_MAX 6

static uint32_t g_menu, g_hudStack;
static volatile int   g_on = 0;
static volatile float g_every = 30.0f;
static volatile float g_hold = 60.0f;
static volatile int   g_fire = 0;

static uint64_t g_next;
static uint32_t g_seed = 1;

/* xorshift, seeded from the clock. rand() is per thread
 * and the CRT one is coarse.
 */
static uint32_t Rnd(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

static int RndBelow(int n) {
    return n > 0 ? (int)(Rnd() % (uint32_t)n) : 0;
}

static float RndSpan(float lo, float hi) {
    return lo + (hi - lo) * ((float)(Rnd() & 0xFFFF) / 65535.0f);
}

/* ---- input meddling ---- */

/* Low level hooks, so the block lands before the game
 * reads anything. Alt, Tab and F4 always pass, so the
 * player can always leave or open the menu. */
static volatile int g_noAll = 0;
static volatile int g_noMove = 0;
static volatile int g_noFire = 0;
static volatile int g_noAim = 0;

static HHOOK g_kbHook, g_msHook;

static int Escapes(DWORD vk) {
    return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_TAB || vk == VK_F4 || vk == VK_LWIN ||
           vk == VK_RWIN || vk == VK_ESCAPE;
}

static LRESULT CALLBACK KbProc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)l;

        if (!Escapes(k->vkCode)) {
            if (g_noAll) return 1;
            if (g_noMove && (k->vkCode == 'W' || k->vkCode == 'A' ||
                             k->vkCode == 'S' || k->vkCode == 'D' ||
                             k->vkCode == VK_SPACE))
                return 1;
        }
    }
    return CallNextHookEx(g_kbHook, code, w, l);
}

static LRESULT CALLBACK MsProc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        if (g_noAll) return 1;
        if (g_noFire && (w == WM_LBUTTONDOWN || w == WM_LBUTTONUP))
            return 1;
        if (g_noAim && (w == WM_RBUTTONDOWN || w == WM_RBUTTONUP))
            return 1;
    }
    return CallNextHookEx(g_msHook, code, w, l);
}

/* ---- the blackout ---- */

/* A black popup sized to the game's client area. Owned by
 * the hook thread because that is the one with a pump.
 */
static HWND g_black;
static volatile int g_wantBlack = 0;

/* Shape of the blackout, applied as a window region. */
enum { BLK_FULL, BLK_PORTRAIT, BLK_LETTERBOX, BLK_PEEPHOLE };
static volatile int g_blackMode = BLK_FULL;

static void ShapeBlack(int w, int h) {
    static int lastMode = -1, lastW, lastH;
    HRGN rgn = NULL, cut;
    int mode = g_blackMode;

    if (mode == lastMode && w == lastW && h == lastH) return;
    lastMode = mode; lastW = w; lastH = h;
    if (mode == BLK_PORTRAIT) {
        int bw = h * 9 / 16;
        rgn = CreateRectRgn((w - bw) / 2, 0, (w + bw) / 2, h);
    } else if (mode == BLK_LETTERBOX) {
        int band = w * 9 / 21;
        rgn = CreateRectRgn(0, 0, w, h);
        cut = CreateRectRgn(0, (h - band) / 2, w, (h + band) / 2);
        CombineRgn(rgn, rgn, cut, RGN_DIFF);
        DeleteObject(cut);
    } else if (mode == BLK_PEEPHOLE) {
        int r = h / 4;
        rgn = CreateRectRgn(0, 0, w, h);
        cut = CreateEllipticRgn(w / 2 - r, h / 2 - r,
                                w / 2 + r, h / 2 + r);
        CombineRgn(rgn, rgn, cut, RGN_DIFF);
        DeleteObject(cut);
    }
    /* The window owns rgn after this call. */
    SetWindowRgn(g_black, rgn, TRUE);
}

static BOOL CALLBACK PickWindow(HWND h, LPARAM l) {
    DWORD pid = 0;
    RECT rc;

    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    if (!GetClientRect(h, &rc)) return TRUE;
    if (rc.right < 320 || rc.bottom < 240) return TRUE;
    *(HWND *)l = h;
    return FALSE;
}

static void ShowBlack(int on);

static LRESULT CALLBACK BlackProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER) {
        static int shown = 0;
        int want = g_wantBlack;

        if (want != shown) { shown = want; ShowBlack(want); }
        else if (want) ShowBlack(1);
        return 0;
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;

        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void ShowBlack(int on) {
    HWND game = NULL;
    RECT rc;
    POINT tl;

    if (!g_black) return;
    if (!on) { ShowWindow(g_black, SW_HIDE); return; }

    EnumWindows(PickWindow, (LPARAM)&game);
    if (!game) return;
    if (!GetClientRect(game, &rc)) return;
    tl.x = rc.left;
    tl.y = rc.top;
    ClientToScreen(game, &tl);
    SetWindowPos(g_black, HWND_TOPMOST, tl.x, tl.y,
                 rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShapeBlack(rc.right - rc.left, rc.bottom - rc.top);
}

static DWORD WINAPI HookThread(LPVOID p) {
    MSG msg;
    WNDCLASSA wc;
    (void)p;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = BlackProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "GRWChaosBlackout";
    RegisterClassA(&wc);
    g_black = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        "GRWChaosBlackout", "", WS_POPUP, 0, 0, 16, 16,
        NULL, NULL, wc.hInstance, NULL);
    if (g_black) SetTimer(g_black, 1, 100, NULL);

    g_kbHook = SetWindowsHookExA(WH_KEYBOARD_LL, KbProc, NULL, 0);
    g_msHook = SetWindowsHookExA(WH_MOUSE_LL, MsProc, NULL, 0);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

/* Injection, for the effects that move you rather than
 * stop you. Tagged so our own hooks ignore it.
 */
static void TapKey(WORD vk, int down) {
    INPUT in;

    memset(&in, 0, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    in.ki.dwExtraInfo = 0xC4A05;
    SendInput(1, &in, sizeof(in));
}

static void NudgeMouse(int dx, int dy) {
    INPUT in;

    memset(&in, 0, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwExtraInfo = 0xC4A05;
    SendInput(1, &in, sizeof(in));
}

/* ---- helpers the effects share ---- */

static int PlayerPos(ShVec3 *out) {
    return ShGetPlayerPosition(out);
}

static void SetAmmoAll(uint32_t n) {
    int i;
    for (i = 0; i < 3; i++) ShSetAmmo(i, n);
}

static int NearbyNpcs(ShEntity *out, int max, float radius) {
    return ShFindEntities(SH_KIND_NPC, radius, 0, out, max);
}

static void DropVehicles(int count, float height) {
    ShVec3 p;
    int i;

    if (!PlayerPos(&p)) return;
    for (i = 0; i < count; i++) {
        const ShVehicle *v = ShVehicleAt(RndBelow(ShVehicleCount()));
        ShVec3 at = p;

        if (!v) continue;
        at.x += RndSpan(-8.0f, 8.0f);
        at.y += RndSpan(-8.0f, 8.0f);
        at.z += height + RndSpan(0.0f, 10.0f);
        ShSpawnVehicle(v->id, &at);
    }
}

/* ---- instant effects ---- */

static void FxSkydive(void) {
    ShVec3 p;
    if (!PlayerPos(&p)) return;
    p.z += 300.0f;
    ShTeleportPlayer(&p, NULL);
}

static void FxSlam(void) {
    ShVec3 p;
    if (!PlayerPos(&p)) return;
    ShTeleportPlayerToGround(p.x, p.y, 0.5f);
}

static void FxCarRain(void)  { DropVehicles(12, 60.0f); }
static void FxOneCar(void)   { DropVehicles(1, 25.0f); }

static void FxHeal(void) {
    uint32_t cur, max;
    if (ShGetHealthPlayer(&cur, &max)) ShSetHealthPlayer(max);
}

static void FxOneHp(void) { ShSetHealthPlayer(1); }

static void FxSmite(void) {
    ShEntity ents[64];
    int n = NearbyNpcs(ents, 64, 120.0f), i;

    for (i = 0; i < n; i++) ShSetHealthEntity(ents[i].entity, 0);
}

static void FxRich(void)  { ShSetAllResources(9999); ShSetSkillPoints(50); }
static void FxBroke(void) { ShSetAllResources(0); }
static void FxDry(void)   { SetAmmoAll(0); }
static void FxLoaded(void) { SetAmmoAll(999); }

static void FxMidnight(void) { ShSetTime(0.5f); }
static void FxNoon(void)     { ShSetTime(12.0f); }
static void FxDawn(void)     { ShSetTime(6.0f); }
/* The in game roulettes spin too: cycle fast, slow down,
 * settle on whatever it lands on.
 */
static uint64_t g_wxNext, g_wxStep;

static void OnDice(void) {
    g_wxStep = 200;
    g_wxNext = 0;
}

static void TickDice(void) {
    uint64_t now = GetTickCount64();

    if (now < g_wxNext) return;
    ShSetWeather(RndBelow(6));
    g_wxStep += g_wxStep / 3 + 40;
    g_wxNext = now + g_wxStep;
}

static uint64_t g_tpNext, g_tpStep;

static void OnTpSpin(void) {
    g_tpStep = 260;
    g_tpNext = 0;
}
static void FxStorm(void)    { ShSetWeather(SH_WEATHER_RAIN_HEAVY); }
static void FxClear(void)    { ShSetWeather(SH_WEATHER_SUNNY); }

static void TickTpSpin(void) {
    uint64_t now = GetTickCount64();
    ShVec3 p;

    if (now < g_tpNext) return;
    if (PlayerPos(&p))
        ShTeleportPlayerToGround(p.x + RndSpan(-TP_RANGE, TP_RANGE),
                                 p.y + RndSpan(-TP_RANGE, TP_RANGE),
                                 1.0f);
    g_tpStep += g_tpStep / 3 + 60;
    g_tpNext = now + g_tpStep;
}

static void FxFling(void) {
    ShVec3 p;
    if (!PlayerPos(&p)) return;
    p.z += 60.0f;
    ShTeleportPlayer(&p, NULL);
}

/* ---- timed effects ---- */

static void TickGod(void) {
    uint32_t cur, max;

    if (ShGetHealthPlayer(&cur, &max) && cur != max)
        ShSetHealthPlayer(max);
}

static void OnGhost(void)  { ShSetVisibility(0.0f); }
static void OnSeen(void)   { ShSetVisibility(6.0f); }
static void OffSight(void) { ShSetVisibility(1.0f); }

static void OnFirst(void)  { ShCameraFirstPerson(0.15f, 0.0f); }
static void OnFar(void)    { ShCameraOrbit(18.0f, 9.0f); }
static void OnClose(void)  { ShCameraOrbit(0.8f, 0.2f); }
static void OffCam(void)   { ShCameraReleaseFields(SH_CAM_POS); }

static void ApplyFov(float f) {
    ShCameraOverride o;

    memset(&o, 0, sizeof(o));
    o.apply = SH_CAM_FOV;
    o.fov = f;
    ShCameraApply(&o);
}

static void OnFish(void)   { ApplyFov(2.0f); }
static void OnTunnel(void) { ApplyFov(0.35f); }
static void OffFov(void)   { ShCameraReleaseFields(SH_CAM_FOV); }

static void SkewTo(float x, float y) {
    ShCameraOverride o;

    memset(&o, 0, sizeof(o));
    o.apply = SH_CAM_SKEW;
    o.skewX = x;
    o.skewY = y;
    ShCameraApply(&o);
}

static void OnDrunk(void)  { SkewTo(0.0f, 0.0f); }
static void TickDrunk(void) {
    float t = (float)(GetTickCount64() % 6000) * 0.001047f;
    SkewTo(0.25f * (float)sin(t), 0.15f * (float)sin(t * 1.7f));
}
static void OffSkew(void)  { ShCameraReleaseFields(SH_CAM_SKEW); }

static uint64_t PlayerRoot(void) {
    ShPlayer p;

    memset(&p, 0, sizeof(p));
    if (!ShGetPlayer(&p)) return 0;
    return p.root ? p.root : p.entity;
}

static void OnVanish(void) {
    uint64_t r = PlayerRoot();
    if (r) ShSetVisible(r, 0, 0, 1);
}

static void OffVanish(void) {
    uint64_t r = PlayerRoot();
    if (r) ShSetVisible(r, 0, 1, 0);
}

static void OnFog(void)  { ShSetWeather(SH_WEATHER_FOG); }
static void OffFog(void) { ShReleaseWeather(); }

static void OnNight(void) { ShSetTime(1.0f); }
static void OffNight(void) { ShSetTime(11.0f); }

/* crucified.wav beside GRW.exe, cwd is not trusted. */
static const char *JojoTrack(void) {
    static char path[MAX_PATH];
    char *slash;

    if (!path[0]) {
        GetModuleFileNameA(NULL, path, MAX_PATH);
        slash = strrchr(path, '\\');
        if (slash) slash[1] = 0;
        strncat(path, "crucified.wav",
                MAX_PATH - strlen(path) - 1);
    }
    return path;
}

/* 4s Crucified jingle, then the sun races. */
static void OnJojo(void) {
    ShSetTimeSpeed(100.0f);
    PlaySoundA(JojoTrack(), NULL,
               SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

static void OffJojo(void) {
    ShSetTimeSpeed(1.0f);
    PlaySoundA(NULL, NULL, 0);
}

static void OnBlur(void)  { ShSetCameraBlur(1); }
static void OffBlur(void) { ShSetCameraBlur(0); }

/* ---- effects that fight the player ---- */

/* Bits held here rather than set outright, so two effects
 * running at once cannot cancel each other's block.
 */
static volatile uint32_t g_inMask = 0;

static void InHold(uint32_t bits, int on) {
    if (on) g_inMask |= bits;
    else    g_inMask &= ~bits;
    ShBlockInput(g_inMask);
}

static void OnStare(void)   { InHold(SH_INPUT_KEYS | SH_INPUT_LOOK, 1); }
static void OffStare(void)  { InHold(SH_INPUT_KEYS | SH_INPUT_LOOK, 0); }
static void OnRoot(void)    { InHold(SH_INPUT_MOVE, 1); }
static void OffRoot(void)   { InHold(SH_INPUT_MOVE, 0); }
static void OnSafety(void)  { InHold(SH_INPUT_FIRE, 1); }
static void OffSafety(void) { InHold(SH_INPUT_FIRE, 0); }
static void OnNoAim(void)   { InHold(SH_INPUT_AIM, 1); }
static void OffNoAim(void)  { InHold(SH_INPUT_AIM, 0); }

static void OnMarch(void)  { TapKey('W', 1); }
static void OffMarch(void) { TapKey('W', 0); }

static void TickTwitch(void) {
    NudgeMouse(RndBelow(41) - 20, RndBelow(21) - 10);
}

static void TickShove(void) {
    NudgeMouse(RndBelow(9) - 4, 0);
}

/* The camera stays where it was and the player walks out
 * of frame, which is funnier than it sounds.
 */
static void OnCctv(void) {
    ShVec3 p;

    if (!ShGetHeadPosition(&p) && !PlayerPos(&p)) return;
    ShSetCamera(&p);
}
static void OffCctv(void) { ShCameraReleaseFields(SH_CAM_POS); }

static void OnWorm(void)  { ShCameraOrbit(2.5f, -1.8f); }
static void OnBird(void)  { ShCameraOrbit(1.0f, 28.0f); }

static void FxSuicide(void) { ShKillPlayer(); }

/* The game over sequence without the reload: the scene
 * object plays the death and holds black until Exit.
 */
static void OnFlatline(void) {
    uint64_t s = ShGameFlowObject(SH_FLOW_GAMEOVER_SCENE);
    if (s) ShSceneEnter(s);
}
static void OffFlatline(void) {
    uint64_t s = ShGameFlowObject(SH_FLOW_GAMEOVER_SCENE);
    if (s) ShSceneExit(s);
}

static void OnBlind(void)  { g_blackMode = BLK_FULL; g_wantBlack = 1; }
static void OffBlind(void) { g_wantBlack = 0; }

/* A 9:16 phone screen worth of nothing, dead centre. */
static void OnPortrait(void) { g_blackMode = BLK_PORTRAIT; g_wantBlack = 1; }
/* 21:9 bars, for the cinema feel. */
static void OnCinema(void)   { g_blackMode = BLK_LETTERBOX; g_wantBlack = 1; }
/* Everything gone but a circle in the middle. */
static void OnPeephole(void) { g_blackMode = BLK_PEEPHOLE; g_wantBlack = 1; }

/* Slamming the clock between noon and midnight makes the
 * eye adaptation chase itself, which is the joke.
 */
static uint64_t g_eyeNext;
static int      g_eyeDay;

static void OnEyes(void) { g_eyeNext = 0; g_eyeDay = 0; }

static void TickEyes(void) {
    uint64_t now = GetTickCount64();

    if (now < g_eyeNext) return;
    g_eyeDay = !g_eyeDay;
    ShSetTime(g_eyeDay ? 12.0f : 0.5f);
    g_eyeNext = now + 260;
}

/* In a car the root re-parents to it, so that is the
 * thing to turn. On foot it is the soldier.
 */
static uint64_t TurnTarget(int *inCar) {
    ShPlayer p;
    uint64_t root = 0;

    if (inCar) *inCar = 0;
    memset(&p, 0, sizeof(p));
    if (!ShGetPlayer(&p)) return 0;
    if (ShIsInVehicle() && ShWalkToRoot(p.entity, &root) &&
        root && root != p.entity) {
        if (inCar) *inCar = 1;
        return root;
    }
    return p.entity;
}

static void Fx180(void) {
    ShVec3 pos;
    float yaw = 0, pitch = 0, roll = 0;
    uint64_t t = TurnTarget(NULL);

    if (!t) return;
    if (!ShGetEntityTransform(t, &pos, &yaw, &pitch, &roll)) return;
    ShPlaceEntityRot(t, &pos, yaw + 3.14159f, pitch, roll);
}

/* Driving gets you parked on the roof, walking gets you
 * spun around. Either way you are facing the wrong way.
 */
static void FxBalkan(void) {
    ShVec3 pos;
    float yaw = 0, pitch = 0, roll = 0;
    int inCar = 0;
    uint64_t t = TurnTarget(&inCar);

    if (!t) return;
    if (!inCar) { Fx180(); return; }
    if (!ShGetEntityTransform(t, &pos, &yaw, &pitch, &roll)) return;
    pos.z += 0.6f;
    ShPlaceEntityRot(t, &pos, yaw, pitch, 3.14159f);
}

/* Every car nearby turns on the spot, eased over the
 * effect's life rather than snapping.
 */
#define FIDGET_MAX  24
#define FIDGET_RATE 3.4f
#define FIDGET_SCAN 400

/* Each caught car keeps its own anchor and its own start,
 * so one that drives in late joins the spin from wherever
 * it happened to be rather than snapping. */
static uint64_t g_fidEnt[FIDGET_MAX];
static float    g_fidYaw[FIDGET_MAX];
static ShVec3   g_fidPos[FIDGET_MAX];
static uint64_t g_fidT0[FIDGET_MAX];
static int      g_fidN;
static uint64_t g_fidScan;

static int FidgetHas(uint64_t e) {
    int i;

    for (i = 0; i < g_fidN; i++)
        if (g_fidEnt[i] == e) return 1;
    return 0;
}

/* Rescanned while it runs, so nothing that wanders into
 * range gets to drive away unspun.
 */
static void FidgetScan(void) {
    ShEntity ents[FIDGET_MAX];
    uint64_t now = GetTickCount64();
    int n, i;

    n = ShFindEntities(SH_KIND_VEHICLE, 50.0f, 0, ents, FIDGET_MAX);
    for (i = 0; i < n && g_fidN < FIDGET_MAX; i++) {
        float y = 0, p = 0, r = 0;
        ShVec3 pos;

        if (FidgetHas(ents[i].entity)) continue;
        if (!ShGetEntityTransform(ents[i].entity, &pos, &y, &p, &r))
            continue;
        g_fidEnt[g_fidN] = ents[i].entity;
        g_fidYaw[g_fidN] = y;
        g_fidPos[g_fidN] = pos;
        g_fidT0[g_fidN] = now;
        g_fidN++;
    }
}

static void OnFidget(void) {
    g_fidN = 0;
    g_fidScan = 0;
}

/* Queued for the frame path. Set transform off a game
 * thread hits a null thread local allocator and dies at
 * GRW.exe+0x16B93C1D. */
static void TickFidget(void) {
    uint64_t now = GetTickCount64();
    int i;

    if (now - g_fidScan >= FIDGET_SCAN) {
        g_fidScan = now;
        FidgetScan();
    }

    for (i = 0; i < g_fidN; i++) {
        float t = (float)(now - g_fidT0[i]) / 1000.0f;

        ShQueueTransform(g_fidEnt[i], &g_fidPos[i],
                         g_fidYaw[i] + t * FIDGET_RATE, 0.0f, 0.0f);
    }
}

/* Six cars in a ring, then rolled onto their roofs. */
static void FxParking(void) {
    ShVec3 p;
    int i;

    if (!PlayerPos(&p)) return;
    for (i = 0; i < 6; i++) {
        const ShVehicle *v = ShVehicleAt(RndBelow(ShVehicleCount()));
        float a = (float)i * 1.0472f;
        ShVec3 at = p;
        uint64_t e;

        if (!v) continue;
        at.x += (float)cos(a) * 7.0f;
        at.y += (float)sin(a) * 7.0f;
        at.z += 1.0f;
        e = ShSpawnVehicle(v->id, &at);
        if (!e) continue;
        at.z += 0.6f;
        ShPlaceEntityRot(e, &at, a + 1.5708f, 0.0f, 3.14159f);
    }
}

/* Witness Protection: twelve NPCs in a ring, spun around
 * the player on the frame path, z pinned to the player's.
 */
#define WP_N     12
#define WP_R     3.5f
#define WP_RATE  1.2f
static uint64_t g_wpEnt[WP_N];
static int      g_wpN = 0;
static uint64_t g_wpT0 = 0;

/* A few kind 1 archetypes never produce an entity, so keep
 * drawing until the ring is full or the list runs out. */
static void OnWitness(void) {
    ShVec3 p;
    int i, n;

    g_wpN = 0;
    if (!PlayerPos(&p)) return;
    n = ShNpcCount();
    for (i = 0; i < n && g_wpN < WP_N; i++) {
        const ShNpcArchetype *a = ShNpcAt(i);
        float ang = (float)g_wpN * (6.2832f / WP_N);
        ShVec3 at = p;
        uint64_t e;

        if (!a || a->kind != 1) continue;
        at.x += (float)cos(ang) * WP_R;
        at.y += (float)sin(ang) * WP_R;
        e = ShSpawnNpc(a->id, &at);
        if (e) g_wpEnt[g_wpN++] = e;
    }
    g_wpT0 = GetTickCount64();
}

static void TickWitness(void) {
    float t = (float)(GetTickCount64() - g_wpT0) / 1000.0f;
    ShVec3 p;
    int i;

    if (!g_wpN || !PlayerPos(&p)) return;
    for (i = 0; i < g_wpN; i++) {
        float a = (float)i * (6.2832f / WP_N) + t * WP_RATE;
        ShVec3 at = p;

        at.x += (float)cos(a) * WP_R;
        at.y += (float)sin(a) * WP_R;
        /* Face the player. Measured: the engine's yaw runs
         * clockwise, so the spoke angle is negated. */
        ShQueueTransform(g_wpEnt[i], &at, -(a + 3.14159f), 0.0f, 0.0f);
    }
}

static void OffWitness(void) { g_wpN = 0; }

/* ---- the table ---- */

/* secs is last so the rows that leave it out get 0, which
 * means hold for the configured time.
 */
typedef struct {
    const char *name;
    int   timed;
    void (*start)(void);
    void (*tick)(void);
    void (*stop)(void);
    int   secs;
} Effect;

static const Effect g_fx[] = {
    { "Skydive",            0, FxSkydive,  NULL, NULL, 0 },
    { "Ground Slam",        0, FxSlam,     NULL, NULL, 0 },
    { "It's Raining Cars",  0, FxCarRain,  NULL, NULL, 0 },
    { "Company Car",        0, FxOneCar,   NULL, NULL, 0 },
    { "Patched Up",         0, FxHeal,     NULL, NULL, 0 },
    { "One Hit Point",      0, FxOneHp,    NULL, NULL, 0 },
    { "Smite the Cartel",   0, FxSmite,    NULL, NULL, 0 },
    { "Supply Drop",        0, FxRich,     NULL, NULL, 0 },
    { "Bankrupt",           0, FxBroke,    NULL, NULL, 0 },
    { "Click Click",        0, FxDry,      NULL, NULL, 0 },
    { "Full Mags",          0, FxLoaded,   NULL, NULL, 0 },
    { "Midnight",           0, FxMidnight, NULL, NULL, 0 },
    { "High Noon",          0, FxNoon,     NULL, NULL, 0 },
    { "Sunrise",            0, FxDawn,     NULL, NULL, 0 },
    { "Weather Roulette",   1, OnDice,   TickDice,  NULL, 4 },
    { "Downpour",           0, FxStorm,    NULL, NULL, 0 },
    { "Blue Skies",         0, FxClear,    NULL, NULL, 0 },
    { "Teleport Roulette",  1, OnTpSpin, TickTpSpin, NULL, 4 },
    { "Yeet",               0, FxFling,    NULL, NULL, 0 },

    { "Invincible",         1, NULL,     TickGod,   NULL, 0 },
    { "Ghost",              1, OnGhost,  NULL,      OffSight, 0 },
    { "Lighthouse",         1, OnSeen,   NULL,      OffSight, 0 },
    { "First Person",       1, OnFirst,  NULL,      OffCam, 0 },
    { "Way Too Far",        1, OnFar,    NULL,      OffCam, 0 },
    { "Personal Space",     1, OnClose,  NULL,      OffCam, 0 },
    { "Fisheye",            1, OnFish,   NULL,      OffFov, 0 },
    { "Tunnel Vision",      1, OnTunnel, NULL,      OffFov, 0 },
    { "Had a Few",          1, OnDrunk,  TickDrunk, OffSkew, 0 },
    { "Predator",           1, OnVanish, NULL,      OffVanish, 0 },
    { "Thick Fog",          1, OnFog,    NULL,      OffFog, 0 },
    { "Night Shift",        1, OnNight,  NULL,      OffNight, 0 },
    { "Jojo Moment",        1, OnJojo,   NULL,      OffJojo, 0 },
    { "Soft Focus",         1, OnBlur,   NULL,      OffBlur, 0 },

    { "Suicide",            0, FxSuicide, NULL, NULL, 0 },
    { "Flatline",           1, OnFlatline, NULL, OffFlatline, 8 },
    { "Stop and Stare",     1, OnStare,  NULL,      OffStare, 6 },
    { "Rooted",             1, OnRoot,   NULL,      OffRoot, 12 },
    { "Safety On",          1, OnSafety, NULL,      OffSafety, 20 },
    { "No Peeking",         1, OnNoAim,  NULL,      OffNoAim, 20 },
    { "Forward March",      1, OnMarch,  NULL,      OffMarch, 8 },
    { "Nervous Twitch",     1, NULL,     TickTwitch, NULL, 12 },
    { "Gentle Shove",       1, NULL,     TickShove, NULL, 25 },
    { "Security Camera",    1, OnCctv,   NULL,      OffCctv, 12 },
    { "Worm's Eye",         1, OnWorm,   NULL,      OffCam, 20 },
    { "Bird's Eye",         1, OnBird,   NULL,      OffCam, 20 },
    { "Blinded",            1, OnBlind,  NULL,      OffBlind, 8 },
    { "Anti-Portrait Mode", 1, OnPortrait, NULL,    OffBlind, 0 },
    { "Cinematic",          1, OnCinema, NULL,      OffBlind, 0 },
    { "Peephole",           1, OnPeephole, NULL,    OffBlind, 0 },
    { "MY EYES!",           1, OnEyes,   TickEyes,  NULL, 3 },
    { "Can't Park There Mate", 0, FxParking, NULL, NULL, 0 },
    { "180",                0, Fx180,    NULL, NULL, 0 },
    { "Balkan Parking",     0, FxBalkan, NULL, NULL, 0 },
    { "Fidget Spinner",     1, OnFidget, TickFidget, NULL, 10 },
    { "Witness Protection", 1, OnWitness, TickWitness, OffWitness, 20 }
};

#define FX_COUNT ((int)(sizeof(g_fx) / sizeof(g_fx[0])))

/* ---- running state ---- */

typedef struct {
    int      idx;
    uint64_t until;
} Active;

static Active  g_act[MAX_ACTIVE];
static int     g_nact = 0;

/* The roll is shown spinning before it lands: names cycle
 * fast and then slow to a stop, so the result feels drawn
 * rather than announced. */
static int      g_spin = 0;
static int      g_spinIdx = 0;
static uint64_t g_spinEnd, g_spinNext;
static uint64_t g_spinStep;

/* What just landed, and what landed before that. The roll
 * is the point of the mod, so it gets announced loudly and
 * then keeps a short history. */
static char     g_call[80];
static uint64_t g_callUntil;
static char     g_hist[HIST_MAX][48];
static int      g_nhist = 0;

static void PushHistory(const char *name, int timed) {
    int i;

    for (i = HIST_MAX - 1; i > 0; i--)
        memcpy(g_hist[i], g_hist[i - 1], sizeof(g_hist[0]));
    snprintf(g_hist[0], sizeof(g_hist[0]), "%s%s",
             timed ? "* " : "  ", name);
    if (g_nhist < HIST_MAX) g_nhist++;
}

/* On disk as well as on screen: a crash takes the HUD with
 * it, and the last few lines here are what say which
 * effect was live when the game went down. */
static void LogRoll(const char *name, int timed) {
    SYSTEMTIME t;
    FILE *f = fopen("chaos.log", "a");

    if (!f) return;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d  %-22s %s\n",
            t.wHour, t.wMinute, t.wSecond, name,
            timed ? "timed" : "instant");
    fclose(f);
}

static int IsActive(int idx) {
    int i;
    for (i = 0; i < g_nact; i++)
        if (g_act[i].idx == idx) return 1;
    return 0;
}

static void StopAll(void) {
    int i;

    for (i = 0; i < g_nact; i++)
        if (g_fx[g_act[i].idx].stop) g_fx[g_act[i].idx].stop();
    g_nact = 0;

    /* Never leave input blocked or the screen black,
     * whatever went wrong.
     */
    g_inMask = 0;
    ShBlockInput(0);
    g_wantBlack = 0;
}

static void FireIdx(int idx) {
    uint64_t now = GetTickCount64();

    if (idx < 0 || idx >= FX_COUNT) return;
    if (IsActive(idx)) return;
    if (g_fx[idx].timed && g_nact >= MAX_ACTIVE) return;

    if (g_fx[idx].start) g_fx[idx].start();

    if (g_fx[idx].timed) {
        float hold = g_fx[idx].secs > 0
                   ? (float)g_fx[idx].secs : g_hold;

        g_act[g_nact].idx = idx;
        g_act[g_nact].until = now + (uint64_t)(hold * 1000.0f);
        g_nact++;
        snprintf(g_call, sizeof(g_call), ">>  %s  <<   %.0fs",
                 g_fx[idx].name, hold);
    } else {
        snprintf(g_call, sizeof(g_call), ">>  %s  <<",
                 g_fx[idx].name);
    }
    g_callUntil = now + FLASH_MS;
    PushHistory(g_fx[idx].name, g_fx[idx].timed);
    LogRoll(g_fx[idx].name, g_fx[idx].timed);
}

/* Fired by name from outside, for testing one effect
 * without waiting for the wheel. The chaos thread picks
 * it up, so nothing races the active list. */
static volatile int g_fireIdx = -1;

__declspec(dllexport) int ChaosCount(void) { return FX_COUNT; }

__declspec(dllexport) const char *ChaosName(int i) {
    return (i >= 0 && i < FX_COUNT) ? g_fx[i].name : NULL;
}

__declspec(dllexport) int ChaosFire(const char *name) {
    int i;

    if (!name || !*name) return 0;
    for (i = 0; i < FX_COUNT; i++)
        if (_stricmp(g_fx[i].name, name) == 0) {
            g_fireIdx = i;
            return 1;
        }
    for (i = 0; i < FX_COUNT; i++)
        if (strstr(g_fx[i].name, name)) {
            g_fireIdx = i;
            return 1;
        }
    return 0;
}

static void Trigger(void) {
    uint64_t now = GetTickCount64();
    int idx, tries;

    /* Never stack an effect on itself, and never start a
     * timed one with no slot left to hold it.
     */
    for (tries = 0; tries < 24; tries++) {
        idx = RndBelow(FX_COUNT);
        if (IsActive(idx)) continue;
        if (g_fx[idx].timed && g_nact >= MAX_ACTIVE) continue;
        break;
    }
    if (tries >= 24) return;

    if (g_fx[idx].start) g_fx[idx].start();

    if (g_fx[idx].timed) {
        float hold = g_fx[idx].secs > 0
                   ? (float)g_fx[idx].secs : g_hold;

        g_act[g_nact].idx = idx;
        g_act[g_nact].until = now + (uint64_t)(hold * 1000.0f);
        g_nact++;
        snprintf(g_call, sizeof(g_call), ">>  %s  <<   %.0fs",
                 g_fx[idx].name, g_hold);
    } else {
        snprintf(g_call, sizeof(g_call), ">>  %s  <<",
                 g_fx[idx].name);
    }
    g_callUntil = now + FLASH_MS;
    PushHistory(g_fx[idx].name, g_fx[idx].timed);
    LogRoll(g_fx[idx].name, g_fx[idx].timed);
}

static void Expire(uint64_t now) {
    int i = 0;

    while (i < g_nact) {
        if (now >= g_act[i].until) {
            if (g_fx[g_act[i].idx].stop) g_fx[g_act[i].idx].stop();
            g_act[i] = g_act[g_nact - 1];
            g_nact--;
        } else {
            if (g_fx[g_act[i].idx].tick) g_fx[g_act[i].idx].tick();
            i++;
        }
    }
}

static int NowActive(const char *name) {
    int i;

    for (i = 0; i < g_nact; i++)
        if (strcmp(g_fx[g_act[i].idx].name, name) == 0) return 1;
    return 0;
}

/* One stack on the right: the timer, then what is holding
 * with its countdown, then what rolled before that.
 */
static void DrawHud(uint64_t now) {
    char out[1024];
    int filled, i, n = 0;
    float left;

    left = (g_next > now) ? (float)(g_next - now) / 1000.0f : 0.0f;
    filled = (int)((1.0f - left / g_every) * BAR_CELLS);
    if (filled < 0) filled = 0;
    if (filled > BAR_CELLS) filled = BAR_CELLS;

    n += snprintf(out + n, sizeof(out) - n, "CHAOS  ");
    for (i = 0; i < BAR_CELLS; i++)
        out[n++] = (g_spin || i < filled) ? '#' : '.';
    if (g_spin)
        n += snprintf(out + n, sizeof(out) - n,
                      "\n\n   [ %s ]\n\n", g_fx[g_spinIdx].name);
    else
        n += snprintf(out + n, sizeof(out) - n, "  %2.0fs\n\n", left);

    for (i = 0; i < g_nact && n < (int)sizeof(out) - 64; i++) {
        uint64_t ms = g_act[i].until > now ? g_act[i].until - now : 0;
        n += snprintf(out + n, sizeof(out) - n, "> %-22s %2llus\n",
                      g_fx[g_act[i].idx].name,
                      (unsigned long long)(ms / 1000));
    }

    for (i = 0; i < g_nhist && n < (int)sizeof(out) - 64; i++) {
        const char *nm = g_hist[i] + 2;
        if (NowActive(nm)) continue;
        n += snprintf(out + n, sizeof(out) - n, "  %s\n", nm);
    }
    out[n] = 0;
    ShHudSet(g_hudStack, out);
}

static DWORD WINAPI ChaosThread(LPVOID p) {
    (void)p;

    g_seed = (uint32_t)GetTickCount64() | 1u;
    g_next = GetTickCount64() + (uint64_t)(g_every * 1000.0f);

    for (;;) {
        uint64_t now;

        Sleep(TICK_MS);
        now = GetTickCount64();

        if (!ShIsInGame()) {
            g_next = now + (uint64_t)(g_every * 1000.0f);
            continue;
        }

        /* Runs even with chaos switched off, so a single
         * effect can be tried on its own.
         */
        if (g_fireIdx >= 0) {
            int k = g_fireIdx;

            g_fireIdx = -1;
            FireIdx(k);
        }
        Expire(now);

        if (!g_on) {
            g_next = now + (uint64_t)(g_every * 1000.0f);
            DrawHud(now);
            continue;
        }
        /* The world is frozen behind the pause menu, so
         * the timer waits rather than burning effects.
         */
        if (ShGetGameState() == SH_STATE_PAUSED) {
            g_next = now + (uint64_t)(g_every * 1000.0f);
            continue;
        }

        if (g_fire) { g_fire = 0; g_next = now; }

        if (g_spin) {
            /* Each step is longer than the last, so it
             * winds down instead of stopping dead.
             */
            if (now >= g_spinNext) {
                g_spinIdx = RndBelow(FX_COUNT);
                g_spinStep += g_spinStep / 3 + 8;
                g_spinNext = now + g_spinStep;
            }
            if (now >= g_spinEnd) {
                g_spin = 0;
                Trigger();
                g_next = now + (uint64_t)(g_every * 1000.0f);
            }
        } else if (now >= g_next) {
            g_spin = 1;
            g_spinIdx = RndBelow(FX_COUNT);
            g_spinStep = SPIN_FIRST;
            g_spinNext = now + g_spinStep;
            g_spinEnd = now + SPIN_MS;
        }
        DrawHud(now);
    }
    return 0;
}

/* ---- menu ---- */

static void OnEnable(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    g_on = v;
    if (!v) {
        StopAll();
        ShHudSet(g_hudStack, "");
    }
    ShMenuStatus(g_menu, v ? "running" : "off");
}

static void OnEvery(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    g_every = (float)v;
}

static void OnHold(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    g_hold = (float)v;
}

static void OnNow(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)v; (void)u;
    g_fire = 1;
}

static void OnClearAll(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)v; (void)u;
    StopAll();
}

static DWORD WINAPI InitThread(LPVOID p) {
    char line[64];
    (void)p;

    while (!ShGetVersion()) Sleep(500);

    g_menu = ShMenuCreate("Chaos");
    ShMenuToggle(g_menu, "Enabled", 0, OnEnable, NULL);
    ShMenuNumber(g_menu, "Seconds between", 30, 5, 180, 5,
                 OnEvery, NULL);
    ShMenuNumber(g_menu, "Effect seconds", 60, 15, 180, 15,
                 OnHold, NULL);
    ShMenuAction(g_menu, "Roll one now", OnNow, NULL);
    ShMenuAction(g_menu, "Clear active", OnClearAll, NULL);

    snprintf(line, sizeof(line), "off, %d effects", FX_COUNT);
    ShMenuStatus(g_menu, line);

    g_hudStack = ShHudCreate("chaos", SH_HUD_TOPRIGHT, 0);
    ShHudColour(g_hudStack, 0xFFCC33);

    CreateThread(NULL, 0, HookThread, NULL, 0, NULL);
    CreateThread(NULL, 0, ChaosThread, NULL, 0, NULL);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
    }
    return TRUE;
}
