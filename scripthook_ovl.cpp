// ScriptHook menu overlay - Dear ImGui 1.92.7 rendered inside the
// game's D3D11 Present call.
//
// Replaces the old native-UI (Phoenix widget) menu renderer in
// scripthook_menu.c while leaving the menu model, navigation and
// every plugin unchanged:
//   - The menu model lives in scripthook_menu.c.
//   - Every frame this file snapshots the current menu through
//     ShMenuCaptureView() and draws it with ImGui using the same
//     geometry and colours as the original native menu.
//   - Once ImGui is up it calls ShMenuSetOverlayReady(1) so the
//     menu thread knows the overlay can actually show the menu
//     before it starts swallowing the keyboard.
//
// Hook technique: capture the swapchain the game creates for itself (route 1,
// below), then give that swapchain - and only that one - a table of its own
// with Present (index 8) and ResizeBuffers (index 13) replaced. The driver's
// shared table is never written to, so no other swapchain in the process ends
// up in this file's path; the old dummy-device route survives only as the
// fallback for a session where no swapchain is ever captured.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>    // IDXGIFactory2, for the swapchain capture
#include <dxgi1_4.h>    // IDXGISwapChain3, for the interface-table check
#include <stdio.h>      // snprintf, for the font path list
#include <string.h>
#include <imm.h>
#include <psapi.h>
#include <vector>
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "psapi.lib")

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#define SH_BUILD 1
#include "scripthook.h"
#include "scripthook_tick.h"
#include "scripthook_draw.h"
#include "log.h"
#include "third_party/minhook/include/MinHook.h"   // dxgi's Present, in code

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

/* Leak-probe counters supplied by the C modules (see scripthook_ui.c /
 * scripthook_scene.c).  Called from the 5s render heartbeat below. */
extern "C" {
int ShUiLeakProbe(int *widgets, int *zombies, int *textures);
int ShSceneLeakProbe(int *used, int *live, int *dead);
}

#define OVL_LOG "scripthook_ovl.log"
static void OvlLog(const char* fmt, ...)
{
    static int ready;
    if (!ready) { LogInit(OVL_LOG); ready = 1; }
    va_list ap;
    va_start(ap, fmt);
    Logv(fmt, ap);
    va_end(ap);
}

/* A milestone: written to this module's own log at info and debug, and to the
 * session's floor (logs\scripthook.log) when [Settings] LogLevel has dropped
 * that file - which is what a released package runs at. Which route the
 * overlay took, and whether it came up at all, is the first thing a "the menu
 * never appeared" or "black screen" report needs, and it used to be visible
 * only to whoever thought to raise the level first. */
#define OvlLogFloor(...)                       \
    do {                                       \
        LogInit(OVL_LOG);                      \
        LogAt(LOG_ALWAYS, __VA_ARGS__);         \
    } while (0)

/* ---- ImGui's assertion handler in a release build ------------------------
 * imconfig.h points IM_ASSERT here when SH_RELEASE is defined, so an assert
 * inside Dear ImGui is a line in logs\scripthook.log rather than a modal
 * dialog in front of a player's game: the report of 2026-09-19 is a Proton
 * session stopped by "Could not load font file!" (imgui_draw.cpp:3199) before
 * the menu could come up, because every font path the overlay tried was a
 * Windows one. The first few only - one broken invariant can fire every frame,
 * and a log is a support artefact, not a fuse box - and never fatal: the
 * library carries on, which is the point of doing it here instead of aborting.
 * A development or -Beta build keeps the default abort. */
extern "C" void ShUiAssertFail(const char *expr, const char *file, int line)
{
    static volatile LONG seen = 0;
    LONG n = InterlockedIncrement(&seen);

    LogAlways("imgui assert: %s (%s:%d)%s", expr ? expr : "?",
              file ? file : "?", line,
              n > 4 ? " - further ones are not logged" : "");
}

/* [loader] overlay: 0 = off. Set by the loader thread once the config is up;
 * 0 means "not answered yet", and the overlay's install waits for it rather
 * than assuming. */
static volatile LONG g_ovlAllow = 0;   /* 0 = waiting, 1 = on, -1 = off */

/* Called by loader.c. Declared through C linkage: the loader is C. */
extern "C" void ShOvlAllow(int on)
{
    InterlockedExchange(&g_ovlAllow, on ? 1 : -1);
}

// ---------------------------------------------------------------------------
// hooked swapchain methods
// ---------------------------------------------------------------------------
typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Present1Fn)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* ResizeFn)(IDXGISwapChain*, UINT, UINT, UINT,
                                             DXGI_FORMAT, UINT);

/* One body serves both present slots, and the slot decides which original it
 * hands the call on to. The form they are called through is the one every
 * forwarder in this project uses: the wider signature, with the extra
 * argument passed on - the three-argument form ignores it, exactly the way
 * the proxy in loader.c forwards exports without guessing at prototypes. */
typedef HRESULT(STDMETHODCALLTYPE* PresentAnyFn)(IDXGISwapChain*, UINT, UINT,
                                                 const void*);

static PresentFn  g_origPresent  = nullptr;
static Present1Fn g_origPresent1 = nullptr;
static ResizeFn   g_origResize   = nullptr;

/* The swapchain this file hooked: kept so a present arriving from anywhere can
 * say whether it is the same object (see the present line) and so the table it
 * carries can be watched for a while after the install. */
static void* volatile g_swapPtr = nullptr;

/* Which module owns an address, for the log: dxgi.dll, nvspcap64.dll, this
 * file's own module, or "private" - the last being what a hook in a heap block
 * looks like, and worth knowing about for exactly that reason. */
static const char* OwnerName(const void* p, char* buf, int cap)
{
    wchar_t w[MAX_PATH];
    HMODULE m = nullptr;
    char*   leaf;

    if (!p) return "none";
    buf[0] = '\0';
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)p, &m) || !m)
        return "private";
    if (GetModuleFileNameW(m, w, MAX_PATH) <= 0) return "?";
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, cap, NULL, NULL) <= 0)
    {
        buf[0] = '\0';
        return "?";
    }
    leaf = strrchr(buf, '\\');
    if (leaf) memmove(buf, leaf + 1, strlen(leaf + 1) + 1);
    return buf;
}

static ID3D11Device*        g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dContext = nullptr;
/* Cached view of the swapchain's back buffer. Creating one per Present
 * ran the driver every frame for nothing; it is dropped in
 * HookResizeBuffers (which invalidates every view of the old buffer) and
 * whenever the device is recreated. */
static ID3D11RenderTargetView* g_backRtv = nullptr;
static HWND   g_hwnd = nullptr;
static WNDPROC g_origWndProc = nullptr;
static volatile LONG g_ready = 0;
/* One line, once: the first frame that went through the overlay. Written at
 * floor level, because everything else after it in a log is a session that
 * really was rendering - which is what tells a hang from a slow start. */
static volatile LONG g_firstFrameSaid = 0;

/* Set once per frame by HookPresent: is any plugin drawer registered?
 * The window subclass reads it to decide whether the mouse should be fed
 * to ImGui (a plugin's window has real widgets; the framework's own
 * drawing has none).  Cached per frame so a burst of mouse messages does
 * not take the draw registry's lock once each. */
static volatile LONG g_drawers = 0;

/* [loader] leak_probe: the 5s leak-hunting heartbeat is opt-in.
 * Resolved once on the render thread when ImGui becomes ready
 * (the loader has parsed the ini long before the first Present);
 * disabled it costs one branch per frame and nothing else. */
static int g_leakProbe = 0;

// Bold CJK font used for the chat UI (input text, composition, hints
// and the candidate list).  Loaded next to the default font; falls back
// to the normal font when no bold variant exists.
static ImFont* g_chatFont = nullptr;

// ---------------------------------------------------------------------------
// WndProc subclass: feed messages to ImGui while the menu is open
// ---------------------------------------------------------------------------

/* The game disabled the IME on its window (DirectInput keyboard), which
 * is why no IME text ever reached it.  While the chat box is up we undo
 * that once per session: (re)associate a default IME input context with
 * the window and force the IME open, so composition messages arrive.
 *
 * KNOWN ISSUE (not ours, documented 2026-09): after playing with Sogou
 * Pinyin and exiting, Ubisoft Connect keeps reporting the game as
 * running until the "搜狗拼音输入法 工具" (SGTool) process is killed.
 * GRW.exe itself is gone; killing the Sogou tool process is what makes
 * Ubisoft notice.  The evidence points at Sogou: its tool process holds
 * a handle to the game, which keeps the (exited) process object alive,
 * and Ubisoft's running check is fooled by that zombie.  Our code has
 * no cross-process footprint at all - no SetWindowsHookEx injection,
 * no OpenProcess/DuplicateHandle, no named objects; the only windows
 * we touch (SoPY_* / CiceroUIWndFrame) are ones Sogou itself created
 * INSIDE the game process, and they die with it.  ImeProbeDisable
 * below restores the whole IME stack at session end as a best-effort
 * mitigation (it also cancels a composition the IME may still hold
 * when the box is closed for us); it did NOT stop the Ubisoft hang in
 * testing, so the fix lives on the Sogou side (e.g. disable its game
 * panel) or nowhere. */
static int g_imeProbed = 0;   /* one attempt per chat session */
/* What the IME looked like before ImeProbeEnable touched it, so
 * ImeProbeDisable can put it back once the chat session ends. */
static int g_imeOpenWas = 0;  /* ImmGetOpenStatus before forcing */
static int g_imeCtxMade = 0;  /* we manufactured the context     */
static HIMC g_imeMadeCtx = NULL; /* the context we manufactured  */

static void ImeProbeEnable(HWND hWnd)
{
    HIMC hImc;
    DWORD err;

    g_imeOpenWas = 0;
    g_imeCtxMade = 0;
    g_imeMadeCtx = NULL;

    /* 1. Make sure the window owns an input context.  IACE_DEFAULT
     * (re)installs the thread default context on this window. */
    if (!ImmAssociateContextEx(hWnd, HIMC(0), IACE_DEFAULT)) {
        err = GetLastError();
        OvlLog("ime probe: ImmAssociateContextEx failed err=%u", err);
    }
    hImc = ImmGetContext(hWnd);
    if (!hImc) {
        /* No context even after the default association: manufacture a
         * fresh one (a game that called ImmDisableIME still accepts a
         * context created explicitly and attached to the window). */
        hImc = ImmCreateContext();
        if (hImc) {
            ImmAssociateContext(hWnd, hImc);
            g_imeCtxMade = 1;
            g_imeMadeCtx = hImc;
            OvlLog("ime probe: created fresh context %p", (void*)hImc);
        } else {
            err = GetLastError();
            OvlLog("ime probe: ImmCreateContext failed err=%u", err);
            return;
        }
    }

    /* 2. Force the IME open on that context. */
    g_imeOpenWas = ImmGetOpenStatus(hImc) ? 1 : 0;
    if (!ImmSetOpenStatus(hImc, TRUE)) {
        err = GetLastError();
        OvlLog("ime probe: ImmSetOpenStatus failed err=%u", err);
    }
    OvlLog("ime probe: context=%p open=%d probed=1",
           (void*)hImc, (int)ImmGetOpenStatus(hImc));
    ImmReleaseContext(hWnd, hImc);
}

/* ---- Self-drawn IME UI (Dear ImGui with IMM32, adapted) -------------
 * The game window never drew IME UI, so the system candidate window
 * floats at a default spot and fights the game's own fullscreen.  We
 * therefore suppress the system candidate/composition windows (clear
 * the ISC_SHOWUI* bits on WM_IME_SETCONTEXT) and read the IME state
 * through IMM32 instead, then draw it ourselves in the ImGui overlay.
 *
 * Threading: WM_IME_* arrive on the window-message thread (SubWndProc)
 * which updates g_ime; the overlay render thread snapshots it under a
 * lock once per frame. */
#define IME_CAND_MAX 16
#define IME_CAND_TXT 64
struct ImeState {
    int   active;                /* composing */
    int   candOpen;              /* candidate list visible */
    int   candCount;             /* total candidates */
    int   candSel;               /* absolute selected index */
    int   candPage;              /* page start index */
    int   candShow;              /* number of stored entries */
    char  comp[192];             /* full composition, UTF-8 */
    char  cand[IME_CAND_MAX][IME_CAND_TXT]; /* page, UTF-8 */
    int   gen;
};
static CRITICAL_SECTION g_imeLock;
static volatile int g_imeLockReady = 0;
static ImeState g_ime;
static int g_imeResetGen = 0;

static void ImeLock(void)   { if (g_imeLockReady) EnterCriticalSection(&g_imeLock); }
static void ImeUnlock(void) { if (g_imeLockReady) LeaveCriticalSection(&g_imeLock); }

static void ImeStateReset(void)
{
    ImeLock();
    memset(&g_ime, 0, sizeof(g_ime));
    g_ime.gen = ++g_imeResetGen;
    ImeUnlock();
}

/* The system caret is what IMM/TSF IMEs anchor their own candidate and
 * composition windows to.  We draw the composition and the candidate
 * list ourselves inside the ImGui overlay, so the IMEs must not draw
 * any native UI.  Some IMEs (MS Pinyin, TSF) honour the cleared
 * ISC_SHOWUI* bits; others (Sogou) position their own window by the
 * system caret and draw it anyway.  With no caret at all there is
 * nothing for them to anchor to, so they have to give up. */
static void ImePlaceCaret(HWND hWnd)
{
    (void)hWnd;
    DestroyCaret();
}

/* ---- anchor for the input method's own candidate window -------------
 * When CandMode = 1 the IME draws its own candidate / composition
 * windows.  IMM IMEs place them where ImmSetCompositionWindow /
 * ImmSetCandidateWindow says; TSF IMEs follow the window's system
 * caret.  The chat input box is drawn at a fixed spot - horizontally
 * centred, top at 72% of the client height, CHAT_H tall - so on the
 * first composition message we move the caret and point both IMM
 * windows at the spot just below the box.  This runs on the window
 * thread with coordinates derived from GetClientRect, so it is always
 * correct (no cross-thread async race that would leave them at 0,0).
 * The caret is deliberately made tiny and hidden: only its position
 * matters to the IME, the overlay draws its own cursor. */

// ---------------------------------------------------------------------------
// UI metrics, scaled by the game resolution ([Settings] MenuScale*).
// The values below are the 1080p baseline (= scale 1.0); ApplyUiScale()
// rewrites them from the same literals whenever the resolved scale
// changes, so re-applying never compounds.  Everything the menu and the
// chat box draw derives from these, so scaling them scales the UI.
// ---------------------------------------------------------------------------

// proportional, never scaled
#define CHAT_ANCHOR_Y_RATIO 0.72f

namespace {

float MENU_X = 16.0f, MENU_Y = 16.0f, MENU_W = 520.0f;
/* PAD_TOP is not 0 any more: the two credit lines in the top right (18 px
 * each, 2 px apart = 38) are centred on the same band as the title, and a
 * 36 px band cannot hold them, so the panel needs a little room above. 4 px
 * leaves the credits 3 px clear of the first hint line. */
float PAD = 16.0f, PAD_TOP = 4.0f;
/* The title band and the gap under it are tuned together: the title (28
 * px) is centred in TITLE_H, so its box ends (TITLE_H - 28) / 2 above the
 * band bottom, and the hints sit MENU_HINT_GAP below that. 36 + 4 puts the
 * title-to-hints gap on the same visual footing as the gap between the
 * hints and the first row, (ROW_H - 24) / 2 - which is what the eye
 * compares them to. */
float TITLE_H = 36.0f, ROW_H = 34.0f, BAR_H = 26.0f, VALUE_W = 130.0f;
// Menu text uses the same bold CJK font as the chat box, ~24px.
float MENU_FS = 24.0f, MENU_TITLE_FS = 28.0f;
float MENU_HINT_FS = 18.0f, MENU_HINT_LH = 26.0f, MENU_HINT_GAP = 4.0f;
// How many hint lines a page may draw. The pages that carry the most
// are the plugin switches one (note + rules + the mode blacklist line,
// three) and the CPU one (note + caveat + the live line); the cap only
// exists to keep a runaway string from building a panel taller than the
// screen.
int MENU_HINT_LINES = 8;
float MENU_CREDIT_FS = 18.0f;
// Title auto-shrink (root menu only): floor and step, scaled too.
float TITLE_MIN_FS = 18.0f, TITLE_STEP_FS = 2.0f, TITLE_CREDIT_GAP = 10.0f;

float CHAT_W_MAX = 720.0f, CHAT_H = 56.0f, CHAT_PAD = 14.0f;
float CHAT_FS = 24.0f, HINT_FS = 18.0f;
float CAND_FS = 22.0f, CAND_ROW = 34.0f, CAND_PADX = 12.0f;
// IME anchor: the caret sits CHAT_ANCHOR_H below the 72% line - same
// value as CHAT_H so the candidate windows track the drawn box.
float CHAT_ANCHOR_H = 56.0f, CHAT_ANCHOR_GAP = 6.0f;

// ---- scale resolution ------------------------------------------------

static float g_menuScaleCfg = 0.0f;  // [Settings] MenuScale, 0 = auto
static float g_menuScaleMin = 0.75f; // [Settings] MenuScaleMin
static float g_menuScaleMax = 3.0f;  // [Settings] MenuScaleMax
static float g_uiScale = 1.0f;       // last applied
static unsigned g_scaleW = 0, g_scaleH = 0;

static float ParseFloat(const char *s, float def)
{
    char *end = nullptr;
    float v;
    if (!s || !*s) return def;
    v = strtof(s, &end);
    return (end && *end == 0) ? v : def;
}

// Read [Settings] MenuScale / MenuScaleMin / MenuScaleMax once.
// Min/max clamp the FINAL scale in both auto and fixed modes; an
// inverted pair is swapped.  Called from HookPresent's init branch.
static void MenuScaleLoadConfig(void)
{
    char buf[64];
    ShConfigGetStr("Settings", "MenuScale", "0", buf, sizeof(buf));
    g_menuScaleCfg = ParseFloat(buf, 0.0f);
    /* NaN survives the float compare chain untouched, so a "nan"
     * value in the ini would poison every metric through
     * ApplyUiScale - any non-positive parse means auto. */
    if (!(g_menuScaleCfg > 0.0f)) g_menuScaleCfg = 0.0f;
    ShConfigGetStr("Settings", "MenuScaleMin", "0.75", buf, sizeof(buf));
    g_menuScaleMin = ParseFloat(buf, 0.75f);
    if (!(g_menuScaleMin >= 0.25f)) g_menuScaleMin = 0.25f;
    ShConfigGetStr("Settings", "MenuScaleMax", "3.0", buf, sizeof(buf));
    g_menuScaleMax = ParseFloat(buf, 3.0f);
    if (!(g_menuScaleMax >= 0.25f)) g_menuScaleMax = 3.0f;
    if (g_menuScaleMin < 0.25f) g_menuScaleMin = 0.25f;
    if (g_menuScaleMax > 6.0f)  g_menuScaleMax = 6.0f;
    if (g_menuScaleMin > g_menuScaleMax) {
        float t = g_menuScaleMin; g_menuScaleMin = g_menuScaleMax;
        g_menuScaleMax = t;
    }
    OvlLog("menu scale cfg: MenuScale=%.2f Min=%.2f Max=%.2f "
           "([Settings])", g_menuScaleCfg, g_menuScaleMin,
           g_menuScaleMax);
}

static void ApplyUiScale(float s)
{
    g_uiScale = s;
    MENU_X  = 16.0f * s;  MENU_Y  = 16.0f * s;  MENU_W  = 520.0f * s;
    PAD     = 16.0f * s;  PAD_TOP = 4.0f * s;
    TITLE_H = 36.0f * s;  ROW_H  = 34.0f * s;   BAR_H   = 26.0f * s;
    VALUE_W = 130.0f * s;
    MENU_FS = 24.0f * s;  MENU_TITLE_FS = 28.0f * s;
    MENU_HINT_FS = 18.0f * s; MENU_HINT_LH = 26.0f * s;
    MENU_HINT_GAP = 4.0f * s; MENU_CREDIT_FS = 18.0f * s;
    TITLE_MIN_FS = 18.0f * s; TITLE_STEP_FS = 2.0f * s;
    TITLE_CREDIT_GAP = 10.0f * s;
    CHAT_W_MAX = 720.0f * s;  CHAT_H  = 56.0f * s;
    CHAT_PAD   = 14.0f * s;   CHAT_FS = 24.0f * s;
    HINT_FS    = 18.0f * s;
    CAND_FS    = 22.0f * s;   CAND_ROW = 34.0f * s;
    CAND_PADX  = 12.0f * s;
    CHAT_ANCHOR_H = 56.0f * s; CHAT_ANCHOR_GAP = 6.0f * s;
}

// Recompute the scale when the swapchain size changed.  Auto mode is
// the buffer height over the 1080p baseline; a fixed MenuScale>0 wins.
// Runs on the render thread right after GetDesc, before any drawing.
static void MaybeRescale(unsigned w, unsigned h)
{
    float s;
    if (!w || !h) return;
    if (w == g_scaleW && h == g_scaleH) return;
    g_scaleW = w;
    g_scaleH = h;
    s = (g_menuScaleCfg > 0.0f) ? g_menuScaleCfg
                                : (float)h / 1080.0f;
    if (s < g_menuScaleMin) s = g_menuScaleMin;
    if (s > g_menuScaleMax) s = g_menuScaleMax;
    if (s != g_uiScale) {
        ApplyUiScale(s);
        OvlLog("ui scale %.2f (res %ux%u, cfg %.2f)",
               s, w, h, g_menuScaleCfg);
    }
}

} // namespace

