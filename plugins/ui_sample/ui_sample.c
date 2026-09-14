/* ui_sample: a window from the ScriptHook UI ABI alone. */
/* F7 toggles it; arrows move the bar, Enter counts. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "scripthook.h"

#define ROWS     4
#define PANEL_X  700.0f
#define PANEL_Y  300.0f
#define PANEL_W  520.0f
#define ROW_H    34.0f
#define TITLE_H  44.0f
#define PAD      16.0f

static uint32_t g_scene, g_panel, g_title, g_bar, g_rows[ROWS], g_foot;
static int g_sel, g_open, g_presses;
static const char *g_names[ROWS] = {
    "First row", "Second row", "Third row", "Fourth row"
};

static void Layout(void) {
    float v[3];
    ShUiBegin();
    v[0] = PAD; v[1] = TITLE_H + (float)g_sel * ROW_H; v[2] = 0.0f;
    ShUiSetV(g_bar, SH_P_POSITION, v, 3);
    ShUiCommit();
}

static void SetFooter(void) {
    char text[96];
    snprintf(text, sizeof(text), "Enter pressed %d times. F7 closes.",
             g_presses);
    ShUiSetS(g_foot, SH_P_TEXT, text);
}

static int Build(void) {
    int i;
    float h = TITLE_H + ROWS * ROW_H + 40.0f;

    g_panel = ShUiCreateIn(g_scene, 0, SH_W_PANEL, PANEL_X, PANEL_Y,
                           PANEL_W, h);
    if (!g_panel) return 0;
    g_bar = ShUiCreateIn(g_scene, g_panel, SH_W_IMAGE, PAD, TITLE_H,
                         PANEL_W - 2 * PAD, ROW_H - 6.0f);
    ShUiSetColour(g_bar, 0x28465A);
    g_title = ShUiCreateIn(g_scene, g_panel, SH_W_LABEL, PAD, 8.0f,
                           PANEL_W - 2 * PAD, 30.0f);
    ShUiSetS(g_title, SH_P_TEXT, "UI SAMPLE");
    ShUiSetColour(g_title, 0xFFD25A);
    for (i = 0; i < ROWS; i++) {
        g_rows[i] = ShUiCreateIn(g_scene, g_panel, SH_W_LABEL, PAD + 8.0f,
                                 TITLE_H + i * ROW_H + 2.0f,
                                 PANEL_W - 2 * PAD, ROW_H);
        ShUiSetS(g_rows[i], SH_P_TEXT, g_names[i]);
        ShUiSetColour(g_rows[i], 0xD2D2D2);
    }
    g_foot = ShUiCreateIn(g_scene, g_panel, SH_W_LABEL, PAD,
                          TITLE_H + ROWS * ROW_H + 6.0f,
                          PANEL_W - 2 * PAD, 28.0f);
    ShUiSetColour(g_foot, 0x8C8C8C);
    SetFooter();
    Layout();
    ShUiSceneShow(g_scene, g_open);
    return 1;
}

/* focused scene gets every key; 1 hides it from the game */
static int OnInput(uint32_t scene, const ShUiEvent *e, void *user) {
    (void)scene; (void)user;
    if (e->type != SH_UI_EV_DOWN) return 0;
    switch (e->key) {
    case VK_UP:
        g_sel = (g_sel + ROWS - 1) % ROWS;
        Layout();
        return 1;
    case VK_DOWN:
        g_sel = (g_sel + 1) % ROWS;
        Layout();
        return 1;
    case VK_RETURN:
        g_presses++;
        SetFooter();
        return 1;
    }
    return 0;
}

/* after a world reload the widgets are gone, rebuild */
static void OnReset(uint32_t scene, void *user) {
    (void)scene; (void)user;
    Build();
}

static DWORD WINAPI Main(LPVOID arg) {
    int built = 0, wasDown = 0;
    (void)arg;
    g_scene = ShUiSceneCreate("ui_sample", 10);
    ShUiSetReset(g_scene, OnReset, NULL);
    ShUiSetInput(g_scene, OnInput, NULL);
    for (;;) {
        int down;
        Sleep(20);
        if (!built && ShUiReady()) built = Build();
        /* edge on the held bit; the "since last call" bit */
        /* is eaten by whoever else polls the key */
        down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (down && !wasDown) {
            g_open = !g_open;
            ShUiSceneShow(g_scene, g_open);
            ShUiFocus(g_scene, g_open);
        }
        wasDown = down;
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        CreateThread(NULL, 0, Main, NULL, 0, NULL);
    return TRUE;
}
