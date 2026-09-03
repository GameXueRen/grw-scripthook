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

// ---------------------------------------------------------------------------
// WndProc subclass: feed messages to ImGui while the menu is open
// ---------------------------------------------------------------------------
static LRESULT CALLBACK SubWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
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
const float MENU_W  = 400.0f;
const float PAD     = 16.0f;  // horizontal + bottom padding
const float PAD_TOP = 0.0f;
const float TITLE_H = 32.0f;
const float ROW_H   = 28.0f;
// Highlight bar. Empirical: msyh.ttc rendered glyphs sit ~13px below
// what baked->Ascent/Descent predict. Text visual centre is at ry+27
// and BAR_DY=16 keeps the bar (BAR_H=22) centred on it.
const float BAR_DY  = 16.0f;
const float BAR_H   = 22.0f;
const float VALUE_W = 150.0f;

// 0xRRGGBB -> ImU32 (0xAABBGGRR)
ImU32 Col(uint32_t rgb, int a = 255)
{
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a);
}

// Compute the baseline Y that vertically centres the glyph bbox of
// text rendered at `font_size` within a row of height `row_h` that
// starts at `ry`. ImGui 1.92 keeps per-size metrics on ImFontBaked
// (Ascent is positive: baseline->glyph top; Descent is negative:
// baseline->glyph bottom). AddText uses pos.y as the baseline, so
// the glyph bbox [pos.y - Ascent, pos.y - Descent] is centred when
// pos.y = ry + row_h/2 + (Ascent + Descent)/2.
static float CenteredBaseline(ImFont* font, float font_size, float ry, float row_h)
{
    ImFontBaked* baked = font->GetFontBaked(font_size);
    if (!baked) baked = font->GetFontBaked(font->LegacySize);
    if (!baked) return ry + row_h * 0.5f;
    float ascent  = baked->Ascent;   // >=0 (pixels above baseline)
    float descent = baked->Descent;  // <=0 (pixels below baseline)
    return ry + row_h * 0.5f + (ascent + descent) * 0.5f;
}

