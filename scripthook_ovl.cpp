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
// Hook technique: create a dummy D3D11 device+swapchain, read the
// shared IDXGISwapChain vtable, patch Present (index 8) and
// ResizeBuffers (index 13). All swapchains of the same driver
// share that vtable, so the game's swapchain is hooked too.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <string.h>
#include <imm.h>
#include <vector>
#pragma comment(lib, "imm32.lib")

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#define SH_BUILD 1
#include "scripthook.h"
#include "log.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

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

// ---------------------------------------------------------------------------
// hooked swapchain methods
// ---------------------------------------------------------------------------
typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* ResizeFn)(IDXGISwapChain*, UINT, UINT, UINT,
                                             DXGI_FORMAT, UINT);

static PresentFn g_origPresent = nullptr;
static ResizeFn  g_origResize = nullptr;

static ID3D11Device*        g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dContext = nullptr;
static HWND   g_hwnd = nullptr;
static WNDPROC g_origWndProc = nullptr;
static volatile LONG g_ready = 0;

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
 * the window and force the IME open, so composition messages arrive. */
static int g_imeProbed = 0;   /* one attempt per chat session */

static void ImeProbeEnable(HWND hWnd)
{
    HIMC hImc;
    DWORD err;

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
            OvlLog("ime probe: created fresh context %p", (void*)hImc);
        } else {
            err = GetLastError();
            OvlLog("ime probe: ImmCreateContext failed err=%u", err);
            return;
        }
    }

    /* 2. Force the IME open on that context. */
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

#define CHAT_ANCHOR_Y_RATIO 0.72f
#define CHAT_ANCHOR_H       56.0f   /* CHAT_H, same as the render box */
#define CHAT_ANCHOR_GAP     6.0f    /* px below the box */

static void ImeApplyAnchor(HWND hWnd)
{
    HIMC hImc;
    POINT pt;
    RECT rc;
    LONG cx, cy;

    if (!hWnd || !IsWindow(hWnd)) return;
    if (!GetClientRect(hWnd, &rc)) return;

    cx = (rc.right - rc.left) / 2;
    cy = (LONG)((rc.bottom - rc.top) * CHAT_ANCHOR_Y_RATIO)
         + (LONG)CHAT_ANCHOR_H + (LONG)CHAT_ANCHOR_GAP;

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
    OvlLog("ime anchor: client=%dx%d caret=(%ld,%ld) screen=(%ld,%ld) imc=%p",
           rc.right - rc.left, rc.bottom - rc.top,
           cx, cy, (long)pt.x, (long)pt.y, (void*)hImc);
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
    RECT rc;
    POINT pt;
    if (!g_hwnd || !IsWindow(g_hwnd)) return;
    if (!GetClientRect(g_hwnd, &rc)) return;
    pt.x = (rc.right - rc.left) / 2;
    pt.y = (LONG)((rc.bottom - rc.top) * CHAT_ANCHOR_Y_RATIO)
           + (LONG)CHAT_ANCHOR_H + (LONG)CHAT_ANCHOR_GAP;
    ClientToScreen(g_hwnd, &pt);
    g_imeAnchorX = pt.x;
    g_imeAnchorY = pt.y;
}

/* UTF-16 -> UTF-8 into dst (dstSize bytes, NUL terminated). */
static void W2U8(const wchar_t *w, char *dst, int dstSize)
{
    if (!w || !dst || dstSize <= 0) return;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, dst, dstSize, NULL, NULL);
}

