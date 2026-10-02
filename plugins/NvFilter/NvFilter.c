/* NvFilter - force-replace the look of the in-game night vision filter.
 *
 * WHAT THE ENGINE DOES
 * --------------------
 * Night vision is drawn by one dedicated pixel shader in the HDR lighting pass,
 * HDRLighting_NV_ps, and that shader picks its look from an integer in constant
 * buffer 5, cb5[13].y, which the engine derives from the headgear being worn:
 *
 *   0   monochrome, green-boosted by (0.7, 1.3, 0.7), over a blurred base.
 *       Every ordinary set of goggles lands here, and it is what "night vision"
 *       looks like out of the box.
 *   1   monochrome, tinted by a near-neutral (0.95, 1.05, 0.98), over the sharp
 *       scene. The Splinter Cell set - the one the game calls the Sonar Goggles
 *       - lands here. Confirmed on screen.
 *   2   monochrome, run through a two-term ramp that is yellow where the scene
 *       is bright and green where it is dark. The "panoramic" and "special" NVG
 *       helmets land here. Confirmed on screen.
 *
 * So mode 0 is "no gear in particular", and modes 1 and 2 are the two pieces of
 * headgear that were given a look of their own. Nothing else in the game
 * selects a look, which is why only three exist.
 *
 * THE TWO ROWS
 * ------------
 *   "Night vision filter replace"    off / the ordinary goggles only / everything
 *   "Night vision filter"            black and white / yellow-green / sepia
 *
 * The replace row comes first and is the master switch: OFF is the only state in
 * which the plugin does nothing at all, and it is what restores the game's own
 * look - by substituting nothing, which leaves mode 0 as the engine drew it.
 * That split is the whole reason for the first row, and it is worth spelling
 * out because this plugin was restructured to get there: when "green" doubled
 * as both "the game's own look" and "do nothing", choosing it with "everything"
 * quietly did nothing at all - a reach that could not reach. The look row now
 * offers only looks, and "the game's own look" left it for the switch above,
 * where it is unambiguous.
 *
 * Two of the looks are arithmetic - a tint and a ramp. Sepia is neither: it is
 * the game's own Photo Mode "sepia" grade, which exists in the archives as a
 * 32^3 look-up cube. Night vision works on one scalar, so only that cube's grey
 * axis can ever reach the screen, and the axis is what nv_looks.hlsl tabulates.
 * More of those grades can be added the same way; see
 * .codebuddy/plans/nightvision-more-looks_3f7c21d0.md.
 *
 * Dropping it also removed machinery: a shader variant that folded mode 0 onto
 * itself, and with it a transcription of mode 0's branch, which does not draw
 * the scene colour at all - it draws a blurred luminance. Nothing reaches that
 * branch now, so nothing carries it.
 *
 * HOW FAR THE REPLACE REACHES IS VISIBLE IN THE BUILD
 * ---------------------------------------------------
 * The scope "everything" variants do not read cb5[13].y at all - the compiler
 * folds it to a constant - which is why they are smaller than their "ordinary
 * goggles only" twins. See the shader's own header.
 *
 * WHY A SUBSTITUTION AND NOT A DATA PATCH
 * ---------------------------------------
 * Every archive was searched: there is no HDRLighting resource to edit, and the
 * night-vision headgear records carry no filter field - nine goggles were
 * compared byte for byte and their resource reference sets are identical. The
 * look lives in the shader, so the shader is where it has to be changed.
 * docs and the full trail: .codebuddy/plans/nightvision-filter-feasibility_4c81ab07.md
 *
 * The rewritten shader is plugins/NvFilter/nv_filter_ps.hlsl; every combination
 * above is one compile of it, and the compiled bytes live in the generated
 * nv_filters.h.
 *
 * HOW THE SUBSTITUTION IS DONE
 * ----------------------------
 *   - MinHook the two d3d11.dll exports a process creates its device through,
 *     to hold the ID3D11Device and its immediate context.
 *   - Hook that device's CreatePixelShader, and keep it hooked: this is a cold
 *     path (a few thousand calls in a whole session). When the bytecode being
 *     created is the night-vision shader - recognised by its length and a
 *     CRC32 of its bytes, length first because only a blob of the right size is
 *     worth hashing - build one shader per combination through the same
 *     function and remember the engine's object.
 *   - Hook that context's PSSetShader. This one is on a hot path and is
 *     treated as such: the hook is created but left DISABLED, and is only
 *     enabled while a replacement is actually selected. Enabling and disabling
 *     happens on the plugin's own monitor thread, never inside a hook and
 *     never from a menu callback.
 *
 * WHAT THE HOT PATH COSTS
 * -----------------------
 * PSSetShader is called by the engine far more often than it binds night
 * vision: the NvProbe timelines put the total at upwards of 25k calls a second,
 * or 400-500 per frame. So the hook body is one load of the replacement
 * pointer and one compare - nothing else on the path that does not match, no
 * lock, no call out. Measured on this machine with the same hook shape
 * (out/nightvision/_work/bench): 0.62-0.66 ns per call over an unhooked call,
 * which at 30k calls/s is 0.019 ms of CPU per second, or about 320 ns out of
 * the 16.7 ms a 60 fps frame has - 0.002 per cent of a frame.
 *
 * And while the replace row is off - the default - the hook is not installed at
 * all, so the cost is not "small", it is zero. The same benchmark confirms the
 * disarm puts a call back within noise of never having been hooked (-0.25 ns,
 * i.e. unmeasurable).
 *
 * The rest of the work happens once, when the shader is compiled: start up,
 * whether or not night vision is ever switched on. There is no per-frame work
 * of any kind and no timer that reads the menu.
 *
 * WHAT IT DOES NOT DO
 * -------------------
 * It never writes to a game buffer, never touches a texture, never patches
 * engine code, never installs a breakpoint and never hooks the swapchain. If
 * the shader hooks fail to install, or a combination will not build, the log
 * says so and the game is left exactly as it was.
 *
 * A NOTE ON THE FINGERPRINT
 * -------------------------
 * len 4864 / crc B4230EAA was measured in two separate sessions, so it is
 * stable for this build of the game. A game patch that recompiles the shader
 * will change it; when that happens, put the new pair in NvFilter.ini
 * (target_len / target_crc) rather than rebuilding this plugin. If the pair
 * does not match anything, the log says so and nothing is substituted - a miss
 * is silent on screen but never invisible in the log.
 *
 * ADDING A LOOK
 * -------------
 * Four places, and three of them are lists that have to agree: the look list in
 * gen_looks.py's LOOKS (which generates nv_looks.hlsl), the VARIANTS list in
 * tools/embed_nv_filters.py (which compiles them and writes the header), one row
 * in kLooks below, and its label in lang.ini. Then
 *
 *   python tools/embed_nv_filters.py --build
 *
 * Which is one command because embed_nv_filters.py owns both the compiles and
 * the header. check_looks.py compares all four lists and refuses a drift; run it
 * after touching any of them. Nothing else changes: rows are matched to the
 * generated table by name, and a variant that no pair of rows offers is reported
 * in the log rather than silently compiled in.
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

#include "nv_filters.h"

#define LOG_NAME "NvFilter.log"
#define INI_NAME "NvFilter.ini"

/* The engine's HDRLighting_NV_ps in this build. Overridable from the ini so a
 * game patch does not need this plugin rebuilt. */
