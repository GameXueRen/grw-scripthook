/* Plugin-drawn UI, the worked example.
 *
 * This plugin is what docs/ui-drawing.md points at, the way
 * blacklist_sample.c is what docs/plugin-blacklist.md points at and
 * file_watch_sample.c is what docs/file-interception.md points at, so it
 * ships switched on and is built with every other plugin.
 *
 * It is deliberately a tour of the whole surface, because that is what a
 * sample is for:
 *
 *   window  a drawer with a title bar that draws one of every primitive -
 *           text, a hint, a separator, a button, a toggle, a number, a
 *           slider, a drop-down - and, when its text box is switched on,
 *           the input box with an IME attached.
 *   hud     a second drawer with SH_DRAW_FRAMELESS, parked in a corner:
 *           no title bar, no background, just a line of text.  This is
 *           the shape the framework's own chat box uses, and what a
 *           plugin wants for a corner readout.
 *   box     the input box: ShDrawInputOpen when it goes on, and from then
 *           on the framework collects every character the game window
 *           receives (WM_CHAR, and WM_IME_CHAR - which is how a system
 *           IME hands over composed text) while this plugin owns the
 *           buffer those characters land in.  The framework owns the
 *           box's looks, its focus, the composition string, the candidate
 *           list and the IME session; this plugin owns g_text and nothing
 *           else.  Type with the IME of your choice while the box is on -
 *           that is the whole point of the feature, seen from a plugin.
 *
 * Three switches, all live from the F4 menu ("Drawing sample"), all
 * persisted to the plugin's own ini:
 *
 *   [Settings]
 *   window=1    the sample window
 *   hud=0       the frameless corner readout
 *   box=0       the input box (opens the framework's input session)
 *
 * Two things worth noticing in a session:
 *   - the drawer callbacks run on the RENDER thread, once per frame,
 *     inside the game's Present call.  They must not block: no sleeps, no
 *     waiting on another thread, no file I/O.  Everything here is a
 *     handful of widget calls for exactly that reason.
 *   - ShDrawScale() is the same factor the menu scales itself by
 *     ([Settings] MenuScale), which is why the corner readout stays the
 *     right size at any resolution.
 *
 * There is nothing game-specific in here at all: this plugin never asks
 * the framework for a game object.  It is a plain C DLL that draws.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scripthook.h"
#include "log.h"

#define WINDOW_NAME "draw_sample"
#define HUD_NAME    "draw_sample_hud"
#define BOX_ID      "draw_sample_box"

/* ---- state ------------------------------------------------------------
 *
 * The two drawer switches and the box switch are flipped by the menu on
 * its own thread while the render thread reads them, so they are volatile
 * LONG touched through Interlocked*.  The widget state below them is
 * touched only by the drawer callback, which is the render thread and
 * the only thread that runs one frame at a time - plain variables are
 * correct there (and the menu cannot see them anyway).
 */
static volatile LONG g_win = 1;      /* the sample window               */
static volatile LONG g_hud;          /* the frameless corner readout    */
static volatile LONG g_box;          /* the input session               */
static volatile LONG g_pressed;      /* button presses                  */
static volatile LONG g_frames;       /* HUD frames drawn                */

static int   g_toggle = 1;
static int   g_number = 8;
static float g_slider = 0.5f;
static int   g_choice = 0;
static const char *const kChoices[] = { "Alpha", "Bravo", "Charlie" };

#define TEXT_MAX 256
static char g_text[TEXT_MAX];        /* the box's buffer: ours, not the
                                      * framework's                    */

static uint32_t g_menu;

/* ---- the box's buffer -------------------------------------------------
 *
 * The framework hands over characters, never edits: appending is this
 * plugin's job, and so is the cap.  UTF-8 arrives a whole code point at
 * a time, so the only care needed is not to split one at the end.
 */
static void TextAppend(const char *utf8) {
    size_t used = strlen(g_text);
    size_t add = strlen(utf8);
    if (used + add >= TEXT_MAX) return;
    memcpy(g_text + used, utf8, add + 1);
}

/* ---- the input session ------------------------------------------------ */

static void BoxOn(void) {
    InterlockedExchange(&g_box, 1);
    g_text[0] = 0;
    /* One session at a time: opening one takes the keyboard from whoever
     * held it, and the previous owner notices (its box reads back as not
     * focused and ends itself).  Say so, because taking it from the chat
     * box mid-sentence is a thing worth seeing in the log. */
    if (ShDrawInputIsOpen())
        Log("taking the input session from another box");
    /* Open the session and hand the framework the box's name: that name
     * is what ShDrawInputBox below passes in, and what makes the box the
     * focused one (only a focused box anchors the IME). */
    ShDrawInputOpen(BOX_ID);
    ShDrawInputSetMode(0);          /* 0 = the overlay draws the candidates */
    Log("input session %s", ShDrawInputIsOpen() ? "open" : "refused");
}

