/* Native widgets in the engine's own HUD tree. */
/* Containers, white quads and labels made through the
 * phoenix factory on template instances we build ourselves.
 * Every engine write runs on the game thread as one job. */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"
#include "log.h"

/* Engine entry points as RVAs. See FINDINGS, UI SYSTEM. */
#define F_ALLOC_CTX     SH_IMG(0xE064390)
#define F_ALLOC         SH_IMG(0x60ACBF0)
#define G_POOL          SH_IMG(0x4D78D00)
#define F_CONT_CTOR     SH_IMG(0x32F5E70)
#define F_ATTACH        SH_IMG(0x32F4D00)
#define F_DIRTY         SH_IMG(0x17408BA0)
#define F_LINST_CTOR    SH_IMG(0x336F0B0)
#define F_LABEL_CREATE  SH_IMG(0x336F7F0)
#define F_LABEL_APPLY   SH_IMG(0x336F7B0)
#define F_LABEL_TEXT    SH_IMG(0x3336E30)
#define F_LABEL_SIZE    SH_IMG(0x3336EE0)
#define F_LABEL_UPDATE  SH_IMG(0x17490300)
#define F_LABEL_REGIST  SH_IMG(0x33359F0)
#define F_IINST_CTOR    SH_IMG(0x336E7F0)
#define F_IMAGE_CREATE  SH_IMG(0x336EC80)
#define F_IMAGE_APPLY   SH_IMG(0x336EC30)
#define F_WIDGET_COLOUR SH_IMG(0x32F3DD0)

#define VT_LABEL        SH_IMG(0x3CF89D0)
#define VT_CONTAINER    SH_IMG(0x3CF09C0)
#define VT_CONT_PRIV    SH_IMG(0x3CF09F8)
#define VT_IMAGE        SH_IMG(0x3CF1660)
#define VT_LABEL_INST   SH_IMG(0x3D052C8)
#define VT_IMAGE_INST   SH_IMG(0x3D04EA0)

/* Widget private layout. */
#define P_LOCAL      0x90
#define P_COLOUR     0xC0
#define P_LOCAL2     0xF0
#define P_SCENE      0x130
#define P_PARENT     0x138
#define P_GROUP      0x148
#define P_ROTATION   0x15C
#define P_ALPHA      0x160
#define P_TEXT       0x1B0
#define P_SIZE       0x210
#define P_DIRTY      0x248
#define P_REGFLAG    0x251
#define C_KIDS       0x178
#define C_ORDER      0x198

/* Template instance layout. */
#define LI_STYLE     0xD8
#define LI_FONTNAME  0xE0
#define LI_FONTREF   0xE8
#define II_IMAGEREF  0xC8
#define II_NAME      0xD8

#define JOB_WAIT_MS  3000
#define MAX_UI       256
#define MAX_TEXT     120

extern void ShSetError(int err);
extern int  ShIsInGame(void);
extern uint32_t ShGetGameStateHash(void);
extern int  ShQueueCall(uint64_t fn, uint64_t a0, uint64_t a1,
                        uint64_t a2, uint64_t a3);
extern int  ShQueueResult(uint64_t *outRet);

#define HASH_PLAYING 0x8816ABC6u

typedef uint64_t (__attribute__((ms_abi)) *Fn1)(uint64_t);
typedef uint64_t (__attribute__((ms_abi)) *Fn2)(uint64_t, uint64_t);
typedef uint64_t (__attribute__((ms_abi)) *Fn3)(uint64_t, uint64_t,
                                                uint64_t);
typedef uint64_t (__attribute__((ms_abi)) *Fn4)(uint64_t, uint64_t,
                                                uint64_t, uint64_t);

enum { K_NONE = 0, K_PANEL, K_LABEL, K_IMAGE, K_CONTAINER };
enum { OP_PANEL = 1, OP_LABEL, OP_IMAGE, OP_TEXT, OP_POS, OP_SIZE,
       OP_COLOUR, OP_ALPHA, OP_DESTROY, OP_PURGE, OP_PROP,
       OP_TEXTURE, OP_IMGSET, OP_CONTAINER, OP_BATCH, OP_AUTOSIZE,
       OP_REPARENT };

/* label size flags at +0x24c/+0x24d: 1 fixed, 0 by text */
#define F_LABEL_FIXW  SH_IMG(0x33366A0)
#define F_LABEL_FIXH  SH_IMG(0x3337140)

/* Textures of our own: the engine's texture object, pixels
 * pushed via its direct map, drawn by name "ptr_<hex>". */
#define F_TEX_CTOR    SH_IMG(0xDC4D920)
#define F_TEX_CREATE  SH_IMG(0xDCC7280)
#define F_TEX_MAP     SH_IMG(0xDC79B00)
#define F_TEX_PUSH    SH_IMG(0x14FEEB0)
#define F_IMG_UV0     SH_IMG(0x32FDDD0)
#define F_IMG_UV1     SH_IMG(0x32FDF70)
#define G_DEVICE      SH_IMG(0x4D5B058)
#define MAX_TEX       64

typedef struct {
    uint64_t obj;         /* the 0x90 texture object */
    int      w, h;
    int      alive;
} Texture;

typedef struct {
    const uint8_t *rgba;
    int w, h, stride;
    uint64_t obj;
} TexCall;

static Texture g_tex[MAX_TEX];

/* A typed property call carried through the job. */
typedef struct {
    int      op;          /* 1 setF 2 setU 3 setV 4 setS
                             5 getF 6 getU 7 getV 8 getS 9 measure */
    uint32_t prop;
    int      n;
    float    f[4];
    uint32_t u;
    uint64_t block;       /* string block in, block out */
    int      ok;
} PropCall;

extern int ShPropSetF(uint64_t h, uint32_t id, float v);
extern int ShPropSetU(uint64_t h, uint32_t id, uint32_t v);
extern int ShPropSetV(uint64_t h, uint32_t id, const float *v, int n);
extern int ShPropSetS(uint64_t h, uint32_t id, uint64_t block);
extern int ShPropGetF(uint64_t h, uint32_t id, float *out);
extern int ShPropGetU(uint64_t h, uint32_t id, uint32_t *out);
extern int ShPropGetV(uint64_t h, uint32_t id, float *out, int n);
extern uint64_t ShPropGetS(uint64_t h, uint32_t id);
extern int ShPropMeasure(uint64_t h, float *w, float *hgt);
extern int ShPropType(uint64_t h, uint32_t id);
extern int ShPropCount(void);
extern int ShPropAt(int i, char *cls, int n, uint32_t *id, int *type,
                    uint64_t *set, uint64_t *get);

extern void ShSceneFree(uint64_t p);
extern int  ShSceneAlloc(int order);
extern int  ShSceneEnsure(int sid, uint64_t *scene, uint64_t *rootH,
                          uint64_t *rootP);
extern int  ShSceneLive(int sid);
extern int  ShSceneRelease(int sid);
extern int  ShSceneSetOrder(int sid, int order);
extern int  ShSceneShow(int sid, int visible);
extern uint64_t ShScenePriv(int sid);
extern uint64_t ShSceneDeadPriv(int sid);
extern void ShSceneInvalidate(void);
extern int  ShPropRttiOf(uint64_t obj, char *out, int n);

#define MAX_SCENES 16

typedef struct {
    int      kind;
    int      alive;
    int      zombie;      /* dead scene, memory still ours */
    int      gen;
    uint32_t scene;       /* scene id, 1 is the default */
    uint32_t parent;      /* id, 0 for the scene root */
    uint64_t handle, priv;
    uint64_t inst;        /* template instance we allocated */
    uint64_t plate;       /* panel: its quad's private */
    uint64_t plateHandle, plateInst;
    float    x, y, w, h, alpha, shown;
    uint32_t rgb;
    int      autoW, autoH;
    uint64_t scratch;     /* PropCall for OP_PROP */
    char     text[MAX_TEXT];
} Widget;

static Widget g_w[MAX_UI];
static CRITICAL_SECTION g_lock;
static int g_lockInit = 0;

static int RunJob(int op, Widget *w);
static int HaveZombies(void);

static struct {
    int      gen;
    uint64_t fontAsset, imageAsset, pool;
} g_ctx;

/* The asset records sit in the image's data section, so the
 * offset from the module base is stable across sessions. A
 * cached offset is verified before use; a recycled or moved
 * block simply misses and falls back to the full scan. */
static uint64_t g_assetFontOff, g_assetImageOff;

/* batches: edits recorded per thread, one job per commit */
typedef struct {
    int      op;
    uint32_t id;
    PropCall pc;
} BOp;

#define MAX_BOPS   256
#define MAX_BATCH  8

typedef struct {
    int  used;
    int  n;
    BOp  ops[MAX_BOPS];
} Batch;

static Batch g_batches[MAX_BATCH];

