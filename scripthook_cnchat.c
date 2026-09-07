/* Chinese chat input (in-process, mirroring GRW-CNChat):
 *
 * The game's own text chat field cannot take IME composition - its
 * input is DirectInput based, so composed Chinese never reaches it.
 * GRW-CNChat solved this from outside with an AutoHotkey window that
 * owns a real Edit control (full IME), then posts the finished text
 * into the game window as WM_CHAR (verified to work on GRW).
 *
 * This module does the same thing inside the game process, but it
 * does NOT depend on window keyboard messages: the game reads its
 * keyboard through DirectInput and never translates keyboard messages
 * to this window, so WM_CHAR / WM_IME_CHAR from the system IME never
 * arrive here either.  Instead all editing is polled with
 * GetAsyncKeyState exactly like the menu and the gadget wheel:
 *   - The player presses the chat hotkey (default T).  The key is
 *     let through to the game (it opens its own chat box); when T
 *     comes back up we open the ImGui input box and capture the
 *     keyboard (ShCaptureKeys), so the game chat box stays open but
 *     never sees the keys we type.
 *   - Chinese text is brought in through the clipboard (Ctrl+V),
 *     which works regardless of the game's input handling.
 *   - Enter: release the capture, then PostMessage WM_CHAR for every
 *     character to the game window (the channel GRW-CNChat proved
 *     works on GRW), followed by an Enter key press to submit.
 *   - Esc: release and post Esc so the game closes its chat box.
 */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "log.h"

#define VK_CHAT     0x54            /* 'T' */
#define POLL_MS     15

/* ---- shared state -------------------------------------------------
 * Text is edited on the poll thread only (paste, backspace) except
 * for the optional WM_CHAR path that some window modes may deliver,
 * so the buffer is guarded by a critical section.  The overlay
 * render thread reads a snapshot under the same lock. */
#define TEXT_MAX    400             /* UTF-16 code units (DIAG: 400) */
typedef struct {
    volatile int open;              /* box visible / typing           */
    volatile int cmd;               /* 0 none 1 commit 2 cancel       */
    wchar_t text[TEXT_MAX];
    int     len;
    uint64_t hwnd;                  /* game window to post text into  */
} ChatState;

static ChatState g_chat;
static volatile int g_started = 0;
static volatile int g_ownsKeys = 0;
static volatile int g_sending = 0;   /* injecting into the native box */
static CRITICAL_SECTION g_lock;
static volatile int g_lockReady = 0;
static unsigned char g_keyWas[256];

static void Lock(void)  { if (g_lockReady) EnterCriticalSection(&g_lock); }
static void Unlock(void){ if (g_lockReady) LeaveCriticalSection(&g_lock); }

/* ---- text helpers --------------------------------------------------- */

static void TextAppendLocked(unsigned w) {
    if (g_chat.len >= TEXT_MAX - 1) return;
    if (w < 0x20 || w == 0x7F) return;
    /* Newlines from pasted multiline text become spaces. */
    if (w == '\r' || w == '\n' || w == '\t') w = ' ';
    g_chat.text[g_chat.len++] = (wchar_t)w;
    g_chat.text[g_chat.len] = 0;
}

static void TextBackLocked(void) {
    if (g_chat.len <= 0) return;
    g_chat.len--;
    g_chat.text[g_chat.len] = 0;
}

/* ---- clipboard paste (works no matter how the game reads keys) ----- */

