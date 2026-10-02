/* NvProbe - which D3D11 pixel shader draws night vision?
 *
 * WHY THIS EXISTS
 * ---------------
 * The night-vision filter is not a resource. Every archive was listed and
 * there is no HDRLighting entry anywhere to patch; what the engine has is a
 * dedicated pixel shader in the HDR lighting pass - GRW.exe carries the
 * names HDRLighting_NV_ps, HDRLighting_THERMAL_ps, HDRLighting_ps and an
 * HDRDisplay_* twin of each, each registered as g_<name>_ps_size beside its
 * sg_<name>_ps "shader group" - and that pass runs BEFORE the HUD is
 * composited. That is why the minimap keeps its colour inside night vision,
 * and why a full-screen desaturation at Present (the only place the
 * framework already hooks) is not an option: it would grey the HUD too.
 *
 * So replacing the filter means replacing that shader, and before anything
 * is replaced one thing has to be true: the shader object the engine binds
 * for night vision has to be identifiable at run time, from outside the
 * engine. This probe answers exactly that and nothing else.
 *
 * THE METHOD: TWO SNAPSHOTS, TAKEN BY HAND
 * -----------------------------------------
 * A shader counts as "in use" if it was bound within the last couple of
 * seconds. The menu offers "snapshot A" and "snapshot B", so the whole method
 * is: open the menu, take A with night vision off, press the key to turn it
 * on, take B, and ask for the report. The difference between two clicks
 * cannot be thrown off by timing, and it does not care that the world keeps
 * drawing other things in the meantime - the author can take A and B a second
 * apart, because the mod menu does not take the night-vision key away from
 * the game.
 *
 * The timeline is still written as background: an ON/off line each time a
 * shader enters or leaves use, with a timestamp, a bytecode length and a
 * CRC32. It is context for reading the log around the two snapshots rather
 * than the method itself.
 *
 * A WINDOW OF ITS OWN WOULD NOT WORK, WHICH IS WHY THERE ISN'T ONE
 * ---------------------------------------------------------------
 * The first cut of this file drew its own ImGui window through ShDrawAddEx.
 * That was the wrong shape: the overlay only takes the mouse while the MOD
 * MENU is open, so a plugin's own window is painted during play but cannot be
 * clicked - and the only way to get a cursor back is the game's pause menu,
 * which changes the very state being observed.
 *
 * WHAT IT DOES NOT DO
 * -------------------
 * It never binds a shader, never rewrites one, never writes engine memory,
 * never touches the swapchain, and installs no breakpoint. It counts.
 *
 * HOW
 * ---
 *   - MinHook the two d3d11.dll exports a process creates its device
 *     through, so we can hold the ID3D11Device and its immediate context.
 *   - Hook that device's CreatePixelShader and that context's PSSetShader.
 *     MinHook patches the implementation inside d3d11.dll, NOT the vtable -
 *     the same instinct scripthook_ovl.cpp shows when it gives its swapchain
 *     a private table: another module's table is not ours to rewrite.
 *   - For every created shader keep its bytecode length, a CRC32 of the
 *     bytecode, and the first identifier-looking string in the blob. Engines
 *     that generate their shaders usually leave the generator's own name in
 *     there; if this one does, the question is answered on the first line of
 *     the log and none of the counting is needed.
 *
 * Read-only by construction: the only writes are into this plugin's own
 * arrays and its own log file.
 */
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <d3d11.h>

#include "scripthook.h"
#include "log.h"
#include "third_party/minhook/include/MinHook.h"

#define LOG_NAME   "NvProbe.log"
#define INI_NAME   "NvProbe.ini"

/* ---- capacity -----------------------------------------------------------
 * The engine builds a few hundred pixel shaders a session. 4096 shaders of
 * bookkeeping is 32 KB of counters and covers any build of any game this
 * probe is likely to be pointed at; past the cap a shader is simply not
 * counted, and that is logged once rather than silently.
 */
#define SHADER_MAX   16384
#define TABLE_SLOTS  32768          /* power of two, open addressing        */
#define PROBE_MAX    32             /* bounded linear probe, never a loop   */
#define SWEEP_MS     250            /* how often the activity flag is read  */
#define ACTIVE_MS    2500           /* bound this recently = "in use"       */
#define MIN_BINDS    20             /* below this a shader is not worth a
                                     * timeline line: one-off compiles and
                                     * menu-only shaders are noise here     */
#define TIMELINE_MAX 20000          /* a session emits a few thousand       */
#define SNAPSHOT_MAX 400            /* rows a manual dump prints            */