/* explicit TLS, no winpthread dependency */
static DWORD g_tlsBatch = TLS_OUT_OF_INDEXES;

static Batch *BatchOf(void) {
    if (g_tlsBatch == TLS_OUT_OF_INDEXES) return NULL;
    return (Batch *)TlsGetValue(g_tlsBatch);
}

static void SetBatch(Batch *b) {
    if (g_tlsBatch == TLS_OUT_OF_INDEXES) g_tlsBatch = TlsAlloc();
    if (g_tlsBatch != TLS_OUT_OF_INDEXES) TlsSetValue(g_tlsBatch, b);
}
#define t_batch (BatchOf())

/* reset callbacks per scene */
typedef void (*ResetFn)(uint32_t scene, void *user);
static ResetFn g_resetFn[MAX_SCENES + 1];
static void   *g_resetUser[MAX_SCENES + 1];
static volatile int g_resetDue[MAX_SCENES + 1];

/* Default assets by GUID (bytes as stored at asset+0x10)
 * with their class GUIDs (asset+0x30): the HUD font and the
 * white 16x16 texture. Plugins can point these elsewhere. */
static uint8_t g_fontGuid[16] = {
    0x87,0x3f,0xe5,0x3f,0x3b,0x90,0xdb,0x4d,
    0x98,0x87,0xd3,0xcc,0x6e,0xde,0xab,0xa9 };
static uint8_t g_imageGuid[16] = {
    0x09,0xc6,0xfb,0x11,0x32,0x95,0x07,0x4e,
    0xb3,0xca,0x4f,0xc8,0x9b,0x06,0x90,0x38 };

static void Lock(void) {
    if (!g_lockInit) {
        InitializeCriticalSection(&g_lock);
        LogInit("scripthook_ui.log");
        g_lockInit = 1;
    }
    EnterCriticalSection(&g_lock);
}
static void Unlock(void) { LeaveCriticalSection(&g_lock); }

static int Sane(uint64_t p) {
    return p >= 0x10000ULL && p < 0x800000000000ULL;
}

static int Readable(uint64_t addr, size_t len) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!Sane(addr)) return 0;
    if (!VirtualQuery((void *)(uintptr_t)addr, &mbi, sizeof(mbi)))
        return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize
           >= addr + len;
}

static uint64_t RQ(uint64_t a) {
    uint64_t v = 0;
    if (Readable(a, 8)) memcpy(&v, (void *)(uintptr_t)a, 8);
    return v;
}
static void WQ(uint64_t a, uint64_t v) { memcpy((void *)(uintptr_t)a, &v, 8); }
static void WF(uint64_t a, float v)    { memcpy((void *)(uintptr_t)a, &v, 4); }
static void WD(uint64_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, 4); }

/* ---- scanning, one shot at resolve time ---- */

typedef int (*ScanFn)(uint64_t obj, void *user);

static uint64_t ScanVtable(uint64_t vt, ScanFn test, void *user) {
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = NULL;

    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD) &&
            (uint64_t)(uintptr_t)mbi.BaseAddress < 0x800000000000ULL)
        {
            /* Copied in chunks through the kernel: a region
             * released mid scan then fails the copy instead
             * of faulting the game. */
            static uint8_t chunk[0x10000];
            uint8_t *b = (uint8_t *)mbi.BaseAddress;
            size_t done = 0, sz = mbi.RegionSize;
            while (done < sz) {
                size_t want = sz - done, got = 0, o;
                if (want > sizeof(chunk)) want = sizeof(chunk);
                if (!ReadProcessMemory(GetCurrentProcess(), b + done,
                                       chunk, want, &got) || got < 8)
                    break;
                for (o = 0; o + 8 <= got; o += 8) {
                    uint64_t v;
                    memcpy(&v, chunk + o, 8);
                    if (v != vt) continue;
                    if (test((uint64_t)(uintptr_t)(b + done + o), user))
                        return (uint64_t)(uintptr_t)(b + done + o);
                }
                done += got;
            }
        }
        scan = next;
    }
    return 0;
}

/* An asset is a PhoenixAtom object: GUID at +0x10, class
 * GUID at +0x30. Verify a candidate record: the GUID must
 * still match, or a recycled block would pass as the wrong
 * asset and the labels would render blank. */
static uint64_t AssetAt(uint64_t rec, const uint8_t *guid) {
    uint64_t obj;
    char nm[96];
    if (!Readable(rec, 0x28)) return 0;
    if (memcmp((void *)(uintptr_t)(rec + 0x10), guid, 16) != 0) return 0;
    obj = RQ(rec);
    if (!Readable(obj, 8)) return 0;
    if (!ShPropRttiOf(obj, nm, sizeof(nm))) return 0;
    if (!strstr(nm, "Asset")) return 0;
    return rec;
}

/* One shot scan of the heap for the two asset records, in a
 * single walk so a cold first build does not pay for the
 * missing assets twice. Stops early once both are found. */
static void ScanAssets(uint64_t *font, uint64_t *image) {
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = NULL;

    while (VirtualQuery(scan, &mbi, sizeof(mbi)) &&
           (!*font || !*image)) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD) &&
            (uint64_t)(uintptr_t)mbi.BaseAddress < 0x800000000000ULL)
        {
            static uint8_t chunk[0x10000 + 0x40];
            uint8_t *b = (uint8_t *)mbi.BaseAddress;
            size_t done = 0, sz = mbi.RegionSize;
            while (done < sz) {
                size_t want = sz - done, got = 0, o;
                if (want > 0x10000) want = 0x10000;
                if (!ReadProcessMemory(GetCurrentProcess(), b + done,
                                       chunk, want, &got) || got < 0x40)
                    break;
                for (o = 0; o + 0x40 <= got; o += 8) {
                    uint64_t rec;
                    /* record {object, flags, guid}: the object
                       must carry an Asset class vtable */
                    if (!*font &&
                        memcmp(chunk + o, g_fontGuid, 16) == 0) {
                        rec = (uint64_t)(uintptr_t)(b + done + o) - 0x10;
                        if (AssetAt(rec, g_fontGuid)) *font = rec;
                    }
                    if (!*image &&
                        memcmp(chunk + o, g_imageGuid, 16) == 0) {
                        rec = (uint64_t)(uintptr_t)(b + done + o) - 0x10;
                        if (AssetAt(rec, g_imageGuid)) *image = rec;
                    }
                }
                /* overlap so a pair on the chunk edge is seen */
                done += (got > 0x38) ? got - 0x38 : got;
            }
        }
        scan = next;
    }
}

/* Cached asset records, verified in place, then one combined
 * scan for whatever is still missing. The scan is a whole
 * address space walk and takes seconds on a cold session, so
 * the verified offsets matter for the very first F4. */
static void FindAssets(uint64_t *font, uint64_t *image) {
    uint64_t rec;
    DWORD t0 = GetTickCount();

    if (!*font && g_assetFontOff) {
        rec = AssetAt(ShImageBase() + g_assetFontOff, g_fontGuid);
        if (rec) *font = rec; else g_assetFontOff = 0;
    }
    if (!*image && g_assetImageOff) {
        rec = AssetAt(ShImageBase() + g_assetImageOff, g_imageGuid);
        if (rec) *image = rec; else g_assetImageOff = 0;
    }
    if (*font && *image) return;
    ScanAssets(font, image);
    if (*font) g_assetFontOff = *font - ShImageBase();
    if (*image) g_assetImageOff = *image - ShImageBase();
    Log("asset scan: %lu ms, font %llx image %llx",
        (unsigned long)(GetTickCount() - t0),
        (unsigned long long)*font, (unsigned long long)*image);
}

static int Nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "873fe53f-3b90-db4d-9887-d3cc6edeaba9": 16 bytes in
 * storage order, dashes optional. */