#define DEFAULT_TARGET_LEN 4864u
#define DEFAULT_TARGET_CRC 0xB4230EAAu

/* ---- the two rows ------------------------------------------------------ */

/* Row one, the master switch first and the look second, in the order the menu
 * offers them. Each key is what NvFilter.ini stores and how a row is matched to
 * the generated shader table. */
typedef struct Row {
    const char *key;
    const char *label;          /* lang key */
} Row;

/* The look. All of them are substitutions; leaving the engine's own look alone
 * is what the replace row's OFF does, so there is no "green" row here.
 *
 * Two families, and the order below is the order the row presents them in:
 * the two the framework can draw as arithmetic (a tint and a ramp, modes 1 and
 * 2 in the shader), then the twelve grades the game itself ships for Photo
 * Mode - each of those is a 32^3 look-up cube in the archives, reduced to its
 * grey axis because night vision works on one scalar - then three more that are
 * arithmetic (amber, cool blue, high contrast). The names are ours: the assets
 * are codenamed (BURNER, LIGHTHOUSE, ...) and a menu wants what it looks like.
 *
 * Seventeen is past what a single left/right row is comfortable with - the
 * value column holds about five CJK characters, and reaching the last entry
 * takes eight presses. It works, and every step changes the screen immediately,
 * but this is the row that will want a second level (family, then look) before
 * much else is added. */
static const Row kLooks[] = {
    { "black-and-white", "@nvf.bw"       },   /* 1  engine, as arithmetic  */
    { "yellow-green",    "@nvf.yg"       },   /* 2                         */
    { "sepia",           "@nvf.sepia"    },   /* 3  Photo Mode cubes       */
    { "apollo",          "@nvf.apollo"   },   /* 4                         */
    { "lighthouse",      "@nvf.lighthouse" }, /* 5                         */
    { "tennessee",       "@nvf.tennessee" },  /* 6                         */
    { "arlington",       "@nvf.arlington" },  /* 7                         */
    { "bridge",          "@nvf.bridge"   },   /* 8                         */
    { "montenegro",      "@nvf.montenegro" }, /* 9                         */
    { "chief",           "@nvf.chief"    },   /* 10                        */
    { "songbird",        "@nvf.songbird" },   /* 11                        */
    { "madre",           "@nvf.madre"    },   /* 12                        */
    { "burner",          "@nvf.burner"   },   /* 13                        */
    { "neutral",         "@nvf.neutral"  },   /* 14                        */
    { "amber",           "@nvf.amber"    },   /* 15 arithmetic             */
    { "cool-blue",       "@nvf.cool"     },   /* 16                        */
    { "contrast",        "@nvf.contrast" }    /* 17                        */
};
#define LOOK_N ((int)(sizeof(kLooks) / sizeof(kLooks[0])))

/* How far the replacement reaches. "off" matches no shader and is the only
 * state in which nothing is substituted. */