/* The rectangle of the last input box that was drawn (the overlay's own
 * coordinate space, which is the window's client area - the win32
 * backend feeds ImGui exactly that).  Published by the input box
 * primitive while the frame draws, read on the window thread when an
 * IME message arrives, so it is exchanged atomically rather than
 * locked: the worst a race can do is anchor one message at the previous
 * frame's position.  A zero width means "no box drawn yet", and the
 * fixed spot under the 72% line is used instead - which is exactly
 * where the framework's own box lives, so the very first composition
 * (it can arrive before the box's first frame) still lands right. */
static volatile LONG g_boxX = 0, g_boxY = 0, g_boxW = 0, g_boxH = 0;

/* Client-space point the IME should hang off: the middle of the drawn
 * box's bottom edge, or the old fixed spot when there is no box. */
static void ImeAnchorClient(HWND hWnd, LONG *px, LONG *py)
{
    RECT rc;
    LONG w;
    *px = 0;
    *py = 0;
    if (!hWnd || !IsWindow(hWnd)) return;
    if (!GetClientRect(hWnd, &rc)) return;
    w = (LONG)InterlockedCompareExchange(&g_boxW, 0, 0);
    if (w > 0) {
        *px = (LONG)InterlockedCompareExchange(&g_boxX, 0, 0) + w / 2;
        *py = (LONG)InterlockedCompareExchange(&g_boxY, 0, 0) +
              (LONG)InterlockedCompareExchange(&g_boxH, 0, 0);
        return;
    }
    *px = (rc.right - rc.left) / 2;
    *py = (LONG)((rc.bottom - rc.top) * CHAT_ANCHOR_Y_RATIO)
        + (LONG)CHAT_ANCHOR_H + (LONG)CHAT_ANCHOR_GAP;
}

static void ImeApplyAnchor(HWND hWnd)
{
    HIMC hImc;
    POINT pt;
    RECT rc;
    LONG cx, cy;

    if (!hWnd || !IsWindow(hWnd)) return;
    if (!GetClientRect(hWnd, &rc)) return;

    ImeAnchorClient(hWnd, &cx, &cy);

    /* NULL bitmap = solid caret; tiny and hidden below.  Only the
     * position matters to the IME. */
    CreateCaret(hWnd, NULL, 1, 1);
    SetCaretPos(cx, cy);
    HideCaret(hWnd);

    pt.x = cx;
    pt.y = cy;
    ClientToScreen(hWnd, &pt);
    hImc = ImmGetContext(hWnd);
    if (hImc) {
        COMPOSITIONFORM cf;
        memset(&cf, 0, sizeof(cf));
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos = pt;
        ImmSetCompositionWindow(hImc, &cf);

        CANDIDATEFORM cdf;
        memset(&cdf, 0, sizeof(cdf));
        cdf.dwIndex = 0;
        cdf.dwStyle = CFS_CANDIDATEPOS;
        cdf.ptCurrentPos = pt;
        ImmSetCandidateWindow(hImc, &cdf);
        ImmReleaseContext(hWnd, hImc);
    }
    /* No per-message log: ImeApplyAnchor runs on every WM_IME_* message
     * while the IME is up, and the log flushes every line - it showed up
     * as typing stutter. */
}

/* Screen-space anchor under the chat input box (horizontal centre),
 * used to steer IME candidate windows that place themselves at a
 * screen corner.  Refreshed on the RENDER thread while the chat box is
 * up (see ImeHideForeignWindows), so it always matches the game window
 * position; other threads only read it, no cross-thread geometry. */
static LONG g_imeAnchorX = -32000;
static LONG g_imeAnchorY = -32000;

static void ImeUpdateAnchor(void)
{
    POINT pt;
    if (!g_hwnd || !IsWindow(g_hwnd)) return;
    ImeAnchorClient(g_hwnd, &pt.x, &pt.y);
    ClientToScreen(g_hwnd, &pt);
    g_imeAnchorX = pt.x;
    g_imeAnchorY = pt.y;
}

/* UTF-16 -> UTF-8 into dst (dstSize bytes, NUL terminated). */
static void W2U8(const wchar_t *w, char *dst, int dstSize)
{
    if (!w || !dst || dstSize <= 0) return;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, dst, dstSize, NULL, NULL);
    /* A composition or a candidate longer than the buffer is cut by the
     * converter; half a character is what the overlay would draw. */
    ShUtf8Trim(dst);
}

/* Read the IME composition string and current candidate page, and report
 * what is there in BOTH directions: a composition or a candidate list the
 * IME no longer has has to clear the cached state.
 *
 * It used to only ever SET the two flags, and clear them from
 * WM_IME_ENDCOMPOSITION / IMN_CLOSECANDIDATE.  An IME that stops sending
 * those - Sogou does, once we have parked its own candidate window off
 * screen - then left "a composition is live" true forever.  The box's
 * owner asks exactly that before it dares to act on Enter / Esc / Backspace
 * (during a composition those keys belong to the IME), so a stuck flag
 * locks the box open: T, Enter and Esc all stop working, the keyboard
 * stays captured and the player has to kill the game.  That is the bug
 * the clearing half below exists to prevent.
 *
 * `gen` is bumped only when something actually changed: it is the "the
 * IME is still moving" signal ImeComposingNow reads.
 *
 * Returns 1 when the IME could be asked, 0 when it could not (no window,
 * no context): the teardown path treats "could not ask" as "still
 * composing", because that is the state where touching the context is
 * dangerous (see ImeProbeDisable). */
static int ImeReadState(HWND hWnd)
{
    HIMC hImc;
    char comp[192];
    char cand[IME_CAND_MAX][IME_CAND_TXT];
    int  active = 0, changed;
    int  count = 0, sel = 0, page = 0, pageSize = 0;

    comp[0] = 0;
    memset(cand, 0, sizeof(cand));

    if (!hWnd || !IsWindow(hWnd)) return 0;
    hImc = ImmGetContext(hWnd);
    if (!hImc) return 0;

    /* --- composition string --- */
    {
        LONG n = ImmGetCompositionStringW(hImc, GCS_COMPSTR, NULL, 0);
        if (n > 0) {
            int wn = (int)(n / sizeof(wchar_t)) + 1;
            wchar_t *buf = (wchar_t *)malloc((size_t)wn * sizeof(wchar_t));
            active = 1;              /* the IME has one, empty string or not */
            if (buf) {
                ImmGetCompositionStringW(hImc, GCS_COMPSTR, buf,
                                         (DWORD)(wn * sizeof(wchar_t)));
                buf[wn - 1] = 0;
                W2U8(buf, comp, sizeof(comp));
                free(buf);
            }
        }
    }

    /* --- candidate list --- */
    {
        DWORD sz = ImmGetCandidateListW(hImc, 0, NULL, 0);
        if (sz >= sizeof(CANDIDATELIST)) {
            std::vector<char> raw(sz);
            if (ImmGetCandidateListW(hImc, 0, (LPCANDIDATELIST)raw.data(),
                                     (DWORD)raw.size()) != 0)
            {
                const CANDIDATELIST *cl = (const CANDIDATELIST *)raw.data();
                count = (int)cl->dwCount;
                page  = (int)cl->dwPageStart;
                sel   = (int)cl->dwSelection;
                pageSize = (int)cl->dwPageSize;
                if (pageSize <= 0 || pageSize > count - page)
                    pageSize = count - page;
                if (pageSize > IME_CAND_MAX) pageSize = IME_CAND_MAX;
                for (int i = 0; i < pageSize; i++) {
                    DWORD ofs = cl->dwOffset[page + i];
                    const wchar_t *w;
                    /* The offsets come from an in-process IME; a bad one
                     * would walk past the buffer looking for a NUL. */
                    if (ofs >= raw.size()) { cand[i][0] = 0; continue; }
                    w = (const wchar_t *)(raw.data() + ofs);
                    W2U8(w, cand[i], sizeof(cand[i]));
                }
            }
        }
    }
    ImmReleaseContext(hWnd, hImc);

    /* Commit it - and count it as movement only when it differs. */
    ImeLock();
    changed = (g_ime.active    != active) ||
              (g_ime.candOpen  != (pageSize > 0 ? 1 : 0)) ||
              (g_ime.candCount != count) || (g_ime.candSel != sel) ||
              (g_ime.candPage  != page)  || (g_ime.candShow != pageSize) ||
              (strcmp(g_ime.comp, comp) != 0);
    for (int i = 0; !changed && i < pageSize; i++)
        if (strcmp(g_ime.cand[i], cand[i]) != 0) changed = 1;
    if (changed) {
        g_ime.active    = active;
        g_ime.candOpen  = (pageSize > 0) ? 1 : 0;
        g_ime.candCount = count;
        g_ime.candSel   = sel;
        g_ime.candPage  = page;
        g_ime.candShow  = pageSize;
        snprintf(g_ime.comp, sizeof(g_ime.comp), "%s", comp);
        for (int i = 0; i < IME_CAND_MAX; i++)
            snprintf(g_ime.cand[i], sizeof(g_ime.cand[i]), "%s", cand[i]);
        g_ime.gen++;
    }
    ImeUnlock();
    return 1;
}

/* How long a composition may sit completely still before it is treated as
 * over rather than as live.  A live one moves - every keystroke and every
 * candidate change bumps g_ime.gen - so silence for this long means the
 * cached state is not following an IME any more. */
#define IME_STALE_MS 10000

/* ImeComposingNow's bookkeeping, touched only under the IME lock. */
static int   g_imeMovedGen    = -1;
static DWORD g_imeMovedAt     = 0;
static int   g_imeStaleNagged = 0;

/* Is a composition (or its candidate list) actually alive right now?
 *
 * Everything else asks THIS, never the raw flags, because the answer is
 * the difference between "the IME owns Enter / Esc / Backspace" and "the
 * box can never be closed again".  The raw flags are still cleared by the
 * messages that say so, and by ImeReadState's clearing half; this adds
 * the last resort: a state that has not moved for IME_STALE_MS is dropped
 * outright, so even an IME that goes completely silent cannot keep the
 * box - and the keyboard - hostage.  Callable from any thread. */
static int ImeComposingNow(void)
{
    DWORD now = GetTickCount();
    int on, gen;

    ImeLock();
    on  = (g_ime.active || g_ime.candOpen) ? 1 : 0;
    gen = g_ime.gen;

    if (!on) {
        g_imeMovedGen    = -1;
        g_imeMovedAt     = now;
        g_imeStaleNagged = 0;
    } else if (gen != g_imeMovedGen) {
        g_imeMovedGen = gen;
        g_imeMovedAt  = now;
    } else if ((int)(now - g_imeMovedAt) >= IME_STALE_MS) {
        if (!g_imeStaleNagged) {
            g_imeStaleNagged = 1;
            ImeUnlock();
            OvlLog("ime: the composition state has not moved for %dms - "
                   "dropping it so Enter/Esc reach the box again (this is "
                   "the stuck-IME state that used to lock the box open)",
                   (int)(now - g_imeMovedAt));
            ImeLock();
        }
        ImeStateReset();             /* the lock is recursive: same thread */
        g_imeMovedGen = g_ime.gen;
        g_imeMovedAt  = now;
        on = 0;
    }
    ImeUnlock();
    return on;
}

/* Mirror one IME message into g_ime while the chat box is open.  All
 * these messages are still forwarded to DefWindowProcW afterwards so
 * the IMM32 state machine keeps advancing (WM_IME_COMPOSITION with
 * GCS_RESULTSTR is what turns committed text into WM_IME_CHAR). */
/* Third-party IMEs (Sogou) inject their candidate window into the game
 * process (diagnosed: class "SoPY_Comp" = candidate list, "SoPY_Status"
 * = the status bar) and ignore the ISC_SHOWUI* bits.  We draw our own
 * candidate list, so while the chat box is open those windows must not
 * appear.  Merely ShowWindow(SW_HIDE)-ing them every message makes the
 * IME and us fight (it re-shows, we re-hide) = flicker.  Instead we
 * subclass each such window once: every WM_WINDOWPOSCHANGING parks it
 * off screen and strips the SWP_SHOWWINDOW flag, so the IME may move
 * and resize it freely but it can never become visible. */
#define IMEFOREIGN_MAX 8
/* cand: 1 = candidate window (must follow the chat anchor), 0 = status
 * bar / language indicator (must stay where the IME puts it). */
struct ImeForeignSlot { HWND wnd; WNDPROC orig; int cand; };
static ImeForeignSlot g_imeForeign[IMEFOREIGN_MAX];

static LRESULT CALLBACK ImeForeignProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    int i, cand = 0;
    WNDPROC orig = NULL;
    for (i = 0; i < IMEFOREIGN_MAX; i++)
        if (g_imeForeign[i].wnd == h) {
            orig = g_imeForeign[i].orig;
            cand = g_imeForeign[i].cand;
            break;
        }

    /* This subclass lives for the whole session once installed, so it
     * must respect the current candidate mode:
     *   - overlay-drawn (CandMode = 0): park every IME window off
     *     screen, the overlay draws the candidate list itself;
     *   - input method's own window (CandMode = 1): candidate windows
     *     are the feature - but MS Pinyin's TSF window (CiceroUIWndFrame,
     *     diagnosed in the log) anchors itself to screen (0,0) because
     *     the game exposes no TSF document, and Sogou's SoPY_Comp does
     *     the same.  Rewrite each move to sit under the chat box.
     *     Status bars (cand = 0) keep whatever spot the IME chose. */
    if (m == WM_WINDOWPOSCHANGING) {
        WINDOWPOS *wp = (WINDOWPOS *)l;
        if (wp) {
            if (ShDrawInputGetMode() == 0) {
                /* Force to a point that can never be seen, and strip
                 * the show flag so it cannot appear where it asked. */
                wp->x = -32000;
                wp->y = -32000;
                wp->flags &= ~SWP_SHOWWINDOW;
            }
            else if (cand && g_imeAnchorX > -30000) {
                /* Horizontally centre the candidate window under the
                 * chat box.  wp->cx may be 0 on a pure move, fall back
                 * to the window's current width. */
                int w = wp->cx;
                RECT rc;
                if (w <= 0 && GetWindowRect(h, &rc))
                    w = rc.right - rc.left;
                wp->x = g_imeAnchorX - w / 2;
                wp->y = g_imeAnchorY;
            }
        }
        return orig ? CallWindowProcW(orig, h, m, w, l)
                    : DefWindowProcW(h, m, w, l);
    }
    return orig ? CallWindowProcW(orig, h, m, w, l)
                : DefWindowProcW(h, m, w, l);
}

static BOOL CALLBACK ImeForeignEnum(HWND h, LPARAM lp)
{
    char cls[80];
    DWORD pid = 0;
    int i, slot = -1, cand = 0;
    if (IsWindowVisible(h) == FALSE) return TRUE;
    /* IME UI windows that run in a different process cannot be
     * subclassed safely (SetWindowLongPtrW would fail there); only the
     * windows the text service created inside this process matter. */
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (GetClassNameA(h, cls, sizeof(cls)) == 0) return TRUE;
    /* Sogou: "SoPY_Comp" candidate / "SoPY_Status" status bar; MS
     * Pinyin (TSF): "CiceroUIWndFrame" candidate.  QQ-style IMEs use
     * QQ_*.  Class names ending in Comp/Candidate are candidate
     * windows; everything else is a status bar. */
    if (!(strncmp(cls, "CiceroUIWndFrame", 16) == 0 ||
          strncmp(cls, "SoPY_", 5) == 0 ||
          strncmp(cls, "QQ_", 3) == 0))
        return TRUE;
    cand = (strncmp(cls, "CiceroUIWndFrame", 16) == 0 ||
            strstr(cls, "Comp") != NULL);
    for (i = 0; i < IMEFOREIGN_MAX; i++) {
        if (g_imeForeign[i].wnd == h) {
            g_imeForeign[i].cand = cand;  /* class is stable, keep it */
            return TRUE;
        }
        if (g_imeForeign[i].wnd && !IsWindow(g_imeForeign[i].wnd))
            g_imeForeign[i].wnd = NULL;              /* stale slot */
        if (slot < 0 && !g_imeForeign[i].wnd) slot = i;
    }
    if (slot < 0) return TRUE;
    g_imeForeign[slot].wnd = h;
    g_imeForeign[slot].cand = cand;
    g_imeForeign[slot].orig = (WNDPROC)SetWindowLongPtrW(
        h, GWLP_WNDPROC, (LONG_PTR)ImeForeignProc);
    /* A first-seen window was created AND shown by the IME before we
     * ever subclassed it - that is the one native candidate flash in
     * the bottom right corner on the very first composition.  The
     * subclass only steers FUTURE moves, so a window that is already
     * visible must be parked right now, or it stays on screen until
     * the IME happens to move it again.  The async flags post the
     * change to the window's own thread, so this is safe from any
     * thread; the move passes through our own WM_WINDOWPOSCHANGING
     * handler, which parks it anyway in self-drawn mode. */
    if (ShDrawInputGetMode() == 0) {
        SetWindowPos(h, NULL, -32000, -32000, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_ASYNCWINDOWPOS);
        ShowWindowAsync(h, SW_HIDE);
    }
    return TRUE;
}

/* Find and subclass IME candidate/status windows (Sogou SoPY_*, MS
 * Pinyin TSF CiceroUIWndFrame, QQ_*).  Runs on the RENDER thread - NOT
 * on the window-message thread, where touching another thread's window
 * can block the game's message pump.  SetWindowLongPtrW itself does not
 * send messages, so it is safe from any thread.  What the subclass does
 * with each WM_WINDOWPOSCHANGING depends on the candidate mode (see
 * ImeForeignProc); both modes need the subclass installed, so this runs
 * in CandMode = 0 AND CandMode = 1. */
/* One scan at a time: both the window thread (on IME composition
 * messages) and the render thread (periodic backstop) call this, and
 * two concurrent passes would double-subclass a window - the second
 * SetWindowLongPtrW would record ImeForeignProc itself as the
 * original, breaking the restore chain in ImeProbeDisable. */
static volatile LONG g_imeScanBusy = 0;

static void ImeHideForeignWindows(void)
{
    if (InterlockedCompareExchange(&g_imeScanBusy, 1, 0)) return;
    ImeUpdateAnchor();
    EnumWindows(ImeForeignEnum, 0);
    InterlockedExchange(&g_imeScanBusy, 0);
}