static int ParseGuid(const char *s, uint8_t *out) {
    int n = 0;
    while (*s && n < 16) {
        int hi, lo;
        if (*s == '-') { s++; continue; }
        hi = Nibble(s[0]);
        lo = s[0] ? Nibble(s[1]) : -1;
        if (hi < 0 || lo < 0) return 0;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return n == 16 && *s == 0;
}

/* Kill switch for the native UI, on by default. */
static volatile int g_enabled = 1;

SH_API void ShUiEnable(int on) {
    g_enabled = on ? 1 : 0;
}

/* New defaults apply to widgets made after the call; the
 * asset must be loaded by the game at that point. */
SH_API int ShUiSetDefaultFont(const char *guid) {
    uint8_t g[16];
    if (!guid || !ParseGuid(guid, g)) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    Lock();
    memcpy(g_fontGuid, g, 16);
    g_ctx.fontAsset = 0;
    Unlock();
    return 1;
}

SH_API int ShUiSetDefaultImage(const char *guid) {
    uint8_t g[16];
    if (!guid || !ParseGuid(guid, g)) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    Lock();
    memcpy(g_imageGuid, g, 16);
    g_ctx.imageAsset = 0;
    Unlock();
    return 1;
}

/* scene 1 is the default, made on first use */
static int g_defaultSid = 0;

static int DefaultScene(void) {
    if (!g_defaultSid) g_defaultSid = ShSceneAlloc(0);
    return g_defaultSid;
}

/* assets, purge of dead widgets, then the scene */
static int Resolve(int sid) {
    uint64_t scene, root, rootPriv;
    DWORD t0 = GetTickCount();

    if (!g_enabled) { ShSetError(SH_ERR_NOT_IN_GAME); return 0; }
    if (!ShIsInGame()) { ShSetError(SH_ERR_NOT_IN_GAME); return 0; }
    if (ShGetGameStateHash() != HASH_PLAYING) {
        ShSetError(SH_ERR_UI_NOT_READY);
        return 0;
    }
    if (ShSceneLive(sid)) return 1;
    g_ctx.pool = RQ(G_POOL);
    FindAssets(&g_ctx.fontAsset, &g_ctx.imageAsset);
    if (!g_ctx.fontAsset || !g_ctx.imageAsset) {
        ShSetError(SH_ERR_UI_ASSET);
        return 0;
    }
    if (!g_ctx.pool) { ShSetError(SH_ERR_UI_NOT_READY); return 0; }
    if (HaveZombies() && !RunJob(OP_PURGE, &g_w[0])) return 0;
    if (!ShSceneEnsure(sid, &scene, &root, &rootPriv)) return 0;
    if (!Readable(rootPriv, 0x240) || RQ(root) != VT_CONTAINER) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    Log("resolve scene %d: %lu ms", sid,
        (unsigned long)(GetTickCount() - t0));
    return 1;
}

/* ---- game thread side ---- */

static uint64_t EAlloc(size_t size, size_t align) {
    uint64_t ctx = ((Fn3)F_ALLOC_CTX)(size, align, g_ctx.pool);
    uint64_t mem = ((Fn3)F_ALLOC)(size, align, ctx);
    if (mem) memset((void *)(uintptr_t)mem, 0, size);
    return mem;
}

/* Shrinking moves the counts before the pointer: the engine
 * walks these arrays every frame. Moot on the game thread,
 * kept because it documents the rule. */
static int ArrayAdd(uint64_t cont, uint64_t off, uint64_t h) {
    uint64_t n = RQ(cont + off + 8), old = RQ(cont + off + 16);
    uint64_t arr = EAlloc(8 * (n + 1), 8);
    if (!arr) return 0;
    if (n) memcpy((void *)(uintptr_t)arr, (void *)(uintptr_t)old, 8 * n);
    WQ(arr + 8 * n, h);
    WQ(cont + off + 16, arr);
    WQ(cont + off, n + 1);
    WQ(cont + off + 8, n + 1);
    ShSceneFree(old);
    return 1;
}

/* insert at a sibling index, clamped to the end */
static int ArrayInsert(uint64_t cont, uint64_t off, uint64_t h, int at) {
    uint64_t n = RQ(cont + off + 8), old = RQ(cont + off + 16);
    uint64_t arr, i, k = 0;
    if (at < 0 || (uint64_t)at > n) at = (int)n;
    arr = EAlloc(8 * (n + 1), 8);
    if (!arr) return 0;
    for (i = 0; i < n; i++) {
        if (i == (uint64_t)at) WQ(arr + 8 * k++, h);
        WQ(arr + 8 * k++, RQ(old + 8 * i));
    }
    if ((uint64_t)at == n) WQ(arr + 8 * k++, h);
    WQ(cont + off + 16, arr);
    WQ(cont + off, n + 1);
    WQ(cont + off + 8, n + 1);
    ShSceneFree(old);
    return 1;
}

static void ArrayRemove(uint64_t cont, uint64_t off, uint64_t h) {
    uint64_t n = RQ(cont + off + 8), old = RQ(cont + off + 16);
    uint64_t arr, i, k = 0;
    if (!n) return;
    arr = EAlloc(8 * n, 8);
    if (!arr) return;
    for (i = 0; i < n; i++) {
        uint64_t v = RQ(old + 8 * i);
        if (v != h) WQ(arr + 8 * k++, v);
    }
    WQ(cont + off, k);
    WQ(cont + off + 8, k);
    WQ(cont + off + 16, arr);
    ShSceneFree(old);
}

/* Run the handle's destructor without its delete, then free
 * handle and instance through the engine's pool free, the
 * way the scene dtor treats its own root. */
static void DestroyOne(uint64_t handle, uint64_t inst) {
    uint64_t vt = handle ? RQ(handle) : 0;
    if (vt) ((Fn2)RQ(vt))(handle, 0);
    ShSceneFree(handle);
    ShSceneFree(inst);
}

static void SetLocal(uint64_t priv, float x, float y) {
    WF(priv + P_LOCAL, x);     WF(priv + P_LOCAL + 4, y);
    WF(priv + P_LOCAL2, x);    WF(priv + P_LOCAL2 + 4, y);
}

static void Dirty(uint64_t priv, uint64_t parentPriv) {
    ((Fn2)F_DIRTY)(priv, 0x8021);
    if (parentPriv) ((Fn2)F_DIRTY)(parentPriv, 0x800);
}

static void SetColour(uint64_t handle, uint32_t rgb) {
    float c[4];
    c[0] = (float)((rgb >> 16) & 0xFF);
    c[1] = (float)((rgb >> 8) & 0xFF);
    c[2] = (float)(rgb & 0xFF);
    c[3] = 0.0f;
    ((Fn2)F_WIDGET_COLOUR)(handle, (uint64_t)(uintptr_t)c);
}

/* Shared string block: len, cap, refcount, chars. Refcount
 * starts at 0: the setter's copy adds the one reference the
 * label holds, and its release frees the block. */
static uint64_t MakeBlock(const char *text) {
    uint32_t len = (uint32_t)strlen(text);
    uint64_t blk = EAlloc(16 + len, 8);
    if (!blk) return 0;
    WD(blk, len); WD(blk + 4, len); WD(blk + 8, 0);
    memcpy((void *)(uintptr_t)(blk + 12), text, len + 1);
    return blk;
}

/* An asset reference: the object pointer and a zero flags
 * word, the shape the engine reads back. */
static void SetRef(uint64_t at, uint64_t asset) {
    WQ(at, asset);
    WQ(at + 8, 0);
}

static uint64_t SceneOf(const Widget *w, uint64_t *rootH, uint64_t *rootP) {
    uint64_t scene = 0, rh = 0, rp = 0;
    ShSceneEnsure((int)w->scene, &scene, &rh, &rp);
    if (rootH) *rootH = rh;
    if (rootP) *rootP = rp;
    return scene;
}

static int AttachChild(const Widget *w, uint64_t priv, uint64_t handle,
                       uint64_t parentPriv, float x, float y) {
    ((Fn2)F_ATTACH)(priv, SceneOf(w, NULL, NULL));
    if (!ArrayAdd(parentPriv, C_KIDS, handle)) return 0;
    if (!ArrayAdd(parentPriv, C_ORDER, handle)) return 0;
    SetLocal(priv, x, y);
    return 1;
}

static int JobContainer(Widget *w, uint64_t parentH, uint64_t parentP) {
    uint64_t h = EAlloc(0x28, 8), p, vt;
    if (!h) return 0;
    ((Fn1)F_CONT_CTOR)(h);
    p = RQ(h + 0x20);
    if (RQ(h) != VT_CONTAINER || RQ(p) != VT_CONT_PRIV) return 0;
    vt = RQ(p);
    ((Fn2)RQ(vt + 0x38))(p, RQ(parentP + P_GROUP));
    WQ(p + P_PARENT, parentH);
    ((Fn1)RQ(vt + 0x08))(p);
    if (!AttachChild(w, p, h, parentP, w->x, w->y)) return 0;
    Dirty(p, parentP);
    w->handle = h; w->priv = p;
    return 1;
}

static int JobImage(Widget *w, uint64_t parentH, uint64_t parentP,
                    uint64_t *outH, uint64_t *outP, uint64_t *outInst) {
    uint64_t inst = EAlloc(0x108, 8), h, p;
    int32_t err = -1;
    (void)parentH;
    if (!inst) return 0;
    *outInst = inst;
    ((Fn1)F_IINST_CTOR)(inst);
    SetRef(inst + II_IMAGEREF, g_ctx.imageAsset);
    h = 0;
    ((Fn4)F_IMAGE_CREATE)(inst, (uint64_t)(uintptr_t)&err,
                          (uint64_t)(uintptr_t)&h, parentP);
    if (err < 0 || !h) return 0;
    p = RQ(h + 0x20);
    ((Fn3)F_IMAGE_APPLY)(inst, (uint64_t)(uintptr_t)&err, p);
    if (!AttachChild(w, p, h, parentP, w->x, w->y)) return 0;
    WF(p + P_SIZE, w->w); WF(p + P_SIZE + 4, w->h);
    SetColour(h, w->rgb);
    ShPropSetF(h, SH_P_ALPHA, w->alpha);
    ShPropSetF(h, SH_P_ROTATION, 0.0f);
    Dirty(p, parentP);
    *outH = h; *outP = p;
    return 1;
}

static void SetText(Widget *w) {
    uint64_t blk = MakeBlock(w->text);
    uint64_t str[3];
    if (!blk) return;
    str[0] = RQ(w->priv + P_TEXT);
    str[1] = blk;
    str[2] = 5;
    ((Fn2)F_LABEL_TEXT)(w->handle, (uint64_t)(uintptr_t)str);
}

static int JobLabel(Widget *w, uint64_t parentH, uint64_t parentP) {
    uint64_t inst = EAlloc(0x120, 8), h, p;
    int32_t err = -1, code = 0;
    float size[2];
    (void)parentH;
    if (!inst) return 0;
    w->inst = inst;
    ((Fn1)F_LINST_CTOR)(inst);
    SetRef(inst + LI_FONTREF, g_ctx.fontAsset);
    h = 0;
    ((Fn4)F_LABEL_CREATE)(inst, (uint64_t)(uintptr_t)&err,
                          (uint64_t)(uintptr_t)&h, parentP);
    if (err < 0 || !h) return 0;
    p = RQ(h + 0x20);
    ((Fn3)F_LABEL_APPLY)(inst, (uint64_t)(uintptr_t)&err, p);
    if (!AttachChild(w, p, h, parentP, w->x, w->y)) return 0;
    size[0] = w->w; size[1] = w->h;
    ((Fn2)F_LABEL_SIZE)(h, (uint64_t)(uintptr_t)size);
    w->handle = h; w->priv = p;
    SetText(w);
    SetColour(h, w->rgb);
    ShPropSetF(h, SH_P_ALPHA, w->alpha);
    Dirty(p, parentP);
    WD(p + P_DIRTY, 0x80001FFF);
    ((Fn2)F_LABEL_UPDATE)(p, (uint64_t)(uintptr_t)&code);
    *(uint8_t *)(uintptr_t)(p + P_REGFLAG) = 1;
    ((Fn1)F_LABEL_REGIST)(p);
    return 1;
}

static void ParentOf(const Widget *w, uint64_t *h, uint64_t *p) {
    if (w->parent && g_w[w->parent - 1].alive) {
        *h = g_w[w->parent - 1].handle;
        *p = g_w[w->parent - 1].priv;
    } else {
        SceneOf(w, h, p);
    }
}

static void Detach(Widget *w, uint64_t parentP) {
    ArrayRemove(parentP, C_KIDS, w->handle);
    ArrayRemove(parentP, C_ORDER, w->handle);
    ((Fn2)F_ATTACH)(w->priv, 0);
    WQ(w->priv + P_PARENT, 0);
}

extern void ShSceneLock(uint64_t priv);
extern void ShSceneUnlock(uint64_t priv);

/* Effective alpha is own alpha x own shown x the shown of
 * every ancestor; applied down the whole subtree. */
static void CascadeAlpha(Widget *w, float inherit, uint64_t pp) {
    float a = w->alpha * w->shown * inherit;
    int i;

    ShPropSetF(w->handle, SH_P_ALPHA, a);
    if (w->kind == K_PANEL && w->plate) {
        ShPropSetF(w->plateHandle, SH_P_ALPHA, a);
        Dirty(w->plate, w->priv);
    }
    Dirty(w->priv, pp);
    for (i = 0; i < MAX_UI; i++) {
        Widget *k = &g_w[i];
        if (k->alive && k->parent == (uint32_t)(w - g_w) + 1)
            CascadeAlpha(k, w->shown * inherit, w->priv);
    }
}

/* Children first, then the plate, then the widget itself,
 * every handle and instance freed. */
static void DestroySubtree(Widget *w, uint64_t pp) {
    int i;

    for (i = 0; i < MAX_UI; i++) {
        Widget *k = &g_w[i];
        if (k->alive && k->parent == (uint32_t)(w - g_w) + 1) {
            DestroySubtree(k, w->priv);
            k->alive = 0;
        }
    }
    if (w->kind == K_PANEL && w->plate) {
        Widget plate;
        memset(&plate, 0, sizeof(plate));
        plate.handle = w->plateHandle; plate.priv = w->plate;
        Detach(&plate, w->priv);
        DestroyOne(w->plateHandle, w->plateInst);
    }
    Detach(w, pp);
    DestroyOne(w->handle, w->inst);
}

static uint64_t JobBody(int op, Widget *w);

/* Runs through the queue under the scene's own lock, so a
 * tick on another hook thread never sees a half edit. */
static uint64_t ApplyOp(int op, Widget *w);

/* purge and batch lock per scene inside; the rest lock the
   widget's own scene here */
static uint64_t __attribute__((ms_abi)) Job(uint64_t op, uint64_t arg,
                                            uint64_t b, uint64_t c) {
    Widget *w = (Widget *)(uintptr_t)arg;
    uint64_t scenePriv = 0, r;
    (void)b; (void)c;

    if (op != OP_PURGE && op != OP_BATCH && op != OP_TEXTURE)
        scenePriv = ShScenePriv((int)w->scene);
    ShSceneLock(scenePriv);
    r = JobBody((int)op, w);
    ShSceneUnlock(scenePriv);
    Log("job op %d on thread %lu -> %llu", (int)op,
        (unsigned long)GetCurrentThreadId(), (unsigned long long)r);
    return r;
}

static uint64_t JobBody(int op, Widget *w) {
    int i;

    if (op == OP_BATCH) {
        Batch *bt = (Batch *)(uintptr_t)w->scratch;
        uint64_t ok = 1;
        for (i = 0; i < bt->n; i++) {
            BOp *bo = &bt->ops[i];
            Widget *x = &g_w[bo->id - 1];
            uint64_t priv;
            if (!x->alive) continue;
            priv = ShScenePriv((int)x->scene);
            ShSceneLock(priv);
            x->scratch = (uint64_t)(uintptr_t)&bo->pc;
            if (!ApplyOp(bo->op, x)) ok = 0;
            x->scratch = 0;
            if (bo->op == OP_DESTROY) x->alive = 0;
            ShSceneUnlock(priv);
        }
        return ok;
    }
    if (op == OP_PURGE) {
        /* Dead scenes: their widgets are freed when the scene's
         * own destructor runs on recreate. Destroying them here
         * leaves freed handles in the parents' child arrays for
         * that destructor to walk, or double frees widgets the
         * engine already tore down with the world. Drop the
         * references only; the natives belong to the scene
         * teardown. */
        for (i = 0; i < MAX_UI; i++) {
            Widget *k = &g_w[i];
            if (k->zombie) memset(k, 0, sizeof(*k));
        }
        return 1;
    }
    return ApplyOp(op, w);
}

static uint64_t ApplyOp(int op, Widget *w) {
    uint64_t ph, pp;

    ParentOf(w, &ph, &pp);
    switch ((int)op) {
    case OP_PANEL: {
        Widget plate;
        if (!JobContainer(w, ph, pp)) return 0;
        memset(&plate, 0, sizeof(plate));
        plate.scene = w->scene;
        plate.w = w->w; plate.h = w->h;
        plate.rgb = w->rgb; plate.alpha = w->alpha;
        if (!JobImage(&plate, w->handle, w->priv,
                      &w->plateHandle, &w->plate, &w->plateInst)) return 0;
        return 1;
    }
    case OP_CONTAINER:
        return JobContainer(w, ph, pp);
    case OP_LABEL:
        return JobLabel(w, ph, pp);
    case OP_IMAGE:
        return JobImage(w, ph, pp, &w->handle, &w->priv, &w->inst);
    case OP_TEXT:
        SetText(w);
        Dirty(w->priv, pp);
        return 1;
    case OP_POS: {
        /* setter #01, plus the layout copy at +0xf0 */
        float v[3] = { w->x, w->y, 0.0f };
        ShPropSetV(w->handle, SH_P_POSITION, v, 3);
        SetLocal(w->priv, w->x, w->y);
        Dirty(w->priv, pp);
        return 1;
    }
    case OP_SIZE: {
        float s[2] = { w->w, w->h };
        if (w->kind == K_LABEL) {
            ShPropSetV(w->handle, SH_P_SIZE, s, 2);
        } else if (w->kind == K_CONTAINER) {
            ShPropSetV(w->handle, SH_P_CONTSIZE, s, 2);
        } else if (w->kind == K_PANEL) {
            /* images have no size property; +0x210 is it */
            WF(w->plate + P_SIZE, w->w); WF(w->plate + P_SIZE + 4, w->h);
            Dirty(w->plate, w->priv);
        } else {
            WF(w->priv + P_SIZE, w->w); WF(w->priv + P_SIZE + 4, w->h);
        }
        Dirty(w->priv, pp);
        return 1;
    }
    case OP_COLOUR:
        SetColour(w->kind == K_PANEL ? w->plateHandle : w->handle, w->rgb);
        Dirty(w->kind == K_PANEL ? w->plate : w->priv, w->priv);
        return 1;
    case OP_REPARENT: {
        /* scratch: new parent id in the low 32, index high */
        uint32_t np = (uint32_t)(w->scratch & 0xFFFFFFFFu);
        int at = (int)(w->scratch >> 32);
        uint64_t nh, npv;
        Detach(w, pp);
        w->parent = np;
        ParentOf(w, &nh, &npv);
        /* a panel's plate is its first child, keep it first */
        if (np && g_w[np - 1].kind == K_PANEL && at >= 0) at += 1;
        ((Fn2)F_ATTACH)(w->priv, SceneOf(w, NULL, NULL));
        if (!ArrayInsert(npv, C_KIDS, w->handle, at)) return 0;
        if (!ArrayInsert(npv, C_ORDER, w->handle, at)) return 0;
        WQ(w->priv + P_PARENT, nh);
        Dirty(w->priv, npv);
        ((Fn2)F_DIRTY)(pp, 0x800);
        return 1;
    }
    case OP_AUTOSIZE:
        if (w->kind != K_LABEL) return 0;
        ((Fn2)F_LABEL_FIXW)(w->priv, w->autoW ? 0 : 1);
        ((Fn2)F_LABEL_FIXH)(w->priv, w->autoH ? 0 : 1);
        Dirty(w->priv, pp);
        return 1;
    case OP_ALPHA:
        CascadeAlpha(w, 1.0f, pp);
        return 1;
    case OP_DESTROY:
        DestroySubtree(w, pp);
        ((Fn2)F_DIRTY)(pp, 0x800);
        return 1;
    case OP_PROP: {
        PropCall *pc = (PropCall *)(uintptr_t)w->scratch;
        uint64_t h = w->handle;
        switch (pc->op) {
        case 1: pc->ok = ShPropSetF(h, pc->prop, pc->f[0]); break;
        case 2: pc->ok = ShPropSetU(h, pc->prop, pc->u); break;
        case 3: pc->ok = ShPropSetV(h, pc->prop, pc->f, pc->n); break;
        case 4: pc->ok = ShPropSetS(h, pc->prop, pc->block); break;
        case 5: pc->ok = ShPropGetF(h, pc->prop, &pc->f[0]); break;
        case 6: pc->ok = ShPropGetU(h, pc->prop, &pc->u); break;
        case 7: pc->ok = ShPropGetV(h, pc->prop, pc->f, pc->n); break;
        case 8: pc->block = ShPropGetS(h, pc->prop); pc->ok = pc->block != 0; break;
        case 9: pc->ok = ShPropMeasure(h, &pc->f[0], &pc->f[1]); break;
        default: pc->ok = 0;
        }
        if (pc->ok && pc->op <= 4) Dirty(w->priv, pp);
        return pc->ok;
    }
    case OP_TEXTURE: {
        TexCall *tc = (TexCall *)(uintptr_t)w->scratch;
        uint64_t tex = EAlloc(0x90, 8), buf, dev = RQ(G_DEVICE);
        uint32_t pitch = 0;
        int y;
        typedef uint64_t (__attribute__((ms_abi)) *Fn6)(uint64_t, uint64_t,
            uint64_t, uint64_t, uint64_t, uint64_t);
        if (!tex || !dev) return 0;
        ((Fn1)F_TEX_CTOR)(tex);
        ((Fn6)F_TEX_CREATE)(dev, tex, (uint64_t)tc->w, (uint64_t)tc->h,
                            0, 0x80009);
        if (!RQ(tex + 0x88)) return 0;
        WD(tex + 0x10, 0);
        WD(tex + 0x58, (uint32_t)tc->w);
        WD(tex + 0x5C, (uint32_t)tc->h);
        buf = ((Fn3)F_TEX_MAP)(tex, (uint64_t)(uintptr_t)&pitch, 1);
        if (!buf || pitch < (uint32_t)tc->w * 4) return 0;
        for (y = 0; y < tc->h; y++)
            memcpy((void *)(uintptr_t)(buf + (uint64_t)y * pitch),
                   tc->rgba + (size_t)y * tc->stride, (size_t)tc->w * 4);
        ((Fn3)F_TEX_PUSH)(tex, buf, pitch);
        tc->obj = tex;
        return 1;
    }
    case OP_IMGSET: {
        TexCall *tc = (TexCall *)(uintptr_t)w->scratch;
        char name[32];
        uint64_t blk;
        float uv[2];
        snprintf(name, sizeof(name), "ptr_%llx", (unsigned long long)tc->obj);
        blk = MakeBlock(name);
        if (!blk || !ShPropSetS(w->handle, 0x2A, blk)) return 0;
        /* the quad maps the texture rotated 180 degrees */
        uv[0] = 1.0f; uv[1] = 1.0f;
        ((Fn2)F_IMG_UV0)(w->handle, (uint64_t)(uintptr_t)uv);
        uv[0] = 0.0f; uv[1] = 0.0f;
        ((Fn2)F_IMG_UV1)(w->handle, (uint64_t)(uintptr_t)uv);
        Dirty(w->priv, pp);
        return 1;
    }
    }
    return 0;
}

static int RunJob(int op, Widget *w) {
    uint64_t ret = 0;
    int waited = 0, queued;

    while (!ShQueueCall((uint64_t)(uintptr_t)Job, (uint64_t)op,
                        (uint64_t)(uintptr_t)w, 0, 0)) {
        if (++waited > JOB_WAIT_MS) {
            ShSetError(SH_ERR_HOOK_FAILED);
            Log("job op %d on thread %lu FAILED: the call queue "
                "never accepted it in %d ms", op,
                (unsigned long)GetCurrentThreadId(), JOB_WAIT_MS);
            return 0;
        }
        Sleep(1);
    }
    queued = waited;
    while (!ShQueueResult(&ret)) {
        if (++waited > JOB_WAIT_MS) {
            ShSetError(SH_ERR_HOOK_FAILED);
            Log("job op %d on thread %lu FAILED: no result in %d ms "
                "(%d ms of that waiting to queue)", op,
                (unsigned long)GetCurrentThreadId(), JOB_WAIT_MS,
                queued);
            return 0;
        }
        Sleep(1);
    }
    if (!ret) {
        ShSetError(SH_ERR_HOOK_FAILED);
        Log("job op %d on thread %lu FAILED: the job itself "
            "returned 0 after %d ms", op,
            (unsigned long)GetCurrentThreadId(), waited);
        return 0;
    }
    ShSetError(SH_OK);
    return 1;
}

/* ---- bookkeeping ---- */

static Widget *Slot(uint32_t *id) {
    int i;
    for (i = 0; i < MAX_UI; i++) {
        if (!g_w[i].alive && !g_w[i].zombie) {
            memset(&g_w[i], 0, sizeof(g_w[i]));
            g_w[i].gen = g_ctx.gen;
            *id = (uint32_t)i + 1;
            return &g_w[i];
        }
    }
    return NULL;
}

static Widget *Get(uint32_t id) {
    Widget *w;
    if (id == 0 || id > MAX_UI) { ShSetError(SH_ERR_BAD_ARG); return NULL; }
    w = &g_w[id - 1];
    if (!w->alive || w->gen != g_ctx.gen) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return NULL;
    }
    return w;
}

