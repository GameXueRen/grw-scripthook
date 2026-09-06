/* The game reads its keyboard through DirectInput, which */
/* arrives through this proxy. Blocked keys vanish here. */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "log.h"

extern int ShKeySuppressedVk(int vk);

typedef HRESULT (WINAPI *CreateDevice_t)(void *, const GUID *, void **,
                                         IUnknown *);
typedef HRESULT (WINAPI *GetState_t)(void *, DWORD, void *);
typedef HRESULT (WINAPI *GetData_t)(void *, DWORD, void *, DWORD *, DWORD);

static const GUID g_sysKeyboard = {
    0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00} };

#define MAX_VT 8
static void **g_diVt[MAX_VT];
static CreateDevice_t g_origCreate[MAX_VT];
static int g_nDi;
static void **g_devVt[MAX_VT];
static GetState_t g_origState[MAX_VT];
static GetData_t  g_origData[MAX_VT];
static int g_nDev;

/* DIK codes above 0x7f are the E0 prefixed scancodes */
static int DikToVk(DWORD dik) {
    UINT vk;
    switch (dik) {
    case 0xC8: return VK_UP;      case 0xD0: return VK_DOWN;
    case 0xCB: return VK_LEFT;    case 0xCD: return VK_RIGHT;
    case 0x9C: return VK_RETURN;  case 0xC7: return VK_HOME;
    case 0xCF: return VK_END;     case 0xC9: return VK_PRIOR;
    case 0xD1: return VK_NEXT;    case 0xD2: return VK_INSERT;
    case 0xD3: return VK_DELETE;  case 0x9D: return VK_RCONTROL;
    case 0xB8: return VK_RMENU;   case 0xB5: return VK_DIVIDE;
    case 0xDB: return VK_LWIN;    case 0xDC: return VK_RWIN;
    default: break;
    }
    if (dik & 0x80)
        vk = MapVirtualKeyA(0xE000u | (dik & 0x7Fu), MAPVK_VSC_TO_VK_EX);
    else
        vk = MapVirtualKeyA(dik, MAPVK_VSC_TO_VK_EX);
    return (int)vk;
}

/* Reverse map for the fake-key injector. */
static DWORD VkToDik(int vk) {
    if (vk >= '1' && vk <= '9')
        return 0x02u + (DWORD)(vk - '1');   /* DIK_1=2 .. DIK_9=10 */
    if (vk == '0') return 0x0Bu;            /* DIK_0 = 11 */
    switch (vk) {
    case VK_ESCAPE: return 0x01;
    case VK_RETURN: return 0x1C;
    case VK_SPACE:  return 0x39;
    case VK_TAB:    return 0x0F;
    case VK_LSHIFT: return 0x2A;
    case VK_RSHIFT: return 0x36;
    case VK_LCONTROL:return 0x1D;
    case VK_UP:     return 0xC8;
    case VK_DOWN:   return 0xD0;
    case VK_LEFT:   return 0xCB;
    case VK_RIGHT:  return 0xCD;
    default: {
        UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
        if (sc) return sc;
    }
    }
    return 0;
}

/* ---- fake key injector --------------------------------------------
 * The game reads its keyboard here, so a synthetic press is nothing
 * more than driving the reported key state through down -> held ->
 * up a set number of times.  Two drivers:
 *   - TIME based: each phase lasts HOLD_MS/REL_MS real time.  Used
 *     when the game needs a human-length press to register.
 *   - FAST based: each phase lasts a couple of GetState reports, no
 *     wall-clock delay.  Used to burst N cycles in one go; the game
 *     only samples this device once per frame, so two reports is a
 *     full frame of pressed state. */