static void BoxOff(void) {
    InterlockedExchange(&g_box, 0);
    ShDrawInputClose();
    Log("input session closed (%d bytes typed)", (int)strlen(g_text));
}

/* ---- the drawers ------------------------------------------------------ */

static void DrawWindow(void *user) {
    char line[160];
    (void)user;

    ShDrawPushFont(SH_DRAW_FONT_BOLD);
    ShDrawText("Plugin-drawn UI");
    ShDrawPopFont();
    ShDrawHint("every pixel here is drawn by draw_sample.c through ShDraw*");
    ShDrawSeparator();

    if (ShDrawButton("Count it")) {
        Log("button: %ld presses",
            (long)InterlockedIncrement(&g_pressed));
    }
    ShDrawSameLine();
    snprintf(line, sizeof(line), "pressed %ld times",
             (long)InterlockedCompareExchange(&g_pressed, 0, 0));
    ShDrawTextColored(line, SH_DRAW_COL_GOOD, 255);

    ShDrawToggle("A toggle", &g_toggle);
    ShDrawNumber("A number (0-100)", &g_number, 1, 0, 100);
    ShDrawSlider("A slider", &g_slider, 0.0f, 1.0f);
    ShDrawList("A drop-down", &g_choice, kChoices, 3);
    ShDrawSeparator();

    if (InterlockedCompareExchange(&g_box, 0, 0)) {
        ShDrawInput in;
        /* The session is the framework's to end: it closes one whose box
         * was not drawn for a while - which is exactly what happens to a
         * box switched on from the ini before the overlay is up.  The
         * switch is still on and we are drawing now, so take it back. */
        if (!ShDrawInputIsOpen()) {
            ShDrawInputOpen(BOX_ID);
            Log("took the input session back (the framework had closed it)");
        }
        /* The box: the framework draws it, owns the IME and hands the
         * characters over; the buffer above is ours. */
        ShDrawInputBox(BOX_ID, g_text,
                       "type here - Chinese and the IME's own candidates "
                       "work; the characters arrive in this plugin",
                       &in);
        for (;;) {
            char got[128];
            int n = ShDrawInputTake(got, (int)sizeof(got));
            if (n <= 0) break;
            TextAppend(got);
        }
        snprintf(line, sizeof(line),
                 "focused %d, composing %d, candidates %s, %d bytes",
                 in.focused, in.composing, in.mode ? "IME native" : "drawn",
                 (int)strlen(g_text));
        ShDrawHint(line);
        ShDrawText("The menu's \"Text box\" switch ends the session.");
    } else {
        ShDrawText("Text box is off (menu: \"Text box\").");
    }
}

static void DrawHud(void *user) {
    char line[96];
    (void)user;

    /* A frameless drawer: no title bar, no background, hugging its
     * content.  Pinned by ShDrawOpts below, and drawn at the same scale
     * the menu uses. */
    snprintf(line, sizeof(line), "draw_sample: scale %.2f, frame %ld",
             ShDrawScale(), (long)InterlockedIncrement(&g_frames));
    ShDrawTextColored(line, SH_DRAW_COL_HI, 255);
}

/* ---- plugin ini ------------------------------------------------------- */

static HINSTANCE g_inst;
static char      g_iniPath[MAX_PATH];

static void ResolveIniPath(void) {
    char mod[MAX_PATH];
    const char *dot;
    size_t n;

    g_iniPath[0] = 0;
    if (!g_inst || !GetModuleFileNameA(g_inst, mod, sizeof(mod))) return;
    dot = strrchr(mod, '.');
    n = dot ? (size_t)(dot - mod) : strlen(mod);
    if (n >= sizeof(g_iniPath)) n = sizeof(g_iniPath) - 1;
    memcpy(g_iniPath, mod, n);
    g_iniPath[n] = 0;
    strncat(g_iniPath, ".ini", sizeof(g_iniPath) - n - 1);
}

static int IniInt(const char *key, int def) {
    return g_iniPath[0] ? GetPrivateProfileIntA("Settings", key, def,
                                                g_iniPath)
                        : def;
}

static void SaveIni(void) {
    char buf[8];
    if (!g_iniPath[0]) return;
    snprintf(buf, sizeof(buf), "%d",
             (int)InterlockedCompareExchange(&g_win, 0, 0));
    WritePrivateProfileStringA("Settings", "window", buf, g_iniPath);
    snprintf(buf, sizeof(buf), "%d",
             (int)InterlockedCompareExchange(&g_hud, 0, 0));
    WritePrivateProfileStringA("Settings", "hud", buf, g_iniPath);
    snprintf(buf, sizeof(buf), "%d",
             (int)InterlockedCompareExchange(&g_box, 0, 0));
    WritePrivateProfileStringA("Settings", "box", buf, g_iniPath);
}

