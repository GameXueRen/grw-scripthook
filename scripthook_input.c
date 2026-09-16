/* Input blocking. Keys: the game reads DirectInput through
 * our proxy (scripthook_dinput.c). Look: GetCursorPos, via
 * its import slot. GetAsyncKeyState: the modifiers only. */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"
#include "log.h"

#define IAT_ASYNCKEY   SH_IMG(0x182B2B90)
#define IAT_CURSORPOS  SH_IMG(0x182B2BA0)

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
/* How many entries of the table above are set, so the predicate does not
 * have to walk all 255 of them to answer "is any key hidden". */
static volatile LONG g_keyBlockN = 0;

/* Never swallowed, so a player can always pause, alt tab
 * or reach the menu whatever a mod is doing.
 */
static int Escapes(int vk) {
    return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_TAB || vk == VK_F4 || vk == VK_ESCAPE ||
           vk == VK_LWIN || vk == VK_RWIN;
}

static volatile int g_capture;
static int Install(void);

/* Is the game's own window the one in front? Any window of this process
 * counts, which covers windowed and borderless fullscreen alike; a
 * backgrounded game reports the window in front instead.
 *
 * Exported because a plugin that polls a hotkey with GetAsyncKeyState
 * reads the PHYSICAL key: without asking this first, a press meant for
 * whichever window the player switched to still fires the plugin's action
 * on the game sitting behind it. Answered uncached - one
 * GetForegroundWindow - because a stale "yes" is the one case this exists
 * to prevent.
 */
SH_API int ShGameFocused(void) {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;

    if (!fg) return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() ? 1 : 0;
}

/* Our own poll stub runs hundreds of times a frame, so the answer it
 * works from is cached for 50ms: focus changes are rare, and 50ms of
 * stale "focused" only delays a suppression by one poll burst. */
static int FocusedCached(void) {
    static DWORD lastAt = 0;
    static DWORD lastAns = 0;
    DWORD now = GetTickCount();

    if (!lastAt || (int)(now - lastAt) >= 50) {
        lastAns = (DWORD)ShGameFocused();
        lastAt = now ? now : 1;
    }
    return (int)lastAns;
}

/* one rule for the poll stub and the DirectInput wrapper */
static int Suppressed(int vk) {
    uint32_t b = g_block;

    if (vk <= 0 || vk >= 256) return 0;
    /* Deactivated game: hand back nothing. This sits above the
     * escape list on purpose - pausing and alt-tabbing are OS
     * actions that never needed the game to receive the keys. */
    if (!FocusedCached()) return 1;
    /* An explicitly blocked key wins over the escape list, so
     * a mod menu can claim ESC while it is open. Escapes still
     * pass when nothing claims them, so pause and alt-tab
     * always work outside the menu. */
    if (g_anyKeyBlock && g_keyBlock[vk]) return 1;
    if (Escapes(vk)) return 0;
    if (g_capture) return 1;
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

/* keyboard capture: every key but the escapes hidden */
SH_API int ShCaptureKeys(int on) {
    if (on && !Install()) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    g_capture = on ? 1 : 0;
    return 1;
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
    uint64_t cur = 0;

    if (g_hooked) return 1;
    /* Both slots are pinned RVAs into the import table. After an update
     * that moves it, Redirect refuses and every caller sees only
     * SH_ERR_NO_CANDIDATE: say which slot and what was in it, once. */
    if (!Redirect(IAT_ASYNCKEY, (void *)KeyStub, (void **)&g_realKey)) {
        if (ShReadableAddr(IAT_ASYNCKEY, 8))
            memcpy(&cur, (const void *)(uintptr_t)IAT_ASYNCKEY, 8);
        LogFirst("scripthook_input.log",
                 "input slot %llX is %llX - import table moved?",
                 (unsigned long long)IAT_ASYNCKEY, (unsigned long long)cur);
        return 0;
    }
    if (!Redirect(IAT_CURSORPOS, (void *)PosStub, (void **)&g_realPos)) {
        if (ShReadableAddr(IAT_CURSORPOS, 8))
            memcpy(&cur, (const void *)(uintptr_t)IAT_CURSORPOS, 8);
        LogFirst("scripthook_input.log",
                 "input slot %llX is %llX - import table moved?",
                 (unsigned long long)IAT_CURSORPOS, (unsigned long long)cur);
        return 0;
    }
    g_hooked = 1;
    LogFirst("scripthook_input.log",
             "input hooked: keys at %llX, cursor at %llX",
             (unsigned long long)IAT_ASYNCKEY,
             (unsigned long long)IAT_CURSORPOS);
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
    int want = on ? 1 : 0;
    if (vk <= 0 || vk >= 256) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (on && !Install()) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    if (g_keyBlock[vk] == want) { ShSetError(SH_OK); return 1; }
    g_keyBlock[vk] = want;
    /* Counted, not rescanned: the menu hides seven keys in a row when it
     * opens, and every one of those calls walked all 255 entries to answer
     * a question the previous call had already answered. */
    if (want) InterlockedIncrement(&g_keyBlockN);
    else      InterlockedDecrement(&g_keyBlockN);
    g_anyKeyBlock = g_keyBlockN > 0;
    ShSetError(SH_OK);
    return 1;
}