typedef struct ShaderRec {
    void        *ps;                /* ID3D11PixelShader *, the key         */
    uint32_t     crc;               /* CRC32 of the bytecode                */
    uint32_t     len;               /* bytecode length in bytes             */
    char         name[40];          /* first identifier-looking run         */
    volatile LONG  binds;           /* bindings since load                  */
    volatile LONG  episodes;        /* times this shader came back in use   */
    volatile LONG64 lastMs;         /* tick of its most recent binding      */
    int          active;            /* sweep-owned: in use on the last pass */
    int          inA;               /* in use when snapshot A was taken     */
    int          inB;               /* in use when snapshot B was taken     */
    int          scratch;           /* snapshot dump scratch                */
    int          hdr;               /* blob literally contained HDRLighting */
} ShaderRec;

static ShaderRec     g_slot[TABLE_SLOTS];
static ShaderRec    *g_rec[SHADER_MAX];      /* creation order -> record    */
static volatile LONG g_created;
static volatile LONG g_over;
static volatile LONG g_bindings;
static volatile LONG g_timeline;             /* lines written               */
static volatile LONG g_episodes;             /* ON transitions across all   */
static uint32_t      g_menu;

/* The night-vision pixel shader, identified by the two snapshots (see the
 * note on the method above) and identical in two separate sessions:
 * bytecode 4864 bytes, CRC32 B4230EAA. Only its length and CRC are needed to
 * recognise it; the bytecode itself is dumped once so it can be disassembled
 * offline and matched by a replacement shader. */
static uint32_t g_capCrc  = 0xB4230EAAu;
static uint32_t g_capLen  = 4864;
static int      g_capDone;

static volatile PVOID g_dev;
static ID3D11DeviceContext *g_ctx;

static CRITICAL_SECTION g_cs;                /* table writes + log order     */
static uint32_t g_crcTable[256];
static uint64_t g_startMs;

static void *g_hookedDev;                    /* kept so the retry loop knows */
static void *g_hookedDevSc;

/* ---- small helpers ----------------------------------------------------- */

static void CrcInit(void) {
    uint32_t i, j, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crcTable[i] = c;
    }
}