/* ---- menu ------------------------------------------------------------- */

typedef void (*MenuFn_t)(uint32_t menu, uint32_t item, int value, void *user);

static void Register(const char *name, ShDrawFn fn, const ShDrawOpts *opts) {
    if (!ShDrawAddEx(name, fn, NULL, opts))
        Log("%s was refused - see logs\\scripthook_draw.log", name);
}

static void OnWindow(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    InterlockedExchange(&g_win, v ? 1 : 0);
    if (v) Register(WINDOW_NAME, DrawWindow, NULL);
    else   ShDrawDel(WINDOW_NAME);
    Log("menu: window=%d", v);
    SaveIni();
}

static void OnHud(uint32_t m, uint32_t it, int v, void *u) {
    ShDrawOpts o;
    (void)m; (void)it; (void)u;
    InterlockedExchange(&g_hud, v ? 1 : 0);
    if (v) {
        /* The frameless flag is the HUD shape: no chrome at all, the
         * content is the window.  x/y are in scaled pixels, first use
         * only - after that the user's own placement wins. */
        memset(&o, 0, sizeof(o));
        o.flags = SH_DRAW_FRAMELESS;
        o.x = 24.0f;
        o.y = 140.0f;
        Register(HUD_NAME, DrawHud, &o);
    } else {
        ShDrawDel(HUD_NAME);
    }
    Log("menu: hud=%d", v);
    SaveIni();
}

static void OnBox(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    if (v) BoxOn(); else BoxOff();
    Log("menu: box=%d", v);
    SaveIni();
}

static void BuildMenu(HMODULE m) {
    uint32_t (*menuCreate)(const char *) = NULL;
    int (*menuToggle)(uint32_t, const char *, int, MenuFn_t, void *) = NULL;
    int (*menuHint)(uint32_t, const char *) = NULL;

    *(FARPROC *)&menuCreate = GetProcAddress(m, "ShMenuCreate");
    *(FARPROC *)&menuToggle = GetProcAddress(m, "ShMenuToggle");
    *(FARPROC *)&menuHint   = GetProcAddress(m, "ShMenuHint");
    if (!menuCreate || !menuToggle) return;

    g_menu = menuCreate("Drawing sample");
    menuToggle(g_menu, "Drawer window", (int)InterlockedCompareExchange(
                   &g_win, 0, 0), OnWindow, NULL);
    menuToggle(g_menu, "Corner readout (frameless)", (int)InterlockedCompareExchange(
                   &g_hud, 0, 0), OnHud, NULL);
    menuToggle(g_menu, "Text box", (int)InterlockedCompareExchange(
                   &g_box, 0, 0), OnBox, NULL);
    if (menuHint)
        menuHint(g_menu, "A worked example of the framework's drawing API: "
                         "see docs/ui-drawing.md.  Drawer callbacks run on "
                         "the render thread, once per frame.");
    Log("menu created");
}

/* ---- startup ---------------------------------------------------------- */

static DWORD WINAPI InitThread(LPVOID p) {
    HMODULE di;
    (void)p;

    /* log.h: this translation unit gets its own file and its own Log. */
    LogInit("draw_sample.log");
    Log("--- drawing sample ---");

    ResolveIniPath();
    InterlockedExchange(&g_win, IniInt("window", 1));
    InterlockedExchange(&g_hud, IniInt("hud", 0));
    InterlockedExchange(&g_box, IniInt("box", 0));
    Log("window=%ld hud=%ld box=%ld",
        (long)InterlockedCompareExchange(&g_win, 0, 0),
        (long)InterlockedCompareExchange(&g_hud, 0, 0),
        (long)InterlockedCompareExchange(&g_box, 0, 0));

    if (InterlockedCompareExchange(&g_win, 0, 0))
        Register(WINDOW_NAME, DrawWindow, NULL);
    if (InterlockedCompareExchange(&g_hud, 0, 0)) {
        ShDrawOpts o;
        memset(&o, 0, sizeof(o));
        o.flags = SH_DRAW_FRAMELESS;
        o.x = 24.0f;
        o.y = 140.0f;
        Register(HUD_NAME, DrawHud, &o);
    }
    if (InterlockedCompareExchange(&g_box, 0, 0)) BoxOn();

    di = GetModuleHandleA("dinput8.dll");
    if (di) BuildMenu(di);

    Log("ready");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = inst;
        DisableThreadLibraryCalls(inst);
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
    }
    return TRUE;
}