/* any container of the same scene, at any depth */
static int ValidParent(uint32_t scene, uint32_t parent) {
    if (parent == 0) return 1;
    if (Get(parent) == NULL) return 0;
    if (g_w[parent - 1].scene != scene) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    return g_w[parent - 1].kind == K_PANEL ||
           g_w[parent - 1].kind == K_CONTAINER;
}

/* The scene dies on reload; so does every widget in it. */
void ShUiOnEnterPlaying(void) {
    int i;
    Lock();
    Log("state change: gen %d to %d", g_ctx.gen, g_ctx.gen + 1);
    ShSceneInvalidate();
    /* The asset record's object pointer is rewritten by the
     * engine on every world reload. The cached offset to the
     * record is stable (it sits in the image's data section),
     * but the object it points to is not, so the resolved
     * handles must be cleared here for the next resolve to
     * re-verify and pick up the current object. */
    g_ctx.fontAsset = 0;
    g_ctx.imageAsset = 0;
    g_ctx.gen++;
    for (i = 0; i < MAX_UI; i++) {
        if (g_w[i].alive) g_w[i].zombie = 1;
        g_w[i].alive = 0;
    }
    for (i = 1; i <= MAX_SCENES; i++)
        if (g_resetFn[i]) g_resetDue[i] = 1;
    Unlock();
}