/* Undo everything ImeProbeEnable and the foreign-window subclass
 * did, once the input session is over: cancel any composition the
 * IME still holds, put the open status back, detach the context
 * and un-subclass the IME windows.  When the game later exits
 * nothing of ours is left touching the IME stack.
 *
 * With one exception, learned the hard way: the detach (and the
 * destruction of a context we manufactured) is SKIPPED when the IME
 * still owns a composition.  Detaching the window's input context at
 * that moment leaves the text service with a document it can no longer
 * reach, and the next input event makes textinputframework.dll
 * dereference null.  Field crash 2026-09-13 23:38:44: a box was closed
 * while Esc was held down during a live composition, and the fault
 * landed 2ms after this function's last log line.  So: cancel and
 * restore first, then read the IME and only do the risky half when it
 * says the composition is really gone - and treat "cannot be asked" as
 * "still composing" rather than guessing.  Leaving the association in
 * place costs a window that keeps an input context; crashing costs the
 * session. */
static void ImeProbeDisable(HWND hWnd)
{
    HIMC hImc;
    int i, read, composing;

    if (hWnd && (hImc = ImmGetContext(hWnd)) != NULL) {
        ImmNotifyIME(hImc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ImmSetOpenStatus(hImc, g_imeOpenWas);
        ImmReleaseContext(hWnd, hImc);
    }

    /* Ask the IME where it stands instead of trusting the cache: the
     * cache may have been dropped on purpose (ImeComposingNow's stale
     * rule) while the IME still owns a composition. */
    ImeStateReset();
    read = hWnd ? ImeReadState(hWnd) : 0;
    ImeLock();
    composing = (!read || g_ime.active || g_ime.candOpen) ? 1 : 0;
    ImeUnlock();

    if (hWnd && !composing) ImmAssociateContextEx(hWnd, NULL, 0);
    if (!composing && g_imeCtxMade && g_imeMadeCtx) {
        ImmDestroyContext(g_imeMadeCtx);
        g_imeCtxMade = 0;
        g_imeMadeCtx = NULL;
    }
    for (i = 0; i < IMEFOREIGN_MAX; i++) {
        if (g_imeForeign[i].wnd && IsWindow(g_imeForeign[i].wnd) &&
            g_imeForeign[i].orig)
            SetWindowLongPtrW(g_imeForeign[i].wnd, GWLP_WNDPROC,
                              (LONG_PTR)g_imeForeign[i].orig);
        g_imeForeign[i].wnd = NULL;
        g_imeForeign[i].orig = NULL;
    }
    /* The caret created by ImeApplyAnchor must not survive the
     * session: a leftover caret keeps anchoring IME windows to a
     * dead spot and grows the show-caret count per message. */
    DestroyCaret();
    if (composing)
        OvlLog("ime probe disabled (session over; the IME still had a "
               "composition, so the window keeps its input context - "
               "detaching it here is what crashes the TSF stack)");
    else
        OvlLog("ime probe disabled (session over)");
}

static void ImeMirrorMsg(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION:
        ImeReadState(hWnd);
        break;
    case WM_IME_ENDCOMPOSITION:
        ImeStateReset();
        break;
    case WM_IME_NOTIFY:
        if (wp == IMN_OPENCANDIDATE || wp == IMN_CHANGECANDIDATE) {
            ImeReadState(hWnd);
        }
        else if (wp == IMN_CLOSECANDIDATE) {
            ImeLock();
            g_ime.candOpen = 0;
            g_ime.gen++;
            ImeUnlock();
        }
        break;
    default:
        break;
    }
}

static LRESULT CALLBACK SubWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    /* An input box is up: one IME-enable attempt per session. */
    if (g_ready && ShDrawInputIsOpen()) {
        int selfDrawn = (ShDrawInputGetMode() == 0); /* 0=overlay-drawn */

        if (!g_imeProbed) {
            g_imeProbed = 1;
            ImeProbeEnable(hWnd);
            if (selfDrawn) ImePlaceCaret(hWnd);
            else ImeApplyAnchor(hWnd);
        }

        /* Keep the IME state fresh even when the IME has gone quiet:
         * ImeReadState's clearing half is what stops a composition from
         * being "live" forever, and some IMEs stop sending the messages
         * that would drive it.  The window thread is the only thread
         * allowed to ask IMM about this window, so it is asked here - on
         * a throttle, not once per message. */
        {
            static DWORD lastRead = 0;
            DWORD now = GetTickCount();
            if ((int)(now - lastRead) >= 200) {
                lastRead = now;
                ImeReadState(hWnd);
            }
        }

        /* Every IME message must keep advancing the IMM state machine,
         * so both modes go through DefWindowProcW (its GCS_RESULTSTR
         * handling is what turns the final text into WM_IME_CHAR for
         * the chat buffer).  The modes only differ in what is drawn:
         *   - overlay-drawn (default): swallow WM_IME_STARTCOMPOSITION
         *     so no system composition window ever appears, hide the
         *     IME's own windows, and draw the composition + candidate
         *     list ourselves;
         *   - input method's own window (CandMode=1): the IME's own
         *     UI is the feature.  The ISC_SHOWUI* bits must survive so
         *     the IME renders its full candidate list - clearing them
         *     makes MS Pinyin draw only the thin pinyin bar without
         *     any candidate words.  Instead of suppressing its windows
         *     we subclass them (ImeForeignProc) and force every move
         *     to the anchor under the chat box. */
        switch (msg)
        {
        case WM_IME_SETCONTEXT:
            /* Overlay-drawn mode only: clear the "draw your own
             * composition/candidate UI" bits so the system UI never
             * appears.  In CandMode = 1 keep them intact and anchor
             * the IME's windows under the chat box. */
            if (selfDrawn)
                lParam &= ~(ISC_SHOWUICOMPOSITIONWINDOW | ISC_SHOWUIALL);
            else
                ImeApplyAnchor(hWnd);
            ImeMirrorMsg(hWnd, msg, wParam, lParam);
            return DefWindowProcW(hWnd, msg, wParam, lParam);
        case WM_IME_STARTCOMPOSITION:
            ImeApplyAnchor(hWnd);
            ImeMirrorMsg(hWnd, msg, wParam, lParam);
            if (selfDrawn) {
                /* Swallow: this is the message that makes DefWindowProc
                 * create its default composition window.  We draw the
                 * composition ourselves, so it must never appear. */
                return 1;
            }
            return DefWindowProcW(hWnd, msg, wParam, lParam);
        case WM_IME_COMPOSITION:
            if (!selfDrawn) ImeApplyAnchor(hWnd);
            else ImeHideForeignWindows();
            ImeMirrorMsg(hWnd, msg, wParam, lParam);
            /* DefWindowProc's GCS_RESULTSTR handling turns the final
             * text into WM_IME_CHAR for our chat buffer. */
            return DefWindowProcW(hWnd, msg, wParam, lParam);
        case WM_IME_ENDCOMPOSITION:
            ImeMirrorMsg(hWnd, msg, wParam, lParam);
            return DefWindowProcW(hWnd, msg, wParam, lParam);
        case WM_IME_NOTIFY:
            if (!selfDrawn && wParam == IMN_OPENCANDIDATE)
                ImeApplyAnchor(hWnd);
            ImeMirrorMsg(hWnd, msg, wParam, lParam);
            return DefWindowProcW(hWnd, msg, wParam, lParam);
        default:
            break;
        }
    } else {
        /* Input session just ended (any path: sent, cancelled, menu
         * opened, feature switched off): put the IME stack back the
         * way the game had it, exactly once.  The box rectangle goes
         * with it, so the next session starts from the fixed anchor
         * until its own first frame publishes one.
         *
         * The flag is cleared BEFORE the teardown, not after:
         * ImmNotifyIME / ImmAssociateContextEx / ImmDestroyContext all
         * pump messages, and a message that re-enters this window
         * procedure sees the session already gone and starts the
         * teardown a second time.  That second pass would re-read the
         * IME right after the first pass cancelled the composition -
         * and, if the read came back clean, would detach the context
         * that the first pass is still unwinding (field log
         * 2026-09-13 23:55:11: two "ime probe disabled" lines 0.1ms
         * apart, the second one on the plain path).  One flag write
         * buys exactly-once. */
        if (g_imeProbed) {
            g_imeProbed = 0;
            ImeProbeDisable(hWnd);
        }
        if (g_ime.active || g_ime.candOpen) ImeStateReset();
        InterlockedExchange(&g_boxW, 0);
    }

    /* An input box consumes the keyboard while it is up:
     * WM_CHAR/WM_IME_CHAR feed the box's owner, navigation keys are
     * swallowed so the game's own chat field never sees them.  While
     * the owner is injecting (sending) the hook must stay active too,
     * so the user's physical Enter keyup cannot leak through and
     * submit early - but the injected WM_CHAR characters must fall
     * through to the game window procedure (ShDrawInputWndMsg returns
     * 0 for those). */
    if (g_ready && (ShDrawInputIsOpen() || ShDrawInputSending()) &&
        ShDrawInputWndMsg((uint64_t)(uintptr_t)hWnd, (uint32_t)msg,
                          (uint64_t)wParam, (uint64_t)lParam))
        return 1;
    /* A registered drawer draws a real window with real widgets, and
     * widgets need the mouse.  These messages are only FED to ImGui,
     * never swallowed: the game reads its look input through DirectInput
     * and does not want the window's mouse messages either way, and
     * swallowing them here could only break something. */
    if (g_ready && g_drawers &&
        msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
    if (g_ready && ShMenuIsOpen() &&
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return 1;
    return CallWindowProcW(g_origWndProc, hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// ScriptHook menu look: same geometry and colours as the original
// native-UI menu (scripthook_menu.c before the overlay switch).
// ---------------------------------------------------------------------------
namespace {

// Menu geometry lives in the scaled-metrics block near the top of the
// file (MENU_*, TITLE_*, see ApplyUiScale).

// 0xRRGGBB -> ImU32 (0xAABBGGRR)
ImU32 Col(uint32_t rgb, int a = 255)
{
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a);
}

// The same colour as ImVec4, for ImGuiStyle (which keeps colours as
// floats; only PushStyleColor takes the packed form).
ImVec4 Col4(uint32_t rgb, int a = 255)
{
    return ImGui::ColorConvertU32ToFloat4(Col(rgb, a));
}

// Text layout helpers.
//
// IMPORTANT: in this ImGui version ImDrawList::AddText() treats pos.y as
// the TOP of the text line, not the baseline (glyph.Y0 is stored as an
// offset from the line top and is ~0 for the first row).  A visual text
// box is [top, top + font_size].  To centre that box in a row
// [ry, ry + row_h] use TextTopForRow; a caret or a selection bar that
// must line up with the glyphs is centred on that same box, NOT on an
// imagined baseline.  The chat box, its candidate list and the menu all
// share this scheme.
static float TextLineHeight(ImFont*, float font_size)
{
    return font_size;
}

// The top Y to pass to AddText so the text box is centred in a row.
static float TextTopForRow(ImFont*, float font_size, float ry, float row_h)
{
    return ry + (row_h - font_size) * 0.5f;
}

// Status toasts (see scripthook_hud.c): a short line across the top of
// the screen, a little below the edge, that a plugin put up for a
// moment - "first person on, scanning for the head".
//
// Drawn on the foreground list like the menu and the chat box, so it
// costs nothing of the engine's own UI: that one takes tens of seconds
// to come up after a load and charges a widget per line, which is why
// the toasts are here and not there.
//
// The text arrives already faded - ShToastView::alpha carries the fade
// in and out - so this only has to place it.
static const float TOAST_Y = 56.0f;   // below the top edge, unscaled

void RenderToasts(const ShToastView* v, int n)
{
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = g_chatFont ? g_chatFont : ImGui::GetFont();
    const ImGuiIO& io = ImGui::GetIO();
    const float s = g_uiScale;
    const float fs = MENU_FS;
    const float padX = 18.0f * s, padY = 9.0f * s;
    const float gap = 8.0f * s;
    const float radius = 9.0f * s;
    float y = TOAST_Y * s;
    int i;

    for (i = 0; i < n; i++)
    {
        const ShToastView& t = v[i];
        int a = t.alpha;
        ImVec2 sz;
        float w, h, x;

        if (!t.text[0] || a <= 0) continue;
        sz = font->CalcTextSizeA(fs, 10000.0f, 0.0f, t.text);
        w = sz.x + padX * 2.0f + 7.0f * s;
        h = sz.y + padY * 2.0f;
        x = (io.DisplaySize.x - w) * 0.5f;

        // A dark plate so the line reads over any scene, a faint edge
        // to lift it off a bright sky, and a bar in the line's own
        // colour as the state cue.
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h),
                          IM_COL32(8, 10, 14, 178 * a / 255), radius);
        dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h),
                    IM_COL32(255, 255, 255, 40 * a / 255),
                    radius, 0, 1.2f * s);
        dl->AddRectFilled(ImVec2(x + 5.0f * s, y + 5.0f * s),
                          ImVec2(x + 8.0f * s, y + h - 5.0f * s),
                          Col(t.rgb, a), 1.5f * s);
        dl->AddText(font, fs, ImVec2(x + padX + 7.0f * s, y + padY),
                    Col(t.rgb, a), t.text);
        y += h + gap;
    }
}

void RenderMenu(const ShMenuView* v)
{
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // Same bold CJK font as the chat box; falls back to the default.
    ImFont* font = g_chatFont ? g_chatFont : ImGui::GetFont();
    const float fs = MENU_FS, titleFs = MENU_TITLE_FS, hintFs = MENU_HINT_FS;
    const float hintLh = MENU_HINT_LH, hintGap = MENU_HINT_GAP;
    const float x = MENU_X, y = MENU_Y;
    const float s = g_uiScale;   // for the few inline pixel values
    int i;

    // Control hints under the title: every \n-separated line the page
    // carries. This used to stop at two, which silently dropped the
    // last line of the pages that have three - the mode blacklist
    // notice on the plugin switches page was never on screen at all.
    int hintLines = 0;
    for (const char* hl = v->hint; *hl; ) {
        hintLines++;
        hl = strchr(hl, '\n');
        if (!hl) break;
        hl++;
    }
    if (hintLines > MENU_HINT_LINES) hintLines = MENU_HINT_LINES;
    // Each hint line occupies a hintFs-tall text box; the first row
    // starts just below the last hint line's box.
    float hintH = 0.0f;
    if (hintLines)
        hintH = hintGap + (float)(hintLines - 1) * hintLh + hintFs;

    float h = PAD_TOP + TITLE_H + hintH + ROW_H * (float)v->rows + PAD;
    if (v->footer[0]) h += ROW_H;
    // A status line may carry more than one line of its own: a page that
    // reports two readings under each other writes one text with a newline
    // in it. The panel has to grow for every line, or the second one is
    // drawn outside the background. Lines after the first are stacked by
    // the font's own line height, so they cost that and not a whole row.
    if (v->status[0]) {
        int statusLines = 1;
        for (const char* sl = v->status; (sl = strchr(sl, '\n')) != NULL; sl++)
            statusLines++;
        h += ROW_H + (float)(statusLines - 1) * fs;
    }

    // Panel: translucent rounded quad.
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + MENU_W, y + h),
                      IM_COL32(0, 0, 0, 204), 6.0f * s);
    // Root menu top-right credits keep their own width; the title is
    // shrunk (never below TITLE_MIN_FS) if it would run into them.
    float rightX = x + MENU_W - PAD;
    float creditW = 0.0f, cFs = MENU_CREDIT_FS, cGap = 2.0f * s;
    ImVec2 s1, s2;
    // The two credit lines are framework text, one row per language, so
    // they follow the menu language like every other line of the panel
    // (a language with no row of its own falls back to English). The
    // build line takes the version as a value: the template is checked
    // and formatted the same way a page's status line is, and the version
    // it formats is SH_VERSION, so this line cannot name a build other
    // than the one drawing it.
    const char* c1 = "";
    const char* c2 = "";
    char c2buf[192];
    c2buf[0] = 0;
    if (v->isRoot) {
        const char* en = ShTextEnUS(nullptr, "@ui.credit.build");
        const char* tr = ShLang("@ui.credit.build");

        c1 = ShLang("@ui.credit.author");
        ShTextFormat(c2buf, sizeof(c2buf), en ? en : tr, tr, SH_VERSION);
        c2 = c2buf;
        s1 = font->CalcTextSizeA(cFs, FLT_MAX, 0.0f, c1);
        s2 = font->CalcTextSizeA(cFs, FLT_MAX, 0.0f, c2);
        creditW = s1.x > s2.x ? s1.x : s2.x;
    }
    float tFs = titleFs;
    float tLimit = creditW > 0.0f ? rightX - creditW - TITLE_CREDIT_GAP
                                  : rightX - PAD;
    while (tFs > TITLE_MIN_FS &&
           font->CalcTextSizeA(tFs, FLT_MAX, 0.0f, v->title).x > tLimit)
        tFs -= TITLE_STEP_FS;
    // Title, vertically centred in the title band by its text TOP
    // (AddText pos.y is the line top, same convention as the chat box).
    float ttop = TextTopForRow(font, tFs, y + PAD_TOP, TITLE_H);
    dl->AddText(font, tFs, ImVec2(x + PAD, ttop),
                Col(0xFFD25Au), v->title);
    // Root menu top-right credits: original author + this build.
    if (v->isRoot) {
        float blockH = cFs + cGap + cFs;
        float bTop = y + PAD_TOP + (TITLE_H - blockH) * 0.5f;
        dl->AddText(font, cFs, ImVec2(rightX - s1.x, bTop),
                    Col(0xE6E6E6u), c1);
        dl->AddText(font, cFs, ImVec2(rightX - s2.x, bTop + cFs + cGap),
                    Col(0x8C9BA8u), c2);
    }
    // Control hints, small grey lines under the title.
    {
        float hy = y + PAD_TOP + TITLE_H + hintGap;
        const char* hl = v->hint;
        int li = 0;
        while (*hl && li < hintLines) {
            const char* nl = strchr(hl, '\n');
            const char* end = nl ? nl : hl + strlen(hl);
            // Drawn straight from the string, line by line. This used
            // to copy each line through a 128 byte buffer, which cut a
            // long hint mid-character and left a "?" where the rest of
            // the character should be.
            dl->AddText(font, hintFs, ImVec2(x + PAD, hy),
                        Col(0x8C9BA8u), hl, end);
            hy += hintLh;
            li++;
            hl = nl ? nl + 1 : end;
        }
    }
    // Row area starts below the title and the hints.
    const float top = y + PAD_TOP + TITLE_H + hintH;
    // Selection bar behind the selected row, centred on that row's text
    // box - the same scheme as the chat candidate rows, no manual
    // offset, so text and highlight always sit on the same centre line.
    if (v->rows > 0) {
        float sy = top + ROW_H * (float)v->sel;
        float rTop = TextTopForRow(font, fs, sy, ROW_H);
        float hc = rTop + TextLineHeight(font, fs) * 0.5f;
        dl->AddRectFilled(ImVec2(x + PAD * 0.5f, hc - BAR_H * 0.5f),
                          ImVec2(x + MENU_W - PAD * 0.5f, hc + BAR_H * 0.5f),
                          Col(0x28465Au, 230), 3.0f * s);
    }
    // Rows: name left, value right-aligned in its column.
    for (i = 0; i < v->rows; i++) {
        const ShMenuRow* r = &v->row[i];
        float ry = top + ROW_H * (float)i;
        float rTop = TextTopForRow(font, fs, ry, ROW_H);
        ImU32 c = r->selected ? Col(0x8CF0FFu) : Col(0xD2D2D2u);
        dl->AddText(font, fs, ImVec2(x + PAD + 8.0f * s, rTop), c, r->name);
        if (r->value[0]) {
            ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, r->value);
            dl->AddText(font, fs,
                        ImVec2(x + MENU_W - PAD - VALUE_W +
                               (VALUE_W - sz.x), rTop),
                        c, r->value);
        }
    }
    // Footer and status lines.
    float fy = top + ROW_H * (float)v->rows;
    if (v->footer[0]) {
        float fTop = TextTopForRow(font, fs, fy, ROW_H);
        dl->AddText(font, fs, ImVec2(x + PAD, fTop),
                    Col(0x8C8C8Cu), v->footer);
        fy += ROW_H;
    }
    if (v->status[0]) {
        float sTop = TextTopForRow(font, fs, fy, ROW_H);
        dl->AddText(font, fs, ImVec2(x + PAD, sTop),
                    Col(0xA0E6A0u), v->status);
    }
}

