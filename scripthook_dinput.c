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

static int Suppressed(DWORD dik) {
    UINT vk;
    if (dik == 0 || dik > 255) return 0;
    vk = MapVirtualKeyA(dik, MAPVK_VSC_TO_VK_EX);
    return vk ? ShKeySuppressedVk((int)vk) : 0;
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

static HRESULT WINAPI HookGetState(void *dev, DWORD cb, void *data) {
    int i = DevIndex(dev);
    HRESULT hr;
    if (i < 0) return E_FAIL;
    hr = g_origState[i](dev, cb, data);
    if (SUCCEEDED(hr) && cb == 256 && data) {
        uint8_t *k = (uint8_t *)data;
        DWORD d;
        for (d = 1; d < 256; d++) if (k[d] && Suppressed(d)) k[d] = 0;
    }
    return hr;
}

static HRESULT WINAPI HookGetData(void *dev, DWORD cb, void *data,
                                  DWORD *inout, DWORD flags) {
    int i = DevIndex(dev);
    HRESULT hr;
    if (i < 0) return E_FAIL;
    hr = g_origData[i](dev, cb, data, inout, flags);
    if (SUCCEEDED(hr) && data && inout && cb) {
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