/* resets fire from the ShUiReady poller, world back */
static void FireResets(void) {
    int i;
    for (i = 1; i <= MAX_SCENES; i++) {
        ResetFn fn;
        void *user;
        if (!g_resetDue[i] || !g_resetFn[i]) { g_resetDue[i] = 0; continue; }
        if (!Resolve(i)) continue;
        g_resetDue[i] = 0;
        fn = g_resetFn[i]; user = g_resetUser[i];
        fn((uint32_t)i, user);
    }
}

static int HaveZombies(void) {
    int i;
    for (i = 0; i < MAX_UI; i++) if (g_w[i].zombie) return 1;
    return 0;
}

SH_API int ShUiReady(void) {
    int ok;
    Lock();
    ok = Resolve(DefaultScene());
    Unlock();
    if (ok) FireResets();
    return ok;
}

/* ---- scenes ---- */

SH_API uint32_t ShUiSceneCreate(const char *name, int order) {
    int sid = ShSceneAlloc(order);
    Log("scene create %s order %d id %d", name ? name : "", order, sid);
    return (uint32_t)sid;
}

SH_API int ShUiSceneSetOrder(uint32_t scene, int order) {
    return ShSceneSetOrder((int)scene, order);
}

SH_API int ShUiSceneShow(uint32_t scene, int visible) {
    return ShSceneShow((int)scene, visible);
}