// The input box primitive: a single-line field with the IME's
// composition string, a blinking caret and (in self-drawn mode) the
// candidate list.  Larger bold font for readability: the chat UI uses
// g_chatFont (msyh bold) and a 24px body size.  All metrics are the
// scaled globals from the top-of-file block (CHAT_*, CAND_*).
//
// This is BOTH the framework's chat box and the widget a plugin gets
// through ShDrawInputBox().  That is deliberate: the framework's own
// chat is the only box with a year of field mileage behind it, so
// keeping one implementation is what makes the primitive provable -
// while the chat box looks and behaves exactly as it did, a plugin's
// box does too.  Everything that is the box: panel, text, composition,
// caret, candidates, hint, and the IME anchor that follows the drawn
// rectangle rather than a fixed ratio.
//
// It draws at the CURRENT CURSOR position, so the caller decides where
// the box lives (the chat pins a frameless window at 72% of the screen;
// a plugin draws it wherever its window's cursor happens to be), and it
// advances the cursor past the box.

/* Snapshot the IME state for one frame of drawing. */
static void ImeSnapshot(ImeState* out)
{
    memset(out, 0, sizeof(*out));
    ImeLock();
    *out = g_ime;
    ImeUnlock();
}

int DrawInputBoxAt(const char *id, const char *text, const char *hint,
                   int focused, ShDrawInput *out)
{
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // Chat text uses the bold CJK font (falls back to the default).
    ImFont* font = g_chatFont ? g_chatFont : ImGui::GetFont();
    const float fs = CHAT_FS;
    const float hpx = ImGui::GetIO().DisplaySize.y;
    const float s = g_uiScale;   // for the few inline pixel values
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    const char* txt = text ? text : "";

    if (out) out->composing = 0;

    /* IME composition string (the pinyin/characters not committed yet)
     * renders right after the confirmed text so the user sees both.
     * Only in overlay-drawn mode: with the input method's own window
     * the system IME draws its composition and candidate UI itself. */
    ImeState ime;
    ImeSnapshot(&ime);
    bool selfDrawn = (ShDrawInputGetMode() == 0);
    /* ImeComposingNow, not the raw flags: a stale composition must not
     * keep the owner's hands off Enter/Esc (see the comment on it), and
     * the composition string is only drawn while it is really live. */
    const bool composing = ImeComposingNow() != 0;
    const char* comp = (selfDrawn && composing && ime.active) ? ime.comp : "";
    if (out) out->composing = composing ? 1 : 0;

    // Width hugs text + composition, capped.
    ImVec2 tsz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, txt);
    ImVec2 csz = comp[0] ? font->CalcTextSizeA(fs, FLT_MAX, 0.0f, comp)
                         : ImVec2(0, 0);
    float tw = tsz.x + csz.x + CHAT_PAD * 2.0f;
    if (tw < 240.0f * s) tw = 240.0f * s;
    if (tw > CHAT_W_MAX) tw = CHAT_W_MAX;

    float y = cur.y;
    float x = cur.x;

    // Panel: translucent rounded quad centred horizontally.
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + tw, y + CHAT_H),
                      IM_COL32(0, 0, 0, 210), 6.0f * s);

    // Hint line above the field text (same bold font, smaller size).
    if (hint && hint[0]) {
        dl->AddText(font, HINT_FS,
                    ImVec2(x + CHAT_PAD, y - HINT_FS - 10.0f * s),
                    Col(0x8CF0FFu), hint);
    }

    // Text sits vertically centred in the box by its TOP (AddText's
    // pos.y is the text top in this ImGui, not a baseline).  The caret
    // is drawn from the same top so text and caret line up.
    float ttop = TextTopForRow(font, fs, y, CHAT_H);
    float tlh  = TextLineHeight(font, fs);
    dl->AddText(font, fs, ImVec2(x + CHAT_PAD, ttop),
                Col(0xF0F0F0u), txt);

    // Composition string in a brighter tone right after the text.
    float compX = x + CHAT_PAD + tsz.x;
    if (comp[0])
        dl->AddText(font, fs, ImVec2(compX, ttop),
                    Col(0xA8EFC0u), comp);

    // Caret: after text + composition, same height as the text box.
    bool on = ((GetTickCount() / 400) & 1) != 0;
    if (on) {
        float cx = x + CHAT_PAD + tsz.x + csz.x + 2.0f * s;
        if (cx < x + tw - 4.0f * s)
            dl->AddRectFilled(ImVec2(cx, ttop + 1.0f),
                              ImVec2(cx + 2.5f * s, ttop + tlh - 1.0f),
                              Col(0x8CF0FFu));
    }

    // Candidate list below the box, self-drawn (system IME UI is off).
    // Skipped when the input method draws its own candidate window.
    if (selfDrawn && ime.candOpen && ime.candShow > 0) {
        int n = ime.candShow;
        float cw = 240.0f * s;
        float maxw = 0.0f;
        char num[8];
        for (int i = 0; i < n; i++) {
            snprintf(num, sizeof(num), "%d.", i + 1);
            ImVec2 a = font->CalcTextSizeA(CAND_FS, FLT_MAX, 0.0f, num);
            ImVec2 b = font->CalcTextSizeA(CAND_FS, FLT_MAX, 0.0f,
                                           ime.cand[i]);
            float row = a.x + b.x + CAND_PADX * 2.0f;
            if (row > maxw) maxw = row;
        }
        cw = maxw + 28.0f * s;
        if (cw < 220.0f * s) cw = 220.0f * s;
        if (cw > CHAT_W_MAX) cw = CHAT_W_MAX;

        float cy = y + CHAT_H + 8.0f * s;
        float cx0 = x + (tw - cw) * 0.5f;   // centred on the box
        float chh = CAND_ROW * (float)n + 10.0f * s;
        if (cy + chh > hpx - 8.0f * s)
            cy = y - chh - 8.0f * s; /* flip above */

        dl->AddRectFilled(ImVec2(cx0, cy), ImVec2(cx0 + cw, cy + chh),
                          IM_COL32(16, 18, 22, 235), 6.0f * s);
        for (int i = 0; i < n; i++) {
            int absIdx = ime.candPage + i;
            bool sel = (absIdx == ime.candSel);
            float ry = cy + 5.0f * s + CAND_ROW * (float)i;
            /* Candidate text is centred in its row by its TOP, and the
             * highlight bar is drawn around that same centred box. */
            float ctop = TextTopForRow(font, CAND_FS, ry, CAND_ROW);
            if (sel) {
                float clh = TextLineHeight(font, CAND_FS);
                float hc  = ctop + clh * 0.5f;          /* text centre */
                float hh  = CAND_ROW - 8.0f * s;        /* bar height  */
                dl->AddRectFilled(ImVec2(cx0 + 4.0f * s, hc - hh * 0.5f),
                                  ImVec2(cx0 + cw - 4.0f * s,
                                         hc + hh * 0.5f),
                                  IM_COL32(0x28, 0x46, 0x5A, 220),
                                  3.0f * s);
            }
            snprintf(num, sizeof(num), "%d.", i + 1);
            dl->AddText(font, CAND_FS,
                        ImVec2(cx0 + CAND_PADX, ctop),
                        sel ? Col(0x8CF0FFu) : Col(0x9AA4B0u), num);
            dl->AddText(font, CAND_FS,
                        ImVec2(cx0 + CAND_PADX + 32.0f * s, ctop),
                        sel ? Col(0xFFFFFFu) : Col(0xD8D8D8u),
                        ime.cand[i]);
        }
    }

    /* Anchor the input method under the box that is actually on screen:
     * the IME's own candidate/composition windows and the system caret
     * both hang off this point.  Only a focused box publishes it - an
     * unfocused one is just a drawing - and the window thread reads it
     * while composing, hence plain atomically exchanged coordinates
     * rather than a lock (they are cleared when the session ends). */
    if (focused) {
        InterlockedExchange(&g_boxX, (LONG)x);
        InterlockedExchange(&g_boxY, (LONG)y);
        InterlockedExchange(&g_boxW, (LONG)tw);
        InterlockedExchange(&g_boxH, (LONG)CHAT_H);
    }

    /* The box is drawn on the foreground list, so it still has to claim
     * its layout slot: the frameless window that holds it hugs this, and
     * a plugin that draws more after the box continues underneath it. */
    ImGui::Dummy(ImVec2(tw, CHAT_H));
    return 1;
}

} // namespace

// ---------------------------------------------------------------------------
// The plugin drawing layer's renderer half: the primitives a drawer's
// callback calls, and the window each drawer is wrapped in.  The registry
// is C (scripthook_draw.c) and forwards to the vtable below, which this
// file publishes once ImGui is up - see scripthook_draw.h for why the
// layer is split that way, and @defgroup draw in scripthook.h for the
// contract a plugin sees.
// ---------------------------------------------------------------------------

/* Set while a drawer's callback runs.  The primitives draw at the cursor
 * of the drawer's own window, so they only mean anything inside one; a
 * call from anywhere else is a plugin's mistake worth one log line
 * rather than an ImGui assert. */
static int g_inDraw = 0;
static int g_outsideDrawLogged = 0;
static int g_drawFrameless = 0;
static int g_fontPushes = 0;

static int InDraw(void)
{
    if (g_inDraw) return 1;
    if (!g_outsideDrawLogged) {
        g_outsideDrawLogged = 1;
        OvlLog("draw: a primitive was called outside a drawer callback - "
               "ignored (draw from the callback ShDrawAdd registered)");
    }
    return 0;
}

static int DrawBegin(const char *name, const ShDrawOpts *opts)
{
    ImGuiWindowFlags f = ImGuiWindowFlags_NoSavedSettings;
    ShDrawOpts o;
    memset(&o, 0, sizeof(o));
    if (opts) o = *opts;

    if (o.flags & SH_DRAW_FRAMELESS) {
        /* The shape the framework's own text box uses: no decoration,
         * no background, and the window hugs whatever is drawn in it. */
        f |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
             ImGuiWindowFlags_NoScrollWithMouse |
             ImGuiWindowFlags_NoBackground |
             ImGuiWindowFlags_AlwaysAutoResize |
             ImGuiWindowFlags_NoNav |
             ImGuiWindowFlags_NoFocusOnAppearing |
             ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        g_drawFrameless = 1;
    } else {
        f |= ImGuiWindowFlags_AlwaysAutoResize;
        if (o.flags & SH_DRAW_NO_MOVE)   f |= ImGuiWindowFlags_NoMove;
        if (o.flags & SH_DRAW_NO_RESIZE) f |= ImGuiWindowFlags_NoResize;
        ImGui::SetNextWindowSize(ImVec2(360.0f * g_uiScale, 0.0f),
                                 ImGuiCond_FirstUseEver);
        g_drawFrameless = 0;
    }

    /* Where it first appears.  A plugin that wants a screen anchor rather
     * than a pixel offset asks for it with the two POS_* flags - that is
     * how the framework's own box sits at the centre of the 72% line
     * without knowing the resolution, and the pivot keeps it centred
     * even though the width depends on the drawer's content. */
    if (o.flags & (SH_DRAW_POS_CENTER_X | SH_DRAW_POS_Y_RATIO)) {
        ImGuiIO& io = ImGui::GetIO();
        float px = (o.flags & SH_DRAW_POS_CENTER_X) ? io.DisplaySize.x * 0.5f
                                                   : o.x;
        float py = (o.flags & SH_DRAW_POS_Y_RATIO) ? io.DisplaySize.y * o.y
                                                   : o.y;
        ImGui::SetNextWindowPos(ImVec2(px, py), ImGuiCond_Always,
                                ImVec2((o.flags & SH_DRAW_POS_CENTER_X)
                                           ? 0.5f : 0.0f,
                                       0.0f));
    } else if (o.x > 0.0f || o.y > 0.0f) {
        ImGui::SetNextWindowPos(ImVec2(o.x, o.y), ImGuiCond_FirstUseEver);
    }

    /* The close box writes back through p_open: the drawer keeps its
     * slot but stops being called until ShDrawShow() turns it on again.
     * A first-use position that the user then drags away is remembered
     * by ImGui for the session; the framework deliberately keeps no
     * imgui.ini on disk. */
    {
        bool open = ShDrawShown(name) ? true : false;
        bool visible = ImGui::Begin(name, &open, f);
        if (!open) {
            ShDrawSetShown(name, 0);
            OvlLog("draw: '%s' closed by the user (ShDrawShow puts it back)",
                   name);
        }
        g_inDraw = 1;
        g_fontPushes = 0;
        if (g_drawFrameless) return 1;   /* no chrome: nothing can collapse */
        return visible ? 1 : 0;
    }
}

static void DrawEnd(void)
{
    /* A callback that pushed a font without popping it must not shift the
     * stack of whatever draws next, so the layer balances it here. */
    while (g_fontPushes > 0) {
        ImGui::PopFont();
        g_fontPushes--;
    }
    g_inDraw = 0;
    ImGui::End();
    if (g_drawFrameless) {
        ImGui::PopStyleVar();
        g_drawFrameless = 0;
    }
}

static float DrawScaleImpl(void)
{
    return g_uiScale;
}

static void DrawTextImpl(const char *utf8)
{
    if (!InDraw() || !utf8) return;
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::ColorConvertU32ToFloat4(Col(SH_DRAW_COL_TEXT)));
    ImGui::TextUnformatted(utf8);
    ImGui::PopStyleColor();
}

static void DrawTextColoredImpl(const char *utf8, unsigned rgb, int a)
{
    if (!InDraw() || !utf8) return;
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::ColorConvertU32ToFloat4(Col(rgb, a)));
    ImGui::TextUnformatted(utf8);
    ImGui::PopStyleColor();
}

static void DrawTextWrappedImpl(const char *utf8)
{
    if (!InDraw() || !utf8) return;
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::ColorConvertU32ToFloat4(Col(SH_DRAW_COL_TEXT)));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(utf8);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

static void DrawHintImpl(const char *utf8)
{
    if (!InDraw() || !utf8) return;
    // Same face and tone as the menu's own control hints.
    ImGui::PushFont(g_chatFont ? g_chatFont : ImGui::GetFont(), MENU_HINT_FS);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::ColorConvertU32ToFloat4(Col(SH_DRAW_COL_DIM)));
    ImGui::TextUnformatted(utf8);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

static void DrawSpacingImpl(void)   { if (InDraw()) ImGui::Spacing(); }
static void DrawSameLineImpl(void)  { if (InDraw()) ImGui::SameLine(); }
static void DrawSeparatorImpl(void) { if (InDraw()) ImGui::Separator(); }

/* A filled rectangle of an explicit size: the shape is reserved with a
 * Dummy and then drawn over it, so the cursor ends up past it and the
 * content-hugging window measures it like any other item. */
static void DrawFill(float w, float h, unsigned rgb, int a, float rounding)
{
    if (!InDraw()) return;
    if (w <= 0.0f) w = 1.0f;
    if (h <= 0.0f) h = 1.0f;
    ImGui::Dummy(ImVec2(w, h));
    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(),
                                              ImGui::GetItemRectMax(),
                                              Col(rgb, a), rounding);
}

static void DrawPanelImpl(float w, float h, unsigned rgb, int a)
{
    DrawFill(w, h, rgb, a, 6.0f * g_uiScale);
}

static void DrawRectImpl(float w, float h, unsigned rgb, int a)
{
    DrawFill(w, h, rgb, a, 0.0f);
}

static int DrawButtonImpl(const char *label)
{
    if (!InDraw() || !label) return 0;
    return ImGui::Button(label) ? 1 : 0;
}

static int DrawToggleImpl(const char *label, int *v)
{
    bool b;
    if (!InDraw() || !label || !v) return 0;
    b = (*v != 0);
    if (ImGui::Checkbox(label, &b)) {
        *v = b ? 1 : 0;
        return 1;
    }
    return 0;
}

static int DrawNumberImpl(const char *label, int *v, int step, int mn, int mx)
{
    int before;
    if (!InDraw() || !label || !v) return 0;
    if (mn > mx) { int t = mn; mn = mx; mx = t; }
    if (step < 1) step = 1;
    before = *v;
    ImGui::SetNextItemWidth(120.0f * g_uiScale);
    /* ImGui's field commits on Enter or when it loses focus, which is
     * what makes typing a number possible at all; the clamp is applied
     * to whatever came out. */
    ImGui::InputInt(label, v, step, step * 10);
    if (*v < mn) *v = mn;
    if (*v > mx) *v = mx;
    return (*v != before) ? 1 : 0;
}

static int DrawSliderImpl(const char *label, float *v, float mn, float mx)
{
    if (!InDraw() || !label || !v) return 0;
    ImGui::SetNextItemWidth(160.0f * g_uiScale);
    return ImGui::SliderFloat(label, v, mn, mx) ? 1 : 0;
}

static int DrawListImpl(const char *label, int *idx, const char *const *items,
                        int n)
{
    if (!InDraw() || !label || !idx || !items || n <= 0) return 0;
    if (*idx < 0) *idx = 0;
    if (*idx >= n) *idx = n - 1;
    return ImGui::Combo(label, idx, items, n) ? 1 : 0;
}

static void DrawPushFontImpl(int which)
{
    if (!InDraw()) return;
    if (which == SH_DRAW_FONT_BOLD)
        ImGui::PushFont(g_chatFont ? g_chatFont : ImGui::GetFont(), MENU_FS);
    else
        ImGui::PushFont(NULL, MENU_FS);   /* keep the face, frame body size */
    g_fontPushes++;
}

static void DrawPopFontImpl(void)
{
    if (!InDraw()) return;
    if (g_fontPushes > 0) {
        ImGui::PopFont();
        g_fontPushes--;
    }
}

static int DrawInputBoxImpl(const char *id, const char *text, const char *hint,
                            int focused, ShDrawInput *out)
{
    if (!InDraw()) return 0;
    return DrawInputBoxAt(id, text, hint, focused, out);
}

/* Live while a pinyin composition or its candidate list is on screen.
 * A box's owner reads it to keep its hands off Enter / Esc / Backspace:
 * during composition those keys belong to the IME (shorten pinyin,
 * commit its letters, cancel it), and acting on them as well is what
 * deleted committed Chinese from the buffer while Backspace was only
 * trimming pinyin letters.  Callable from any thread.
 *
 * It answers through ImeComposingNow, so a stale composition - an IME
 * that stopped reporting one - reads as "over" instead of keeping the
 * box's only way out disabled. */
static int DrawComposingImpl(void)
{
    return ImeComposingNow();
}

static const ShDrawVtbl g_drawVtbl = {
    DrawBegin,
    DrawEnd,
    DrawScaleImpl,
    DrawTextImpl,
    DrawTextColoredImpl,
    DrawTextWrappedImpl,
    DrawHintImpl,
    DrawSpacingImpl,
    DrawSameLineImpl,
    DrawSeparatorImpl,
    DrawPanelImpl,
    DrawRectImpl,
    DrawButtonImpl,
    DrawToggleImpl,
    DrawNumberImpl,
    DrawSliderImpl,
    DrawListImpl,
    DrawPushFontImpl,
    DrawPopFontImpl,
    DrawInputBoxImpl,
    DrawComposingImpl
};