static uint32_t Crc32(const unsigned char *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    for (i = 0; i < n; i++)
        c = g_crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* Pick the most name-like identifier out of a shader blob.
 *
 * The first cut just took the first run of >= 6 identifier characters. On
 * this engine that is always "SV_Position", from the input signature chunk
 * of the DXBC container - true, useless, and identical for two thousand
 * shaders. So the scan now knows what a container looks like:
 *
 *   - DXBC chunk tags (DXBC, RDEF, ISGN, OSGN, SHEX, SHDR, STAT, IFCE) and
 *     the system-value semantics (SV_Position, SV_Target, ...) score zero;
 *     every shader has them and they name nothing about the shader;
 *   - a run that contains "HDR" is what this probe came for, and wins
 *     outright, because the family we are chasing is HDRLighting_*;
 *   - otherwise a long run with an underscore beats a long run without:
 *     engine shader names look like HDRLighting_NV_ps, and words pulled out
 *     of a constant buffer do not.
 *
 * Two outputs: the best identifier (for the log's own readability) and
 * whether the blob literally contains "HDRLighting" (the instant answer -
 * when that is 1, the shader needs no timeline at all). */
static const char *const BLOB_NOISE[] = {
    "SV_Position", "SV_Target", "SV_Depth", "SV_ClipDistance",
    "SV_CullDistance", "SV_IsFrontFace", "SV_SampleIndex", "SV_Coverage",
    "POSITION", "NORMAL", "TEXCOORD", "COLOR", "BINORMAL", "TANGENT",
    "BLENDINDICES", "BLENDWEIGHT", "DXBC", "RDEF", "ISGN", "OSGN",
    "SHEX", "SHDR", "STAT", "IFCE", "PCSG", "Aon9", "TEXT",
};

static int BlobNoise(const char *s, size_t n) {
    size_t i;
    for (i = 0; i < sizeof BLOB_NOISE / sizeof BLOB_NOISE[0]; i++) {
        if (strlen(BLOB_NOISE[i]) == n && memcmp(BLOB_NOISE[i], s, n) == 0)
            return 1;
    }
    return 0;
}

static int BlobHasHdr(const unsigned char *p, size_t n) {
    static const char needle[] = "HDRLighting";
    size_t m = sizeof needle - 1, i;
    if (n < m) return 0;
    for (i = 0; i + m <= n; i++)
        if (p[i] == (unsigned char)needle[0] && memcmp(p + i, needle, m) == 0)
            return 1;
    return 0;
}

static void BlobName(const unsigned char *p, size_t n, char *out, size_t cap,
                     int *hasHdr) {
    size_t i = 0;
    int bestScore = 0;

    out[0] = 0;
    *hasHdr = BlobHasHdr(p, n);

    while (i < n) {
        size_t s = i, k;
        int score = 0;
        int letters = 0, digits = 0, underscore = 0;
        while (i < n && ((p[i] >= 'A' && p[i] <= 'Z') ||
                         (p[i] >= 'a' && p[i] <= 'z') ||
                         (p[i] >= '0' && p[i] <= '9') || p[i] == '_')) {
            if (p[i] >= '0' && p[i] <= '9') digits++;
            else if (p[i] == '_') underscore++;
            else letters++;
            i++;
        }
        if (i - s >= 6 && i - s + 1 < cap && letters >= 3 &&
            !BlobNoise((const char *)p + s, i - s)) {
            /* Straight byte compare, not strstr: this is a slice of a heap
             * blob, not a C string, and strstr would read past its end. */
            if (i - s >= 3 && p[s] == 'H' && p[s + 1] == 'D' && p[s + 2] == 'R')
                score = 1000;
            else if (underscore && i - s >= 10) score = 100;
            else if (underscore)                score = 50;
            else if (i - s >= 10)               score = 10;
            if (score > bestScore) {
                bestScore = score;
                for (k = 0; k < i - s; k++) out[k] = (char)p[s + k];
                out[i - s] = 0;
            }
        }
        i++;
    }
}

static ShaderRec *Find(void *ps) {
    uintptr_t h = (((uintptr_t)ps >> 4) * 2654435761u) & (TABLE_SLOTS - 1);
    int i;
    for (i = 0; i < PROBE_MAX; i++) {
        ShaderRec *r = &g_slot[(h + i) & (TABLE_SLOTS - 1)];
        if (r->ps == ps) return r;
        if (!r->ps) return NULL;
    }
    return NULL;
}

/* ---- the hooks --------------------------------------------------------- */

typedef HRESULT (STDMETHODCALLTYPE *CreatePsFn)(
    ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *,
    ID3D11PixelShader **);
typedef void (STDMETHODCALLTYPE *SetPsFn)(
    ID3D11DeviceContext *, ID3D11PixelShader *,
    const ID3D11ClassInstance *const *, UINT);

static CreatePsFn g_origCreatePs;
static SetPsFn    g_origSetPs;

static HRESULT STDMETHODCALLTYPE HookCreatePs(
        ID3D11Device *dev, const void *code, SIZE_T len,
        ID3D11ClassLinkage *linkage, ID3D11PixelShader **out) {
    HRESULT hr = g_origCreatePs(dev, code, len, linkage, out);

    /* Record only what the device actually produced: the engine does fail
     * creates on purpose while probing for features, and those have no
     * object to count. */
    if (hr == S_OK && out && *out && code && len) {
        uintptr_t h = (((uintptr_t)*out >> 4) * 2654435761u)
                      & (TABLE_SLOTS - 1);
        int i;
        EnterCriticalSection(&g_cs);
        for (i = 0; i < PROBE_MAX; i++) {
            ShaderRec *r = &g_slot[(h + i) & (TABLE_SLOTS - 1)];
            if (!r->ps) {
                LONG idx = InterlockedIncrement(&g_created) - 1;
                r->ps = *out;
                r->crc = Crc32((const unsigned char *)code, len);
                r->len = (uint32_t)len;
                BlobName((const unsigned char *)code, len, r->name,
                         sizeof(r->name), &r->hdr);
                if (idx >= 0 && idx < SHADER_MAX) g_rec[idx] = r;
                else InterlockedIncrement(&g_over);

                /* The one shader we came for: spill its bytecode so it can be
                 * taken apart offline. Written from the create path, which
                 * runs at start up - the shader is compiled then, whether or
                 * not night vision is ever switched on. */
                if (!g_capDone && r->crc == g_capCrc && r->len == g_capLen) {
                    char path[MAX_PATH];
                    FILE *f;
                    g_capDone = 1;
                    if (LogPath(path, sizeof path, "NvProbe_capture.bin")) {
                        f = fopen(path, "wb");
                        if (f) {
                            fwrite(code, 1, len, f);
                            fclose(f);
                            Log("CAPTURED the night-vision pixel shader: "
                                "%u bytes -> logs\\NvProbe_capture.bin",
                                (unsigned)len);
                        } else {
                            Log("could not write logs\\NvProbe_capture.bin");
                        }
                    }
                }

                Log("created #%ld  len %u  crc %08X  %s%s",
                    (long)idx, (unsigned)r->len, (unsigned)r->crc,
                    r->name[0] ? r->name : "(no identifier in blob)",
                    r->hdr ? "  <<< BLOB CARRIES \"HDRLighting\"" : "");
                break;
            }
        }
        LeaveCriticalSection(&g_cs);
    }
    return hr;
}

/* Hot path: one lookup and two stores, no allocation, no logging. */
static void STDMETHODCALLTYPE HookSetPs(
        ID3D11DeviceContext *ctx, ID3D11PixelShader *ps,
        const ID3D11ClassInstance *const *inst, UINT n) {
    if (ps) {
        ShaderRec *r = Find(ps);
        if (r) {
            r->lastMs = (LONG64)GetTickCount64();
            InterlockedIncrement(&r->binds);
            InterlockedIncrement(&g_bindings);
        }
    }
    g_origSetPs(ctx, ps, inst, n);
}

/* Patch the two implementations, not the tables they live in. Enabling them
 * one at a time rather than MH_ALL_HOOKS keeps this call away from the
 * device-creation hook we are standing in. */
static void InstallShaderHooks(void) {
    void **dvt = *(void ***)g_dev;
    void **cvt = *(void ***)g_ctx;
    void  *fnCreate = dvt[offsetof(ID3D11DeviceVtbl, CreatePixelShader)
                          / sizeof(void *)];
    void  *fnSet = cvt[offsetof(ID3D11DeviceContextVtbl, PSSetShader)
                       / sizeof(void *)];

    if (MH_CreateHook(fnCreate, (LPVOID)HookCreatePs,
                      (LPVOID *)&g_origCreatePs) != MH_OK) {
        Log("MH_CreateHook(CreatePixelShader) failed");
        return;
    }
    if (MH_EnableHook(fnCreate) != MH_OK) {
        Log("MH_EnableHook(CreatePixelShader) failed");
        return;
    }
    if (MH_CreateHook(fnSet, (LPVOID)HookSetPs,
                      (LPVOID *)&g_origSetPs) != MH_OK) {
        Log("MH_CreateHook(PSSetShader) failed");
        return;
    }
    if (MH_EnableHook(fnSet) != MH_OK) {
        Log("MH_EnableHook(PSSetShader) failed");
        return;
    }
    Log("shader hooks live: CreatePixelShader %p, PSSetShader %p",
        fnCreate, fnSet);
}

/* ---- device capture ---------------------------------------------------- */

typedef HRESULT (WINAPI *CreateDevFn)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL *, UINT, UINT,
    ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT (WINAPI *CreateDevScFn)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL *, UINT, UINT, const DXGI_SWAP_CHAIN_DESC *,
    IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *,
    ID3D11DeviceContext **);

static CreateDevFn   g_origCreateDev;
static CreateDevScFn g_origCreateDevSc;

static void TakeDevice(ID3D11Device *dev, ID3D11DeviceContext *ctx) {
    if (!dev || !ctx) return;
    if (InterlockedCompareExchangePointer(&g_dev, dev, NULL) == NULL) {
        g_ctx = ctx;
        Log("captured device %p, immediate context %p", dev, ctx);
        InstallShaderHooks();
    }
}

static HRESULT WINAPI HookCreateDev(
        IDXGIAdapter *a, D3D_DRIVER_TYPE t, HMODULE sw, UINT f,
        const D3D_FEATURE_LEVEL *fl, UINT nfl, UINT sdk,
        ID3D11Device **out, D3D_FEATURE_LEVEL *got,
        ID3D11DeviceContext **outCtx) {
    HRESULT hr = g_origCreateDev(a, t, sw, f, fl, nfl, sdk, out, got, outCtx);
    if (hr == S_OK) TakeDevice(out ? *out : NULL, outCtx ? *outCtx : NULL);
    return hr;
}

static HRESULT WINAPI HookCreateDevSc(
        IDXGIAdapter *a, D3D_DRIVER_TYPE t, HMODULE sw, UINT f,
        const D3D_FEATURE_LEVEL *fl, UINT nfl, UINT sdk,
        const DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **sc,
        ID3D11Device **out, D3D_FEATURE_LEVEL *got,
        ID3D11DeviceContext **outCtx) {
    HRESULT hr = g_origCreateDevSc(a, t, sw, f, fl, nfl, sdk, desc, sc,
                                   out, got, outCtx);
    if (hr == S_OK) TakeDevice(out ? *out : NULL, outCtx ? *outCtx : NULL);
    return hr;
}

/* Hook the exports rather than waiting for a swapchain to appear: the device
 * is created before anything is presented, and a plugin is loaded before
 * either. If the game turns out to create its device first - a different
 * loader order, or a front end that inits graphics early - the log says so
 * instead of leaving us guessing why the counts stayed at zero. */
static int HookDeviceCreation(void) {
    HMODULE d3d = GetModuleHandleA("d3d11.dll");
    void *fnDev, *fnDevSc;

    if (!d3d) return 0;                       /* caller retries           */

    fnDev = (void *)GetProcAddress(d3d, "D3D11CreateDevice");
    fnDevSc = (void *)GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain");
    if (!fnDev && !fnDevSc) return 0;

    if (fnDev) {
        if (MH_CreateHook(fnDev, (LPVOID)HookCreateDev,
                          (LPVOID *)&g_origCreateDev) != MH_OK) {
            Log("MH_CreateHook(D3D11CreateDevice) failed");
            return 0;
        }
        g_hookedDev = fnDev;
        if (MH_EnableHook(fnDev) != MH_OK) {
            Log("MH_EnableHook(D3D11CreateDevice) failed");
            return 0;
        }
    }
    if (fnDevSc) {
        if (MH_CreateHook(fnDevSc, (LPVOID)HookCreateDevSc,
                          (LPVOID *)&g_origCreateDevSc) != MH_OK) {
            Log("MH_CreateHook(...AndSwapChain) failed");
            return 0;
        }
        g_hookedDevSc = fnDevSc;
        if (MH_EnableHook(fnDevSc) != MH_OK) {
            Log("MH_EnableHook(...AndSwapChain) failed");
            return 0;
        }
    }
    Log("holding D3D11CreateDevice=%p AndSwapChain=%p", fnDev, fnDevSc);
    return 1;
}

/* ---- the sweep: activity flag, timeline, status line ------------------- */

static int ActiveCount(void) {
    int i, n = 0;
    for (i = 0; i < SHADER_MAX; i++)
        if (g_rec[i] && g_rec[i]->active) n++;
    return n;
}

/* which: 0 = snapshot A, 1 = snapshot B */
static int CountSnap(int which) {
    int i, n = 0;
    for (i = 0; i < SHADER_MAX; i++) {
        if (!g_rec[i]) continue;
        if (which ? g_rec[i]->inB : g_rec[i]->inA) n++;
    }
    return n;
}

/* In B and not in A - the interesting direction when A is night vision off
 * and B is night vision on. */
static int CountNewInB(void) {
    int i, n = 0;
    for (i = 0; i < SHADER_MAX; i++)
        if (g_rec[i] && g_rec[i]->inB && !g_rec[i]->inA) n++;
    return n;
}

static void Stamp(char *buf, size_t cap) {
    SYSTEMTIME st;
    uint64_t ms = GetTickCount64() - g_startMs;
    GetLocalTime(&st);
    snprintf(buf, cap, "%02u:%02u:%02u.%03u +%llu.%02us",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
             (unsigned long long)(ms / 1000U), (unsigned)((ms % 1000U) / 10U));
}

static void Sweep(void) {
    uint64_t now = GetTickCount64();
    int i, active;
    char stamp[56];

    for (i = 0; i < SHADER_MAX; i++) {
        ShaderRec *r = g_rec[i];
        int on;
        if (!r) continue;

        on = (now - (uint64_t)r->lastMs) <= ACTIVE_MS;

        /* Only a state change is worth reporting, so the timeline is the
         * record of what came and went rather than a per-frame echo. */
        if (on == r->active) continue;
        r->active = on;

        if (on) {
            InterlockedIncrement(&r->episodes);
            InterlockedIncrement(&g_episodes);
        }

        /* One-off compiles and menu-only shaders would drown the timeline;
         * a shader that has never been bound MIN_BINDS times is not a
         * candidate for "the pass that draws the world" - BUT the first
         * episode of every shader is always written. A shader's first use
         * has binds == 1, so filtering on binds alone threw the ON line away
         * and kept the off line, leaving an interval with no beginning: the
         * one thing a timeline cannot afford. */
        if (r->episodes > 1 && r->binds < MIN_BINDS) continue;
        if (InterlockedIncrement(&g_timeline) > TIMELINE_MAX) continue;

        Stamp(stamp, sizeof stamp);
        Log("%s  %-3s #%d  len %u  crc %08X  binds %u  episodes %u  %s%s",
            stamp, on ? "ON" : "off", i, (unsigned)r->len, (unsigned)r->crc,
            (unsigned)r->binds, (unsigned)r->episodes,
            r->name[0] ? r->name : "(no identifier)",
            r->hdr ? "  <<< HDRLighting blob" : "");
    }

    active = ActiveCount();
    if (g_menu) {
        ShMenuStatusF(g_menu, "@nvp.status",
                      active, CountSnap(0), CountSnap(1), CountNewInB());
    }
}

static void LogSnapshot(void) {
    int i, n = 0, round;
    char stamp[56];

    Stamp(stamp, sizeof stamp);
    Log("--- snapshot %s: created %ld, bindings %ld, over cap %ld ---",
        stamp, (long)g_created, (long)g_bindings, (long)g_over);
    EnterCriticalSection(&g_cs);
    /* Most-toggled first: the shader that came and went as often as the
     * player pressed the night-vision key is at the top of this list. */
    for (round = 0; round < SNAPSHOT_MAX; round++) {
        int j, best = -1;
        LONG bestEp = -1, bestBind = -1;
        for (j = 0; j < SHADER_MAX; j++) {
            ShaderRec *r = g_rec[j];
            if (!r || r->scratch || !r->binds) continue;
            if (r->episodes > bestEp ||
                (r->episodes == bestEp && r->binds > bestBind)) {
                bestEp = r->episodes;
                bestBind = r->binds;
                best = j;
            }
        }
        if (best < 0) break;
        g_rec[best]->scratch = 1;
        Log("  #%d  len %u  crc %08X  binds %u  episodes %u  %s%s",
            best, (unsigned)g_rec[best]->len, (unsigned)g_rec[best]->crc,
            (unsigned)g_rec[best]->binds, (unsigned)g_rec[best]->episodes,
            g_rec[best]->name[0] ? g_rec[best]->name : "(no identifier)",
            g_rec[best]->hdr ? "  <<< HDRLighting blob" : "");
        n++;
    }
    for (i = 0; i < SHADER_MAX; i++)
        if (g_rec[i]) g_rec[i]->scratch = 0;
    LeaveCriticalSection(&g_cs);
    Log("--- end snapshot: %d shaders ---", n);
}

/* The manual A/B, which is the method now.
 *
 * Two point-in-time snapshots, taken by the player from the menu - A with
 * night vision off, B with it on. The difference between two clicks is the
 * whole method, so nothing here depends on timing, on a hysteresis window,
 * or on the player reproducing a rhythm. The author can keep the menu open
 * and press the night-vision key, because the menu does not take that key
 * away from the game - so A and B can be taken a second apart.
 *
 * Both directions are printed, and the second matters as much as the first:
 * if the night-vision pass REPLACES the normal HDR lighting pass instead of
 * being added to it, the shader that normally draws the world is in A and
 * not in B. Either list on its own names a candidate; the pair says which
 * way round it works. */
static void LogAB(void) {
    int i, na, nb, nnew = 0, ngone = 0, skipped = 0;
    char stamp[56];

    na = CountSnap(0);
    nb = CountSnap(1);
    Stamp(stamp, sizeof stamp);
    Log("=== A vs B  %s ===", stamp);
    Log("A (night vision OFF): %d shaders in use", na);
    Log("B (night vision ON):  %d shaders in use", nb);

    EnterCriticalSection(&g_cs);
    Log("--- IN B, NOT IN A - the night-vision pass is one of these ---");
    for (i = 0; i < SHADER_MAX; i++) {
        ShaderRec *r = g_rec[i];
        if (!r || !r->inB || r->inA) continue;
        if (r->binds < MIN_BINDS) { skipped++; continue; }
        Log("  #%d  len %u  crc %08X  binds %u  episodes %u  %s%s",
            i, (unsigned)r->len, (unsigned)r->crc, (unsigned)r->binds,
            (unsigned)r->episodes,
            r->name[0] ? r->name : "(no identifier)",
            r->hdr ? "  <<< HDRLighting blob" : "");
        nnew++;
    }
    if (skipped) Log("  (%d more below %d bindings, not worth naming)",
                     skipped, MIN_BINDS);

    Log("--- IN A, NOT IN B - what night vision replaced ---");
    for (i = 0; i < SHADER_MAX; i++) {
        ShaderRec *r = g_rec[i];
        if (!r || !r->inA || r->inB) continue;
        if (r->binds < MIN_BINDS) continue;
        Log("  #%d  len %u  crc %08X  binds %u  episodes %u  %s%s",
            i, (unsigned)r->len, (unsigned)r->crc, (unsigned)r->binds,
            (unsigned)r->episodes,
            r->name[0] ? r->name : "(no identifier)",
            r->hdr ? "  <<< HDRLighting blob" : "");
        ngone++;
    }
    LeaveCriticalSection(&g_cs);
    Log("=== end A vs B: %d listed in B-not-A, %d in A-not-B ===",
        nnew, ngone);
}

/* ---- menu ------------------------------------------------------------- */

static void OnRow(uint32_t menu, uint32_t item, int value, void *user) {
    intptr_t row = (intptr_t)user;
    int i;
    (void)menu; (void)item; (void)value;

    switch (row) {
    case 0:                                   /* snapshot A: NV off */
        EnterCriticalSection(&g_cs);
        for (i = 0; i < SHADER_MAX; i++)
            if (g_rec[i]) g_rec[i]->inA = g_rec[i]->active;
        LeaveCriticalSection(&g_cs);
        Log("SNAPSHOT A taken: %d shaders in use", CountSnap(0));
        ShMenuStatusF(g_menu, "@nvp.aDone", CountSnap(0));
        break;
    case 1:                                   /* snapshot B: NV on */
        EnterCriticalSection(&g_cs);
        for (i = 0; i < SHADER_MAX; i++)
            if (g_rec[i]) g_rec[i]->inB = g_rec[i]->active;
        LeaveCriticalSection(&g_cs);
        Log("SNAPSHOT B taken: %d shaders in use", CountSnap(1));
        ShMenuStatusF(g_menu, "@nvp.bDone", CountSnap(1));
        break;
    case 2:                                   /* A vs B */
        LogAB();
        ShMenuStatusF(g_menu, "@nvp.diffDone", CountNewInB());
        break;
    case 3:                                   /* whole table */
        LogSnapshot();
        ShMenuStatus(g_menu, "@nvp.dumped");
        break;
    default:                                  /* clear */
        EnterCriticalSection(&g_cs);
        for (i = 0; i < SHADER_MAX; i++) {
            if (!g_rec[i]) continue;
            g_rec[i]->inA = 0;
            g_rec[i]->inB = 0;
        }
        LeaveCriticalSection(&g_cs);
        Log("A and B cleared");
        ShMenuStatus(g_menu, "@nvp.cleared");
        break;
    }
}

/* ---- start up ---------------------------------------------------------- */

/* Reads the plugin's own ini. `enabled` is the escape hatch; the two
 * fingerprint keys let the shader be re-pointed at another build without a
 * rebuild, because a game patch changes the bytecode and with it the CRC. */
static int IniOn(void) {
    char path[MAX_PATH];
    char buf[32];

    if (!ShPluginIniPath("NvProbe", path, (int)sizeof path)) return 1;
    if (!GetPrivateProfileIntA("Settings", "enabled", 1, path)) return 0;

    if (GetPrivateProfileStringA("Settings", "capture_crc", "", buf,
                                 sizeof buf, path) && buf[0])
        g_capCrc = (uint32_t)strtoul(buf, NULL, 16);
    g_capLen = (uint32_t)GetPrivateProfileIntA("Settings", "capture_len",
                                               (int)g_capLen, path);
    Log("fingerprint: len %u  crc %08X", (unsigned)g_capLen,
        (unsigned)g_capCrc);
    return 1;
}

/* ---- text ---------------------------------------------------------------
 * The probe's own words, compiled in so the page reads in the player's
 * language with or without plugins\NvProbe\lang.ini beside it: that file is
 * an override, not a requirement, and a page that falls back to raw keys
 * ("@nvp.snapA") when somebody tidies their plugin folder is not a page.
 * The owner is the .asi's base name - that is what the menu stamps on a page
 * it hands a plugin - and a declared row is overridden per key by lang.ini.
 *
 * The backslash in the log path is ONE character: neither this file nor the
 * ini parser treats "\" as an escape, so what is written here is what the
 * player reads. */
static const ShText kEn[] = {
    { "@nvp.page",     "Night vision shader probe" },
    { "@nvp.hint",     "A/B by hand, in three steps. 1) With night vision OFF, "
                       "click 1. 2) Turn night vision ON (the key still "
                       "reaches the game with this menu open) and click 2. "
                       "3) Click 3 and read logs\\NvProbe.log: the shader "
                       "that is in B and not in A is the night-vision pass. "
                       "Nothing here changes a shader or the game." },
    { "@nvp.snapA",    "1  Snapshot A  (night vision OFF)" },
    { "@nvp.snapB",    "2  Snapshot B  (night vision ON)" },
    { "@nvp.diff",     "3  Write A vs B to the log" },
    { "@nvp.full",     "Write the whole shader table" },
    { "@nvp.clear",    "Clear A and B" },
    { "@nvp.idle",     "waiting" },
    { "@nvp.aDone",    "A taken: %d shaders in use (night vision off)" },
    { "@nvp.bDone",    "B taken: %d shaders in use (night vision on)" },
    { "@nvp.diffDone", "written - %d shader(s) in B and not in A" },
    { "@nvp.dumped",   "table written to logs\\NvProbe.log" },
    { "@nvp.cleared",  "A and B cleared" },
    { "@nvp.status",   "in use %d   A %d   B %d   new in B %d" }
};

static const ShText kZh[] = {
    { "@nvp.page",     "夜视着色器探针" },
    { "@nvp.hint",     "手动两点对比，三步：① 夜视关闭时点「1」；② 按夜视键打开"
                       "夜视（菜单开着时这个键照样传给游戏），点「2」；③ 点「3」，"
                       "然后看 logs\\NvProbe.log —— 在 B 里、不在 A 里的那个着色器"
                       "就是画夜视的那个。本页不改任何着色器、不改游戏。" },
    { "@nvp.snapA",    "1  拍快照 A（夜视关闭时）" },
    { "@nvp.snapB",    "2  拍快照 B（夜视开启时）" },
    { "@nvp.diff",     "3  把 A / B 对比写进日志" },
    { "@nvp.full",     "写出整张着色器表" },
    { "@nvp.clear",    "清除 A 和 B" },
    { "@nvp.idle",     "等待中" },
    { "@nvp.aDone",    "A 已拍：在用 %d 个（夜视关）" },
    { "@nvp.bDone",    "B 已拍：在用 %d 个（夜视开）" },
    { "@nvp.diffDone", "已写入 —— B 有而 A 没有的着色器：%d 个" },
    { "@nvp.dumped",   "整张表已写入 logs\\NvProbe.log" },
    { "@nvp.cleared",  "A 和 B 已清除" },
    { "@nvp.status",   "在用 %d   A %d   B %d   B 新增 %d" }
};

static void ProbeText(void) {
    static int done;

    if (done) return;
    done = 1;
    ShLangDeclare("NvProbe", "en-US", kEn, (int)(sizeof kEn / sizeof kEn[0]));
    ShLangDeclare("NvProbe", "zh-CN", kZh, (int)(sizeof kZh / sizeof kZh[0]));
}

static DWORD WINAPI InitThread(LPVOID p) {
    int tries;
    (void)p;

    CrcInit();
    InitializeCriticalSection(&g_cs);
    g_startMs = GetTickCount64();
    /* A probe's log is the deliverable, so it must survive a release build's
     * quiet level: LogInitAlways writes at every level but "none". */
    LogInitAlways(LOG_NAME);

    if (!IniOn()) { Log("disabled by ini"); return 0; }

    if (MH_Initialize() != MH_OK) {
        Log("MH_Initialize failed - nothing can be hooked");
        return 0;
    }

    Log("NvProbe up: night vision is an HDR lighting pass, so we are looking "
        "for the pixel shader that comes and goes with the night-vision key");

    /* The menu first: it is the control surface, and it does not depend on
     * the device being hooked yet. Text before the page, or the first
     * capture draws lang keys. */
    ProbeText();
    for (tries = 0; tries < 40 && !g_menu; tries++) {
        g_menu = ShMenuCreate("@nvp.page");
        if (g_menu) {
            ShMenuHint(g_menu, "@nvp.hint");
            ShMenuAction(g_menu, "@nvp.snapA", OnRow, (void *)(intptr_t)0);
            ShMenuAction(g_menu, "@nvp.snapB", OnRow, (void *)(intptr_t)1);
            ShMenuAction(g_menu, "@nvp.diff", OnRow, (void *)(intptr_t)2);
            ShMenuAction(g_menu, "@nvp.full", OnRow, (void *)(intptr_t)3);
            ShMenuAction(g_menu, "@nvp.clear", OnRow, (void *)(intptr_t)4);
            ShMenuStatus(g_menu, "@nvp.idle");
            Log("menu page up (F4 opens the root menu)");
        } else {
            Sleep(500);
        }
    }
    if (!g_menu) {
        Log("no menu page (%08x) - the timeline still runs", ShLastError());
    }

    /* d3d11.dll is loaded by the game, not by us: wait for it rather than
     * pulling it in sideways, and stop after two minutes so a session with
     * no device at all still produces a readable log instead of nothing. */
    for (tries = 0; tries < 480 && !g_dev; tries++) {
        if (!g_hookedDev && !g_hookedDevSc) HookDeviceCreation();
        Sweep();
        if (g_dev) break;
        Sleep(SWEEP_MS);
    }
    if (!g_dev) {
        Log("no D3D11 device after 120 s - was graphics initialised before "
            "the plugin was loaded, or is this session not using D3D11?");
    } else {
        Log("ready - switch night vision on and off a few times; the "
            "timeline below is the answer");
    }

    /* This thread becomes the ticker: watch the activity flags flip and
     * write the timeline. No window, no clicks, nothing to interrupt. */
    for (;;) {
        Sleep(SWEEP_MS);
        Sweep();
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        /* Never do the work here: the loader calls this under its own lock,
         * which is exactly the deadlock loader.c warns about. */
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
    }
    return TRUE;
}