/* every widget of the scene, then the scene itself */
SH_API int ShUiSceneDestroy(uint32_t scene) {
    int i, ok = 1;
    if (scene == 0 || scene > MAX_SCENES) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    Lock();
    if ((int)scene == g_defaultSid) g_defaultSid = 0;
    for (i = 0; i < MAX_UI; i++) {
        Widget *w = &g_w[i];
        if (w->alive && w->scene == scene && w->parent == 0) {
            if (!RunJob(OP_DESTROY, w)) ok = 0;
            w->alive = 0;
        }
    }
    for (i = 0; i < MAX_UI; i++)
        if (g_w[i].scene == scene) g_w[i].zombie = 0;
    g_resetFn[scene] = NULL;
    g_resetDue[scene] = 0;
    ok = ShSceneRelease((int)scene) && ok;
    Unlock();
    return ok;
}

SH_API int ShUiSetReset(uint32_t scene, void (*fn)(uint32_t, void *),
                        void *user) {
    if (scene == 0 || scene > MAX_SCENES) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    g_resetFn[scene] = fn;
    g_resetUser[scene] = user;
    return 1;
}

/* ---- batches ---- */

SH_API int ShUiBegin(void) {
    int i;
    if (t_batch) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    Lock();
    for (i = 0; i < MAX_BATCH; i++) {
        if (!g_batches[i].used) {
            g_batches[i].used = 1;
            g_batches[i].n = 0;
            SetBatch(&g_batches[i]);
            break;
        }
    }
    Unlock();
    if (!t_batch) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    return 1;
}

static int Record(int op, uint32_t id, const PropCall *pc) {
    BOp *bo;
    if (t_batch->n >= MAX_BOPS) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    bo = &t_batch->ops[t_batch->n++];
    bo->op = op; bo->id = id;
    if (pc) bo->pc = *pc; else memset(&bo->pc, 0, sizeof(bo->pc));
    return 1;
}

SH_API int ShUiAbort(void) {
    Batch *bt = t_batch;
    if (!bt) return 0;
    bt->used = 0;
    SetBatch(NULL);
    return 1;
}

static int RunBatch(Batch *bt) {
    Widget dummy;
    int ok;
    memset(&dummy, 0, sizeof(dummy));
    dummy.scratch = (uint64_t)(uintptr_t)bt;
    Lock();
    ok = bt->n ? RunJob(OP_BATCH, &dummy) : 1;
    bt->used = 0;
    Unlock();
    return ok;
}

SH_API int ShUiCommit(void) {
    Batch *bt = t_batch;
    if (!bt) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    SetBatch(NULL);
    return RunBatch(bt);
}

typedef struct {
    Batch *bt;
    void (*done)(int, void *);
    void *user;
} AsyncCommit;

static DWORD WINAPI CommitThread(LPVOID arg) {
    AsyncCommit *ac = (AsyncCommit *)arg;
    int ok = RunBatch(ac->bt);
    if (ac->done) ac->done(ok, ac->user);
    HeapFree(GetProcessHeap(), 0, ac);
    return 0;
}

SH_API int ShUiCommitAsync(void (*done)(int ok, void *user), void *user) {
    Batch *bt = t_batch;
    AsyncCommit *ac;
    HANDLE h;
    if (!bt) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    SetBatch(NULL);
    ac = (AsyncCommit *)HeapAlloc(GetProcessHeap(), 0, sizeof(*ac));
    if (!ac) { bt->used = 0; return 0; }
    ac->bt = bt; ac->done = done; ac->user = user;
    h = CreateThread(NULL, 0, CommitThread, ac, 0, NULL);
    if (!h) { HeapFree(GetProcessHeap(), 0, ac); bt->used = 0; return 0; }
    CloseHandle(h);
    return 1;
}

/* Bumps on every scene change; ids from an older value are
 * dead and must be created again. */
SH_API int ShUiGen(void) {
    return g_ctx.gen;
}

static uint32_t Create(uint32_t scene, int kind, int op, uint32_t parent,
                       float x, float y, float w, float h, uint32_t rgb,
                       float alpha, const char *text) {
    Widget *wd;
    uint32_t id = 0;

    /* These three used to return with no log, so a build
     * that stopped part way said nothing about why. */
    if (scene == 0 || scene > MAX_SCENES) {
        ShSetError(SH_ERR_BAD_ARG);
        Log("create kind %d REFUSED: bad scene %u", kind, scene);
        return 0;
    }
    Lock();
    if (!Resolve((int)scene)) {
        Log("create kind %d REFUSED: scene %u not resolved, err %d",
            kind, scene, ShLastError());
        Unlock();
        return 0;
    }
    if (!ValidParent(scene, parent)) {
        Log("create kind %d REFUSED: parent %u invalid, err %d",
            kind, parent, ShLastError());
        Unlock();
        return 0;
    }
    wd = Slot(&id);
    if (!wd) {
        int a = 0, z = 0, i;
        for (i = 0; i < MAX_UI; i++) {
            if (g_w[i].alive) a++;
            if (g_w[i].zombie) z++;
        }
        ShSetError(SH_ERR_NO_CANDIDATE);
        Log("create kind %d REFUSED: no slot, %d alive %d zombie of %d",
            kind, a, z, MAX_UI);
        Unlock();
        return 0;
    }
    wd->kind = kind; wd->parent = parent; wd->scene = scene;
    wd->x = x; wd->y = y; wd->w = w; wd->h = h;
    wd->rgb = rgb; wd->alpha = alpha; wd->shown = 1.0f;
    if (text) { strncpy(wd->text, text, MAX_TEXT - 1); }
    wd->alive = 1;
    if (!RunJob(op, wd)) { wd->alive = 0; id = 0; }
    Log("create kind %d parent %u -> id %u handle %llx priv %llx "
        "plate %llx gen %d", kind, parent, id,
        (unsigned long long)wd->handle, (unsigned long long)wd->priv,
        (unsigned long long)wd->plateHandle, g_ctx.gen);
    Unlock();
    return id;
}

SH_API uint32_t ShUiPanel(float x, float y, float w, float h,
                          uint32_t rgb, float alpha) {
    return Create((uint32_t)DefaultScene(), K_PANEL, OP_PANEL, 0,
                  x, y, w, h, rgb, alpha, NULL);
}

SH_API uint32_t ShUiLabel(uint32_t panel, float x, float y, float w,
                          float h, const char *text, uint32_t rgb) {
    if (!text) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    return Create((uint32_t)DefaultScene(), K_LABEL, OP_LABEL, panel,
                  x, y, w, h, rgb, 1.0f, text);
}

SH_API uint32_t ShUiImage(uint32_t panel, float x, float y, float w,
                          float h, uint32_t rgb, float alpha) {
    return Create((uint32_t)DefaultScene(), K_IMAGE, OP_IMAGE, panel,
                  x, y, w, h, rgb, alpha, NULL);
}

