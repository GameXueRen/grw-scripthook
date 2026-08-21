/* Input blocking. Keys: the game reads DirectInput through
 * our proxy (scripthook_dinput.c). Look: GetCursorPos, via
 * its import slot. GetAsyncKeyState: the modifiers only. */
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
static volatile uint8_t g_keyBlock[256];
static volatile int g_anyKeyBlock = 0;

/* Never swallowed, so a player can always pause, alt tab
 * or reach the menu whatever a mod is doing.
 */
static int Escapes(int vk) {
    return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_TAB || vk == VK_F4 || vk == VK_ESCAPE ||
           vk == VK_LWIN || vk == VK_RWIN;
}

/* one rule for the poll stub and the message hook */
static int Suppressed(int vk) {
    uint32_t b = g_block;

    if (vk <= 0 || vk >= 256 || Escapes(vk)) return 0;
    if (g_anyKeyBlock && g_keyBlock[vk]) return 1;
    if (!b) return 0;
    if (b & SH_INPUT_KEYS) return 1;
    if ((b & SH_INPUT_MOVE) &&
        (vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D' ||
         vk == VK_SPACE || vk == VK_SHIFT || vk == VK_CONTROL))
        return 1;
    if ((b & SH_INPUT_FIRE) && vk == VK_LBUTTON) return 1;
    if ((b & SH_INPUT_AIM) && vk == VK_RBUTTON) return 1;
    return 0;
}

static SHORT WINAPI KeyStub(int vk) {
    if (Suppressed(vk)) return 0;
    return g_realKey ? g_realKey(vk) : 0;
}

/* for the DirectInput proxy in scripthook_dinput.c */
int ShKeySuppressedVk(int vk) {
    return Suppressed(vk);
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

/* one key hidden from the game, for UI that consumed it */
SH_API int ShBlockKey(int vk, int on) {
    int i, any = 0;
    if (vk <= 0 || vk >= 256) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (on && !Install()) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    g_keyBlock[vk] = on ? 1 : 0;
    for (i = 1; i < 256; i++) if (g_keyBlock[i]) any = 1;
    g_anyKeyBlock = any;
    return 1;
}