// ---------------------------------------------------------------------------
// font loading: the menu's own font.
//
// Not a "Chinese font": the same load draws every Latin string the menu shows,
// and the range it asks for also carries kana, CJK punctuation and the
// half-width forms. A session with no font loaded is still usable - ImGui's
// built-in font draws Latin - which is why the last resort is "built-in font
// + English" and not "give up": the field report of 2026-09-19 is a Proton
// session whose menu never came up because the only paths tried were Windows
// ones, and AddFontFromFileTTF asserts BEFORE it returns NULL.
//
// Every call here passes ImFontFlags_NoLoadError, which turns an unreadable
// path into a NULL and lets the list move on. Where the font comes from, in
// order:
//
//   1. [Settings] Font=   (any .ttc/.ttf; the answer for a Linux prefix)
//   2. <gamedir>\font.ttc, \font.ttf, \msyh.ttc   (drop one next to GRW.exe)
//   3. the Windows system paths
//   4. Z:\usr\share\fonts\...  (Wine/Proton map the host root to Z:)
//
// The chat UI's bold variant follows the same order and falls back to the
// normal weight when nothing is there.
// ---------------------------------------------------------------------------
static int g_menuFontCjk = 0;   /* 1 = a CJK-capable font is in use */

/* One path, one try. Both outcomes are logged: the list of paths tried is
 * what "the menu is in English" is read for. */
static ImFont* TryMenuFont(const char* path, float size, const ImWchar* ranges)
{
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg = {};
    cfg.Flags |= ImFontFlags_NoLoadError;
    ImFont* f = io.Fonts->AddFontFromFileTTF(path, size, &cfg, ranges);

    if (f) OvlLogFloor("font loaded: %s", path);
    else   OvlLog("font: not readable: %s", path);
    return f;
}

/* The candidate list, in the order documented above. Returns how many paths
 * it offered; both callers hand it room for 24. */
static int MenuFontCandidates(char paths[][MAX_PATH], int cap)
{
    char dir[MAX_PATH] = "";
    char cfg[MAX_PATH] = "";
    int n = 0;

    if (ShConfigGetStr("Settings", "Font", "", cfg, sizeof(cfg)))
    {
        /* [Settings] Font=none (or -) is the switch that makes a Linux
         * session reproducible on any machine: no candidate is offered at
         * all, so what comes up is the fallback - built-in font, English menu
         * - and the whole path can be checked without a Proton prefix. */
        if (_stricmp(cfg, "none") == 0 || _stricmp(cfg, "-") == 0)
            return 0;
        if (cfg[0] && n < cap)
            snprintf(paths[n++], MAX_PATH, "%s", cfg);
    }

    if (GetModuleFileNameA(NULL, dir, MAX_PATH))
    {
        char* slash = strrchr(dir, '\\');
        if (slash) slash[1] = 0;
        if (n < cap) snprintf(paths[n++], MAX_PATH, "%sfont.ttc", dir);
        if (n < cap) snprintf(paths[n++], MAX_PATH, "%sfont.ttf", dir);
        if (n < cap) snprintf(paths[n++], MAX_PATH, "%smsyh.ttc", dir);
    }
    if (n < cap) snprintf(paths[n++], MAX_PATH, "C:\\Windows\\Fonts\\msyh.ttc");
    if (n < cap) snprintf(paths[n++], MAX_PATH, "C:\\Windows\\Fonts\\msyh.ttf");
    if (n < cap) snprintf(paths[n++], MAX_PATH, "C:\\Windows\\Fonts\\simhei.ttf");
    if (n < cap) snprintf(paths[n++], MAX_PATH, "C:\\Windows\\Fonts\\simsun.ttc");
    /* Wine and Proton map the host's root to Z:, so these are the fonts a
     * Linux player already has once fonts-noto-cjk or wqy is installed. On
     * Windows the opens simply fail and the list moves on. */
    if (n < cap) snprintf(paths[n++], MAX_PATH, "Z:\\usr\\share\\fonts\\opentype\\noto\\NotoSansCJK-Regular.ttc");
    if (n < cap) snprintf(paths[n++], MAX_PATH, "Z:\\usr\\share\\fonts\\truetype\\noto\\NotoSansCJK-Regular.ttc");
    if (n < cap) snprintf(paths[n++], MAX_PATH, "Z:\\usr\\share\\fonts\\noto-cjk\\NotoSansCJK-Regular.ttc");
    if (n < cap) snprintf(paths[n++], MAX_PATH, "Z:\\usr\\share\\fonts\\truetype\\wqy\\wqy-microhei.ttc");
    if (n < cap) snprintf(paths[n++], MAX_PATH, "Z:\\usr\\share\\fonts\\wqy-zenhei\\wqy-zenhei.ttc");
    if (n < cap) snprintf(paths[n++], MAX_PATH, "Z:\\usr\\share\\fonts\\truetype\\arphic\\uming.ttc");
    if (n < cap) snprintf(paths[n++], MAX_PATH, "Z:\\usr\\share\\fonts\\truetype\\Droid\\DroidSansFallbackFull.ttf");
    return n;
}

// The glyphs the menu draws with: the scripts of every language the game
// ships, plus whatever the language names use. ImGui answers a codepoint
// outside the ranges with '?', which is what a row reading "??????" was -
// the range covered Latin and CJK, and Russian, Korean, Arabic and Czech
// all live outside it.
//
// Naming the whole alphabet of each script is not the cost it would have
// been: the DX11 backend reports RendererHasTextures, which is what makes
// ImGui 1.92 rasterize on demand - a range says which codepoints may be
// baked, and only the ones actually drawn are (the atlas grows and updates
// in pieces). The alternative, pre-baking, is what a backend without that
// flag forces, and it is why the ranges here used to be kept small.
//
// So a translation in any of those scripts needs no change here. The labels
// are added as text as well, which covers a name whose characters are in no
// range ImGui ships.
//
// The vector has to outlive the atlas build, ImGui keeps the pointer until
// then, so it is static and built once.
static const ImWchar kArabicRanges[] =
{
    /* The one script the game ships that ImGui has no range for: the Arabic
     * block and its extended letters. The joined forms a shaper would use
     * are not here - a plain ImGui menu draws what the atlas holds, which
     * is the letters unjoined. */
    0x0600, 0x06FF,
    0x0750, 0x077F,
    0
};

static const ImWchar* MenuFontRanges()
{
    static ImVector<ImWchar> out;
    char one[64];
    int i;

    if (!out.empty()) return out.Data;
    {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesDefault());
        b.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesCyrillic());
        b.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesKorean());
        b.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesChineseFull());
        b.AddRanges(kArabicRanges);     /* kana comes with the Chinese range */
        for (i = 0; ShLangBuiltin(i, one, sizeof(one)); i++)
            b.AddText(one);            /* "code<TAB>label": the label counts */
        b.BuildRanges(&out);
    }
    return out.Data;
}

// The two scripts no CJK font carries: a menu language can be Korean or
// Arabic, and neither a Chinese nor a Japanese font on a Windows box has
// Hangul or the Arabic letters - the ranges above say the codepoints are
// allowed, and the font still has nothing to bake from, which is a row of
// '?' with the range in place.
//
// ImGui's answer for that is merging: a second file added with MergeMode
// puts its glyphs into the font already chosen, so one ImFont answers for
// all of them and whichever file has the codepoint is the one asked.
//
// Merged only after a font was chosen, and logged like the base one: an
// unreadable path costs nothing (NoLoadError) and those rows stay '?'.
/* 1 when the file was merged, 0 when it was not read or not needed.
 *
 * A path equal to [Settings] Font= is refused: that file is already the
 * font being merged into, and merging it into itself adds every glyph to
 * the atlas twice and arms the font debugger's overlap warning.
 */
static int MenuFontMerge(const char* path, const ImWchar* ranges)
{
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg = {};
    char forced[MAX_PATH] = "";

    if (ShConfigGetStr("Settings", "Font", "", forced, sizeof(forced)) &&
        forced[0] && !_stricmp(forced, path))
        return 0;
    cfg.Flags |= ImFontFlags_NoLoadError;
    cfg.MergeMode = true;
    if (!io.Fonts->AddFontFromFileTTF(path, 16.0f, &cfg, ranges))
    {
        OvlLog("font: no %s to merge", path);
        return 0;
    }
    OvlLogFloor("font merged: %s", path);
    return 1;
}

/* The candidate files for one missing script, the same shape the base list
 * has: the Windows fonts first, then what a Wine or Proton prefix carries,
 * with [Settings] FontKorean= / FontArabic= ahead of both.
 *
 * The first file that reads wins and the rest are not tried. Trying them all
 * used to be what a FontKorean= pointing at malgun.ttf did: the same face
 * merged twice, its glyphs in the atlas twice over.
 *
 * which 2 is the CJK list, merged behind a font the player pointed at with
 * Font= - one that is not necessarily able to draw the framework's own
 * Chinese text.
 */
static void MenuFontMergeCandidates(int which)
{
    char cfg[MAX_PATH] = "";
    char paths[5][MAX_PATH];
    const ImWchar* ranges;
    int n = 0, i;

    if (which == 0)
    {
        ranges = ImGui::GetIO().Fonts->GetGlyphRangesKorean();
        if (ShConfigGetStr("Settings", "FontKorean", "", cfg, sizeof(cfg)) &&
            cfg[0])
            snprintf(paths[n++], MAX_PATH, "%s", cfg);
        snprintf(paths[n++], MAX_PATH, "C:\\Windows\\Fonts\\malgun.ttf");
        snprintf(paths[n++], MAX_PATH, "Z:\\usr\\share\\fonts\\truetype\\noto\\NotoSansKR-Regular.otf");
        snprintf(paths[n++], MAX_PATH, "Z:\\usr\\share\\fonts\\opentype\\noto\\NotoSansKR-Regular.otf");
    }
    else if (which == 1)
    {
        ranges = kArabicRanges;
        if (ShConfigGetStr("Settings", "FontArabic", "", cfg, sizeof(cfg)) &&
            cfg[0])
            snprintf(paths[n++], MAX_PATH, "%s", cfg);
        snprintf(paths[n++], MAX_PATH, "C:\\Windows\\Fonts\\segoeui.ttf");
        snprintf(paths[n++], MAX_PATH, "C:\\Windows\\Fonts\\tahoma.ttf");
        snprintf(paths[n++], MAX_PATH, "Z:\\usr\\share\\fonts\\truetype\\noto\\NotoSansArabic-Regular.ttf");
    }
    else
    {
        ranges = MenuFontRanges();
        snprintf(paths[n++], MAX_PATH, "C:\\Windows\\Fonts\\msyh.ttc");
        snprintf(paths[n++], MAX_PATH, "C:\\Windows\\Fonts\\msyh.ttf");
        snprintf(paths[n++], MAX_PATH, "Z:\\usr\\share\\fonts\\opentype\\noto\\NotoSansCJK-Regular.ttc");
        snprintf(paths[n++], MAX_PATH, "Z:\\usr\\share\\fonts\\truetype\\wqy\\wqy-microhei.ttc");
    }

    for (i = 0; i < n; i++)
        if (MenuFontMerge(paths[i], ranges)) return;
}

static void LoadCjkFont()
{
    ImGuiIO& io = ImGui::GetIO();
    const ImWchar* ranges = MenuFontRanges();
    char paths[24][MAX_PATH];
    int n = MenuFontCandidates(paths, 24);
    int i;
    ImFont* base = nullptr;

    for (i = 0; i < n && !base; i++)
        base = TryMenuFont(paths[i], 16.0f, ranges);

    if (base)
    {
        char forced[MAX_PATH] = "";

        io.FontDefault = base;
        g_menuFontCjk = 1;
        /* The two scripts no CJK font carries, merged into the one chosen: a
         * Korean or Arabic menu language is otherwise a row of '?' however
         * wide the ranges are. */
        MenuFontMergeCandidates(0);
        MenuFontMergeCandidates(1);
        /* A font the player pointed at with Font= is not necessarily one
         * that can draw the framework's own text, and g_menuFontCjk is what
         * says the menu is readable in the chosen language - it was set
         * unconditionally before, so a Latin-only Font= claimed Chinese and
         * took the English fallback away with it. Merging the CJK
         * candidates behind it makes the claim true instead; when the file
         * pointed at is one of them the merge helper refuses it and the
         * next candidate answers. */
        if (ShConfigGetStr("Settings", "Font", "", forced, sizeof(forced)) &&
            forced[0])
            MenuFontMergeCandidates(2);
    }
    else
    {
        /* Nothing to draw with. The built-in font keeps the menu usable, and
         * because it carries no CJK glyphs the menu goes English - for this
         * session only: ShLangSet does not write scripthook.ini, so the
         * player's own Language= is left waiting for the day a font turns up
         * (they can point [Settings] Font= at one, drop a .ttc next to the
         * game, or install a system CJK font). */
        ImFontConfig defCfg = {};
        defCfg.Flags |= ImFontFlags_NoLoadError;
        base = io.Fonts->AddFontDefault(&defCfg);
        io.FontDefault = base;
        g_menuFontCjk = 0;
        OvlLogFloor("font: none of the %d candidate(s) could be read - the "
                    "built-in font is in use and the menu is English for this "
                    "session ([Settings] Font= overrides the list)", n);
        if (base && !ShLangMatch(ShLangGet(), "en-US"))
            ShLangSet("en-US");
    }

    /* The chat box's heavier weight, same order, then the normal font. */
    g_chatFont = base;
    if (!base || !g_menuFontCjk) return;
    {
        char bold[24][MAX_PATH];
        char cfg[MAX_PATH] = "";
        int nb = 0;
        ImFont* b = nullptr;

        if (ShConfigGetStr("Settings", "FontBold", "", cfg, sizeof(cfg)) &&
            cfg[0])
            snprintf(bold[nb++], MAX_PATH, "%s", cfg);
        snprintf(bold[nb++], MAX_PATH, "C:\\Windows\\Fonts\\msyhbd.ttc");
        snprintf(bold[nb++], MAX_PATH, "C:\\Windows\\Fonts\\msyhbd.ttf");
        snprintf(bold[nb++], MAX_PATH, "Z:\\usr\\share\\fonts\\opentype\\noto\\NotoSansCJK-Bold.ttc");
        snprintf(bold[nb++], MAX_PATH, "Z:\\usr\\share\\fonts\\truetype\\noto\\NotoSansCJK-Bold.ttc");

        for (i = 0; i < nb && !b; i++)
            b = TryMenuFont(bold[i], 16.0f, ranges);
        if (b)
        {
            g_chatFont = b;
            /* The rows are drawn with this face, so the scripts no CJK font
             * carries have to be merged into it too: the merge above went
             * into the regular weight, and a menu row in Korean or Arabic
             * was a row of '?' because this face has neither - the reason
             * pointing [Settings] Font= at malgun changed nothing. */
            MenuFontMergeCandidates(0);
            MenuFontMergeCandidates(1);
            OvlLogFloor("chat bold font loaded: %s", bold[i - 1]);
        }
        else
            OvlLog("no bold font variant, chat UI falls back to normal weight");
    }
}

/* Called once a second from the Present hook, before NewFrame - the one
 * moment the font atlas is not locked. While the menu sits on the fallback
 * font and the active language asks for CJK again (the player switched back
 * to Chinese, or a font arrived), the list is tried once more so the session
 * does not have to be restarted. */
static void MenuFontTick()
{
    static DWORD last = 0;
    char paths[24][MAX_PATH];
    ImFont* f = nullptr;
    int n, i;
    DWORD now = GetTickCount();

    if (g_menuFontCjk || !g_ready) return;
    if ((DWORD)(now - last) < 1000) return;
    last = now;
    if (ShLangMatch(ShLangGet(), "en-US")) return;   /* English needs no CJK */

    n = MenuFontCandidates(paths, 24);
    for (i = 0; i < n && !f; i++)
        f = TryMenuFont(paths[i], 16.0f, MenuFontRanges());
    if (f)
    {
        ImGui::GetIO().FontDefault = f;
        g_chatFont = f;
        g_menuFontCjk = 1;
        MenuFontMergeCandidates(0);
        MenuFontMergeCandidates(1);
        OvlLogFloor("font: the CJK font arrived (%s) - the menu follows the "
                    "language from here", paths[i - 1]);
    }
}

/* ---------------------------------------------------------------------------
 * Which window is ours
 *
 * The game's start up has two windows of its own and both are swapchains: the
 * splash screen comes first and the render window follows. They are told
 * apart by the same two features the framework reads elsewhere
 * (scripthook_corefix.c, docs/cpu-scheduling.md): the class the game gives
 * them - "ScimitarSplashScreenWindow" against "ScimitarEngineWindowClass" -
 * and the title, where the registered mark of the splash comes through
 * mis-encoded ("Ghost Recon?Wildlands") while the render window has it right
 * ("Ghost Recon(R) Wildlands"). The class is asked first, because a class
 * name is structural where the mark is an encoding accident; the title is
 * what is left for a build that renames its windows.
 *
 * Two places have to agree on it, and neither of them did:
 *
 *  - the window scan must never settle on the splash. Attaching ImGui to a
 *    window that is gone once the intro ends is drawing into a dead
 *    swapchain, and the splash is a swapchain too, so it looks like one;
 *  - Present must not draw for a swapchain that is not on our window at all.
 *    A swapchain vtable is the driver's and every swapchain in the process
 *    shares it, so a second window's frame - the game's own splash, or
 *    another module's overlay UI - arrives in these hooks as well. Building
 *    ImGui on that device and painting into that back buffer is how a
 *    foreign Present gets taken apart.
 *
 * Measured on the field machine of 2026-09-25: two swapchains captured inside
 * the same millisecond, the second window gone 0.6 s later, and the game
 * frozen with no frame of its own ever having reached the hook.
 *
 * [loader] window_title replaces the built-in title test with a substring of
 * the player's own; empty - the default - leaves the rule above in force.
 * ------------------------------------------------------------------------- */
static const wchar_t kRenderClass[] = L"ScimitarEngineWindowClass";
static const wchar_t kSplashClass[] = L"ScimitarSplashScreenWindow";
static const wchar_t kTitleMark[]   = L"Wildlands";   /* in both titles */

static char          g_titleWant[96];
static volatile LONG g_titleWantRead = 0;

static void TitleWant(void)
{
    if (InterlockedCompareExchange(&g_titleWantRead, 1, 0) == 0)
    {
        g_titleWant[0] = '\0';
        ShConfigGetStr("loader", "window_title", "", g_titleWant,
                       (int)sizeof g_titleWant);
    }
}

/* The title says which of the two this is: the splash lost its registered
 * mark to an ANSI title call and carries a '?' where the sign should be, and
 * only the game's own windows carry the mark's word at all. */
static int TitleIsRender(const wchar_t* t)
{
    if (!t || !*t) return 0;
    if (!wcsstr(t, kTitleMark)) return 0;
    return wcschr(t, L'?') == NULL;
}

static int TitleHasWant(const wchar_t* t)
{
    wchar_t want[96];
    int n;

    if (!t || !*t || !g_titleWant[0]) return 0;
    /* The setting is bytes: UTF-8 as an editor that knows better writes it,
     * or CP_ACP as a plain Chinese one does. Either spelling counts. */
    n = MultiByteToWideChar(CP_UTF8, 0, g_titleWant, -1, want,
                            (int)(sizeof want / sizeof want[0]));
    if (n > 0 && wcsstr(t, want)) return 1;
    n = MultiByteToWideChar(CP_ACP, 0, g_titleWant, -1, want,
                            (int)(sizeof want / sizeof want[0]));
    if (n > 0 && wcsstr(t, want)) return 1;
    return 0;
}

/* The window the overlay belongs to: the game's own render window, and never
 * the splash. A class name we know answers it outright; anything else is
 * judged by its title. */
static int WindowIsOurs(HWND h)
{
    wchar_t cls[64], t[192];
    int hasCls, hasTitle;

    if (!h || !IsWindow(h)) return 0;

    hasCls = GetClassNameW(h, cls, 64) > 0;
    if (hasCls && _wcsicmp(cls, kSplashClass) == 0) return 0;
    if (hasCls && _wcsicmp(cls, kRenderClass) == 0) return 1;

    /* No title to judge by: this build's windows are named some way this
     * file does not know, and it gets the behaviour it had before rather
     * than no overlay at all. The splash is already answered by its class,
     * so nothing is lost by being generous here. Read on the init thread
     * only - the render thread never asks for a title. */
    hasTitle = GetWindowTextW(h, t, 192) > 0;
    if (!hasTitle) return 1;

    TitleWant();
    if (g_titleWant[0]) return TitleHasWant(t);
    return TitleIsRender(t);
}

