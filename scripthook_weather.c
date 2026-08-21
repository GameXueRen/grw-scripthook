/* Weather and time via the engine's request record.
 * FINDINGS.md "THE WEATHER FRONT DOOR". No code patches.
 */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

/* Served by the env object each frame. */
#define WX_RECORD   SH_IMG(0x4495E90)
#define REC_ENV     0x00
#define REC_TIME    0x08   /* hours */
#define REC_GATE    0xA9   /* 1: ambient off */
#define REC_ID      0xB0   /* requested weather */
#define REC_SECS    0xB8   /* blend seconds */

#define ENV_VTABLE  SH_IMG(0x39D20B8)
#define ENV_TYPE    0x130
#define ENV_BLEND   0x0A0  /* default seconds */

#define TIME_MGR    SH_IMG(0x4B9B4F8)
#define TM_GET      0x270
#define TM_SET      0x274

/* ChangeTimeAndWeather node: its time half. */
#define OBJ_FACTORY   SH_IMG(0xE0E0C70)
#define CTW_OP_DESC   SH_IMG(0x49E2AF0)
#define CTW_DATA_DESC SH_IMG(0x49E2A50)
#define CTW_START     SH_IMG(0x2827B50)

#define D_WEATHER_ON 0x60
#define D_MODE       0x70
#define D_TIME_ON    0x71
#define D_HOURS      0x74
#define D_MINUTES    0x78
#define D_FREEZE     0x80

#define CALL_WAIT_MS 1000

extern int ShReadableAddr(uint64_t addr, size_t len);
extern int ShReadMem(uint64_t addr, void *out, size_t len);
extern uint64_t ShReadQ(uint64_t addr);
extern void ShSetError(int err);
extern int ShRequireInGame(void);
extern int ShQueueCall(uint64_t fn, uint64_t a0, uint64_t a1,
                       uint64_t a2, uint64_t a3);
extern int ShQueueResult(uint64_t *outRet);

/* Kernel write, fails clean on a freed object. */
static int WriteMem(uint64_t addr, const void *v, size_t len) {
    SIZE_T put = 0;

    if (!WriteProcessMemory(GetCurrentProcess(),
                            (void *)(uintptr_t)addr, v, len, &put))
        return 0;
    return put == len;
}

/* Weather ids, indexed by SH_WEATHER_*. */
static const uint64_t g_id[6] = {
    520430410077ULL,   /* sunny        */
    283912574176ULL,   /* clouds light */
    520430611713ULL,   /* clouds heavy */
    532369849126ULL,   /* fog          */
    520430672675ULL,   /* rain light   */
    520430431606ULL,   /* rain heavy   */
};

static int Sane(uint64_t p) {
    return p >= 0x10000ULL && p < 0x800000000000ULL;
}

static uint64_t Env(void) {
    uint64_t env;

    env = ShReadQ(WX_RECORD + REC_ENV);
    if (!Sane(env)) return 0;
    if (ShReadQ(env) != ENV_VTABLE) return 0;
    return env;
}