#define TAP_HOLD_MS 60
#define TAP_REL_MS  60
#define TAP_HOLD_F  2
#define TAP_REL_F   2
static volatile LONG   g_tapDik  = 0;   /* DIK being faked, 0 = none */
static volatile LONG   g_tapLeft = 0;   /* taps still to deliver      */
static volatile LONG   g_tapPh   = 0;   /* 0 = pressing, 1 = releasing */
static volatile DWORD  g_tapAt   = 0;   /* when the current phase began */
static volatile LONG   g_tapFrm  = 0;   /* reports spent in this phase */
static volatile LONG   g_tapFast = 0;   /* 1 = frame based, no delay  */
static DWORD  g_tapLast = 0;            /* last log tick               */

static void StepTap(uint8_t *k, int nbytes) {
    LONG dik, left, ph;
    DWORD now = GetTickCount();

    if (!k || nbytes <= 0) return;
    dik  = g_tapDik;
    left = g_tapLeft;
    if (!dik || left <= 0) return;
    ph  = g_tapPh;

    if (dik < (LONG)nbytes) {
        if (g_tapFast) {
            /* frame driven: advance one report per call */
            if (ph == 0) {
                k[dik] = 0x80;
                if (++g_tapFrm >= TAP_HOLD_F) { ph = 1; g_tapFrm = 0; }
            } else {
                k[dik] = 0;
                if (++g_tapFrm >= TAP_REL_F) {
                    ph = 0; g_tapFrm = 0;
                    left--;
                    if (left <= 0) dik = 0;
                }
            }
        } else {
            /* time driven */
            if (ph == 0) {
                k[dik] = 0x80;
                if ((int)(now - g_tapAt) >= TAP_HOLD_MS) { ph = 1; g_tapAt = now; }
            } else {
                k[dik] = 0;
                if ((int)(now - g_tapAt) >= TAP_REL_MS) {
                    ph = 0; g_tapAt = now;
                    left--;
                    if (left <= 0) dik = 0;
                }
            }
        }
    }
    g_tapPh   = ph;
    g_tapLeft = left;
    g_tapDik  = dik;

    /* progress log, at most once per 100 ms */
    if (dik && (int)(now - g_tapLast) >= 100) {
        g_tapLast = now;
        Log("dinput: tap progress dik=%d left=%d phase=%d fast=%d",
            dik, left, ph, g_tapFast);
    }
}

/* Queue `taps` full press/release cycles of the virtual key. */
SH_API void ShFakeKey(int vk, int taps) {
    DWORD dik = VkToDik(vk);
    if (!dik || taps <= 0) return;
    g_tapDik  = (LONG)dik;
    g_tapLeft = taps;
    g_tapPh   = 0;
    g_tapFrm  = 0;
    g_tapAt   = GetTickCount();
    g_tapFast = 0;
    Log("dinput: fake key vk=%d dik=%u taps=%d (time)", vk, dik, taps);
}

/* Burst driver: no wall-clock delay, phases measured in reports. */
SH_API void ShFakeKeyFast(int vk, int taps) {
    DWORD dik = VkToDik(vk);
    if (!dik || taps <= 0) return;
    g_tapDik  = (LONG)dik;
    g_tapLeft = taps;
    g_tapPh   = 0;
    g_tapFrm  = 0;
    g_tapAt   = GetTickCount();
    g_tapFast = 1;
    Log("dinput: fake key vk=%d dik=%u taps=%d (fast)", vk, dik, taps);
}

int ShFakeKeyBusy(void) {
    return g_tapDik && g_tapLeft > 0;
}

static int Suppressed(DWORD dik) {
    int vk;
    if (dik == 0 || dik > 255) return 0;
    vk = DikToVk(dik);
    return vk ? ShKeySuppressedVk(vk) : 0;
}

static int Patch(void **vt, int slot, void *fn, void **orig) {
    DWORD old;
    if (!VirtualProtect(&vt[slot], sizeof(void *), PAGE_READWRITE, &old))
        return 0;
    *orig = vt[slot];
    vt[slot] = fn;
    VirtualProtect(&vt[slot], sizeof(void *), old, &old);
    return 1;
}

static int DevIndex(void *dev) {
    void **vt = *(void ***)dev;
    int i;
    for (i = 0; i < g_nDev; i++) if (g_devVt[i] == vt) return i;
    return -1;
}