/* The window a swapchain presents into, or null when it cannot be read. */
static HWND SwapWindow(IDXGISwapChain* s)
{
    DXGI_SWAP_CHAIN_DESC d;

    if (!s) return nullptr;
    memset(&d, 0, sizeof d);
    if (FAILED(s->GetDesc(&d))) return nullptr;
    return d.OutputWindow;
}

/* A frame that is not on our window, named once: which window it was is what
 * tells a report from the field that this is what happened.
 *
 * The class goes in with it, because "which module's second swapchain was
 * that" is the next question a report asks and the class is what answers it -
 * another module's overlay UI carries a class of its own, a second window of
 * the game's own carries one of the engine's. GetWindowThreadProcessId and
 * GetClassName both read without sending a message, so they are safe on a
 * window of any process - and a swapchain's window may not be ours. */
static void SwapNotOurs(HWND on, const char* who)
{
    static volatile LONG said;
    DWORD pid = 0;
    char  cls[80] = "";

    if (InterlockedExchange(&said, 1)) return;
    if (on)
    {
        GetWindowThreadProcessId(on, &pid);
        if (!GetClassNameA(on, cls, (int)sizeof cls)) cls[0] = '\0';
    }
    OvlLogFloor("%s: window %llx (pid %lu, class '%s') is not the render "
                "window (%llx) - passed through, no ImGui on it",
                who, (unsigned long long)(uintptr_t)on, (unsigned long)pid,
                cls, (unsigned long long)(uintptr_t)g_hwnd);
}

/* A window's title for a log line: UTF-8, cut short, never a reason to fail. */
static const char* TitleForLog(HWND h)
{
    static char buf[128];
    wchar_t t[192];

    buf[0] = '\0';
    if (!h || GetWindowTextW(h, t, 192) <= 0) return buf;
    if (WideCharToMultiByte(CP_UTF8, 0, t, -1, buf, (int)sizeof buf - 1,
                            NULL, NULL) <= 0)
        buf[0] = '\0';
    return buf;
}

// ---------------------------------------------------------------------------
// Present hook
// ---------------------------------------------------------------------------
/* How long our own part of the previous frame took, from this hook's entry
 * to the moment the real Present is called: the gap the next hitch line
 * measures covers that tail, so the frame before is the one it reports. */
static int g_oursUs = 0;

/* The game presents through one of two slots: Present (8) on IDXGISwapChain,
 * or Present1 (18) on IDXGISwapChain1. Both go into the table this file
 * installs, and which one arrives is written down once - a build that presents
 * through Present1 reached neither hook until Present1 was hooked as well
 * (2026-09-25: a session ran all the way to the main menu with Present never
 * called once, and the menu never appeared on screen). Everything from here on
 * is the same for both; only the original the call is handed on to differs. */
static HRESULT STDMETHODCALLTYPE PresentBody(IDXGISwapChain* pSwap, UINT sync,
                                             UINT flags, PresentAnyFn orig,
                                             const void* params, const char* via)
{
    /* Which entry point the game uses, said once - the first thing a "the menu
     * never appears" report needs, and one compare a frame. The object and its
     * table go in with it: a session where this file's own swapchain was never
     * the one presenting is told apart from one where it was, and from one
     * where the object carries somebody else's table again. */
    {
        static volatile LONG saidVia;

        if (InterlockedExchange(&saidVia, 1) == 0)
        {
            void*  hooked = (void*)InterlockedCompareExchangePointer(&g_swapPtr,
                                                                     nullptr, nullptr);
            void** vt = *(void***)pSwap;
            char   own[64];

            OvlLogFloor("present: the game came in through %s (swap %p, table "
                        "%p in %s%s, sync %u flags %u)", via, (void*)pSwap,
                        (void*)vt,
                        OwnerName((const void*)vt, own, (int)sizeof own),
                        (void*)pSwap == hooked ? ", our captured swapchain"
                                               : ", NOT the one we captured",
                        sync, flags);
        }
    }

    /* Our window, or nothing - see the note on which window is ours. Taken
     * before any bookkeeping, so the frame pacing below stays a picture of
     * the game's own frames and not of whoever else presents. */
    {
        HWND on = SwapWindow(pSwap);

        if (!on || !g_hwnd || on != g_hwnd)
        {
            SwapNotOurs(on, "present");
            return orig(pSwap, sync, flags, params);
        }
    }

    /* Taken here and read at the bottom: only what is in between is ours. */
    uint64_t hookAt = ShTickNow();

    /* The menu's font can arrive late (a Linux prefix where the player
     * installed one, or pointed [Settings] Font= at it): the atlas is only
     * open for changes before a frame begins, which is exactly here. */
    MenuFontTick();

    // Frame pacing, for attribution: what a player calls a stutter is a
    // Present interval far longer than a frame, and this log is the only
    // place that can say afterwards whether it was the mod's doing or the
    // game's own streaming. Only outliers are written, so a clean session
    // costs one compare a frame. A gap past three seconds is written too,
    // under its own name and rate limited, because a hang and a load screen
    // look the same from here - and a hang that wrote nothing would be
    // indistinguishable from a session that simply ended.
    {
        static DWORD lastPresent = 0;
        static DWORD lastStall = 0;
        static uint32_t lastCalls = 0;
        static int decideUs = 0;
        DWORD now = GetTickCount();
        uint32_t calls = ShFileCallCount();
        int gap = (int)(now - lastPresent);

        /* This frame's share of the interception layer. Taken once a frame
         * and read straight out: the take here returns what the layer spent
         * since the previous Present, which is the frame whose length the
         * gap reports - the same span the file count covers, so the two
         * numbers can be read against each other. */
        decideUs = ShDecideTake();

        if (g_ready && lastPresent && gap > 100 &&
            (gap < 3000 || (int)(now - lastStall) > 10000))
        {
            char threads[192];

            if (gap >= 3000) lastStall = now;
            /* The context is what makes the line worth having afterwards: a
             * loading or map bit in the ui state says the game was
             * streaming, the file-call delta says whether the engine was
             * hammering the interception layer while it happened, and the
             * last three fields split the blame between our own Present
             * work, that layer, and the framework's own threads. All of it
             * is paid only on a hitch. */
            ShTickReport(threads, (int)sizeof threads, now);
            /* A hitch is this module's own bookkeeping and stays in its log.
             * A stall is the shape a hang leaves behind, and the level a
             * released package runs at drops this file - so the stall goes to
             * the session's floor too, where a report from a player who never
             * raised the level can still show it. */
            if (gap >= 3000)
                OvlLogFloor("frame stall: %d ms (state %d ui %04X menu %d "
                            "draw %d file %u decide %dus ours %dus%s)",
                            gap, ShGetGameState(), (unsigned)ShGetUiState(),
                            ShMenuIsOpen() ? 1 : 0, ShDrawWantFrame() ? 1 : 0,
                            (unsigned)(calls - lastCalls), decideUs, g_oursUs,
                            threads);
            else
                OvlLog("frame hitch: %d ms (state %d ui %04X menu %d draw %d "
                       "file %u decide %dus ours %dus%s)",
                       gap, ShGetGameState(), (unsigned)ShGetUiState(),
                       ShMenuIsOpen() ? 1 : 0, ShDrawWantFrame() ? 1 : 0,
                       (unsigned)(calls - lastCalls), decideUs, g_oursUs,
                       threads);
        }
        lastCalls = calls;
        lastPresent = now;
    }
    if (!g_ready)
    {
        static DWORD retryAt = 0;   /* back off after a failed init */
        DWORD now = GetTickCount();
        /* Present is already hooked (a captured swapchain gives us that
         * before the window is chosen), so frames can arrive while the init
         * thread is still waiting for the render window. ImGui's Win32
         * backend needs a live window: without this the init would fail once
         * a second for as long as the wait lasts, and the log would be a
         * column of retries saying nothing. */
        if (!g_hwnd || !IsWindow(g_hwnd))
        {
            static int noted;
            if (!noted) { noted = 1; OvlLogFloor("present hooked before the "
                                            "render window is chosen - not "
                                            "starting ImGui yet"); }
            return orig(pSwap, sync, flags, params);
        }
        if (retryAt && (int)(now - retryAt) < 1000) {
            /* fall through: skip re-init attempts this frame */
        }
        else {
        ID3D11Device* dev = nullptr;
        if (SUCCEEDED(pSwap->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) && dev)
        {
            dev->GetImmediateContext(&g_pd3dContext);
            g_pd3dDevice = dev;

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr; // fixed layout, no .ini to save
            /* The menu is walked from the keyboard (scripthook_menu.c, and
             * the hints say so), so the pointer is never wanted - and left
             * alone the ImGui Win32 backend shows the OS arrow the moment
             * ImGui asks for one AND answers WM_SETCURSOR itself, which
             * takes that message away from the game's own window procedure:
             * the pointer the game had hidden came back the moment the menu
             * opened.  Told to leave the cursor alone, the OS cursor stays
             * exactly as the game left it - hidden in play, and the game's
             * own wherever the game's own screens want one. */
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            // Keyboard navigation stays with the menu thread
            // (scripthook_menu.c), ImGui only draws.

            ImGuiStyle& st = ImGui::GetStyle();
            st.WindowRounding = 6.0f;
            st.WindowBorderSize = 0.0f;
            /* A plugin's window is made of ImGui widgets (the draw layer),
             * so give those the menu's palette instead of ImGui's default
             * grey - a plugin then matches the framework without having to
             * guess at colours.  The framework's own drawing never reads
             * these: the menu, the chat box and the toasts all draw with
             * the foreground list and explicit colours. */
            st.FrameRounding = 4.0f;
            st.GrabRounding  = 4.0f;
            st.WindowTitleAlign = ImVec2(0.0f, 0.5f);
            st.Colors[ImGuiCol_WindowBg]         = Col4(0x000000u, 204);
            st.Colors[ImGuiCol_TitleBg]          = Col4(0x28465Au, 230);
            st.Colors[ImGuiCol_TitleBgActive]    = Col4(0x28465Au, 255);
            st.Colors[ImGuiCol_TitleBgCollapsed] = Col4(0x1A2A36u, 230);
            st.Colors[ImGuiCol_Text]             = Col4(SH_DRAW_COL_TEXT);
            st.Colors[ImGuiCol_FrameBg]          = Col4(0xFFFFFFu, 18);
            st.Colors[ImGuiCol_FrameBgHovered]   = Col4(0xFFFFFFu, 30);
            st.Colors[ImGuiCol_FrameBgActive]    = Col4(0xFFFFFFu, 42);
            st.Colors[ImGuiCol_Button]           = Col4(0x28465Au, 200);
            st.Colors[ImGuiCol_ButtonHovered]    = Col4(0x35607Au, 230);
            st.Colors[ImGuiCol_ButtonActive]     = Col4(0x8CF0FFu, 180);
            st.Colors[ImGuiCol_Header]           = Col4(0x28465Au, 200);
            st.Colors[ImGuiCol_HeaderHovered]    = Col4(0x35607Au, 230);
            st.Colors[ImGuiCol_HeaderActive]     = Col4(0x8CF0FFu, 160);
            st.Colors[ImGuiCol_CheckMark]        = Col4(SH_DRAW_COL_HI);
            st.Colors[ImGuiCol_SliderGrab]       = Col4(SH_DRAW_COL_HI);
            st.Colors[ImGuiCol_SliderGrabActive] = Col4(0xFFFFFFu);
            st.Colors[ImGuiCol_Separator]        = Col4(0x8C9BA8u, 90);
            st.Colors[ImGuiCol_ScrollbarBg]      = Col4(0x000000u, 120);

            if (ImGui_ImplWin32_Init(g_hwnd) &&
                ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dContext))
            {
                LoadCjkFont();
                ShMenuSetOverlayReady(1);
                /* Hand the primitives to the C registry: from here a
                 * plugin's drawer callback can draw.  Before this call
                 * (and on a build without the overlay) every primitive
                 * is a no-op, which is what lets the same plugin source
                 * run under both toolchains. */
                ShDrawSetVtbl(&g_drawVtbl);
                g_leakProbe = ShConfigGetBool("loader", "leak_probe", 0)
                            ? 1 : 0;
                InterlockedExchange(&g_ready, 1);
                retryAt = 0;
                OvlLogFloor("imgui ready: hwnd=%llx device=%llx font=%p",
                       (unsigned long long)g_hwnd,
                       (unsigned long long)g_pd3dDevice,
                       (void*)ImGui::GetFont());
                OvlLog("leak probe %s ([loader] leak_probe)",
                       g_leakProbe ? "on" : "off");
                MenuScaleLoadConfig();
            }
            else
            {
                OvlLog("imgui init FAILED (retry in 1s)");
                ImGui_ImplDX11_Shutdown();
                ImGui_ImplWin32_Shutdown();
                ImGui::DestroyContext();
                /* Drop the COM references so a retry starts clean
                 * instead of stacking leaked device/context refs. */
                if (g_backRtv)     { g_backRtv->Release(); g_backRtv = nullptr; }
                if (g_pd3dContext) g_pd3dContext->Release();
                if (g_pd3dDevice)  g_pd3dDevice->Release();
                g_pd3dContext = nullptr;
                g_pd3dDevice = nullptr;
                retryAt = now;
            }
        }
        }
    }

    /* The first frame past the init: everything a report shows after this
     * line is a session that really was rendering. At floor level, so a
     * player who left the log level alone still has the marker - and a log
     * that ends before it says the hang was in the overlay's own start up. */
    if (g_ready && InterlockedExchange(&g_firstFrameSaid, 1) == 0)
        OvlLogFloor("first frame through the overlay (%s, hwnd %llx, device %llx)",
                    via, (unsigned long long)g_hwnd,
                    (unsigned long long)g_pd3dDevice);

    {
        bool drawMenu = ShMenuIsOpen() ? true : false;
        /* A registered drawer draws every frame, with or without the
         * menu: its window is the drawer's own and it decides what to put
         * in it.  The framework's own chat box is one of them now, so
         * there is no chat special case left in this file.  The answer
         * also feeds the window subclass, which only feeds the mouse to
         * ImGui when such a window exists. */
        bool drawPlugins = ShDrawWantFrame() ? true : false;
        InterlockedExchange(&g_drawers, drawPlugins ? 1 : 0);
        /* Status toasts. Taking the snapshot is also what retires a
         * line whose time is up, so it is taken every frame, before
         * the "is ImGui up" test - otherwise a toast said while the
         * overlay is still starting would never leave. */
        ShToastView toasts[SH_TOAST_MAX];
        int toastN = ShHudToastSnapshot(toasts, SH_TOAST_MAX);
        /* While the chat box is up, periodically grab any candidate /
         * status window an IME creates in our process.  This runs on
         * the render thread: EnumWindows + SetWindowLongPtrW here never
         * blocks the game's message pump (unlike on the WndProc thread). */
        if (ShDrawInputIsOpen()) {
            /* Periodically grab any candidate / status window an IME
             * creates in this process and steer it according to the
             * candidate mode (park off screen, or follow the anchor -
             * see ImeForeignProc).  Keyed off the input session, not off
             * the drawing: the IME is up while a box owns the keyboard,
             * whether or not the box is on screen this frame. */
            static DWORD lastImeScan = 0;
            DWORD now = GetTickCount();
            if ((int)(now - lastImeScan) > 120) {
                lastImeScan = now;
                ImeHideForeignWindows();
            }
        }
        if (g_ready && (drawMenu || toastN > 0 || drawPlugins))
        {
            // Bind the swapchain back buffer as the render target so
            // the overlay is drawn on the surface that gets
            // presented, whatever the game left bound.
            DXGI_SWAP_CHAIN_DESC desc = {};
            HRESULT dhr = pSwap->GetDesc(&desc);
            if (!g_backRtv && SUCCEEDED(dhr))
            {
                ID3D11Texture2D* back = nullptr;
                if (SUCCEEDED(pSwap->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                               (void**)&back)))
                {
                    g_pd3dDevice->CreateRenderTargetView(back, nullptr, &g_backRtv);
                    back->Release();
                }
            }
            // A failed GetDesc would leave desc zeroed and the whole
            // frame blank while the menu still swallows every key -
            // the "frozen game with an invisible menu" state.  Skip
            // the frame; keys reach the game until the desc returns.
            if (!SUCCEEDED(dhr) || desc.BufferDesc.Width == 0 ||
                desc.BufferDesc.Height == 0)
            {
                g_oursUs = ShTickUsSince(hookAt);
                return orig(pSwap, sync, flags, params);
            }
            if (g_backRtv)
            {
                g_pd3dContext->OMSetRenderTargets(1, &g_backRtv, nullptr);
                D3D11_VIEWPORT vp;
                vp.TopLeftX = 0; vp.TopLeftY = 0;
                vp.Width = (float)desc.BufferDesc.Width;
                vp.Height = (float)desc.BufferDesc.Height;
                vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
                g_pd3dContext->RSSetViewports(1, &vp);
            }
            // Resolution-driven UI scale: recompute before drawing so
            // a resolution change applies the same frame it is seen.
            MaybeRescale(desc.BufferDesc.Width, desc.BufferDesc.Height);

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            // The swapchain size is authoritative for the draw area.
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2((float)desc.BufferDesc.Width,
                                    (float)desc.BufferDesc.Height);
            ImGui::NewFrame();

            /* The plugin layer first: a drawer's window is an ordinary
             * ImGui window, while the menu, the chat box and the toasts
             * draw on the foreground list and so stay on top of it. */
            ShDrawFrame();

            if (drawMenu) {
                ShMenuView v;
                ShMenuCaptureView(&v);
                RenderMenu(&v);
            }
            if (toastN > 0) RenderToasts(toasts, toastN);

            ImGui::Render();
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            if (g_backRtv)
                g_pd3dContext->OMSetRenderTargets(0, nullptr, nullptr);

            /* The frame dump is diagnostics, so it rides with the leak
             * probe instead of writing to every session's log forever. */
            if (g_leakProbe)
            {
                static DWORD logAt = GetTickCount();
                ImDrawData* dd = ImGui::GetDrawData();
                if ((int)(GetTickCount() - logAt) > 5000)
                {
                    logAt = GetTickCount();
                    OvlLog("frame: display %.0fx%.0f vtx=%d idx=%d",
                           io.DisplaySize.x, io.DisplaySize.y,
                           dd ? dd->TotalVtxCount : -1,
                           dd ? dd->TotalIdxCount : -1);
                }
            }
        }
    }

    // Menu open/close transitions (logged even while closed).
    if (g_ready)
    {
        static int lastOpen = -1;
        int open = ShMenuIsOpen() ? 1 : 0;
        if (open != lastOpen)
        {
            lastOpen = open;
            OvlLog("menu %s", open ? "OPEN" : "closed");
        }
    }

    // 5s heartbeat: process memory + module object counts.  Opt-in
    // through [loader] leak_probe (default off) so a normal session
    // pays a single branch per frame.  When on, it runs on the render
    // thread so a long session shows whether private memory, UI
    // widgets/zombies/textures or scene slots grow.
    if (g_ready && g_leakProbe)
    {
        static DWORD leakAt = GetTickCount();
        if ((int)(GetTickCount() - leakAt) > 5000)
        {
            int uw = -1, uz = -1, ut = -1, su = -1, sl = -1, sd = -1;
            PROCESS_MEMORY_COUNTERS_EX pmc;
            leakAt = GetTickCount();
            memset(&pmc, 0, sizeof(pmc));
            pmc.cb = sizeof(pmc);
            /* The EX struct keeps PrivateUsage; GetProcessMemoryInfo
             * fills what the passed size asks for. */
            if (GetProcessMemoryInfo(GetCurrentProcess(),
                                     (PROCESS_MEMORY_COUNTERS *)&pmc,
                                     sizeof(pmc)))
            {
                ShUiLeakProbe(&uw, &uz, &ut);
                ShSceneLeakProbe(&su, &sl, &sd);
                OvlLog("leak: priv=%llu ws=%llu page=%llu"
                       " ui(w=%d z=%d t=%d) scene(u=%d l=%d d=%d)",
                       (unsigned long long)pmc.PrivateUsage,
                       (unsigned long long)pmc.WorkingSetSize,
                       (unsigned long long)pmc.PagefileUsage,
                       uw, uz, ut, su, sl, sd);
            }
        }
    }

    g_oursUs = ShTickUsSince(hookAt);
    return orig(pSwap, sync, flags, params);
}