SH_API int ShSetWeatherBlend(int type, float seconds) {
    uint8_t on = 1;
    uint64_t zero = 0;

    if (type < 0 || type >= 6 || seconds < 0.0f ||
        seconds > 3600.0f) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    if (!Env()) { ShSetError(SH_ERR_NOT_IN_GAME); return 0; }

    /* The id write arms the request, so it goes last. */
    if (!WriteMem(WX_RECORD + REC_GATE, &on, 1) ||
        !WriteMem(WX_RECORD + REC_ID, &zero, 8) ||
        !WriteMem(WX_RECORD + REC_SECS, &seconds, 4) ||
        !WriteMem(WX_RECORD + REC_ID, &g_id[type], 8)) {
        ShSetError(SH_ERR_UNWRITABLE);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShSetWeather(int type) {
    float secs = 10.0f;
    uint64_t env = Env();

    if (env) {
        float d = 0.0f;
        if (ShReadMem(env + ENV_BLEND, &d, 4) &&
            d > 0.0f && d <= 120.0f)
            secs = d;
    }
    return ShSetWeatherBlend(type, secs);
}

SH_API int ShReleaseWeather(void) {
    uint8_t off = 0;
    uint64_t zero = 0;

    if (!ShReadableAddr(WX_RECORD, 0xC0)) {
        ShSetError(SH_ERR_NOT_IN_GAME);
        return 0;
    }
    if (!WriteMem(WX_RECORD + REC_ID, &zero, 8) ||
        !WriteMem(WX_RECORD + REC_GATE, &off, 1)) {
        ShSetError(SH_ERR_UNWRITABLE);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShGetWeather(int *out) {
    uint64_t env, id;
    int i;

    if (!out) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    env = Env();
    if (!env) { ShSetError(SH_ERR_NOT_IN_GAME); return 0; }

    id = ShReadQ(env + ENV_TYPE);
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

SH_API int ShGetTime(float *out) {
    if (!out) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (!Env()) { ShSetError(SH_ERR_NOT_IN_GAME); return 0; }
    if (!ShReadMem(WX_RECORD + REC_TIME, out, 4)) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}

/* ---- time of day ---- */

typedef struct { uint64_t st, pin, r2, r3; } CtwRes;
typedef uint64_t (*Factory_t)(uint64_t desc, uint64_t existing);
typedef CtwRes *(*CtwStart_t)(CtwRes *out, uint64_t op,
                              uint64_t ctx, uint64_t data);

static uint64_t g_ctwOp, g_ctwData;

/* Game thread only, via the call queue. */
static uint64_t TimeHelper(uint64_t hoursBits, uint64_t a1,
                           uint64_t a2, uint64_t a3) {
    float hours;
    uint32_t bits = (uint32_t)hoursBits;
    CtwRes res;
    uint8_t *d;

    (void)a1; (void)a2; (void)a3;
    memcpy(&hours, &bits, 4);

    if (!g_ctwOp) {
        Factory_t fac = (Factory_t)(uintptr_t)OBJ_FACTORY;
        g_ctwOp = fac(CTW_OP_DESC, 0);
        g_ctwData = fac(CTW_DATA_DESC, 0);
    }
    if (!g_ctwOp || !g_ctwData) return 0;

    d = (uint8_t *)(uintptr_t)g_ctwData;
    d[D_WEATHER_ON] = 0;
    d[D_MODE] = 0;
    d[D_TIME_ON] = 1;
    d[D_FREEZE] = 0;
    memcpy(d + D_HOURS, &hours, 4);
    memset(d + D_MINUTES, 0, 8);

    ((CtwStart_t)(uintptr_t)CTW_START)(&res, g_ctwOp, 0,
                                       g_ctwData);
    return res.st == 1;
}

SH_API int ShSetTime(float hours) {
    uint64_t ret = 0, bits64;
    uint32_t bits;
    int waited = 0;

    if (hours < 0.0f) hours = 0.0f;
    if (hours >= 24.0f)
        hours = hours - 24.0f * (float)(int)(hours / 24.0f);
    if (!ShRequireInGame()) return 0;
    if (!Env()) { ShSetError(SH_ERR_NOT_IN_GAME); return 0; }

    memcpy(&bits, &hours, 4);
    bits64 = bits;
    while (!ShQueueCall((uint64_t)(uintptr_t)TimeHelper,
                        bits64, 0, 0, 0)) {
        if (++waited > CALL_WAIT_MS) {
            ShSetError(SH_ERR_HOOK_FAILED);
            return 0;
        }
        Sleep(1);
    }
    while (!ShQueueResult(&ret)) {
        if (++waited > CALL_WAIT_MS) {
            ShSetError(SH_ERR_HOOK_FAILED);
            return 0;
        }
        Sleep(1);
    }
    if (!ret) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    ShSetError(SH_OK);
    return 1;
}

/* ---- clock rate ---- */

static uint64_t TimeMgr(void) {
    uint64_t mgr = ShReadQ(TIME_MGR);

    if (!Sane(mgr) || !ShReadableAddr(mgr + TM_GET, 8))
        return 0;
    return mgr;
}

SH_API int ShSetTimeSpeed(float multiplier) {
    uint64_t mgr;

    if (multiplier < 0.0f || multiplier > 100000.0f) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    mgr = TimeMgr();
    if (!mgr) { ShSetError(SH_ERR_NOT_IN_GAME); return 0; }
    if (!WriteMem(mgr + TM_SET, &multiplier, 4)) {
        ShSetError(SH_ERR_UNWRITABLE);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShGetTimeSpeed(float *out) {
    uint64_t mgr;

    if (!out) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    mgr = TimeMgr();
    if (!mgr) { ShSetError(SH_ERR_NOT_IN_GAME); return 0; }
    if (!ShReadMem(mgr + TM_GET, out, 4)) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}