static const Row kReplaces[] = {
    { "off",     "@nvf.off"            },
    { "default", "@nvf.replace.normal" },
    { "all",     "@nvf.replace.all"    }
};
#define REPLACE_N ((int)(sizeof(kReplaces) / sizeof(kReplaces[0])))

/* Keep the option labels short. A row's value column is drawn as "< option >"
 * right-aligned into 130 px at 1080p (VALUE_W in scripthook_ovl.cpp), which is
 * about five CJK characters; past that the text grows leftwards over the row's
 * own name. That pixel width is the limit that binds here. The framework's byte
 * ceiling - ShMenuRow.value in scripthook.h - was 48 when these labels were
 * written and is 96 now, and the capture cuts on a character boundary rather
 * than mid-character, so a label past it comes out shorter instead of broken;
 * neither of those helps the column, which is why this says
 * "仅替换默认(特殊装备保留)" rather than spelling the caveat out in full. */

/* ShMenuList wants mutable arrays of labels that outlive the menus, and it
 * stores the pointers, so they cannot be built on the stack. */
static const char *kLookLabel[LOOK_N];
static const char *kReplaceLabel[REPLACE_N];

static volatile PVOID g_dev;
static ID3D11DeviceContext *g_ctx;

/* The hot path reads exactly one of these: g_repl, NULL while there is nothing
 * to do. g_src is the engine's shader object - the one the hook compares
 * against - and it is only ever non-NULL once g_ourPs is complete. */
static volatile PVOID g_src;                     /* the engine's shader      */
static volatile PVOID g_repl;                    /* hand the engine this     */
static PVOID g_ourPs[LOOK_N][REPLACE_N];
static void *g_fnSet;                            /* PSSetShader, for arming  */
static volatile LONG  g_hookOn;                  /* the hook's current state */
static int   g_armFailed;                        /* said so once, do not nag */
static int   g_lookOf[NV_FILTER_SHADERS];        /* variant -> row           */
static int   g_replaceOf[NV_FILTER_SHADERS];
static volatile PVOID g_claim;                   /* the variants exist       */
static volatile LONG  g_look;                    /* the selected rows        */
static volatile LONG  g_replace;
static volatile LONG  g_armedLook = -1;          /* what g_repl is, for the  */
static volatile LONG  g_armedRep  = -1;          /* log and for Explain()    */
static volatile LONG  g_subDone;                 /* the first substitution   */
static volatile LONG  g_ninst;                   /* bound with class instances */
static volatile LONG  g_created;                 /* pixel shaders seen       */
static volatile LONG  g_announced;

static char     g_owner[64];                    /* the plugin's own name */
static char     g_iniPath[MAX_PATH];
static uint32_t g_targetLen = DEFAULT_TARGET_LEN;
static uint32_t g_targetCrc = DEFAULT_TARGET_CRC;
static uint32_t g_menu;
static uint64_t g_startMs;

static void *g_hookedDev;
static void *g_hookedDevSc;

/* ---- crc32 ------------------------------------------------------------- */

static uint32_t g_crcTable[256];

static void CrcInit(void) {
    uint32_t i, j, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crcTable[i] = c;
    }
}