/* One frame, one pass through this file.
 *
 * There are three ways into the body below: the table's two present slots and
 * dxgi's own Present. They chain into each other - a table hook hands the call
 * to the original this file recorded, and that original is the very function
 * the code hook sits on - so one call can arrive twice on one thread. The work
 * has to happen once: two NewFrame calls for a single frame is a corrupted
 * ImGui frame, and which entry point fires depends on the machine (the field
 * machine of 2026-09-25 went through the code hook alone; the development
 * machine went through the table), so both have to survive. A thread already
 * inside forwards to the original and does nothing else. */
static __declspec(thread) int g_inPresent = 0;

/* The guard doing its job, said once - and said out loud rather than left to be
 * worked out: the second door into one frame arrives here and hands the call
 * on instead of doing the work twice. That this happens is knowable without the
 * line (the install record has the table's original and the code hook on the
 * same address), but "it happens" is the difference between a frame drawn once
 * and a corrupted ImGui frame, so the log says which door came second. */
static void OvlSaidForwarded(const char *door)
{
    static volatile LONG said;

    if (InterlockedExchange(&said, 1) == 0)
        OvlLogFloor("present: %s arrived while this thread was already inside - "
                    "handed straight on, the frame is done once", door);
}

static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* pSwap, UINT sync, UINT flags)
{
    HRESULT hr;

    if (g_inPresent)
    {
        OvlSaidForwarded("Present");
        return g_origPresent(pSwap, sync, flags);
    }

    g_inPresent = 1;
    hr = PresentBody(pSwap, sync, flags, (PresentAnyFn)g_origPresent, nullptr,
                     "Present");
    g_inPresent = 0;
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* pSwap, UINT sync,
                                              UINT flags,
                                              const DXGI_PRESENT_PARAMETERS* params)
{
    HRESULT hr;

    if (g_inPresent)
    {
        OvlSaidForwarded("Present1");
        return g_origPresent1(pSwap, sync, flags, params);
    }

    g_inPresent = 1;
    hr = PresentBody(pSwap, sync, flags, (PresentAnyFn)g_origPresent1, params,
                     "Present1");
    g_inPresent = 0;
    return hr;
}

/* ---------------------------------------------------------------------------
 * The present hook that cannot be routed around
 *
 * The table hooks cover the swapchain this file captured - and that is not
 * enough on every machine. The field log of 2026-09-25 21:34 is the one that
 * says so: the frame arrived with the object carrying this file's own table
 * (`table … in DINPUT8.dll, our captured swapchain`) and still never passed
 * through a table slot, because the caller (a platform overlay's own hook, in
 * all likelihood) calls the function by an address it saved earlier. Only a
 * hook in dxgi's code sees that call, and that is what this is.
 *
 * Its address is the one the live table handed over - whose it is, the install
 * line writes down - so there is no RVA to hunt and nothing to verify: it is
 * the function the game would have called. PresentBody is shared with the table
 * hooks, so the overlay draws for whichever swapchain is on the game's window,
 * and the first present line says which object arrived and how.
 * ------------------------------------------------------------------------- */
static PresentFn     g_codeTramp     = nullptr;
static volatile LONG g_codeHookTried = 0;

static HRESULT STDMETHODCALLTYPE PresentCodeHook(IDXGISwapChain* pSwap, UINT sync,
                                                 UINT flags)
{
    HRESULT hr;

    if (g_inPresent)
    {
        OvlSaidForwarded("Present (code hook in dxgi)");
        return g_codeTramp(pSwap, sync, flags);
    }

    g_inPresent = 1;
    hr = PresentBody(pSwap, sync, flags, (PresentAnyFn)g_codeTramp, nullptr,
                     "Present (code hook in dxgi)");
    g_inPresent = 0;
    return hr;
}

static void InstallPresentCodeHook(void)
{
    MH_STATUS s;

    if (InterlockedExchange(&g_codeHookTried, 1)) return;
    if (!g_origPresent) return;

    s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)
    {
        OvlLogFloor("present code hook: MH_Initialize failed (%s) - the table "
                    "hooks are all that is left", MH_StatusToString(s));
        return;
    }
    s = MH_CreateHook((LPVOID)g_origPresent, (LPVOID)PresentCodeHook,
                      (LPVOID*)&g_codeTramp);
    if (s != MH_OK)
    {
        OvlLogFloor("present code hook: MH_CreateHook on %p failed (%s) - the "
                    "table hooks are all that is left", (void*)g_origPresent,
                    MH_StatusToString(s));
        return;
    }
    s = MH_EnableHook((LPVOID)g_origPresent);
    if (s != MH_OK)
    {
        OvlLogFloor("present code hook: MH_EnableHook failed (%s) - backing off",
                    MH_StatusToString(s));
        MH_RemoveHook((LPVOID)g_origPresent);
        g_codeTramp = nullptr;
        return;
    }
    OvlLogFloor("present code hook installed on %p - every present in this "
                "process lands here from now on", (void*)g_origPresent);
}

// ---------------------------------------------------------------------------
// ResizeBuffers hook: rebuild ImGui device objects across resizes
// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* pSwap, UINT bc,
                                                   UINT w, UINT h, DXGI_FORMAT f,
                                                   UINT flags)
{
    /* Nothing here belongs to a swapchain that is not on our window: dropping
     * our view and rebuilding ImGui's device objects against another window's
     * swapchain is the same mistake Present is guarded against - see the note
     * on which window is ours. */
    {
        HWND on = SwapWindow(pSwap);

        if (!on || !g_hwnd || on != g_hwnd)
        {
            SwapNotOurs(on, "resize");
            return g_origResize(pSwap, bc, w, h, f, flags);
        }
    }

    /* The game's own swapchain resizing is the plainest proof that the table
     * this file installed is the one in use: said once, at floor level. */
    {
        static volatile LONG said;

        if (InterlockedExchange(&said, 1) == 0)
            OvlLogFloor("resize: our swapchain came through the hooks "
                        "(back buffer %ux%u)", w, h);
    }

    /* ResizeBuffers refuses to run while a view of the back buffer is
     * still alive, so drop ours first - it is rebuilt on the next frame. */
    if (g_backRtv) { g_backRtv->Release(); g_backRtv = nullptr; }
    if (g_ready)
        ImGui_ImplDX11_InvalidateDeviceObjects();
    HRESULT hr = g_origResize(pSwap, bc, w, h, f, flags);
    if (g_ready)
        ImGui_ImplDX11_CreateDeviceObjects();
    return hr;
}

// ---------------------------------------------------------------------------
// hook installation
//
// Two routes, in this order:
//
//   1. Capture the swapchain the game creates for itself. A DXGI factory's
//      vtable is one shared array per interface version per dxgi.dll, so
//      patching CreateSwapChain / CreateSwapChainForHwnd on the factory we
//      create here intercepts every swapchain the game makes, whichever
//      factory object it happened to use. This route creates no device of
//      its own, which is the point: the old probe device was a second D3D11
//      device created while the game was creating its first, and that race
//      is the one the field reports keep landing on (2026-09-16 19:23: a
//      fresh install crashing 0.35 s after the window was found).
//
//   2. Only if (1) never fires: the probe device, and only long after the
//      render window is up, by which time the game's own renderer has
//      either finished or is not coming. Which route was taken is logged.
// ---------------------------------------------------------------------------
static volatile LONG  g_vtablePatched = 0;  /* Present/ResizeBuffers are ours */
static volatile LONG  g_captured      = 0;  /* route 1 fired */
static PVOID volatile g_capturedHwnd  = nullptr;  /* its swapchain's window */

/* Hook this swapchain: Present=8, ResizeBuffers=13, in a table of its own -
 * see the note above the copy below. Idempotent: the table is built once, and
 * a swapchain that comes later is simply pointed at the same one. */
/* ---------------------------------------------------------------------------
 * Two diagnostic switches
 *
 * The overlay is three things at once: it patches the factory's vtable (so
 * every swapchain the game makes is seen), it patches the swapchain's vtable
 * (so Present runs through this file), and it subclasses the game's window.
 * When a session freezes before the first frame there is nothing in the log
 * that says which of the three a machine objects to, so each half can be left
 * out on its own, from [loader], and the run itself says which:
 *
 *   overlay_hook=0        no Present and no ResizeBuffers: a captured
 *                         swapchain is still logged and still remembered, its
 *                         vtable is left alone, and the probe-device route is
 *                         skipped as well - so nothing in the process hooks a
 *                         swapchain. The menu cannot open: nothing draws.
 *   overlay_subclass=0    the game's window procedure is left alone: no
 *                         keyboard for the menu, and the IME work has no
 *                         target.
 *
 * Both default to on, which is the overlay as it has always been. A session
 * with either turned off is a diagnostic, not a way to play.
 *
 * Read on the init thread, once: the loader has loaded the config before it
 * lets that thread off its wait. */
static int g_hookOn     = -1;   /* -1: not read yet */
static int g_subclassOn = -1;

static void OvlReadSwitches(void)
{
    if (g_hookOn < 0)
        g_hookOn = ShConfigGetBool("loader", "overlay_hook", 1) ? 1 : 0;
    if (g_subclassOn < 0)
        g_subclassOn = ShConfigGetBool("loader", "overlay_subclass", 1) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * One swapchain, one table
 *
 * Present and ResizeBuffers used to be written into the swapchain's vtable in
 * place, and that table is the driver's: every swapchain in the process shares
 * it. So writing it put this file into the present path of every swapchain in
 * the process - another module's overlay UI, the game's own second window,
 * and (measured on the field machine of 2026-09-25) NVIDIA's present
 * interception, which keeps a swapchain of its own on a window classed
 * "InvisibleWindowClassNvPresent", created 0.3 s behind the game's on every
 * start. On that machine a session with the shared table patched froze before
 * its first frame and one with the patch left out did not, over four runs.
 *
 * A table of our own is the fix: the shared array is not written to at all, a
 * copy is made once, and only the swapchains that are on the game's own window
 * are pointed at it. Nothing else in the process is then in this file's path -
 * not merely left alone, never reached - and a swapchain that is not ours
 * cannot be disturbed by the fact that the overlay is up.
 *
 * The copy is read through VirtualQuery rather than assumed: a vtable sits in
 * a read-only section whose end is not marked, and the entries past the
 * interface's own size are not ours to read.
 */
#define OVL_VTBL_MAX 48
static void* g_swapVtbl[OVL_VTBL_MAX];
static int   g_swapVtblN = 0;

static int VtblCopy(void** from, void** to)
{
    MEMORY_BASIC_INFORMATION mi;
    uint64_t start = (uint64_t)(uintptr_t)from, room, i;

    if (!from) return 0;
    if (!VirtualQuery(from, &mi, sizeof mi)) return 0;
    if (mi.State != MEM_COMMIT) return 0;
    room = ((uint64_t)(uintptr_t)mi.BaseAddress + (uint64_t)mi.RegionSize -
            start) / sizeof(void*);
    if (room > (uint64_t)OVL_VTBL_MAX) room = (uint64_t)OVL_VTBL_MAX;
    for (i = 0; i < room; i++) to[i] = from[i];
    return (int)room;
}

/* Which table each of the swapchain's later interfaces answers through, and
 * whether it is the copy this file installed. DXGI is free to hand a later
 * interface a different array, and a game that presents through one this file
 * never touched would look exactly like a game that never presented at all -
 * one session of 2026-09-25 did, and the log was the only way to tell the two
 * apart. Written once, at install time. */
static void LogInterfaceTables(IDXGISwapChain* swap)
{
    char buf[224] = "";
    const IID* iids[3] = { &__uuidof(IDXGISwapChain1),
                           &__uuidof(IDXGISwapChain2),
                           &__uuidof(IDXGISwapChain3) };
    const char* names[3] = { "1", "2", "3" };

    for (int i = 0; i < 3; i++)
    {
        IUnknown* p = nullptr;

        if (SUCCEEDED(swap->QueryInterface(*iids[i], (void**)&p)) && p)
        {
            void** vt = *(void***)p;
            snprintf(buf + strlen(buf), sizeof buf - strlen(buf),
                     "%s=%p%s ", names[i], (void*)vt,
                     vt == (void**)g_swapVtbl ? "(ours)" : "(not ours)");
            p->Release();
        }
    }
    OvlLogFloor("swapchain tables: installed=%p %s", (void*)g_swapVtbl, buf);
}

/* The swapchain this file hooked, watched for a few seconds after the install.
 *
 * The table pointer is written once and something outside this file can write
 * it again: an overlay or a driver that wraps a swapchain does exactly that,
 * and the session that follows looks like one where Present was never called
 * at all. That is what the report of 2026-09-25 21:12 was - the game ran to
 * its main menu, all three later interfaces answered through this file's table
 * at install time, and neither present slot was ever entered. So the pointer
 * is re-read for ten seconds, and any change is written down with the module
 * that owns the new table. The answer either way is written down, so a quiet
 * log cannot be mistaken for a check that never ran.
 *
 * g_swapPtr itself is declared with the other hook state, above the Present
 * body: the present line reads it too. */
static DWORD WINAPI VtableWatchThread(LPVOID)
{
    for (int i = 0; i < 20; i++)
    {
        void* s = (void*)InterlockedCompareExchangePointer(&g_swapPtr, nullptr,
                                                           nullptr);

        if (s)
        {
            void** now = *(void***)s;

            if (now != (void**)g_swapVtbl)
            {
                char own[64];

                OvlLogFloor("swapchain table changed after install: now %p (%s), "
                            "ours was %p - whatever wrapped it is in front of the "
                            "hooks", (void*)now,
                            OwnerName(now, own, (int)sizeof own),
                            (void*)g_swapVtbl);
                return 0;
            }
        }
        Sleep(500);
    }
    OvlLogFloor("swapchain table is still ours after 10 s (nothing wrapped it)");
    return 0;
}

static bool PatchSwapChainVtable(IDXGISwapChain* swap, const char* how)
{
    void** vtbl = *(void***)swap;
    bool patched = false;

    /* Watched, not hooked - see the switch note above. Nothing is written,
     * and g_origPresent stays null, so the init thread's two routes both see
     * an unhooked swapchain and leave it that way. */
    OvlReadSwitches();
    if (!g_hookOn)
    {
        OvlLogFloor("hooks NOT installed (%s): [loader] overlay_hook=0 - the "
                    "swapchain is watched, its vtable is left alone", how);
        return false;
    }

    if (InterlockedCompareExchange(&g_vtablePatched, 0, 0))
    {
        /* Already ours: a swapchain that came later is pointed at the same
         * copy, so the overlay stays on it too. */
        *(void***)swap = g_swapVtbl;
        return g_origPresent != nullptr;
    }

    g_origPresent  = (PresentFn)vtbl[8];
    g_origPresent1 = (Present1Fn)vtbl[18];
    g_origResize   = (ResizeFn)vtbl[13];

    /* The shared table is left alone - see the note above the copy. Slot 18
     * (Present1) is the last one this needs, so a table that cannot be read
     * that far is not hooked at all rather than half hooked. */
    g_swapVtblN = VtblCopy(vtbl, g_swapVtbl);
    if (g_swapVtblN > 18)
    {
        g_swapVtbl[8]  = (void*)&HookPresent;       /* IDXGISwapChain::Present   */
        g_swapVtbl[18] = (void*)&HookPresent1;      /* IDXGISwapChain1::Present1 */
        g_swapVtbl[13] = (void*)&HookResizeBuffers;
        *(void***)swap = g_swapVtbl;    /* this swapchain, and no other */
        patched = true;
        LogInterfaceTables(swap);
        /* And the code hook, for the presents this table never sees - see the
         * note above InstallPresentCodeHook. Armed here because this is where
         * the address it needs comes from. */
        InstallPresentCodeHook();
        /* Watched for a while: see the note on the watch thread. Started
         * once, on the first swapchain hooked - which is the one whose
         * presents are wanted. */
        InterlockedExchangePointer(&g_swapPtr, swap);
        {
            static volatile LONG watchStarted;

            if (InterlockedExchange(&watchStarted, 1) == 0)
            {
                HANDLE t = CreateThread(nullptr, 0, VtableWatchThread, nullptr,
                                        0, nullptr);
                if (t) CloseHandle(t);
            }
        }
    }
    else
    {
        /* Nothing was written, so neither entry point is ours. Report it
         * rather than logging a success the overlay can never live up to. */
        g_origPresent = nullptr;
        g_origResize  = nullptr;
    }
    if (patched)
        InterlockedExchange(&g_vtablePatched, 1);
    {
        char ownP[64], ownR[64];

        OvlLogFloor("hooks installed (%s): origPresent=%llx (%s) origResize=%llx "
               "(%s) ok=%d - own table of %d entries, the shared one untouched",
               how,
               (unsigned long long)(uintptr_t)g_origPresent,
               OwnerName((const void*)g_origPresent, ownP, (int)sizeof ownP),
               (unsigned long long)(uintptr_t)g_origResize,
               OwnerName((const void*)g_origResize, ownR, (int)sizeof ownR),
               patched ? 1 : 0, g_swapVtblN);
    }
    return patched && g_origPresent != nullptr;
}

/* A swapchain the game just created: hook it when it is on the game's own
 * window, and remember that window, so the init thread subclasses the window
 * the renderer actually draws into instead of guessing by size.
 *
 * A swapchain that is on some other window - another module's overlay UI, the
 * game's own second window - is logged and otherwise left entirely alone: not
 * hooked, and it does not become the window to attach to. That is what keeps
 * another module's swapchain out of this file's path altogether, together with
 * the table of our own in PatchSwapChainVtable. A swapchain with no window at
 * all (an offscreen one) is hooked, because nothing tells it apart from the
 * game's; the Present guard passes its frames through untouched. */
static void OnSwapChainSeen(IDXGISwapChain* swap, const char* how)
{
    DXGI_SWAP_CHAIN_DESC d = {};
    char cls[80] = "";
    HWND on = nullptr;
    int  ours;

    if (!swap) return;
    if (SUCCEEDED(swap->GetDesc(&d))) on = d.OutputWindow;
    if (on && !GetClassNameA(on, cls, (int)sizeof cls)) cls[0] = '\0';
    ours = !on || WindowIsOurs(on);
    if (ours)
    {
        if (on) InterlockedExchangePointer(&g_capturedHwnd, (PVOID)on);
        InterlockedExchange(&g_captured, 1);
        PatchSwapChainVtable(swap, how);
    }
    OvlLogFloor("swapchain captured (%s): hwnd=%llx class '%s' buffer %ux%u "
           "windowed=%d%s", how, (unsigned long long)(uintptr_t)on, cls,
           (unsigned)d.BufferDesc.Width, (unsigned)d.BufferDesc.Height,
           d.Windowed ? 1 : 0,
           ours ? "" : " - not our window, left alone");
}

typedef HRESULT(STDMETHODCALLTYPE* FactoryCreateSwapChainFn)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* FactoryCreateSwapChainForHwndFn)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

struct FactoryHook
{
    void** vt;
    FactoryCreateSwapChainFn        origCreate;
    FactoryCreateSwapChainForHwndFn origForHwnd;
};
static FactoryHook g_factoryHooks[8];
static int          g_factoryHookCount = 0;

/* The record for a vtable, no questions asked. */
static FactoryHook* FactoryRecord(void** vt)
{
    for (int i = 0; i < g_factoryHookCount; i++)
        if (g_factoryHooks[i].vt == vt) return &g_factoryHooks[i];
    return nullptr;
}