static uint8_t g_lastState[256];       /* last report handed to game   */
static volatile LONG g_lastGot = 0;    /* 1 once a report was captured */

static HRESULT WINAPI HookGetState(void *dev, DWORD cb, void *data) {
    int i = DevIndex(dev);
    HRESULT hr;
    if (i < 0) return E_FAIL;
    hr = g_origState[i](dev, cb, data);
    if (SUCCEEDED(hr) && cb == 256 && data) {
        uint8_t *k = (uint8_t *)data;
        DWORD d;
        static int logged;
        if (!logged) { logged = 1; Log("dinput: game reads GetDeviceState"); }
        for (d = 0; d < 256; d++) g_lastState[d] = k[d];
        g_lastGot = 1;
        for (d = 1; d < 256; d++) if (k[d] && Suppressed(d)) k[d] = 0;
        /* Fake key injection rides on the same report. */
        StepTap(k, 256);
    }
    return hr;
}

/* Diag: what the game last saw on this keyboard device. */
int ShKeyState(int vk) {
    DWORD dik = VkToDik(vk);
    if (!dik || dik >= 256 || !g_lastGot) return -1;
    return g_lastState[dik] ? 1 : 0;
}

static HRESULT WINAPI HookGetData(void *dev, DWORD cb, void *data,
                                  DWORD *inout, DWORD flags) {
    int i = DevIndex(dev);
    HRESULT hr;
    if (i < 0) return E_FAIL;
    hr = g_origData[i](dev, cb, data, inout, flags);
    if (SUCCEEDED(hr) && data && inout && cb) {
        static int logged;
        if (!logged) { logged = 1; Log("dinput: game reads GetDeviceData"); }
        uint8_t *p = (uint8_t *)data;
        DWORD n = *inout, src, dst = 0;
        for (src = 0; src < n; src++) {
            DWORD ofs = *(DWORD *)(p + src * cb);
            if (Suppressed(ofs)) continue;
            if (dst != src) memcpy(p + dst * cb, p + src * cb, cb);
            dst++;
        }
        *inout = dst;
    }
    return hr;
}

static void WrapDevice(void *dev) {
    void **vt = *(void ***)dev;
    int i;
    for (i = 0; i < g_nDev; i++) if (g_devVt[i] == vt) return;
    if (g_nDev >= MAX_VT) return;
    g_devVt[g_nDev] = vt;
    if (!Patch(vt, 9, (void *)HookGetState, (void **)&g_origState[g_nDev]))
        return;
    Patch(vt, 10, (void *)HookGetData, (void **)&g_origData[g_nDev]);
    g_nDev++;
    Log("dinput: keyboard device wrapped, vtable %p", (void *)vt);
}

static HRESULT WINAPI HookCreateDevice(void *di, const GUID *guid,
                                       void **out, IUnknown *outer) {
    void **vt = *(void ***)di;
    int i;
    HRESULT hr;
    for (i = 0; i < g_nDi; i++) if (g_diVt[i] == vt) break;
    if (i == g_nDi) return E_FAIL;
    hr = g_origCreate[i](di, guid, out, outer);
    if (SUCCEEDED(hr) && out && *out && guid &&
        memcmp(guid, &g_sysKeyboard, sizeof(GUID)) == 0)
        WrapDevice(*out);
    return hr;
}

/* called by the proxy after the real DirectInput8Create */
void ShWrapDirectInput(void *di) {
    void **vt;
    int i;
    static int logInit;
    if (!di) return;
    if (!logInit) { LogInit("scripthook_dinput.log"); logInit = 1; }
    vt = *(void ***)di;
    for (i = 0; i < g_nDi; i++) if (g_diVt[i] == vt) return;
    if (g_nDi >= MAX_VT) return;
    g_diVt[g_nDi] = vt;
    if (Patch(vt, 3, (void *)HookCreateDevice,
              (void **)&g_origCreate[g_nDi])) {
        g_nDi++;
        Log("dinput: interface wrapped, vtable %p", (void *)vt);
    }
}
