/* Weather, from the GRW Environment cheat table. */
/* Set the type at env+0x130, freeze the per-frame writer,
 * force the transition to complete, then call the change
 * function on the game thread so the sky actually blends. */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

#define ENV_VTABLE  SH_IMG(0x39D20B8)
#define OFF_TYPE    0x130
#define OFF_TIMEOBJ 0xA8
#define OFF_TIME    0x08

/* RVAs, verified in Ghidra: the writer is mov [r14+0x130],
 * r15, the reset stores 0.0f to +0x140, the time writer is
 * movss [rax+8], xmm6, all inside FUN_14c903160's family. */
#define WRITER_SITE SH_IMG(0xC90336E)
#define WRITER_LEN  7
#define INSTANT_SITE SH_IMG(0xC9032C6)
#define INSTANT_LEN 11
#define CHANGE_FN   SH_IMG(0xC903160)
#define TIME_SITE   SH_IMG(0xC88AC8E)
#define TIME_LEN    5

extern int ShReadableAddr(uint64_t addr, size_t len);
extern int ShReadMem(uint64_t addr, void *out, size_t len);
extern uint64_t ShReadQ(uint64_t addr);
extern void ShSetError(int err);
extern int ShQueueCall(uint64_t fn, uint64_t a0, uint64_t a1,
                       uint64_t a2, uint64_t a3);

/* Kernel write into a heap object: fails clean if the env
 * was freed between the check and the store.
 */
static int WriteMem(uint64_t addr, const void *v, size_t len) {
    SIZE_T put = 0;

    if (!WriteProcessMemory(GetCurrentProcess(),
                            (void *)(uintptr_t)addr, v, len, &put))
        return 0;
    return put == len;
}

/* The six known weather ids from the table, indexed by the
 * SH_WEATHER_* enum in scripthook.h.
 */
static const uint64_t g_id[6] = {
    520430410077ULL,   /* sunny        */
    283912574176ULL,   /* clouds light */
    520430611713ULL,   /* clouds heavy */
    532369849126ULL,   /* fog          */
    520430672675ULL,   /* rain light   */
    520430431606ULL,   /* rain heavy   */
};

static uint64_t g_env = 0;
static int g_patched = 0;

static int Sane(uint64_t p) {
    return p >= 0x10000ULL && p < 0x800000000000ULL;
}

static int IsEnv(uint64_t obj) {
    uint64_t id;
    int i;

    if (!Sane(obj) || !ShReadableAddr(obj, OFF_TYPE + 8)) return 0;
    if (ShReadQ(obj) != ENV_VTABLE) return 0;
    id = ShReadQ(obj + OFF_TYPE);
    for (i = 0; i < 6; i++)
        if (id == g_id[i]) return 1;
    return 0;
}

/* By vtable, kept while its type field stays valid. */
static uint64_t FindEnv(void) {
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = (uint8_t *)0x10000;

    if (IsEnv(g_env)) return g_env;
    g_env = 0;

    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;
        if ((uint64_t)(uintptr_t)mbi.BaseAddress >= 0x800000000000ULL)
            break;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & PAGE_READWRITE) &&
            !(mbi.Protect & PAGE_GUARD)) {
            uint8_t *b = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize, o, k, got;

            /* Chunked kernel reads: a region freed mid
             * scan skips instead of faulting. Chunks
             * overlap so no candidate spans a seam. */
            static uint8_t buf[0x10000];

            for (o = 0; o + OFF_TYPE + 8 <= sz;
                 o += sizeof(buf) - OFF_TYPE - 8) {
                got = sz - o;
                if (got > sizeof(buf)) got = sizeof(buf);
                if (!ShReadMem((uint64_t)(uintptr_t)(b + o),
                               buf, got))
                    continue;
                for (k = 0; k + OFF_TYPE + 8 <= got; k += 8) {
                    uint64_t obj;
                    uint64_t vt;
                    memcpy(&vt, buf + k, 8);
                    if (vt != ENV_VTABLE) continue;
                    obj = (uint64_t)(uintptr_t)(b + o + k);
                    if (IsEnv(obj)) { g_env = obj; return obj; }
                }
            }
        }
        scan = next;
    }
    return 0;
}