/* The record for a vtable, but only if it can hand over the method the
 * caller is standing in for. A record that cannot is a hook whose only
 * answer is E_FAIL - see the note above PatchFactoryVtable for the session
 * that was read through a black screen because of it. */
static FactoryHook* FactoryRecordFor(void** vt, int needForHwnd)
{
    FactoryHook* h = FactoryRecord(vt);

    if (!h) return nullptr;
    if (needForHwnd) return h->origForHwnd ? h : nullptr;
    return h->origCreate ? h : nullptr;
}

static HRESULT STDMETHODCALLTYPE HookFactoryCreateSwapChain(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
static HRESULT STDMETHODCALLTYPE HookFactoryCreateSwapChainForHwnd(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);

/* Present=10 on every version, CreateSwapChainForHwnd=15 (v2 and up).
 * Slot 16 (CreateSwapChainForCoreWindow) is deliberately left alone: this
 * overlay cannot say which window such a swapchain ends up on, and answering
 * a call we do not understand is worse than not answering it at all.
 *
 * One record per vtable array, and each slot taken once. The v1 and v2
 * factories are the same object with the same vtable - measured, not
 * assumed: IDXGIFactory, IDXGIFactory1 and IDXGIFactory2 all answered the
 * same pointer (dxgi.dll+0xD3670 on the machine of 2026-09-17) - so the two
 * calls InstallFactoryCapture makes land on one array.
 *
 * That measurement is the fix. The first version recorded a row per call:
 * the v1 row (CreateSwapChain only, origForHwnd null) was written first, the
 * v2 row second, and the hook lookup returned the FIRST row matching the
 * vtable. So a CreateSwapChainForHwnd call walked into the v1 row, found no
 * original, and answered E_FAIL without ever calling it. A game that makes
 * its main swapchain that way - which is how a Win32 D3D11 game does it -
 * then has no swapchain to draw the loading screen into: the logos, whose
 * swapchain was created before the patch went in, are fine, the window is
 * up and black, the process spins and eventually gives up. That is the
 * report from 2026-09-17 23:51 on the development machine, and the same
 * shape as the one from the field.
 *
 * Two guards, then: a row is reused rather than duplicated, and a hook that
 * cannot find its original says so in the log instead of failing in
 * silence. */
static bool PatchFactoryVtable(void** vt, bool hasForHwnd)
{
    DWORD oldProtect = 0;
    FactoryHook* h = FactoryRecord(vt);
    bool isNew = false;

    if (!h)
    {
        if (g_factoryHookCount >= (int)(sizeof(g_factoryHooks) /
                                        sizeof(g_factoryHooks[0])))
        {
            OvlLogFloor("overlay: factory hook table is full (%d vtables) - "
                   "the game's own swapchain cannot be captured",
                   g_factoryHookCount);
            return false;
        }
        h = &g_factoryHooks[g_factoryHookCount];
        h->vt = vt;
        h->origCreate = nullptr;
        h->origForHwnd = nullptr;
        isNew = true;
    }

    if (!h->origCreate || (hasForHwnd && !h->origForHwnd))
    {
        if (!VirtualProtect(&vt[10], sizeof(void*) * 7, PAGE_READWRITE,
                            &oldProtect))
        {
            OvlLogFloor("overlay: factory vtable %llx is not writable - the "
                   "game's own swapchain cannot be captured",
                   (unsigned long long)(uintptr_t)vt);
            return false;
        }
        /* A slot that is already ours must never be recorded as its own
         * original: that would make the hook call itself. */
        if (!h->origCreate)
        {
            h->origCreate = (FactoryCreateSwapChainFn)vt[10];
            vt[10] = (void*)&HookFactoryCreateSwapChain;
        }
        if (hasForHwnd && !h->origForHwnd)
        {
            h->origForHwnd = (FactoryCreateSwapChainForHwndFn)vt[15];
            vt[15] = (void*)&HookFactoryCreateSwapChainForHwnd;
        }
        VirtualProtect(&vt[10], sizeof(void*) * 7, oldProtect, &oldProtect);
    }
    if (isNew)
        g_factoryHookCount++;      /* published only once the row is filled */
    OvlLogFloor("hooks installed (factory %llx): origCreate=%llx origForHwnd=%llx",
           (unsigned long long)(uintptr_t)vt,
           (unsigned long long)(uintptr_t)h->origCreate,
           (unsigned long long)(uintptr_t)h->origForHwnd);
    return true;
}

/* A refused call, said out loud, once. E_FAIL was silent before, which is
 * how a broken interception could look exactly like a game that never asked. */
static void FactoryCallRefused(const char* what, void** vt)
{
    static volatile LONG said;

    if (InterlockedExchange(&said, 1) == 0)
        OvlLogFloor("overlay: %s reached the hook with no original recorded "
               "(vtable %llx) - the game's own swapchain cannot be created",
               what, (unsigned long long)(uintptr_t)vt);
}

static HRESULT STDMETHODCALLTYPE HookFactoryCreateSwapChain(
    IDXGIFactory* self, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc,
    IDXGISwapChain** out)
{
    void** vt = self ? *(void***)self : nullptr;
    FactoryHook* h = FactoryRecordFor(vt, 0);
    HRESULT hr;

    if (!h)
    {
        FactoryCallRefused("CreateSwapChain", vt);
        return E_FAIL;
    }
    hr = h->origCreate(self, dev, desc, out);
    if (SUCCEEDED(hr) && out && *out) OnSwapChainSeen(*out, "CreateSwapChain");
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookFactoryCreateSwapChainForHwnd(
    IDXGIFactory2* self, IUnknown* dev, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* restrict,
    IDXGISwapChain1** out)
{
    void** vt = self ? *(void***)self : nullptr;
    FactoryHook* h = FactoryRecordFor(vt, 1);
    HRESULT hr;

    if (!h)
    {
        FactoryCallRefused("CreateSwapChainForHwnd", vt);
        return E_FAIL;
    }
    hr = h->origForHwnd(self, dev, hwnd, desc, fs, restrict, out);
    /* One place decides what to do with a new swapchain, whichever call made
     * it: OnSwapChainSeen hooks it only when it is on the game's own window,
     * and logs what it was. It reads the desc back, so the milestone line
     * carries the same fields on both paths. */
    if (SUCCEEDED(hr) && out && *out)
        OnSwapChainSeen(*out, "CreateSwapChainForHwnd");
    return hr;
}

/* Route 1. Called before the game creates anything, so every factory it makes
 * afterwards lands on a patched vtable. */
static bool InstallFactoryCapture()
{
    IDXGIFactory1* f1 = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f1);

    if (FAILED(hr) || !f1)
    {
        OvlLogFloor("overlay: CreateDXGIFactory1 failed (%08lX) - the probe device "
               "is the only route left", (unsigned long)hr);
        return false;
    }

    PatchFactoryVtable(*(void***)f1, false);            // IDXGIFactory1: slot 10
    {
        /* Every interface version, not just the two that were measured to
         * share one array: a game that creates its swapchain through, say,
         * IDXGIFactory4 would otherwise do it on an array this file never
         * patched, and a swapchain nobody captured is a swapchain nothing
         * hooks (2026-09-25: a game that ran to its main menu with neither
         * present slot ever entered). The rows are deduplicated by the array
         * pointer, so an interface answering through the same array as
         * another costs one compare. */
        IDXGIFactory2* f2 = nullptr;
        IDXGIFactory3* f3 = nullptr;
        IDXGIFactory4* f4 = nullptr;

        if (SUCCEEDED(f1->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2))
            && f2)
        {
            PatchFactoryVtable(*(void***)f2, true);     // + ForHwnd (15) / ForCoreWindow (16)
            f2->Release();
        }
        if (SUCCEEDED(f1->QueryInterface(__uuidof(IDXGIFactory3), (void**)&f3))
            && f3)
        {
            PatchFactoryVtable(*(void***)f3, true);     // an array of its own, if dxgi says so
            f3->Release();
        }
        if (SUCCEEDED(f1->QueryInterface(__uuidof(IDXGIFactory4), (void**)&f4))
            && f4)
        {
            PatchFactoryVtable(*(void***)f4, true);
            f4->Release();
        }
    }
    f1->Release();
    OvlLogFloor("overlay: factory capture installed (%d vtable(s)) - the game's own "
           "swapchain is the target; no probe device is created while it is up",
           g_factoryHookCount);
    return g_factoryHookCount > 0;
}

/* Route 2, kept for the case where the game never goes through a DXGI factory
 * we can see. Only ever called long after the render window is up, so the
 * second device it creates cannot race the game's first one. */
static bool InstallSwapChainHooksProbe()
{
    static const wchar_t kClass[] = L"SHOvlDummy";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    /* Step by step, because this is the one part of start up that has to
     * run while the game is doing the same thing. The field report of
     * 2026-09-17 has a fresh install crashing 0.35 s after the window was
     * found, and this function is everything that runs in between - a log
     * that stops mid-way names the step that died, and the silent returns
     * this used to have could not. */
    OvlLogFloor("overlay: probe device (fallback) - d3d11=%llx dxgi=%llx ntdll=%llx",
           (unsigned long long)(uintptr_t)GetModuleHandleA("d3d11.dll"),
           (unsigned long long)(uintptr_t)GetModuleHandleA("dxgi.dll"),
           (unsigned long long)(uintptr_t)GetModuleHandleA("ntdll.dll"));
    if (!RegisterClassExW(&wc))
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            OvlLog("overlay: RegisterClassExW failed (%lu)", GetLastError());
            return false;
        }

    HWND dummy = CreateWindowExW(0, kClass, L"SHOvlDummy", WS_OVERLAPPED,
                                 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!dummy)
    {
        OvlLog("overlay: CreateWindowExW failed (%lu)", GetLastError());
        return false;
    }
    OvlLog("overlay: probe window %llx, creating the device",
           (unsigned long long)(uintptr_t)dummy);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummy;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &sd, &swap, &dev, nullptr, &ctx);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &sd, &swap, &dev, nullptr, &ctx);
    if (FAILED(hr) || !swap)
    {
        OvlLogFloor("overlay: D3D11CreateDeviceAndSwapChain failed (%08lX), no overlay",
               (unsigned long)hr);
        if (swap) swap->Release();
        DestroyWindow(dummy);
        return false;
    }

    bool ok = PatchSwapChainVtable(swap, "probe");
    swap->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(dummy);
    return ok;
}

// ---------------------------------------------------------------------------
// find the game's render window
//
// Two answers, in the order they can be trusted:
//   1. the window of a swapchain the game itself created (route 1 above) -
//      by definition the window the renderer draws into;
//   2. a visible window of the engine's render class, titled like the game.
//
// Both are "the game's own render window", and nothing else is ever taken -
// there is no third answer and no budget. The window is waited for however
// long it takes, because the splash window comes first and can sit there for
// minutes, and settling on it is a subclass on a window that is gone once the
// intro ends.
//
// It used to be a timed scan - 60 s, then the largest visible window it had
// found. A default install plays its intro in full (~47 s), so on the field
// machine of 2026-09-17 the scan expired during the intro, the 466x310 splash
// window was subclassed, and a second D3D11 device was created while the
// engine was starting its own renderer. Waiting for the right window is what
// removes both halves of that.
// ---------------------------------------------------------------------------
/* kRenderClass and the splash class live above the Present hook: the swapchain
 * test reads them as well, and both places must agree on which window is
 * ours. */
static const DWORD   kProbeGraceMs   = 20000;    /* after the window is up */
static const DWORD   kSettleMs       = 1500;

static HWND g_foundWindow = nullptr;
static int  g_foundW = 0, g_foundH = 0;
static int  g_foundByClass = 0;

static int IsRenderClass(HWND h)
{
    wchar_t cls[64];

    if (!h || GetClassNameW(h, cls, 64) <= 0) return 0;
    return _wcsicmp(cls, kRenderClass) == 0;
}

/* A window worth attaching ImGui to: the engine's render window, or one that
 * is at least big enough to be a render window rather than the splash. */
static int LooksLikeRender(HWND h, int w, int hh)
{
    if (IsRenderClass(h)) return 1;
    return w >= 640 && hh >= 480;
}

static BOOL CALLBACK FindWindowCb(HWND h, LPARAM lp)
{
    DWORD owner = 0;
    GetWindowThreadProcessId(h, &owner);
    if (owner != (DWORD)(uintptr_t)lp)
        return TRUE;
    if (!IsWindowVisible(h))
        return TRUE;
    RECT rc;
    if (!GetClientRect(h, &rc))
        return TRUE;
    int w = rc.right - rc.left, ht = rc.bottom - rc.top;
    if (w < 64 || ht < 64)
        return TRUE;
    /* Only the game's own render window is a candidate - the splash carries a
     * class of its own and a title whose registered mark is mis-encoded, and
     * the overlay must never hang off it, not even when it is the only window
     * there is. Nothing else is recorded: there is no fallback left to fill,
     * and the init thread waits for this one window however long the splash
     * sits there. See the note on which window is ours. */
    if (IsRenderClass(h) && WindowIsOurs(h) &&
        (!g_foundByClass || w * ht > g_foundW * g_foundH))
    {
        g_foundWindow = h;
        g_foundW = w;
        g_foundH = ht;
        g_foundByClass = 1;
    }
    return TRUE; // keep scanning for a better one
}

static HWND FindGameWindow()
{
    g_foundWindow = nullptr;
    g_foundW = g_foundH = 0;
    g_foundByClass = 0;
    EnumWindows(FindWindowCb, (LPARAM)(uintptr_t)GetCurrentProcessId());
    return g_foundWindow;
}

static HWND CapturedWindow()
{
    return (HWND)InterlockedCompareExchangePointer(&g_capturedHwnd, nullptr,
                                                   nullptr);
}

// ---------------------------------------------------------------------------
// init thread
// ---------------------------------------------------------------------------
static DWORD WINAPI InitThread(LPVOID)
{
    DWORD start = GetTickCount();
    HWND  w = nullptr;
    int   choseByCapture = 0;

    if (!g_imeLockReady) {
        InitializeCriticalSection(&g_imeLock);
        g_imeLockReady = 1;
    }

    /* [loader] overlay: 0 takes the whole overlay out of the session. The
     * answer arrives on the loader thread once the config is up; a few
     * seconds of waiting covers it, and no answer at all means on, which is
     * the default. */
    for (int i = 0; i < 100 && !InterlockedCompareExchange(&g_ovlAllow, 0, 0); i++)
        Sleep(50);
    if (InterlockedCompareExchange(&g_ovlAllow, 0, 0) < 0)
    {
        OvlLogFloor("overlay: off ([loader] overlay=0) - no window scan, no "
                    "capture, no Present hook, the game runs untouched");
        return 0;
    }

    /* Route 1 has to be in place before the game creates its first
     * swapchain, which it does within seconds of start up - and this thread
     * starts while the host is still resolving its imports under the loader
     * lock, where creating a factory would block. One second is long before
     * the game's first window (four seconds in, measured on both the field
     * machine and this one) and long after the loader is done. */
    Sleep(1000);
    InstallFactoryCapture();

    for (;;)
    {
        DWORD waited = GetTickCount() - start;

        /* The captured window is the game's own render window, so it is
         * taken as soon as it is one - but not when it is the splash
         * window, which is a swapchain too and comes first. WindowIsOurs
         * says which of the two this is; the size test stays for the build
         * whose windows are neither (see the note on which window is ours). */
        w = CapturedWindow();
        if (w && IsWindow(w) && WindowIsOurs(w))
        {
            RECT rc = {};
            int ww = 0, hh = 0;
            if (GetClientRect(w, &rc)) { ww = rc.right; hh = rc.bottom; }
            if (LooksLikeRender(w, ww, hh))
            {
                g_hwnd = w;
                choseByCapture = 1;
                OvlLogFloor("init thread: render window %llx is the captured "
                       "swapchain's (%dx%d, title '%s')",
                       (unsigned long long)(uintptr_t)w, ww, hh,
                       TitleForLog(w));
                break;
            }
        }
        if (FindGameWindow() && g_foundByClass)
        {
            g_hwnd = g_foundWindow;
            OvlLogFloor("init thread: render window %llx by class, client "
                   "%dx%d, title '%s'",
                   (unsigned long long)(uintptr_t)g_hwnd, g_foundW, g_foundH,
                   TitleForLog(g_hwnd));
            break;
        }
        /* No budget and no fallback: the game's own window is waited for
         * however long it takes. The splash comes first and can sit there for
         * minutes, and settling on it is what the old budget used to do - a
         * subclass on a window that is gone once the intro ends. A machine
         * whose window never appears gets no overlay; the line below says what
         * was being waited for. */
        if ((waited % 5000) < 500)
            OvlLog("waiting for the render window (%lu s; captured=%llx, "
                   "found=%llx %dx%d)",
                   (unsigned long)(waited / 1000),
                   (unsigned long long)(uintptr_t)w,
                   (unsigned long long)(uintptr_t)g_foundWindow, g_foundW,
                   g_foundH);
        Sleep(500);
    }

    /* Subclass, so ImGui gets the keyboard and the IME work has a target.
     * Route 1 already hooked Present on the game's own swapchain, so there
     * is no device to create and nothing to race here.
     *
     * The loop above only ever leaves with the game's own render window in
     * hand, so there is no "no window" case left to handle: a window that
     * died between those two lines makes SetWindowLongPtrW fail, which is
     * logged as NOT installed, and the Present guard then keeps the overlay
     * off the screen - see the note on which window is ours. */
    OvlReadSwitches();
    if (g_subclassOn)
    {
        g_origWndProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                                                   (LONG_PTR)SubWndProc);
        OvlLogFloor("init thread: subclass %s (previous proc %llx) (%s)",
               g_origWndProc ? "installed" : "NOT installed",
               (unsigned long long)(uintptr_t)g_origWndProc,
               choseByCapture ? "captured swapchain" : "render class");
    }
    else
        OvlLogFloor("init thread: subclass NOT installed ([loader] "
               "overlay_subclass=0) - the keyboard stays the game's (diagnostic)");

    /* Observe only: no Present hook and no probe device, so nothing in this
     * process is in the swapchain's path at all. Everything up to here - the
     * factory patch, the window, the capture log - still ran, which is the
     * point: what a freeze reports is then the half that was turned off. */
    if (!g_hookOn)
    {
        OvlLogFloor("overlay: observe only ([loader] overlay_hook=0) - nothing "
               "hooks the swapchain, the game runs untouched and F4 does "
               "nothing (diagnostic)");
        return 0;
    }

    if (InterlockedCompareExchange(&g_captured, 0, 0) &&
        g_origPresent != nullptr)
    {
        OvlLogFloor("overlay: the captured swapchain carries the hooks - no probe "
               "device is created at all");
        return 0;
    }

    /* Route 2, and only here: the render window is up, so the game's own
     * renderer has had its chance to create its device, and the probe
     * cannot race it any more. */
    OvlLogFloor("overlay: no swapchain captured yet - waiting %lu ms before the "
           "probe device (fallback)", (unsigned long)kProbeGraceMs);
    Sleep(kProbeGraceMs);
    if (InterlockedCompareExchange(&g_captured, 0, 0) &&
        g_origPresent != nullptr)
    {
        OvlLogFloor("overlay: the capture arrived while waiting - no probe device");
        return 0;
    }
    for (int attempt = 0; attempt < 3; attempt++)
    {
        if (InstallSwapChainHooksProbe())
            return 0;
        OvlLogFloor("overlay: probe install failed (attempt %d of 3), retrying "
               "in 1s", attempt + 1);
        Sleep(1000);
    }
    OvlLogFloor("overlay: disabled - neither route could hook the swap chain; the "
           "game runs untouched and F4 does nothing");
    return 0;
}

// The loader (loader.c) owns DllMain. Start the overlay thread
// from a static initialiser instead, so no extra entry point or
// loader change is needed: it runs while the DLL loads and the
// thread sleeps before doing any real work.
namespace {
struct OvlStartup
{
    OvlStartup()
    {
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
};
static OvlStartup g_startup;
} // namespace