void RenderMenu(const ShMenuView* v)
{
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float fs = 16.0f, titleFs = 18.0f, hintFs = 14.0f;
    const float hintLh = 18.0f, hintGap = 5.0f;
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
    // hintH must reach down to the actual glyph bottom of the last
    // hint line, not just hintGap + hintLh * lines. msyh's line
    // height at 14px is ~15px, so hintLh=18 leaves a visible gap
    // between the hint block and the first row. Measure descent
    // from the font and use it.
    float hintH = 0.0f;
    if (hintLines) {
        ImFontBaked* hb = font->GetFontBaked(hintFs);
        float descent = hb ? -hb->Descent : 3.0f;
        hintH = hintGap + hintLh * (float)(hintLines - 1) + descent;
    }

    float h = PAD_TOP + TITLE_H + hintH + ROW_H * (float)v->rows + PAD;
    if (v->footer[0]) h += ROW_H;
    if (v->status[0]) h += ROW_H;

    // Panel: translucent rounded quad.
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + MENU_W, y + h),
                      IM_COL32(0, 0, 0, 204), 6.0f);
    // Title.
    float titleBy = CenteredBaseline(font, titleFs, y + PAD_TOP, TITLE_H);
    dl->AddText(font, titleFs, ImVec2(x + PAD, titleBy),
                Col(0xFFD25Au), v->title);
    // Root menu top-right credits: original author + this build.
    if (v->isRoot) {
        const float cFs1 = 14.0f, cFs2 = 14.0f, cGap = 1.0f;
        const char* c1 = "原作者：Phiality · 魔改：GameXueRen";
        const char* c2 = "版本：Beta1.0 · Q群：299177445";
        ImFontBaked* b1 = font->GetFontBaked(cFs1);
        ImFontBaked* b2 = font->GetFontBaked(cFs2);
        if (!b1) b1 = font->GetFontBaked(font->LegacySize);
        if (!b2) b2 = b1 ? b1 : font->GetFontBaked(font->LegacySize);
        if (b1 && b2) {
            ImVec2 s1 = font->CalcTextSizeA(cFs1, FLT_MAX, 0.0f, c1);
            ImVec2 s2 = font->CalcTextSizeA(cFs2, FLT_MAX, 0.0f, c2);
            float rightX = x + MENU_W - PAD;
            float g1 = b1->Ascent - b1->Descent;
            float g2 = b2->Ascent - b2->Descent;
            float blockH = g1 + cGap + g2;
            float t1 = y + PAD_TOP + (TITLE_H - blockH) * 0.5f;
            float by1 = t1 + b1->Ascent;
            float by2 = t1 + g1 + cGap + b2->Ascent;
            dl->AddText(font, cFs1, ImVec2(rightX - s1.x, by1),
                        Col(0xE6E6E6u), c1);
            dl->AddText(font, cFs2, ImVec2(rightX - s2.x, by2),
                        Col(0x8C9BA8u), c2);
        }
    }
    // Control hints, two small grey lines under the title.
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
    // Selection bar behind the selected row (centred on row centre).
    if (v->rows > 0) {
        float sy = top + ROW_H * (float)v->sel;
        dl->AddRectFilled(ImVec2(x + PAD * 0.5f, sy + BAR_DY),
                          ImVec2(x + MENU_W - PAD * 0.5f, sy + BAR_DY + BAR_H),
                          Col(0x28465Au, 230), 3.0f);
    }
    // Rows: name left, value right-aligned in its column.
    for (i = 0; i < v->rows; i++) {
        const ShMenuRow* r = &v->row[i];
        float ry = top + ROW_H * (float)i;
        ImU32 c = r->selected ? Col(0x8CF0FFu) : Col(0xD2D2D2u);
        float by = CenteredBaseline(font, fs, ry, ROW_H);
        dl->AddText(font, fs, ImVec2(x + PAD + 8.0f, by), c, r->name);
        if (r->value[0]) {
            ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, r->value);
            dl->AddText(font, fs,
                        ImVec2(x + MENU_W - PAD - VALUE_W +
                               (VALUE_W - sz.x), by),
                        c, r->value);
        }
    }
    // Footer and status lines.
    float fy = top + ROW_H * (float)v->rows;
    if (v->footer[0]) {
        float fby = CenteredBaseline(font, fs, fy, ROW_H);
        dl->AddText(font, fs, ImVec2(x + PAD, fby),
                    Col(0x8C8C8Cu), v->footer);
        fy += ROW_H;
    }
    if (v->status[0]) {
        float sby = CenteredBaseline(font, fs, fy, ROW_H);
        dl->AddText(font, fs, ImVec2(x + PAD, sby),
                    Col(0xA0E6A0u), v->status);
    }

    // One-shot diagnostics: log the actual font metrics and where
    // the centred baseline lands for this row geometry so we can
    // verify alignment against the cursor.
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            ImFontBaked* b16 = font->GetFontBaked(fs);
            ImFontBaked* b18 = font->GetFontBaked(titleFs);
            OvlLog("font metrics: legacy=%.2f baked16 a=%.2f d=%.2f sz=%.2f | baked18 a=%.2f d=%.2f sz=%.2f",
                   font->LegacySize,
                   b16 ? b16->Ascent : -1.f, b16 ? b16->Descent : -1.f, b16 ? b16->Size : -1.f,
                   b18 ? b18->Ascent : -1.f, b18 ? b18->Descent : -1.f, b18 ? b18->Size : -1.f);
            float sampleRy = 0.0f;
            float by  = CenteredBaseline(font, fs, sampleRy, ROW_H);
            float by18 = CenteredBaseline(font, titleFs, sampleRy, TITLE_H);
            OvlLog("layout: ROW_H=%.0f BAR_DY=%.0f BAR_H=%.0f | row baseline offset=%.2f (text spans %.2f..%.2f, centre=%.2f) | title offset=%.2f",
                   ROW_H, BAR_DY, BAR_H,
                   by, by - (b16 ? b16->Ascent : 0.f),
                   by - (b16 ? b16->Descent : 0.f),
                   (by - (b16 ? b16->Ascent : 0.f) + by - (b16 ? b16->Descent : 0.f)) * 0.5f,
                   by18);
            OvlLog("hint: bar should overlap text bbox; if bar looks high, raise BAR_DY; if low, lower it");
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
    for (const char* path : kCandidates)
    {
        ImFont* f = io.Fonts->AddFontFromFileTTF(
            path, 16.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
        if (f)
        {
            io.FontDefault = f;
            OvlLog("font loaded: %s", path);
            return;
        }
    }
    OvlLog("WARNING: no CJK font loaded, Chinese text will not render");
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

    if (g_ready && ShMenuIsOpen())
    {
        // Bind the swapchain back buffer as the render target so
        // the menu is drawn on the surface that gets presented,
        // whatever the game left bound.
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

        ShMenuView v;
        ShMenuCaptureView(&v);
        RenderMenu(&v);

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