static uint32_t Crc32(const unsigned char *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    for (i = 0; i < n; i++) c = g_crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---- hex decode -------------------------------------------------------- */

static int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The compiled shaders are carried as hex so they stay reviewable in a diff.
 * Returns the byte count, or -1 if the text is malformed or too long. */
static int DecodeHex(const char *src, unsigned char *out, int cap) {
    int n = 0;
    while (src[0] && src[1]) {
        int hi = HexVal(src[0]), lo = HexVal(src[1]);
        if (hi < 0 || lo < 0) return -1;
        if (n >= cap) return -1;
        out[n++] = (unsigned char)((hi << 4) | lo);
        src += 2;
    }
    return n;
}

/* ---- the plugin's own name --------------------------------------------- */

/* From the module path, like every other plugin here: the ini, the log and the
 * text owner all follow the file name. */
static void NameFromModule(HINSTANCE inst) {
    char mod[MAX_PATH];
    char *base, *dot;
    size_t n;

    g_owner[0] = 0;
    if (!inst || !GetModuleFileNameA(inst, mod, sizeof mod)) return;
    base = strrchr(mod, '\\');
    base = base ? base + 1 : mod;
    dot = strrchr(base, '.');
    n = dot ? (size_t)(dot - base) : strlen(base);
    if (n == 0 || n >= sizeof g_owner) { g_owner[0] = 0; return; }
    memcpy(g_owner, base, n);
    g_owner[n] = 0;
}

/* ---- hooks ------------------------------------------------------------- */

typedef HRESULT (STDMETHODCALLTYPE *CreatePsFn)(
    ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *,
    ID3D11PixelShader **);
typedef void (STDMETHODCALLTYPE *SetPsFn)(
    ID3D11DeviceContext *, ID3D11PixelShader *,
    const ID3D11ClassInstance *const *, UINT);

static CreatePsFn g_origCreatePs;
static SetPsFn    g_origSetPs;

/* Turn one embedded variant into a shader through the device's own
 * CreatePixelShader, so the result is exactly as legitimate as the engine's. */
static PVOID CreateVariant(ID3D11Device *dev, const NvFilterShader *v) {
    unsigned char *blob;
    ID3D11PixelShader *ps = NULL;
    HRESULT hr;
    int n;

    blob = (unsigned char *)malloc(v->bytes);
    if (!blob) {
        Log("%s/%s: out of memory decoding it", v->look, v->reach);
        return NULL;
    }
    n = DecodeHex(v->hex, blob, (int)v->bytes);
    if (n != (int)v->bytes ||
        blob[0] != 'D' || blob[1] != 'X' || blob[2] != 'B' || blob[3] != 'C') {
        Log("%s/%s: blob is malformed (%d bytes decoded, wanted %u) - re-run "
            "tools/embed_nv_filters.py --build", v->look, v->reach, n, v->bytes);
        free(blob);
        return NULL;
    }
    /* The header carries the CRC of the file it was generated from, so a
     * header that has drifted from its source is caught here rather than
     * showing up as a strange picture. */
    if (Crc32(blob, v->bytes) != v->crc) {
        Log("%s/%s: the embedded bytes do not match the CRC in the header "
            "(%08X) - re-run tools/embed_nv_filters.py --build",
            v->look, v->reach, (unsigned)v->crc);
        free(blob);
        return NULL;
    }

    hr = g_origCreatePs(dev, blob, (SIZE_T)v->bytes, NULL, &ps);
    free(blob);
    if (hr != S_OK || !ps) {
        Log("%s/%s: the device refused it (hr 0x%08X)",
            v->look, v->reach, (unsigned)hr);
        return NULL;
    }
    return ps;
}

/* Build every combination and remember the engine's shader. Runs once, on
 * whatever thread compiled it. */
static void BuildVariants(ID3D11Device *dev, PVOID game) {
    int i, built = 0;

    if (InterlockedCompareExchangePointer(&g_claim, game, NULL) != NULL)
        return;                          /* another caller got there first */

    for (i = 0; i < NV_FILTER_SHADERS; i++) {
        int look = g_lookOf[i], rep = g_replaceOf[i];
        if (look < 0 || rep < 0) {
            Log("variant %s/%s is compiled in but no pair of rows offers it - "
                "add the missing row", kNvFilterShaders[i].look,
                kNvFilterShaders[i].reach);
            continue;
        }
        g_ourPs[look][rep] = CreateVariant(dev, &kNvFilterShaders[i]);
        if (g_ourPs[look][rep]) built++;
    }

    /* Published last: the hot path only looks at this, and by the time it is
     * set g_ourPs is complete. Arming the hook is the monitor thread's job -
     * this runs on the render thread, in the middle of the engine creating a
     * shader, which is no place to be suspending every other thread. */
    InterlockedExchangePointer(&g_src, game);

    Log("night vision pixel shader found (len %u crc %08X) - %d of %d "
        "filter(s) ready; replace \"%s\", look \"%s\"",
        (unsigned)g_targetLen, (unsigned)g_targetCrc, built, NV_FILTER_SHADERS,
        kReplaces[g_replace].key, kLooks[g_look].key);
}

/* The engine compiled the shader this plugin replaces.
 *
 * The first time, build the variants. Any later time - the engine recompiling
 * the pass after a quality change or a level load, and getting a different
 * object for the same bytes - keep the variants, because they do not belong to
 * that object, and point the substitution at the new one: the engine will bind
 * the new one from here on. One object is tracked, not a set, because the hot
 * path compares against one pointer and pays for every pointer it compares. */
static void AdoptTarget(ID3D11Device *dev, PVOID game) {
    PVOID old;

    if (g_claim == NULL) {
        BuildVariants(dev, game);
        return;
    }
    old = (PVOID)g_src;                   /* still being built, nothing to do */
    if (old == NULL || old == game) return;
    InterlockedExchangePointer(&g_src, game);
    Log("the engine compiled the night-vision shader again (%p -> %p); the "
        "substitution now watches the new one", old, game);
}

static HRESULT STDMETHODCALLTYPE HookCreatePs(
        ID3D11Device *dev, const void *code, SIZE_T len,
        ID3D11ClassLinkage *linkage, ID3D11PixelShader **out) {
    HRESULT hr = g_origCreatePs(dev, code, len, linkage, out);

    if (hr == S_OK && out && *out && code && len) {
        InterlockedIncrement(&g_created);
        /* Length first, and compared at full width: it is a register compare,
         * and only a blob of exactly the right size is worth a CRC32 over
         * every byte of it. The cast matters - truncating len to 32 bits
         * could make 0x1_0000_0000 bytes look like a 4864-byte blob and put a
         * 4 GB hash on this cold path. */
        if (len == (SIZE_T)g_targetLen &&
            Crc32((const unsigned char *)code, (size_t)len) == g_targetCrc)
            AdoptTarget(dev, *out);
    }
    return hr;
}

/* Hot path, and it runs for every PSSetShader call in the game whether or not
 * it has anything to do with night vision - measured at upwards of 25k/s, so
 * it is written to be one load and one compare in the case that matters.
 *
 * Nothing here takes a lock, allocates, or calls out. The hook is not even
 * installed while the replace row is off (see ApplyArm), so the common
 * configuration costs this process exactly nothing. */
static void STDMETHODCALLTYPE HookSetPs(
        ID3D11DeviceContext *ctx, ID3D11PixelShader *ps,
        const ID3D11ClassInstance *const *inst, UINT n) {
    PVOID repl = (PVOID)g_repl;

    if (ps && repl && (PVOID)ps == g_src) {
        if (n == 0) {
            ps = (ID3D11PixelShader *)repl;
            if (g_subDone == 0) {         /* one plain read; cold after this */
                InterlockedExchange(&g_subDone, 1);
                Log("night vision is drawing with \"%s\" instead of the "
                    "engine's shader (replace \"%s\")",
                    kLooks[g_look].key, kReplaces[g_replace].key);
            }
        } else if (g_ninst == 0) {
            /* The replacement has no class linkage. Binding it with the
             * engine's interfaces would be an invalid shader/interface pair,
             * so leave the engine's own shader on the wire and say so. */
            InterlockedExchange(&g_ninst, 1);
            Log("the engine bound the night-vision shader with %u class "
                "instance(s); not substituting, because the replacement has "
                "none. Nothing is wrong on screen - the game's own filter is "
                "still drawing.", n);
        }
    }
    g_origSetPs(ctx, ps, inst, n);
}

/* Create both hooks; enable only the cold one.
 *
 * CreatePixelShader has to be live before the engine compiles anything, or the
 * night-vision shader goes by unseen - so it is enabled here, from inside the
 * device creation hook. PSSetShader is the hot one and is deliberately left
 * installed but disabled: ApplyArm enables it once a replacement is actually
 * selected and takes it back out when the replace row returns to off, so the
 * off state costs this process nothing rather than one detour per call.
 *
 * Creating a hook patches nothing - MinHook only writes the jump when the hook
 * is enabled - so having it sitting here disabled is free. */
static void InstallShaderHooks(void) {
    void **dvt = *(void ***)g_dev;
    void **cvt = *(void ***)g_ctx;
    void  *fnCreate = dvt[offsetof(ID3D11DeviceVtbl, CreatePixelShader)
                          / sizeof(void *)];
    void  *fnSet = cvt[offsetof(ID3D11DeviceContextVtbl, PSSetShader)
                       / sizeof(void *)];

    if (MH_CreateHook(fnCreate, (LPVOID)HookCreatePs,
                      (LPVOID *)&g_origCreatePs) != MH_OK) {
        Log("MH_CreateHook(CreatePixelShader) failed - nothing substituted");
        return;
    }
    if (MH_CreateHook(fnSet, (LPVOID)HookSetPs,
                      (LPVOID *)&g_origSetPs) != MH_OK) {
        Log("MH_CreateHook(PSSetShader) failed - nothing substituted");
        return;
    }
    g_fnSet = fnSet;

    if (MH_EnableHook(fnCreate) != MH_OK) {
        Log("MH_EnableHook(CreatePixelShader) failed - nothing substituted");
        return;
    }
    Log("hooks: CreatePixelShader %p live; PSSetShader %p created but left "
        "disabled until a replacement is selected", fnCreate, fnSet);
}

/* ---- arming ------------------------------------------------------------ */

/* Put the substitution hook in the state the two rows call for. Idempotent,
 * acts only on a change, and is called only from the plugin's own monitor
 * thread - enabling or disabling a hook suspends every other thread for a
 * moment, which is fine from here and not fine from inside the engine's
 * CreatePixelShader call. */
static void ApplyArm(void) {
    PVOID want = NULL;
    int look = (int)g_look, rep = (int)g_replace;
    int was;

    if (g_src != NULL && rep > 0 && rep < REPLACE_N &&
        look >= 0 && look < LOOK_N)
        want = g_ourPs[look][rep];

    if (want == (PVOID)g_repl) return;              /* nothing changed */

    if (want == NULL) {
        /* Clear the pointer before removing the hook, so a bind that is
         * already inside the hook lands on the engine's own shader. */
        g_repl = NULL;
        g_armedLook = g_armedRep = -1;
        if (g_hookOn && g_fnSet) {
            if (MH_DisableHook(g_fnSet) == MH_OK) {
                InterlockedExchange(&g_hookOn, 0);
                Log("substitution off - PSSetShader is unhooked and the game "
                    "pays nothing for this plugin");
            } else {
                Log("MH_DisableHook(PSSetShader) failed - the hook stays in "
                    "place but substitutes nothing");
            }
        }
        return;
    }

    if (g_armFailed) return;
    if (!g_fnSet) return;                           /* no device captured yet */

    was = (int)g_hookOn;
    if (!was) {
        if (MH_EnableHook(g_fnSet) != MH_OK) {
            g_armFailed = 1;
            Log("MH_EnableHook(PSSetShader) failed - nothing will be "
                "substituted");
            return;
        }
        InterlockedExchange(&g_hookOn, 1);
    }
    g_armedLook = look;
    g_armedRep = rep;
    g_repl = want;
    /* Let the next substitution say so again: this line is the evidence that
     * the new selection reached the screen, and it should appear once per
     * selection rather than once per session. */
    InterlockedExchange(&g_subDone, 0);
    Log(was ? "substitution switched to \"%s\" over \"%s\" (hook already in)"
            : "substitution armed: \"%s\" over \"%s\"",
        kLooks[look].key, kReplaces[rep].key);
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

static int HookDeviceCreation(void) {
    HMODULE d3d = GetModuleHandleA("d3d11.dll");
    void *fnDev, *fnDevSc;

    if (!d3d) return 0;                       /* caller retries          */

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

/* ---- the one-shot warning ---------------------------------------------- */

/* A miss is silent on screen, so it must not be silent in the log. Once the
 * device has been up for a while with pixel shaders being created and none of
 * them matching, say so and say what to do about it. */
static void MissCheck(void) {
    if (g_dev && g_claim == NULL && g_announced == 0 &&
        GetTickCount64() - g_startMs > 90000) {
        InterlockedExchange(&g_announced, 1);
        Log("no pixel shader matched len %u crc %08X after %ld created. Either "
            "this session compiled the night-vision shader before the device "
            "hook was in place, or the game build changed and the pair in %s "
            "needs updating. NvProbe reports the pair it sees. Nothing has "
            "been substituted and nothing on screen is wrong.",
            (unsigned)g_targetLen, (unsigned)g_targetCrc, (long)g_created,
            INI_NAME);
    }
}

/* ---- menu -------------------------------------------------------------- */

/* Say out loud what the rows will and will not do, so the log explains the
 * screen without anyone having to remember the semantics. */
/* Say what the rows ask for, and then what is actually in force right now -
 * the two can differ for a tick, or indefinitely if the shader never appeared,
 * and a log that only stated the request would be the misleading half. */
static void Explain(void) {
    int look = (int)g_look, rep = (int)g_replace;

    if (rep == 0) {
        Log("  replace is off - nothing is substituted, the PSSetShader hook "
            "is not installed, and every piece of headgear decides for itself, "
            "which is the game as shipped");
        return;
    }
    if (!g_ourPs[look][rep]) {
        Log("  no shader was built for that pair - it will do nothing");
        return;
    }
    if (rep == 1)
        Log("  only the ordinary goggles change; the helmets that had their "
            "own look keep it");
    else
        Log("  every setup is forced to \"%s\", so the look no longer depends "
            "on what is worn", kLooks[look].key);
    if (g_src == NULL)
        Log("  waiting for the engine to compile the night-vision shader; it "
            "is taken the moment it appears");
    else if (g_repl == NULL)
        Log("  the engine's shader is known; arming on the next tick");
    else {
        int al = (int)g_armedLook, ar = (int)g_armedRep;
        if (al >= 0 && al < LOOK_N && ar > 0 && ar < REPLACE_N)
            Log("  on screen right now: \"%s\" over \"%s\" - the choice "
                "above takes effect on the next tick", kLooks[al].key,
                kReplaces[ar].key);
        else
            Log("  armed and substituting");
    }
}

static void SaveRows(void) {
    if (!g_iniPath[0]) return;
    if (!WritePrivateProfileStringA("Settings", "look", kLooks[g_look].key,
                                    g_iniPath))
        Log("could not write look=%s to %s", kLooks[g_look].key, g_iniPath);
    if (!WritePrivateProfileStringA("Settings", "replace",
                                    kReplaces[g_replace].key, g_iniPath))
        Log("could not write replace=%s to %s", kReplaces[g_replace].key,
            g_iniPath);
}

static void OnReplace(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    if (value < 0 || value >= REPLACE_N) return;
    /* A list row fires twice for one change: once when the value moves and
     * once when the key is released, the second time so a dropped repeat
     * cannot leave the plugin disagreeing with the screen. Acting on the
     * second is wasted work - two more log lines, two more ini writes, and a
     * second Explain that says nothing new - so the value is compared first.
     * Idempotent either way: if the value did change, the change is made. */
    if (value == (int)g_replace) return;
    InterlockedExchange(&g_replace, value);
    Log("replace set to \"%s\"", kReplaces[value].key);
    Explain();
    SaveRows();
}

static void OnLook(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    if (value < 0 || value >= LOOK_N) return;
    if (value == (int)g_look) return;               /* see OnReplace */
    InterlockedExchange(&g_look, value);
    Log("look set to \"%s\"", kLooks[value].key);
    Explain();
    SaveRows();
}

/* ---- start up ---------------------------------------------------------- */

/* Match an ini value to a row. Exact keys only, plus the obvious short
 * aliases - the log prints the valid set when nothing matches, which is more
 * use than guessing at what the author meant. */
/* The keys of a row table as one comma-separated line. Built from the table so
 * a row added later cannot leave the log naming a set that no longer exists. */
static void JoinKeys(const Row *rows, int n, char *out, size_t cap) {
    size_t used = 0;
    int i;

    if (cap == 0) return;
    out[0] = 0;
    for (i = 0; i < n; i++) {
        int w = snprintf(out + used, cap - used, "%s%s", i ? ", " : "",
                         rows[i].key);
        if (w < 0 || (size_t)w >= cap - used) return;
        used += (size_t)w;
    }
}

static int LookFromKey(const char *s) {
    int i;
    if (!s || !s[0]) return -1;
    for (i = 0; i < LOOK_N; i++)
        if (!_stricmp(s, kLooks[i].key)) return i;
    if (!_stricmp(s, "bw")) return 0;
    if (!_stricmp(s, "yg")) return 1;
    return -1;
}

static int ReplaceFromKey(const char *s) {
    int i;
    if (!s || !s[0]) return -1;
    for (i = 0; i < REPLACE_N; i++)
        if (!_stricmp(s, kReplaces[i].key)) return i;
    if (!_stricmp(s, "normal") || !_stricmp(s, "only")) return 1;
    if (!_stricmp(s, "everything")) return 2;
    return -1;
}

static int IniLoad(void) {
    char buf[64];
    int i, j, row;

    g_iniPath[0] = 0;
    if (g_owner[0]) {
        char path[MAX_PATH];
        if (ShPluginIniPath(g_owner, path, (int)sizeof path))
            snprintf(g_iniPath, sizeof g_iniPath, "%s", path);
    }
    if (!g_iniPath[0])
        Log("no ini path - using the built-in defaults and not persisting");

    /* Every row's lang key, and which rows each generated variant belongs to. */
    for (i = 0; i < LOOK_N; i++) kLookLabel[i] = kLooks[i].label;
    for (i = 0; i < REPLACE_N; i++) kReplaceLabel[i] = kReplaces[i].label;
    for (i = 0; i < NV_FILTER_SHADERS; i++) {
        g_lookOf[i] = -1;
        g_replaceOf[i] = -1;
        for (j = 0; j < LOOK_N; j++) {
            if (!_stricmp(kLooks[j].key, kNvFilterShaders[i].look)) {
                g_lookOf[i] = j;
                break;
            }
        }
        for (j = 0; j < REPLACE_N; j++) {
            if (!_stricmp(kReplaces[j].key, kNvFilterShaders[i].reach)) {
                g_replaceOf[i] = j;
                break;
            }
        }
    }

    if (!g_iniPath[0]) return 1;
    if (!GetPrivateProfileIntA("Settings", "enabled", 1, g_iniPath)) {
        Log("disabled by ini");
        return 0;
    }

    if (GetPrivateProfileStringA("Settings", "target_crc", "", buf,
                                 sizeof buf, g_iniPath) && buf[0])
        g_targetCrc = (uint32_t)strtoul(buf, NULL, 16);
    g_targetLen = (uint32_t)GetPrivateProfileIntA("Settings", "target_len",
                                                  (int)g_targetLen, g_iniPath);

    row = -1;
    buf[0] = 0;
    if (GetPrivateProfileStringA("Settings", "replace", "", buf, sizeof buf,
                                 g_iniPath) && buf[0])
        row = ReplaceFromKey(buf);
    if (buf[0] && row < 0) {
        char valid[128];
        JoinKeys(kReplaces, REPLACE_N, valid, sizeof valid);
        Log("replace=\"%s\" is not one of the rows; using \"%s\". Valid: %s",
            buf, kReplaces[0].key, valid);
        row = 0;
    }
    InterlockedExchange(&g_replace, row < 0 ? 0 : row);

    row = -1;
    buf[0] = 0;
    if (GetPrivateProfileStringA("Settings", "look", "", buf, sizeof buf,
                                 g_iniPath) && buf[0])
        row = LookFromKey(buf);
    if (buf[0] && row < 0) {
        char valid[128];
        JoinKeys(kLooks, LOOK_N, valid, sizeof valid);
        Log("look=\"%s\" is not one of the rows; using \"%s\". Valid: %s. "
            "The game's own look is the replace row's \"off\", not a look.",
            buf, kLooks[0].key, valid);
        row = 0;
    }
    InterlockedExchange(&g_look, row < 0 ? 0 : row);

    Log("replace \"%s\", look \"%s\"   target len %u crc %08X   variants %d",
        kReplaces[g_replace].key, kLooks[g_look].key, (unsigned)g_targetLen,
        (unsigned)g_targetCrc, NV_FILTER_SHADERS);
    return 1;
}

/* ---- text ---------------------------------------------------------------
 * This plugin's own words, compiled in, so the page reads in the player's
 * language with or without plugins\NvFilter\lang.ini beside it: that file is
 * an override, not a requirement. The owner is the .asi's base name, which is
 * what the menu stamps on a page it hands a plugin, and a declared row can be
 * overridden per key by lang.ini. */
static const ShText kEn[] = {
    { "@nvf.page",           "Night vision filter" },
    { "@nvf.hint",           "Force-replace the in-game night vision filter" },
    { "@nvf.replace",        "Night vision filter replace" },
    { "@nvf.off",            "Off" },
    { "@nvf.replace.normal", "Ordinary only" },
    { "@nvf.replace.all",    "Everything" },
    { "@nvf.look",           "Night vision filter" },
    { "@nvf.bw",             "Black and white" },
    { "@nvf.yg",             "Yellow-green" },
    { "@nvf.sepia",          "Sepia" },
    { "@nvf.apollo",         "Filmic" },
    { "@nvf.lighthouse",     "Teal" },
    { "@nvf.tennessee",      "Violet" },
    { "@nvf.arlington",      "Cold" },
    { "@nvf.bridge",         "Magenta" },
    { "@nvf.montenegro",     "Faded" },
    { "@nvf.chief",          "Umber" },
    { "@nvf.songbird",       "Soft" },
    { "@nvf.madre",          "Olive" },
    { "@nvf.burner",         "Crimson" },
    { "@nvf.neutral",        "Neutral" },
    { "@nvf.amber",          "Amber" },
    { "@nvf.cool",           "Cool blue" },
    { "@nvf.contrast",       "Contrast" }
};

static const ShText kZh[] = {
    { "@nvf.page",           "夜视滤镜" },
    { "@nvf.hint",           "强制替换游戏内夜视滤镜" },
    { "@nvf.replace",        "夜视滤镜替换" },
    { "@nvf.off",            "关" },
    { "@nvf.replace.normal", "仅替换默认(特殊装备保留)" },
    { "@nvf.replace.all",    "全部替换" },
    { "@nvf.look",           "夜视滤镜" },
    { "@nvf.bw",             "黑白" },
    { "@nvf.yg",             "黄绿" },
    { "@nvf.sepia",          "棕褐" },
    { "@nvf.apollo",         "胶片" },
    { "@nvf.lighthouse",     "青影" },
    { "@nvf.tennessee",      "紫调" },
    { "@nvf.arlington",      "冷调" },
    { "@nvf.bridge",         "洋红" },
    { "@nvf.montenegro",     "褪色" },
    { "@nvf.chief",          "土黄" },
    { "@nvf.songbird",       "柔和" },
    { "@nvf.madre",          "橄榄" },
    { "@nvf.burner",         "绯红" },
    { "@nvf.neutral",        "中性" },
    { "@nvf.amber",          "琥珀" },
    { "@nvf.cool",           "冷蓝" },
    { "@nvf.contrast",       "高对比" }
};

static void FilterText(void) {
    static int done;

    if (done) return;
    done = 1;
    ShLangDeclare("NvFilter", "en-US", kEn, (int)(sizeof kEn / sizeof kEn[0]));
    ShLangDeclare("NvFilter", "zh-CN", kZh, (int)(sizeof kZh / sizeof kZh[0]));
}

static DWORD WINAPI InitThread(LPVOID p) {
    int tries;
    (void)p;

    NameFromModule((HINSTANCE)p);
    CrcInit();
    g_startMs = GetTickCount64();
    LogInitAlways(LOG_NAME);

    if (!IniLoad()) return 0;

    if (MH_Initialize() != MH_OK) {
        Log("MH_Initialize failed - nothing can be hooked");
        return 0;
    }

    Log("NvFilter up: the first row decides whether to replace anything and how "
        "far it reaches, the second which look to put there");

    /* The menu first: it is the control surface, and it does not depend on the
     * device being hooked yet. Text before the page, or the first capture
     * draws lang keys. */
    FilterText();
    for (tries = 0; tries < 40 && !g_menu; tries++) {
        g_menu = ShMenuCreate("@nvf.page");
        if (g_menu) {
            ShMenuHint(g_menu, "@nvf.hint");
            ShMenuList(g_menu, "@nvf.replace", kReplaceLabel, REPLACE_N,
                       (int)g_replace, OnReplace, NULL);
            ShMenuList(g_menu, "@nvf.look", kLookLabel, LOOK_N,
                       (int)g_look, OnLook, NULL);
            Log("menu page up (F4 opens the root menu)");
        } else {
            Sleep(500);
        }
    }
    if (!g_menu)
        Log("no menu page (%08x) - the substitution still runs", ShLastError());

    /* d3d11.dll is loaded by the game, not by us: wait for it rather than
     * pulling it in sideways, and stop after two minutes so a session with no
     * device at all still produces a readable log instead of nothing. */
    for (tries = 0; tries < 480 && !g_dev; tries++) {
        if (!g_hookedDev && !g_hookedDevSc) HookDeviceCreation();
        if (g_dev) break;
        Sleep(250);
    }
    if (!g_dev) {
        Log("no D3D11 device after 120 s - was graphics initialised before the "
            "plugin was loaded, or is this session not using D3D11?");
        return 0;
    }
    Log("ready - the rows take effect the next time night vision is switched on");

    /* The whole steady state of this plugin. ApplyArm is the only thing that
     * enables or disables the hot hook, and it does so from here rather than
     * from a menu callback or from inside the engine's shader compilation;
     * when it has nothing to change it is a few compares, and MissCheck fires
     * at most once. No timer reads the menu, no per-frame work exists. */
    for (;;) {
        Sleep(250);
        ApplyArm();
        MissCheck();
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        /* Never do the work here: the loader calls this under its own lock,
         * which is exactly the deadlock loader.c warns about. */
        CreateThread(NULL, 0, InitThread, (LPVOID)inst, 0, NULL);
    }
    return TRUE;
}