/* a widget of a class under any container of the scene */
SH_API uint32_t ShUiCreateIn(uint32_t scene, uint32_t parent, int cls,
                             float x, float y, float w, float h) {
    switch (cls) {
    case SH_W_CONTAINER:
        return Create(scene, K_CONTAINER, OP_CONTAINER, parent, x, y, w, h,
                      0xFFFFFF, 1.0f, NULL);
    case SH_W_LABEL:
        return Create(scene, K_LABEL, OP_LABEL, parent, x, y, w, h,
                      0xFFFFFF, 1.0f, " ");
    case SH_W_IMAGE:
        return Create(scene, K_IMAGE, OP_IMAGE, parent, x, y, w, h,
                      0xFFFFFF, 1.0f, NULL);
    case SH_W_PANEL:
        return Create(scene, K_PANEL, OP_PANEL, parent, x, y, w, h,
                      0x000000, 0.8f, NULL);
    }
    ShSetError(SH_ERR_BAD_ARG);
    return 0;
}

SH_API uint32_t ShUiCreate(uint32_t parent, int cls, float x, float y,
                           float w, float h) {
    return ShUiCreateIn((uint32_t)DefaultScene(), parent, cls, x, y, w, h);
}

/* in a batch the edit is recorded, not run */
static int Apply(uint32_t id, int op) {
    Widget *w;
    int ok;
    Lock();
    w = Get(id);
    if (!w) { Unlock(); return 0; }
    ok = t_batch ? Record(op, id, NULL) : RunJob(op, w);
    Unlock();
    return ok;
}

SH_API int ShUiSetText(uint32_t id, const char *text) {
    Widget *w;
    if (!text) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    Lock();
    w = Get(id);
    if (!w || w->kind != K_LABEL) { Unlock(); return 0; }
    strncpy(w->text, text, MAX_TEXT - 1);
    w->text[MAX_TEXT - 1] = 0;
    Unlock();
    return Apply(id, OP_TEXT);
}

SH_API int ShUiSetPos(uint32_t id, float x, float y) {
    Widget *w;
    Lock();
    w = Get(id);
    if (!w) { Unlock(); return 0; }
    w->x = x; w->y = y;
    Unlock();
    return Apply(id, OP_POS);
}

SH_API int ShUiSetSize(uint32_t id, float w, float h) {
    Widget *wd;
    Lock();
    wd = Get(id);
    if (!wd) { Unlock(); return 0; }
    wd->w = w; wd->h = h;
    Unlock();
    return Apply(id, OP_SIZE);
}

SH_API int ShUiSetColour(uint32_t id, uint32_t rgb) {
    Widget *w;
    Lock();
    w = Get(id);
    if (!w) { Unlock(); return 0; }
    w->rgb = rgb;
    Unlock();
    return Apply(id, OP_COLOUR);
}

SH_API int ShUiSetAlpha(uint32_t id, float alpha) {
    Widget *w;
    Lock();
    w = Get(id);
    if (!w) { Unlock(); return 0; }
    w->alpha = alpha;
    Unlock();
    return Apply(id, OP_ALPHA);
}

/* ---- tree ---- */

SH_API int ShUiReparent(uint32_t id, uint32_t parent, int index) {
    Widget *w;
    int ok;
    Lock();
    w = Get(id);
    if (!w) { Unlock(); return 0; }
    if (!ValidParent(w->scene, parent) || parent == id) {
        Unlock(); ShSetError(SH_ERR_BAD_ARG); return 0;
    }
    w->scratch = (uint64_t)parent | ((uint64_t)(uint32_t)index << 32);
    ok = RunJob(OP_REPARENT, w);
    w->scratch = 0;
    Unlock();
    return ok;
}

/* children in the engine's draw order */
static int Children(uint32_t id, uint32_t *out, int max) {
    Widget *w = Get(id);
    uint64_t n, arr, i;
    int k = 0, j;
    if (!w) return -1;
    n = RQ(w->priv + C_ORDER + 8);
    arr = RQ(w->priv + C_ORDER + 16);
    for (i = 0; i < n && k < max; i++) {
        uint64_t h = RQ(arr + 8 * i);
        for (j = 0; j < MAX_UI; j++) {
            if (g_w[j].alive && g_w[j].handle == h &&
                g_w[j].parent == id) {
                out[k++] = (uint32_t)j + 1;
                break;
            }
        }
    }
    return k;
}

SH_API int ShUiChildCount(uint32_t id) {
    uint32_t kids[MAX_UI];
    int n;
    Lock();
    n = Children(id, kids, MAX_UI);
    Unlock();
    return n < 0 ? 0 : n;
}

SH_API uint32_t ShUiChildAt(uint32_t id, int index) {
    uint32_t kids[MAX_UI];
    int n;
    Lock();
    n = Children(id, kids, MAX_UI);
    Unlock();
    if (n < 0 || index < 0 || index >= n) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    return kids[index];
}

SH_API int ShUiSetAutoSize(uint32_t id, int autoW, int autoH) {
    Widget *w;
    Lock();
    w = Get(id);
    if (!w || w->kind != K_LABEL) { Unlock(); ShSetError(SH_ERR_BAD_ARG); return 0; }
    w->autoW = autoW ? 1 : 0;
    w->autoH = autoH ? 1 : 0;
    Unlock();
    return Apply(id, OP_AUTOSIZE);
}

SH_API int ShUiShow(uint32_t id, int visible) {
    Widget *w;
    Lock();
    w = Get(id);
    if (!w) { Unlock(); return 0; }
    w->shown = visible ? 1.0f : 0.0f;
    Unlock();
    return Apply(id, OP_ALPHA);
}

/* ---- properties by id ---- */

static int PropJob(uint32_t id, PropCall *pc) {
    Widget *w;
    int ok;
    Lock();
    w = Get(id);
    if (!w) { Unlock(); return 0; }
    if (t_batch && pc->op <= 4) {
        ok = Record(OP_PROP, id, pc);
        Unlock();
        return ok;
    }
    w->scratch = (uint64_t)(uintptr_t)pc;
    ok = RunJob(OP_PROP, w) && pc->ok;
    w->scratch = 0;
    Unlock();
    if (!ok) ShSetError(SH_ERR_UI_PROP);
    return ok;
}

SH_API int ShUiPropType(uint32_t id, uint32_t prop) {
    Widget *w;
    int t;
    Lock();
    w = Get(id);
    t = w ? ShPropType(w->handle, prop) : 0;
    Unlock();
    return t;
}

SH_API int ShUiSetF(uint32_t id, uint32_t prop, float v) {
    PropCall pc; memset(&pc, 0, sizeof(pc));
    pc.op = 1; pc.prop = prop; pc.f[0] = v;
    return PropJob(id, &pc);
}

SH_API int ShUiSetU(uint32_t id, uint32_t prop, uint32_t v) {
    PropCall pc; memset(&pc, 0, sizeof(pc));
    pc.op = 2; pc.prop = prop; pc.u = v;
    return PropJob(id, &pc);
}

SH_API int ShUiSetV(uint32_t id, uint32_t prop, const float *v, int n) {
    PropCall pc; memset(&pc, 0, sizeof(pc));
    if (!v || n < 2 || n > 3) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    pc.op = 3; pc.prop = prop; pc.n = n;
    memcpy(pc.f, v, sizeof(float) * n);
    return PropJob(id, &pc);
}

SH_API int ShUiSetS(uint32_t id, uint32_t prop, const char *utf8) {
    PropCall pc; memset(&pc, 0, sizeof(pc));
    Widget *w;
    if (!utf8) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    Lock();
    w = Get(id);
    if (!w || !Resolve((int)w->scene)) { Unlock(); return 0; }
    pc.block = MakeBlock(utf8);
    Unlock();
    if (!pc.block) return 0;
    pc.op = 4; pc.prop = prop;
    return PropJob(id, &pc);
}

SH_API int ShUiGetF(uint32_t id, uint32_t prop, float *out) {
    PropCall pc; memset(&pc, 0, sizeof(pc));
    if (!out) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    pc.op = 5; pc.prop = prop;
    if (!PropJob(id, &pc)) return 0;
    *out = pc.f[0];
    return 1;
}

SH_API int ShUiGetU(uint32_t id, uint32_t prop, uint32_t *out) {
    PropCall pc; memset(&pc, 0, sizeof(pc));
    if (!out) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    pc.op = 6; pc.prop = prop;
    if (!PropJob(id, &pc)) return 0;
    *out = pc.u;
    return 1;
}

SH_API int ShUiGetV(uint32_t id, uint32_t prop, float *out, int n) {
    PropCall pc; memset(&pc, 0, sizeof(pc));
    if (!out || n < 2 || n > 3) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    pc.op = 7; pc.prop = prop; pc.n = n;
    if (!PropJob(id, &pc)) return 0;
    memcpy(out, pc.f, sizeof(float) * n);
    return 1;
}

