/* Input blocking, at the only place the game reads it.
 * GRW polls GetAsyncKeyState for keys and GetCursorPos for
 * look, and imports no DirectInput and no raw input. */
/* So low level hooks miss it entirely. These are its own
 * import slots, redirected to stubs that answer as though
 * nothing is pressed and the mouse never moved. */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

#define IAT_ASYNCKEY   SH_IMG(0x1880FBB0)
#define IAT_CURSORPOS  SH_IMG(0x1880FBC0)

extern void ShSetError(int err);
extern int ShReadableAddr(uint64_t addr, size_t len);

typedef SHORT (WINAPI *AsyncKey_t)(int);
typedef BOOL  (WINAPI *CursorPos_t)(LPPOINT);

static AsyncKey_t  g_realKey = NULL;
static CursorPos_t g_realPos = NULL;
static int g_hooked = 0;

static volatile uint32_t g_block = 0;
static POINT g_frozen;
static volatile int g_haveFrozen = 0;

/* Never swallowed, so a player can always pause, alt tab
 * or reach the menu whatever a mod is doing.
 */
static int Escapes(int vk) {
    return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_TAB || vk == VK_F4 || vk == VK_ESCAPE ||
           vk == VK_LWIN || vk == VK_RWIN;
}

static SHORT WINAPI KeyStub(int vk) {
    uint32_t b = g_block;

    if (b && !Escapes(vk)) {
        if (b & SH_INPUT_KEYS) return 0;
        if ((b & SH_INPUT_MOVE) &&
            (vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D' ||
             vk == VK_SPACE || vk == VK_SHIFT || vk == VK_CONTROL))
            return 0;
        if ((b & SH_INPUT_FIRE) && vk == VK_LBUTTON) return 0;
        if ((b & SH_INPUT_AIM) && vk == VK_RBUTTON) return 0;
    }
    return g_realKey ? g_realKey(vk) : 0;
}

/* The game turns by the change between polls, so handing
 * back the same point every time means it never turns.
 */
static BOOL WINAPI PosStub(LPPOINT p) {
    BOOL ok = g_realPos ? g_realPos(p) : FALSE;

    if (!p) return ok;
    if (g_block & SH_INPUT_LOOK) {
        if (!g_haveFrozen) {
            g_frozen = *p;
            g_haveFrozen = 1;
        }
        *p = g_frozen;
    } else {
        g_haveFrozen = 0;
    }
    return ok;
}

static int Redirect(uint64_t slot, void *stub, void **outOrig) {
    DWORD old;
    uint64_t cur;

    if (!ShReadableAddr(slot, 8)) return 0;
    memcpy(&cur, (const void *)(uintptr_t)slot, 8);
    if (cur < 0x10000ULL) return 0;

    if (!VirtualProtect((void *)(uintptr_t)slot, 8,
                        PAGE_READWRITE, &old))
        return 0;
    *outOrig = (void *)(uintptr_t)cur;
    *(uint64_t *)(uintptr_t)slot = (uint64_t)(uintptr_t)stub;
    VirtualProtect((void *)(uintptr_t)slot, 8, old, &old);
    return 1;
}

static int Install(void) {
    if (g_hooked) return 1;
    if (!Redirect(IAT_ASYNCKEY, (void *)KeyStub,
                  (void **)&g_realKey))
        return 0;
    if (!Redirect(IAT_CURSORPOS, (void *)PosStub,
                  (void **)&g_realPos)) {
        return 0;
    }
    g_hooked = 1;
    return 1;
}

/** Which inputs the game is told it is not receiving. 0
 *  hands everything back. See the SH_INPUT_ bits.
 */
SH_API int ShBlockInput(uint32_t mask) {
    if (mask && !Install()) {
        ShSetError(SH_ERR_NO_CANDIDATE);
        return 0;
    }
    if (!(mask & SH_INPUT_LOOK)) g_haveFrozen = 0;
    g_block = mask;
    ShSetError(SH_OK);
    return 1;
}

SH_API uint32_t ShBlockedInput(void) {
    return g_block;
}