/* Read the IME composition string and current candidate page. */
static void ImeReadState(HWND hWnd)
{
    HIMC hImc;
    if (!hWnd || !IsWindow(hWnd)) return;
    hImc = ImmGetContext(hWnd);
    if (!hImc) return;

    /* --- composition string --- */
    {
        LONG n = ImmGetCompositionStringW(hImc, GCS_COMPSTR, NULL, 0);
        if (n > 0) {
            int wn = (int)(n / sizeof(wchar_t)) + 1;
            wchar_t *buf = (wchar_t *)malloc((size_t)wn * sizeof(wchar_t));
            if (buf) {
                ImmGetCompositionStringW(hImc, GCS_COMPSTR, buf,
                                         (DWORD)(wn * sizeof(wchar_t)));
                buf[wn - 1] = 0;
                ImeLock();
                W2U8(buf, g_ime.comp, sizeof(g_ime.comp));
                g_ime.active = 1;
                g_ime.gen++;
                ImeUnlock();
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
                int count = (int)cl->dwCount;
                int page  = (int)cl->dwPageStart;
                int sel   = (int)cl->dwSelection;
                int pageSize = (int)cl->dwPageSize;
                if (pageSize <= 0 || pageSize > count - page)
                    pageSize = count - page;
                if (pageSize > IME_CAND_MAX) pageSize = IME_CAND_MAX;

                ImeLock();
                g_ime.candOpen = 1;
                g_ime.candCount = count;
                g_ime.candSel = sel;
                g_ime.candPage = page;
                g_ime.candShow = pageSize;
                for (int i = 0; i < pageSize; i++) {
                    DWORD ofs = cl->dwOffset[page + i];
                    const wchar_t *w = (const wchar_t *)(raw.data() + ofs);
                    W2U8(w, g_ime.cand[i], sizeof(g_ime.cand[i]));
                }
                g_ime.gen++;
                ImeUnlock();
            }
        }
    }
    ImmReleaseContext(hWnd, hImc);
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
            if (ShChatGetCandMode() == 0) {
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
static void ImeHideForeignWindows(void)
{
    ImeUpdateAnchor();
    EnumWindows(ImeForeignEnum, 0);
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
    /* Chat box just opened: one IME-enable attempt per session. */
    if (g_ready && ShChatIsOpen()) {
        int selfDrawn = (ShChatGetCandMode() == 0); /* 0=overlay-drawn */

        if (!g_imeProbed) {
            g_imeProbed = 1;
            ImeProbeEnable(hWnd);
            if (selfDrawn) ImePlaceCaret(hWnd);
            else ImeApplyAnchor(hWnd);
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
        g_imeProbed = 0;   /* re-arm for the next chat session */
        HideCaret(hWnd);
        if (g_ime.active || g_ime.candOpen) ImeStateReset();
    }

    /* The Chinese chat box consumes the keyboard while it is up:
     * WM_CHAR/WM_IME_CHAR feed its text buffer, navigation keys are
     * swallowed so the game's own chat field never sees them. */
    if (g_ready && ShChatIsOpen() &&
        ShChatWndMsg((uint64_t)(uintptr_t)hWnd, (uint32_t)msg,
                     (uint64_t)wParam, (uint64_t)lParam))
        return 1;
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

const float MENU_X  = 16.0f;
const float MENU_Y  = 16.0f;
const float MENU_W  = 520.0f;
const float PAD     = 16.0f;  // horizontal + bottom padding
const float PAD_TOP = 0.0f;
const float TITLE_H = 40.0f;
const float ROW_H   = 34.0f;
// Selection bar: centred on the row's text box (see RenderMenu), no
// manual offset - same scheme as the chat candidate rows.
const float BAR_H   = 26.0f;
const float VALUE_W = 130.0f;
// Menu text uses the same bold CJK font as the chat box, ~24px.
const float MENU_FS       = 24.0f;  /* row text  */
const float MENU_TITLE_FS = 28.0f;  /* title     */
const float MENU_HINT_FS  = 17.0f;  /* hints     */
const float MENU_HINT_LH  = 24.0f;
const float MENU_HINT_GAP = 6.0f;
const float MENU_CREDIT_FS = 14.0f;

// 0xRRGGBB -> ImU32 (0xAABBGGRR)
ImU32 Col(uint32_t rgb, int a = 255)
{
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a);
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

void RenderMenu(const ShMenuView* v)
{
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // Same bold CJK font as the chat box; falls back to the default.
    ImFont* font = g_chatFont ? g_chatFont : ImGui::GetFont();
    const float fs = MENU_FS, titleFs = MENU_TITLE_FS, hintFs = MENU_HINT_FS;
    const float hintLh = MENU_HINT_LH, hintGap = MENU_HINT_GAP;
    const float x = MENU_X, y = MENU_Y;
    int i;

    // Control hints under the title: up to two \n-separated lines.
    int hintLines = 0;
    for (const char* hl = v->hint; *hl; ) {
        hintLines++;
        hl = strchr(hl, '\n');
        if (!hl) break;
        hl++;
    }
    if (hintLines > 2) hintLines = 2;
    // Each hint line occupies a hintFs-tall text box; the first row
    // starts just below the last hint line's box.
    float hintH = 0.0f;
    if (hintLines)
        hintH = hintGap + (float)(hintLines - 1) * hintLh + hintFs;

    float h = PAD_TOP + TITLE_H + hintH + ROW_H * (float)v->rows + PAD;
    if (v->footer[0]) h += ROW_H;
    if (v->status[0]) h += ROW_H;

    // Panel: translucent rounded quad.
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + MENU_W, y + h),
                      IM_COL32(0, 0, 0, 204), 6.0f);
    // Root menu top-right credits keep their own width; the title is
    // shrunk (never below 18px) if it would otherwise run into them.
    float rightX = x + MENU_W - PAD;
    float creditW = 0.0f, cFs = MENU_CREDIT_FS, cGap = 2.0f;
    ImVec2 s1, s2;
    if (v->isRoot) {
        const char* c1 = "原作者：Phiality · 魔改：GameXueRen";
        const char* c2 = "版本：Beta1.0 · Q群：299177445";
        s1 = font->CalcTextSizeA(cFs, FLT_MAX, 0.0f, c1);
        s2 = font->CalcTextSizeA(cFs, FLT_MAX, 0.0f, c2);
        creditW = s1.x > s2.x ? s1.x : s2.x;
    }
    float tFs = titleFs;
    float tLimit = creditW > 0.0f ? rightX - creditW - 10.0f
                                  : rightX - PAD;
    while (tFs > 18.0f &&
           font->CalcTextSizeA(tFs, FLT_MAX, 0.0f, v->title).x > tLimit)
        tFs -= 2.0f;
    // Title, vertically centred in the title band by its text TOP
    // (AddText pos.y is the line top, same convention as the chat box).
    float ttop = TextTopForRow(font, tFs, y + PAD_TOP, TITLE_H);
    dl->AddText(font, tFs, ImVec2(x + PAD, ttop),
                Col(0xFFD25Au), v->title);
    // Root menu top-right credits: original author + this build.
    if (v->isRoot) {
        const char* c1 = "原作者：Phiality · 魔改：GameXueRen";
        const char* c2 = "版本：Beta1.0 · Q群：299177445";
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
            char buf[128];
            const char* nl = strchr(hl, '\n');
            size_t n = nl ? (size_t)(nl - hl) : strlen(hl);
            if (n >= sizeof(buf)) n = sizeof(buf) - 1;
            memcpy(buf, hl, n);
            buf[n] = 0;
            dl->AddText(font, hintFs, ImVec2(x + PAD, hy),
                        Col(0x8C9BA8u), buf);
            hy += hintLh;
            li++;
            hl = nl ? nl + 1 : hl + n;
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
                          Col(0x28465Au, 230), 3.0f);
    }
    // Rows: name left, value right-aligned in its column.
    for (i = 0; i < v->rows; i++) {
        const ShMenuRow* r = &v->row[i];
        float ry = top + ROW_H * (float)i;
        float rTop = TextTopForRow(font, fs, ry, ROW_H);
        ImU32 c = r->selected ? Col(0x8CF0FFu) : Col(0xD2D2D2u);
        dl->AddText(font, fs, ImVec2(x + PAD + 8.0f, rTop), c, r->name);
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

// Chinese chat input box: a single-line field, lower middle.
// The text comes from scripthook_cnchat.c (already UTF-8); this
// only draws the snapshot plus a blinking caret at the end.
// Larger bold font for readability: the chat UI uses g_chatFont
// (msyh bold) and a 24px body size.
const float CHAT_W_MAX = 720.0f;
const float CHAT_H     = 56.0f;
const float CHAT_PAD   = 14.0f;
const float CHAT_FS    = 24.0f;   /* input text size                */
const float HINT_FS    = 17.0f;   /* hint line above the box        */

// IME UI metrics: candidate rows below the input box.
const float CAND_FS   = 22.0f;
const float CAND_ROW  = 34.0f;
const float CAND_PADX = 12.0f;

/* Snapshot the IME state for one frame of drawing. */
static void ImeSnapshot(ImeState* out)
{
    memset(out, 0, sizeof(*out));
    ImeLock();
    *out = g_ime;
    ImeUnlock();
}

void RenderChat(const ShChatView* v)
{
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // Chat text uses the bold CJK font (falls back to the default).
    ImFont* font = g_chatFont ? g_chatFont : ImGui::GetFont();
    const float fs = CHAT_FS;
    const float wpx = ImGui::GetIO().DisplaySize.x;
    const float hpx = ImGui::GetIO().DisplaySize.y;

    if (!v || !v->open) return;

    /* IME composition string (the pinyin/characters not committed yet)
     * renders right after the confirmed text so the user sees both.
     * Only in overlay-drawn mode: with the input method's own window
     * the system IME draws its composition and candidate UI itself. */
    ImeState ime;
    ImeSnapshot(&ime);
    bool selfDrawn = (ShChatGetCandMode() == 0);
    const char* comp = (selfDrawn && ime.active) ? ime.comp : "";

    // Width hugs text + composition, capped.
    ImVec2 tsz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, v->text);
    ImVec2 csz = comp[0] ? font->CalcTextSizeA(fs, FLT_MAX, 0.0f, comp)
                         : ImVec2(0, 0);
    float tw = tsz.x + csz.x + CHAT_PAD * 2.0f;
    if (tw < 240.0f) tw = 240.0f;
    if (tw > CHAT_W_MAX) tw = CHAT_W_MAX;

    float y = hpx * 0.72f;
    float x = (wpx - tw) * 0.5f;

    // Panel: translucent rounded quad centred horizontally.
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + tw, y + CHAT_H),
                      IM_COL32(0, 0, 0, 210), 6.0f);

    // Hint line above the field text (same bold font, smaller size).
    if (v->hint[0]) {
        dl->AddText(font, HINT_FS, ImVec2(x + CHAT_PAD, y - HINT_FS - 10.0f),
                    Col(0x8CF0FFu), v->hint);
    }

    // Text sits vertically centred in the box by its TOP (AddText's
    // pos.y is the text top in this ImGui, not a baseline).  The caret
    // is drawn from the same top so text and caret line up.
    float ttop = TextTopForRow(font, fs, y, CHAT_H);
    float tlh  = TextLineHeight(font, fs);
    dl->AddText(font, fs, ImVec2(x + CHAT_PAD, ttop),
                Col(0xF0F0F0u), v->text);

    // Composition string in a brighter tone right after the text.
    float compX = x + CHAT_PAD + tsz.x;
    if (comp[0])
        dl->AddText(font, fs, ImVec2(compX, ttop),
                    Col(0xA8EFC0u), comp);

    // Caret: after text + composition, same height as the text box.
    bool on = ((GetTickCount() / 400) & 1) != 0;
    if (on) {
        float cx = x + CHAT_PAD + tsz.x + csz.x + 2.0f;
        if (cx < x + tw - 4.0f)
            dl->AddRectFilled(ImVec2(cx, ttop + 1.0f),
                              ImVec2(cx + 2.5f, ttop + tlh - 1.0f),
                              Col(0x8CF0FFu));
    }

    // Candidate list below the box, self-drawn (system IME UI is off).
    // Skipped when the input method draws its own candidate window.
    if (selfDrawn && ime.candOpen && ime.candShow > 0) {
        int n = ime.candShow;
        float cw = 240.0f;
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
        cw = maxw + 28.0f;
        if (cw < 220.0f) cw = 220.0f;
        if (cw > CHAT_W_MAX) cw = CHAT_W_MAX;

        float cy = y + CHAT_H + 8.0f;
        float cx0 = (wpx - cw) * 0.5f;
        float chh = CAND_ROW * (float)n + 10.0f;
        if (cy + chh > hpx - 8.0f) cy = y - chh - 8.0f; /* flip above */

        dl->AddRectFilled(ImVec2(cx0, cy), ImVec2(cx0 + cw, cy + chh),
                          IM_COL32(16, 18, 22, 235), 6.0f);
        for (int i = 0; i < n; i++) {
            int absIdx = ime.candPage + i;
            bool sel = (absIdx == ime.candSel);
            float ry = cy + 5.0f + CAND_ROW * (float)i;
            /* Candidate text is centred in its row by its TOP, and the
             * highlight bar is drawn around that same centred box. */
            float ctop = TextTopForRow(font, CAND_FS, ry, CAND_ROW);
            if (sel) {
                float clh = TextLineHeight(font, CAND_FS);
                float hc  = ctop + clh * 0.5f;          /* text centre */
                float hh  = CAND_ROW - 8.0f;            /* bar height  */
                dl->AddRectFilled(ImVec2(cx0 + 4.0f, hc - hh * 0.5f),
                                  ImVec2(cx0 + cw - 4.0f, hc + hh * 0.5f),
                                  IM_COL32(0x28, 0x46, 0x5A, 220), 3.0f);
            }
            snprintf(num, sizeof(num), "%d.", i + 1);
            dl->AddText(font, CAND_FS,
                        ImVec2(cx0 + CAND_PADX, ctop),
                        sel ? Col(0x8CF0FFu) : Col(0x9AA4B0u), num);
            dl->AddText(font, CAND_FS,
                        ImVec2(cx0 + CAND_PADX + 32.0f, ctop),
                        sel ? Col(0xFFFFFFu) : Col(0xD8D8D8u),
                        ime.cand[i]);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// font loading: a CJK-capable system font so Chinese menu labels
// render (the game's own Phoenix font has no CJK glyphs).
// ---------------------------------------------------------------------------
static void LoadCjkFont()
{
    ImGuiIO& io = ImGui::GetIO();
    static const char* kCandidates[] = {
        "C:\\Windows\\Fonts\\msyh.ttc",   // 微软雅黑
        "C:\\Windows\\Fonts\\msyh.ttf",
        "C:\\Windows\\Fonts\\simhei.ttf", // 黑体
        "C:\\Windows\\Fonts\\simsun.ttc", // 宋体
    };
    /* Bold variant preferred for the chat UI.  msyhbd.ttc ships with
     * the YaHei family on every supported Windows release. */
    static const char* kBoldCandidates[] = {
        "C:\\Windows\\Fonts\\msyhbd.ttc", // 微软雅黑 Bold
        "C:\\Windows\\Fonts\\msyhbd.ttf",
    };
    ImFont* base = nullptr;
    for (const char* path : kCandidates)
    {
        base = io.Fonts->AddFontFromFileTTF(
            path, 16.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
        if (base)
        {
            io.FontDefault = base;
            OvlLog("font loaded: %s", path);
            break;
        }
    }
    if (!base)
    {
        OvlLog("WARNING: no CJK font loaded, Chinese text will not render");
        return;
    }
    for (const char* path : kBoldCandidates)
    {
        ImFont* b = io.Fonts->AddFontFromFileTTF(
            path, 16.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
        if (b)
        {
            g_chatFont = b;
            OvlLog("chat bold font loaded: %s", path);
            return;
        }
    }
    OvlLog("no bold font variant, chat UI falls back to normal weight");
    g_chatFont = base;
}

// ---------------------------------------------------------------------------
// Present hook
// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* pSwap, UINT sync, UINT flags)
{
    if (!g_ready)
    {
        ID3D11Device* dev = nullptr;
        if (SUCCEEDED(pSwap->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) && dev)
        {
            dev->GetImmediateContext(&g_pd3dContext);
            g_pd3dDevice = dev;

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr; // fixed layout, no .ini to save
            // Keyboard navigation stays with the menu thread
            // (scripthook_menu.c), ImGui only draws.

            ImGuiStyle& st = ImGui::GetStyle();
            st.WindowRounding = 6.0f;
            st.WindowBorderSize = 0.0f;

            if (ImGui_ImplWin32_Init(g_hwnd) &&
                ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dContext))
            {
                LoadCjkFont();
                ShMenuSetOverlayReady(1);
                InterlockedExchange(&g_ready, 1);
                OvlLog("imgui ready: hwnd=%llx device=%llx font=%p",
                       (unsigned long long)g_hwnd,
                       (unsigned long long)g_pd3dDevice,
                       (void*)ImGui::GetFont());
            }
            else
            {
                OvlLog("imgui init FAILED");
                ImGui_ImplDX11_Shutdown();
                ImGui_ImplWin32_Shutdown();
                ImGui::DestroyContext();
            }
        }
    }

    {
        bool drawMenu = ShMenuIsOpen() ? true : false;
        bool drawChat = drawMenu ? false
                                 : (ShChatIsOpen() ? true : false);
        /* While the chat box is up, periodically grab any candidate /
         * status window an IME creates in our process.  This runs on
         * the render thread: EnumWindows + SetWindowLongPtrW here never
         * blocks the game's message pump (unlike on the WndProc thread). */
        if (drawChat) {
            /* Periodically grab any candidate / status window an IME
             * creates in this process and steer it according to the
             * candidate mode (park off screen, or follow the chat
             * anchor - see ImeForeignProc). */
            static DWORD lastImeScan = 0;
            DWORD now = GetTickCount();
            if ((int)(now - lastImeScan) > 120) {
                lastImeScan = now;
                ImeHideForeignWindows();
            }
        }
        if (g_ready && (drawMenu || drawChat))
        {
            // Bind the swapchain back buffer as the render target so
            // the overlay is drawn on the surface that gets
            // presented, whatever the game left bound.
            ID3D11Texture2D* back = nullptr;
            ID3D11RenderTargetView* rtv = nullptr;
            DXGI_SWAP_CHAIN_DESC desc = {};
            if (SUCCEEDED(pSwap->GetDesc(&desc)) &&
                SUCCEEDED(pSwap->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                           (void**)&back)))
            {
                g_pd3dDevice->CreateRenderTargetView(back, nullptr, &rtv);
                back->Release();
            }
            if (rtv)
            {
                g_pd3dContext->OMSetRenderTargets(1, &rtv, nullptr);
                D3D11_VIEWPORT vp;
                vp.TopLeftX = 0; vp.TopLeftY = 0;
                vp.Width = (float)desc.BufferDesc.Width;
                vp.Height = (float)desc.BufferDesc.Height;
                vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
                g_pd3dContext->RSSetViewports(1, &vp);
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            // The swapchain size is authoritative for the draw area.
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2((float)desc.BufferDesc.Width,
                                    (float)desc.BufferDesc.Height);
            ImGui::NewFrame();

            if (drawMenu) {
                ShMenuView v;
                ShMenuCaptureView(&v);
                RenderMenu(&v);
            } else {
                ShChatView v;
                ShChatCapture(&v);
                RenderChat(&v);
            }

            ImGui::Render();
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            if (rtv)
            {
                g_pd3dContext->OMSetRenderTargets(0, nullptr, nullptr);
                rtv->Release();
            }

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

    return g_origPresent(pSwap, sync, flags);
}

// ---------------------------------------------------------------------------
// ResizeBuffers hook: rebuild ImGui device objects across resizes
// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* pSwap, UINT bc,
                                                   UINT w, UINT h, DXGI_FORMAT f,
                                                   UINT flags)
{
    if (g_ready)
        ImGui_ImplDX11_InvalidateDeviceObjects();
    HRESULT hr = g_origResize(pSwap, bc, w, h, f, flags);
    if (g_ready)
        ImGui_ImplDX11_CreateDeviceObjects();
    return hr;
}

// ---------------------------------------------------------------------------
// hook installation
// ---------------------------------------------------------------------------
static bool InstallSwapChainHooks()
{
    static const wchar_t kClass[] = L"SHOvlDummy";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc))
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;

    HWND dummy = CreateWindowExW(0, kClass, L"SHOvlDummy", WS_OVERLAPPED,
                                 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!dummy)
        return false;

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
        if (swap) swap->Release();
        DestroyWindow(dummy);
        return false;
    }

    // Patch the shared vtable: Present=8, ResizeBuffers=13.
    void** vtbl = *(void***)swap;
    g_origPresent = (PresentFn)vtbl[8];
    g_origResize  = (ResizeFn)vtbl[13];
    DWORD oldProtect = 0;
    if (VirtualProtect(&vtbl[8], sizeof(void*) * 6, PAGE_READWRITE, &oldProtect))
    {
        vtbl[8]  = (void*)&HookPresent;
        vtbl[13] = (void*)&HookResizeBuffers;
        VirtualProtect(&vtbl[8], sizeof(void*) * 6, oldProtect, &oldProtect);
    }

    swap->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(dummy);
    OvlLog("hooks installed: origPresent=%llx origResize=%llx ok=%d",
           (unsigned long long)(uintptr_t)g_origPresent,
           (unsigned long long)(uintptr_t)g_origResize,
           g_origPresent ? 1 : 0);
    return g_origPresent != nullptr;
}

// ---------------------------------------------------------------------------
// find the game's main window: the LARGEST visible window owned by
// this process. The game may show a small splash/loading window
// first, so the init thread retries until a real render window
// (>= 640x480) appears instead of subclassing a temporary one.
// ---------------------------------------------------------------------------
static HWND g_foundWindow = nullptr;
static int  g_foundW = 0, g_foundH = 0;

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
    if (!g_foundWindow || w * ht > g_foundW * g_foundH)
    {
        g_foundWindow = h;
        g_foundW = w;
        g_foundH = ht;
    }
    return TRUE; // keep scanning for a larger one
}

static HWND FindGameWindow()
{
    g_foundWindow = nullptr;
    g_foundW = g_foundH = 0;
    EnumWindows(FindWindowCb, (LPARAM)(uintptr_t)GetCurrentProcessId());
    return g_foundWindow;
}

// ---------------------------------------------------------------------------
// init thread
// ---------------------------------------------------------------------------
static DWORD WINAPI InitThread(LPVOID)
{
    // Wait for a real render window. The game may show a small
    // splash/loading window first; subclassing that would leave
    // the real window untouched and lose ImGui's input target.
    // Retry every second for up to a minute, then take whatever
    // the largest window is.
    for (int tries = 0; tries < 60; tries++)
    {
        g_hwnd = FindGameWindow();
        if (g_hwnd && g_foundW >= 640 && g_foundH >= 480)
            break;
        OvlLog("waiting for game window (%dx%d)...", g_foundW, g_foundH);
        Sleep(1000);
    }

    OvlLog("init thread: hwnd=%llx client %dx%d",
           (unsigned long long)g_hwnd, g_foundW, g_foundH);
    if (!g_imeLockReady) {
        InitializeCriticalSection(&g_imeLock);
        g_imeLockReady = 1;
    }
    if (g_hwnd)
    {
        g_origWndProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                                                   (LONG_PTR)SubWndProc);
        InstallSwapChainHooks();
    }
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