SH_API int ShUiGetS(uint32_t id, uint32_t prop, char *out, int n) {
    PropCall pc; memset(&pc, 0, sizeof(pc));
    uint32_t len;
    if (!out || n < 1) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    pc.op = 8; pc.prop = prop;
    if (!PropJob(id, &pc)) return 0;
    if (!Readable(pc.block, 12)) return 0;
    memcpy(&len, (void *)(uintptr_t)pc.block, 4);
    if ((int)len > n - 1) len = (uint32_t)(n - 1);
    if (!Readable(pc.block + 12, len)) return 0;
    memcpy(out, (void *)(uintptr_t)(pc.block + 12), len);
    out[len] = 0;
    return 1;
}

SH_API int ShUiMeasure(uint32_t id, float *w, float *h) {
    PropCall pc; memset(&pc, 0, sizeof(pc));
    if (!w || !h) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    pc.op = 9;
    if (!PropJob(id, &pc)) return 0;
    *w = pc.f[0]; *h = pc.f[1];
    return 1;
}

/* ---- textures ---- */

SH_API uint32_t ShUiTextureCreate(int w, int h, const uint8_t *rgba,
                                  int stride) {
    TexCall tc;
    Widget dummy;
    uint32_t id = 0;
    int i;

    if (w < 1 || h < 1 || w > 4096 || h > 4096 || !rgba || stride < w * 4) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    Lock();
    if (!Resolve(DefaultScene())) { Unlock(); return 0; }
    for (i = 0; i < MAX_TEX; i++) if (!g_tex[i].alive) { id = (uint32_t)(i + 1); break; }
    if (!id) { Unlock(); ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    memset(&tc, 0, sizeof(tc));
    tc.rgba = rgba; tc.w = w; tc.h = h; tc.stride = stride;
    memset(&dummy, 0, sizeof(dummy));
    dummy.scratch = (uint64_t)(uintptr_t)&tc;
    if (!RunJob(OP_TEXTURE, &dummy) || !tc.obj) {
        Unlock();
        ShSetError(SH_ERR_HOOK_FAILED);
        return 0;
    }
    g_tex[id - 1].obj = tc.obj;
    g_tex[id - 1].w = w; g_tex[id - 1].h = h;
    g_tex[id - 1].alive = 1;
    Log("texture %u obj %llx %dx%d", id, (unsigned long long)tc.obj, w, h);
    Unlock();
    return id;
}

/* Leak probe: counts for the 5s overlay heartbeat (see ovl).  Callers
 * are on the render thread; the UI lock is held only for the scan, so a
 * long-running session can watch whether widgets (alive/zombie) or
 * engine textures (alive, never recycled - see ShUiTextureCreate) grow. */
int ShUiLeakProbe(int *widgets, int *zombies, int *textures) {
    int i, w = 0, z = 0, t = 0;
    Lock();
    for (i = 0; i < MAX_UI; i++) {
        if (g_w[i].alive) w++;
        if (g_w[i].zombie) z++;
    }
    for (i = 0; i < MAX_TEX; i++)
        if (g_tex[i].alive) t++;
    Unlock();
    if (widgets) *widgets = w;
    if (zombies) *zombies = z;
    if (textures) *textures = t;
    return 0;
}

/* Shows a texture on an image widget or a panel's plate. */
SH_API int ShUiImageSet(uint32_t id, uint32_t texture) {
    TexCall tc;
    Widget *w, plate;
    int ok;

    if (!texture || texture > MAX_TEX || !g_tex[texture - 1].alive) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    Lock();
    w = Get(id);
    if (!w || (w->kind != K_IMAGE && w->kind != K_PANEL)) {
        Unlock();
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    memset(&tc, 0, sizeof(tc));
    tc.obj = g_tex[texture - 1].obj;
    if (w->kind == K_PANEL) {
        memset(&plate, 0, sizeof(plate));
        plate.kind = K_IMAGE; plate.parent = id; plate.alive = 1;
        plate.handle = w->plateHandle; plate.priv = w->plate;
        plate.scratch = (uint64_t)(uintptr_t)&tc;
        ok = RunJob(OP_IMGSET, &plate);
    } else {
        w->scratch = (uint64_t)(uintptr_t)&tc;
        ok = RunJob(OP_IMGSET, w);
        w->scratch = 0;
    }
    Unlock();
    if (!ok) ShSetError(SH_ERR_HOOK_FAILED);
    return ok;
}

/* ---- any widget, by engine handle ---- */

/* The calls above address widgets we made, by id. These
 * address any widget the engine has, including the game's
 * own, so a plugin can read the UI it is looking at. */
/* Read only on purpose: writing into the game's tree is
 * how you corrupt a scene you do not own. */
extern int ShPropClassOf(uint64_t handle, char *out, int n);

/* A widget handle is [vtable, ..., private at +0x20], and
 * the private carries the child array. */
static uint64_t PrivOf(uint64_t widget) {
    if (!Sane(widget) || !Readable(widget, 0x28)) return 0;
    return RQ(widget + 0x20);
}

SH_API int ShWidgetChildCount(uint64_t widget) {
    uint64_t priv = PrivOf(widget), n;
    if (!priv || !Readable(priv + C_ORDER + 16, 8)) return 0;
    n = RQ(priv + C_ORDER + 8);
    return (n > 4096) ? 0 : (int)n;
}

SH_API uint64_t ShWidgetChildAt(uint64_t widget, int i) {
    uint64_t priv = PrivOf(widget), n, arr;
    if (!priv || i < 0 || !Readable(priv + C_ORDER + 16, 8)) return 0;
    n = RQ(priv + C_ORDER + 8);
    if (n > 4096 || (uint64_t)i >= n) return 0;
    arr = RQ(priv + C_ORDER + 16);
    return arr ? RQ(arr + 8 * (uint64_t)i) : 0;
}

/* "LabelWidget", "ImageWidget", "Container" and so on. */
SH_API int ShWidgetClass(uint64_t widget, char *out, int n) {
    if (!out || n < 2 || !Sane(widget)) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    return ShPropClassOf(widget, out, n);
}

SH_API int ShWidgetPropType(uint64_t widget, uint32_t prop) {
    if (!Sane(widget)) return 0;
    return ShPropType(widget, prop);
}

SH_API int ShWidgetGetF(uint64_t widget, uint32_t prop, float *out) {
    if (!out || !Sane(widget)) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    return ShPropGetF(widget, prop, out);
}

SH_API int ShWidgetGetU(uint64_t widget, uint32_t prop, uint32_t *out) {
    if (!out || !Sane(widget)) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    return ShPropGetU(widget, prop, out);
}

SH_API int ShWidgetGetV(uint64_t widget, uint32_t prop, float *out,
                        int n) {
    if (!out || n < 2 || n > 3 || !Sane(widget)) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    return ShPropGetV(widget, prop, out, n);
}

/* The block is {len, cap, refcount, chars}, the same shape
 * the label setter builds. */
SH_API int ShWidgetGetS(uint64_t widget, uint32_t prop, char *out,
                        int n) {
    uint64_t block;
    uint32_t len;

    if (!out || n < 1 || !Sane(widget)) {
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    out[0] = 0;
    block = ShPropGetS(widget, prop);
    if (!block || !Readable(block, 12)) return 0;
    memcpy(&len, (void *)(uintptr_t)block, 4);
    if ((int)len > n - 1) len = (uint32_t)(n - 1);
    if (!Readable(block + 12, len)) return 0;
    memcpy(out, (void *)(uintptr_t)(block + 12), len);
    out[len] = 0;
    return 1;
}

/* The engine handle behind one of our own ids, so the two
 * halves of the API meet. */
SH_API uint64_t ShUiHandle(uint32_t id) {
    Widget *w;
    uint64_t h;
    Lock();
    w = Get(id);
    h = w ? w->handle : 0;
    Unlock();
    return h;
}

SH_API int ShUiPropCount(void) {
    return ShPropCount();
}

extern void ShPropStats(int *sections, uint64_t *bytes, int *records);

SH_API int ShUiPropStats(int *sections, uint64_t *bytes) {
    int records;
    if (!sections || !bytes) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    ShPropCount();
    ShPropStats(sections, bytes, &records);
    return records;
}

SH_API int ShUiPropAt(int i, char *cls, int n, uint32_t *prop, int *type) {
    uint64_t set, get;
    if (!cls || !prop || !type || n < 2) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    return ShPropAt(i, cls, n, prop, type, &set, &get);
}

SH_API int ShUiDestroy(uint32_t id) {
    Widget *w;
    int ok;
    Lock();
    w = Get(id);
    if (!w) { Unlock(); return 0; }
    Log("destroy id %u kind %d handle %llx", id, w->kind,
        (unsigned long long)w->handle);
    if (t_batch) {
        ok = Record(OP_DESTROY, id, NULL);
    } else {
        ok = RunJob(OP_DESTROY, w);
        w->alive = 0;
    }
    Unlock();
    return ok;
}