static int PasteClipboard(void) {
    int added = 0;
    int tries = 0;
    while (tries++ < 3) {
        if (OpenClipboard(NULL)) break;
        Sleep(10);
    }
    if (tries > 3) return 0;
    {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            wchar_t *p = (wchar_t *)GlobalLock(h);
            if (p) {
                int i, n = (int)wcslen(p);
                Lock();
                for (i = 0; i < n; i++) {
                    if (g_chat.len >= TEXT_MAX - 1) break;
                    TextAppendLocked((unsigned)p[i]);
                }
                added = 1;
                Unlock();
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
    return added;
}

/* ---- framework binding --------------------------------------------- */

/* Game flow state, read from the engine's own state machine.  It
 * flips the instant the menu closes, so the T key works again the
 * moment the player is back in the world.  Resolving the player
 * entity would need a heap scan that backs off for two seconds
 * after a menu, which silently ate the first presses of T. */
static int (*g_state)(void);

static int Bind(void) {
    HMODULE m = GetModuleHandleA("dinput8.dll");
    if (!m) return 0;
    *(FARPROC *)&g_state = GetProcAddress(m, "ShGetGameState");
    return g_state != NULL;
}

static int IsPlaying(void) {
    int s;
    if (!g_state) return 0;
    s = g_state();
    /* A live play screen where the game's own chat hotkey works:
     * the world is loaded and no menu covers it. */
    return s == SH_STATE_INGAME || s == SH_STATE_DRONE ||
           s == SH_STATE_BINOCULAR || s == SH_STATE_CINEMATIC;
}

static int WindowFocused(void) {
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static int KeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static int Pressed(int vk) {
    int d = KeyDown(vk);
    int hit = d && !g_keyWas[vk & 0xFF];
    g_keyWas[vk & 0xFF] = (unsigned char)d;
    return hit;
}

static int CtrlHeld(void) {
    return KeyDown(VK_CONTROL) || KeyDown(VK_LCONTROL)
        || KeyDown(VK_RCONTROL);
}

/* ---- keyboard capture ---------------------------------------------- */

static void TakeKeys(void) {
    if (g_ownsKeys) return;
    ShCaptureKeys(1);
    g_ownsKeys = 1;
}

static void ReleaseKeys(void) {
    if (!g_ownsKeys) return;
    ShCaptureKeys(0);
    g_ownsKeys = 0;
}

/* ---- optional window-message channel -------------------------------
 * Some window modes (windowed/borderless) do deliver WM_CHAR for the
 * keys the game did not swallow; when they arrive we append them.
 * Command keys are deliberately NOT handled here - the poll thread
 * owns Enter/Esc/Backspace so nothing fires twice. */
int ShChatWndMsg(uint64_t hwnd, uint32_t msg,
                 uint64_t wp, uint64_t lp) {
    (void)lp;
    if (!g_chat.open && !g_sending) return 0;
    if (msg == WM_CHAR || msg == WM_IME_CHAR) {
        g_chat.hwnd = hwnd;
        /* While typing (open) the char feeds our own buffer; while
         * sending it MUST fall through to the game window procedure -
         * that is the injection channel. */
        if (g_chat.open && wp >= 0x20 && wp != 0x7F && wp < 0x10000) {
            Lock();
            TextAppendLocked((unsigned)wp);
            Unlock();
            return 1;
        }
        return 0;
    }
    /* Swallow the rest of the keyboard while the box is up (typing) OR
     * while we are injecting (sending).  A physical Enter keyup that
     * leaks through during the injection makes the game submit before
     * the injected characters have all landed - the tail-truncation
     * bug.  Tab still reaches the game while typing (channel switch). */
    if (msg == WM_KEYDOWN || msg == WM_KEYUP ||
        msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) {
        if ((uint32_t)wp == VK_TAB && g_chat.open) return 0;
        return 1;
    }
    return 0;
}

int ShChatIsSending(void) { return g_sending; }

/* ---- diagnostics (TEMPORARY, for the truncation bug hunt) ---------- */

static int g_cnLog = 0;
static void CnLog(const char *fmt, ...) {
    va_list ap;
    if (!g_cnLog) { LogInit("scripthook_cnchat.log"); g_cnLog = 1; }
    if (!g_logFile) return;
    va_start(ap, fmt);
    Logv(fmt, ap);
    va_end(ap);
}

/* DIAG: ms between WM_CHAR posts while feeding the game's chat box.
 * Read from [chat] SendDelayMs at startup; see HandleDone. */
static volatile int g_cfgDelay = 3;
/* DIAG: extra ms to wait AFTER the last WM_CHAR and BEFORE the Enter,
 * letting the game's engine-side input buffer drain into the chat
 * field.  Read from [chat] EnterDelayMs.  Hypothesis: Enter is handled
 * on a faster path than the buffered WM_CHAR characters, so characters
 * still in flight when Enter lands are dropped (tail truncation). */
static volatile int g_cfgEnterDelay = 0;

/* ---- open / close --------------------------------------------------- */

/* Real key injection.  The game reads chat-submit keys through
 * DirectInput, which ignores PostMessage'd WM_KEYDOWN/WM_KEYUP (that is
 * why the simulated Enter never submitted and the native box stayed
 * open).  SendInput lands in the hardware input queue DirectInput sees.
 * Observed in game: the native chat box submits on Enter RELEASE, and
 * characters still buffered when Enter goes down are flushed while it
 * is held - so submit must be a down -> hold -> up sequence, exactly
 * like holding the physical key until the text has fully arrived.
 */
static void InjectKeyDown(WORD vk) {
    INPUT in;
    memset(&in, 0, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    SendInput(1, &in, sizeof(in));
}

static void InjectKeyUp(WORD vk) {
    INPUT in;
    memset(&in, 0, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
}

static void InjectKey(WORD vk) {
    InjectKeyDown(vk);
    InjectKeyUp(vk);
}

static void OpenChat(void) {
    HWND hwnd;
    Lock();
    g_chat.len = 0;
    g_chat.text[0] = 0;
    g_chat.cmd = 0;
    hwnd = GetForegroundWindow();
    g_chat.hwnd = (uint64_t)(uintptr_t)hwnd;
    g_chat.open = 1;
    Unlock();
    TakeKeys();
    /* Nudge the window thread so the overlay's IME probe (the one
     * that re-associates the input context) runs right now.  Left to
     * itself it waits for the window's next message, which is the
     * first letter's keydown - and that keydown was already routed
     * past the IME by the system, so it lands in the box as plain
     * English.  A harmless WM_NULL gets the probe done while the
     * box opens, well before typing starts. */
    if (hwnd) PostMessageW(hwnd, WM_NULL, 0, 0);
    /* The game already saw the full T press (it opened its chat box);
     * we only grab the keyboard from here on. */
}

/* Called by the poll thread after a command was set (open still 1). */
static void HandleDone(void) {
    int send;
    HWND hwnd;
    wchar_t txt[TEXT_MAX];
    int len, i;

    Lock();
    send = (g_chat.cmd == 1);
    hwnd = (HWND)(uintptr_t)g_chat.hwnd;
    len = g_chat.len;
    for (i = 0; i < len; i++) txt[i] = g_chat.text[i];
    txt[len] = 0;
    g_chat.cmd = 0;
    g_chat.open = 0;
    g_chat.len = 0;
    g_chat.text[0] = 0;
    Unlock();
    /* Keep the keyboard CAPTURED while injecting.  Releasing it here
     * (the old behaviour) let the user's own physical Enter keyup leak
     * through to the game, which submitted immediately - before the
     * injected characters had all landed (tail truncation; how much
     * was lost depended on how fast the user released Enter).  The
     * capture is released after the injection instead. */
    g_sending = 1;

    CnLog("done: send=%d hwnd=%p len=%d delay=%d", send, (void*)hwnd,
          len, (int)g_cfgDelay);

    if (send && hwnd && txt[0]) {
        /* Feed the text into the game's open chat box, then submit
         * with Enter - the exact channel GRW-CNChat uses.
         * Single send loop: an earlier edit accidentally kept BOTH
         * this loop and the experimental one, so every message went
         * out twice (the "text appears doubled" bug). */
        int fails = 0;
        DWORD t0 = GetTickCount();
        for (i = 0; i < len; i++) {
            if (!PostMessageW(hwnd, WM_CHAR, (WPARAM)txt[i], 1)) fails++;
            if (g_cfgDelay > 0) Sleep((DWORD)g_cfgDelay);
        }
        CnLog("chars: len=%d posted=%d fails=%d delay=%d ms=%u",
              len, len - fails, fails, (int)g_cfgDelay,
              (unsigned)(GetTickCount() - t0));
        /* Submit with a real Enter (the game reads it through
         * DirectInput, posted WM_KEYDOWN never submits).  DI is
         * un-blocked right after the burst (this exact sequence was
         * verified stable on short texts); the press is held 500ms so
         * the down state survives even the stretched frames that
         * follow a long character burst (long texts occasionally
         * missed an 80ms hold). */
        ReleaseKeys();
        Sleep(60);   /* DI re-attach after the capture comes off */
        InjectKeyDown(VK_RETURN);
        Sleep(500);
        InjectKeyUp(VK_RETURN);
        CnLog("send: len=%d delay=%d hold=%d ms=%u tail=%04x%04x",
              len, (int)g_cfgDelay, (int)g_cfgEnterDelay,
              (unsigned)(GetTickCount() - t0),
              len >= 1 ? (unsigned)txt[len - 1] : 0u,
              len >= 2 ? (unsigned)txt[len - 2] : 0u);
    } else {
        /* Cancelled: close the game's chat box too. */
        ReleaseKeys();
        if (hwnd) {
            InjectKey(VK_ESCAPE);
            CnLog("cancel: hwnd=%p", (void*)hwnd);
        }
    }
    g_sending = 0;
    ReleaseKeys();
}

/* ---- runtime configuration ------------------------------------------
 * Persisted in scripthook.ini under [chat]:
 *   Enabled   = 0/1   (default 0: the self-drawn box is off)
 *   StartKey  = VK    (default 0x54 'T'; must match the in-game chat key)
 *   CandMode  = 0/1   (0 overlay-drawn candidate list [default],
 *                     1 the input method's own candidate window)
 * Loaded once at startup; menu toggles update the globals AND write the
 * ini so the change sticks.  ChatThread / ovl read these globals live.
 * Defined before ChatThread, which reads them every poll. */
#define CFG_SECTION "chat"
#define CFG_KEY_CFG  "Enabled"
#define CFG_KEY_KEY  "StartKey"
#define CFG_KEY_CAND "CandMode"
#define CFG_KEY_DLY  "SendDelayMs"      /* DIAG: ms between WM_CHAR */
#define CFG_KEY_EDLY "EnterDelayMs"     /* DIAG: ms before Enter     */

static volatile int g_cfgEnabled = 0;   /* default off */
static volatile int g_cfgKey     = VK_CHAT; /* 'T' */
static volatile int g_cfgCand    = 0;   /* default overlay-drawn */

/* ---- poll thread ---------------------------------------------------- */

static DWORD WINAPI ChatThread(LPVOID arg) {
    (void)arg;
    int tDown = 0;
    int compLatch = 0;   /* just left a composition; see below   */
    DWORD backNext = 0;  /* when a held Backspace repeats next  */
    for (;;) {
        Sleep(POLL_MS);

        if (ShMenuIsOpen()) {
            if (g_chat.open) ShChatClose();
            memset(g_keyWas, 0, sizeof(g_keyWas));
            tDown = 0;
            continue;
        }

        /* Feature switched off (default): never touch the chat key. */
        if (!g_cfgEnabled) {
            if (g_chat.open) ShChatClose();
            memset(g_keyWas, 0, sizeof(g_keyWas));
            tDown = 0;
            continue;
        }

        if (!g_chat.open) {
            compLatch = 0;
            if (!IsPlaying() || !WindowFocused()) {
                memset(g_keyWas, 0, sizeof(g_keyWas));
                tDown = 0;
                continue;
            }
            /* Arm on the chat key going down, then open when it comes
             * back up: the key itself was let through to the game, so
             * its chat box is open by the time we take the keyboard. */
            if (KeyDown(g_cfgKey)) {
                tDown = 1;
            } else if (tDown) {
                tDown = 0;
                memset(g_keyWas, 0, sizeof(g_keyWas));
                OpenChat();
            }
            continue;
        }

        /* Box open: poll the editing keys (the game window does not
         * deliver keyboard messages to us, so nothing else works). */
        if (!WindowFocused()) {
            /* Focus went elsewhere: the keys typed out there belong
             * to that window, so ignore them and keep the text for
             * when the game comes back to the front. */
            memset(g_keyWas, 0, sizeof(g_keyWas));
            continue;
        }
        /* While a pinyin composition is live the command keys belong
         * to the IME: Backspace shortens the pinyin, Enter commits
         * its letters, Esc cancels it.  Acting on them here as well
         * is what wiped committed Chinese when Backspace was only
         * trimming pinyin letters. */
        if (ShChatComposing()) {
            compLatch = 1;
            continue;
        }
        if (compLatch) {
            /* Just left a composition.  The very press that ended it
             * (typically the Backspace that ate the last letter) is
             * usually still down - or lands inside one poll gap -
             * by the time we get here, and our edge detector has not
             * seen it yet, so it would fire again against the
             * buffer.  Swallow the command keys until they come up
             * once; only the next full press reaches the text. */
            if (KeyDown(VK_BACK) || KeyDown(VK_ESCAPE) ||
                KeyDown(VK_RETURN)) {
                memset(g_keyWas, 0, sizeof(g_keyWas));
                continue;
            }
            compLatch = 0;
        }
        if (Pressed(VK_ESCAPE)) {
            Lock(); g_chat.cmd = 2; Unlock();
        } else if (Pressed(VK_RETURN)) {
            Lock(); g_chat.cmd = 1; Unlock();
        } else if (Pressed(VK_BACK)) {
            Lock(); TextBackLocked(); Unlock();
            backNext = GetTickCount() + 350;
        } else if (KeyDown(VK_BACK) && g_keyWas[VK_BACK & 0xFF] &&
                   (int)(GetTickCount() - backNext) >= 0) {
            /* Held past the initial delay: keep deleting just like
             * a native text field (the IME already auto-repeats
             * while trimming pinyin, this covers the buffer). */
            Lock(); TextBackLocked(); Unlock();
            backNext = GetTickCount() + 40;
        } else if (CtrlHeld() && Pressed('V')) {
            PasteClipboard();
        }

        if (g_chat.cmd != 0) HandleDone();
    }
    return 0;
}

/* ---- runtime configuration ------------------------------------------
 * The ini keys and the three globals live above (before ChatThread).
 * ChatCfgLoad runs once at startup and copies the ini into them. */
static void ChatCfgLoad(void) {
    g_cfgEnabled = ShConfigGetBool(CFG_SECTION, CFG_KEY_CFG, 0) ? 1 : 0;
    g_cfgKey = ShConfigGetInt(CFG_SECTION, CFG_KEY_KEY, VK_CHAT);
    if (g_cfgKey < 1 || g_cfgKey > 0xFE) g_cfgKey = VK_CHAT;
    g_cfgCand = ShConfigGetInt(CFG_SECTION, CFG_KEY_CAND, 0) ? 1 : 0;
    g_cfgDelay = ShConfigGetInt(CFG_SECTION, CFG_KEY_DLY, 3);
    if (g_cfgDelay < 0) g_cfgDelay = 0;
    if (g_cfgDelay > 1000) g_cfgDelay = 1000;   /* DIAG cap */
    g_cfgEnterDelay = ShConfigGetInt(CFG_SECTION, CFG_KEY_EDLY, 0);
    if (g_cfgEnterDelay < 0) g_cfgEnterDelay = 0;
    if (g_cfgEnterDelay > 5000) g_cfgEnterDelay = 5000; /* DIAG cap */
}

int ShChatGetEnabled(void)   { return g_cfgEnabled; }
int ShChatGetStartKey(void)  { return g_cfgKey; }
int ShChatGetCandMode(void)  { return g_cfgCand; }

void ShChatSetEnabled(int on) {
    g_cfgEnabled = on ? 1 : 0;
    ShConfigSetBool(CFG_SECTION, CFG_KEY_CFG, g_cfgEnabled);
    if (!g_cfgEnabled && g_chat.open) ShChatClose();
}

void ShChatSetStartKey(int vk) {
    if (vk < 1 || vk > 0xFE) return;
    g_cfgKey = vk;
    ShConfigSetInt(CFG_SECTION, CFG_KEY_KEY, vk);
}

void ShChatSetCandMode(int mode) {
    g_cfgCand = mode ? 1 : 0;
    ShConfigSetInt(CFG_SECTION, CFG_KEY_CAND, g_cfgCand);
}

/* ---- mod-settings page ----------------------------------------------
 * A page under "Mod settings" gating the whole feature.  The rows read
 * and write the ini directly; "Enabled" only takes effect on the next
 * launch (the module loads its config once at startup), so it is not
 * mirrored into g_cfg* live.  StartKey / CandMode are applied live
 * through the ShChatSet* setters, which keep the ini in sync. */

static void ChatOnEnabled(uint32_t menu, uint32_t item, int value,
                          void *user) {
    (void)menu; (void)item; (void)user;
    /* Restart to apply: do not touch the running g_cfgEnabled. */
    ShConfigSetBool(CFG_SECTION, CFG_KEY_CFG, value ? 1 : 0);
}

static void ChatOnCandMode(uint32_t menu, uint32_t item, int value,
                           void *user) {
    (void)menu; (void)item; (void)user;
    ShChatSetCandMode(value);
}

void ShChatMenuRegister(uint32_t parent) {
    static const char *kCandOpts[] = { "Self-drawn", "IME native" };
    /* The start key is not user-configurable yet: it is fixed to the
     * game's own text-chat key ("T").  A single-option list shows the
     * current value without arming a key capture. */
    static const char *kKeyOpts[] = { "T" };
    uint32_t m;

    if (!parent) return;
    m = ShMenuSub(parent, "Chinese chat box");
    if (!m) return;

    ShMenuToggle(m, "Enabled",
                 ShConfigGetBool(CFG_SECTION, CFG_KEY_CFG, 0),
                 ChatOnEnabled, NULL);
    ShMenuList(m, "Start chat key", kKeyOpts, 1, 0, NULL, NULL);
    ShMenuList(m, "Candidate window", kCandOpts, 2,
               ShConfigGetInt(CFG_SECTION, CFG_KEY_CAND, 0),
               ChatOnCandMode, NULL);
    ShMenuHint(m, "Restart to apply.");
}

/* ---- framework hooks ------------------------------------------------ */

void ShChatStartup(void) {
    if (g_started) return;
    g_started = 1;
    InitializeCriticalSection(&g_lock);
    g_lockReady = 1;
    ChatCfgLoad();
    if (!Bind()) return;
    CreateThread(NULL, 0, ChatThread, NULL, 0, NULL);
}

int ShChatIsOpen(void) {
    return g_chat.open ? 1 : 0;
}

/* The F4 menu opens over the input box: drop it and release keys. */
void ShChatClose(void) {
    if (!g_chat.open) return;
    Lock();
    g_chat.open = 0;
    g_chat.cmd = 0;
    g_chat.len = 0;
    g_chat.text[0] = 0;
    Unlock();
    ReleaseKeys();
}

/* Snapshot for the overlay renderer (called on the render thread). */
void ShChatCapture(ShChatView *out) {
    int n;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    Lock();
    out->open  = g_chat.open ? 1 : 0;
    out->phase = g_chat.cmd ? 2 : (g_chat.open ? 1 : 0);
    n = WideCharToMultiByte(CP_UTF8, 0, g_chat.text, -1,
                            out->text, sizeof(out->text), NULL, NULL);
    Unlock();
    if (n <= 0) out->text[0] = 0;
    if (g_chat.cmd == 1)
        strncpy(out->hint, "sending...", sizeof(out->hint) - 1);
    else if (g_chat.open)
        strncpy(out->hint,
                "回车发送 · Esc 取消 · Tab 切换频道",
                sizeof(out->hint) - 1);
}