static void PokeBytes(uint64_t at, const uint8_t *bytes, size_t n) {
    DWORD old;

    if (!VirtualProtect((void *)(uintptr_t)at, n,
                        PAGE_EXECUTE_READWRITE, &old))
        return;
    memcpy((void *)(uintptr_t)at, bytes, n);
    VirtualProtect((void *)(uintptr_t)at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(),
                          (void *)(uintptr_t)at, n);
}

/* Freeze the writer, and store (float)100 at the transition
 * reset so the blend completes at once.
 */
static int Patch(void) {
    static const uint8_t nops[WRITER_LEN] = {
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
    };
    static const uint8_t instant[INSTANT_LEN] = {
        0x41, 0xC7, 0x86, 0x40, 0x01, 0x00, 0x00,
        0x00, 0x00, 0xC8, 0x42
    };
    uint8_t cur[INSTANT_LEN];

    if (g_patched) return 1;
    if (!ShReadableAddr(WRITER_SITE, WRITER_LEN) ||
        !ShReadableAddr(INSTANT_SITE, INSTANT_LEN))
        return 0;

    memcpy(cur, (const void *)(uintptr_t)WRITER_SITE, WRITER_LEN);
    if (cur[0] != 0x4D || cur[1] != 0x89 || cur[2] != 0xBE)
        return 0;
    memcpy(cur, (const void *)(uintptr_t)INSTANT_SITE, INSTANT_LEN);
    if (cur[0] != 0x41 || cur[1] != 0xC7 || cur[2] != 0x86)
        return 0;

    PokeBytes(WRITER_SITE, nops, WRITER_LEN);
    PokeBytes(INSTANT_SITE, instant, INSTANT_LEN);
    g_patched = 1;
    return 1;
}

SH_API int ShSetWeather(int type) {
    uint64_t env;

    if (type < 0 || type >= 6) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    env = FindEnv();
    if (!env) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    if (!Patch()) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }

    if (!WriteMem(env + OFF_TYPE, &g_id[type], 8)) {
        g_env = 0;
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    if (!ShQueueCall(CHANGE_FN, env, g_id[type], 0, 0)) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}

static int g_timePatched = 0;

/* Freeze the day/night writer so a set hour holds. */
static int PatchTime(void) {
    static const uint8_t nops[TIME_LEN] = {
        0x90, 0x90, 0x90, 0x90, 0x90
    };
    uint8_t cur[TIME_LEN];

    if (g_timePatched) return 1;
    if (!ShReadableAddr(TIME_SITE, TIME_LEN)) return 0;
    memcpy(cur, (const void *)(uintptr_t)TIME_SITE, TIME_LEN);
    if (cur[0] != 0xF3 || cur[1] != 0x0F || cur[2] != 0x11)
        return 0;
    PokeBytes(TIME_SITE, nops, TIME_LEN);
    g_timePatched = 1;
    return 1;
}

static uint64_t TimeAddr(uint64_t env) {
    uint64_t tobj;

    tobj = ShReadQ(env + OFF_TIMEOBJ);
    if (!Sane(tobj)) return 0;
    return tobj + OFF_TIME;
}

/* Hours past midnight, 0 to 24. */
SH_API int ShSetTime(float hours) {
    uint64_t env, a;

    if (hours < 0.0f) hours = 0.0f;
    if (hours >= 24.0f) hours = hours - 24.0f * (float)(int)(hours / 24.0f);
    env = FindEnv();
    if (!env) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    a = TimeAddr(env);
    if (!a) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    if (!PatchTime()) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    if (!WriteMem(a, &hours, 4)) {
        g_env = 0;
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShGetTime(float *out) {
    uint64_t env, a;

    if (!out) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    env = FindEnv();
    if (!env) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    a = TimeAddr(env);
    if (!a) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    if (!ShReadMem(a, out, 4)) {
        g_env = 0;
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShGetWeather(int *out) {
    uint64_t env, id;
    int i;

    if (!out) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    env = FindEnv();
    if (!env) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }

    id = ShReadQ(env + OFF_TYPE);
    for (i = 0; i < 6; i++) {
        if (id == g_id[i]) {
            *out = i;
            ShSetError(SH_OK);
            return 1;
        }
    }
    ShSetError(SH_ERR_NO_CANDIDATE);
    return 0;
}
