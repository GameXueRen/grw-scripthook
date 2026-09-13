/* Chinese chat input, as a plugin (in-process, mirroring GRW-CNChat).
 *
 * The game's own text chat field cannot take IME composition - its
 * input is DirectInput based, so composed Chinese never reaches it.
 * GRW-CNChat solved this from outside with an AutoHotkey window that
 * owns a real Edit control (full IME), then posts the finished text
 * into the game window as WM_CHAR (verified to work on GRW).
 *
 * This plugin does the same thing inside the game process, and it is
 * also the second worked example of the drawing API (the first is
 * draw_sample): its box is a frameless drawer whose callback calls
 * ShDrawInputBox, exactly the way a third-party plugin would.  The
 * split of work is the framework's documented one:
 *
 *   the framework   the box's looks, its focus, the IME session
 *                   (composition string, candidate list, candidate
 *                   window anchoring), and the CHARACTERS - it collects
 *                   what the game window receives as WM_CHAR / WM_IME_CHAR
 *                   and hands them over through ShDrawInputTake.
 *   this plugin     the buffer (append, backspace, paste), the hotkey,
 *                   what Enter and Esc do, and this page of the menu.
 *
 * Editing is polled with GetAsyncKeyState exactly like the menu and the
 * gadget wheel, because the game reads its keyboard through DirectInput
 * and the command keys in the window's message queue are not a channel
 * to rely on:
 *   - The player presses the chat hotkey (default T).  The key is
 *     let through to the game (it opens its own chat box); when T
 *     comes back up we open the input box and capture the keyboard
 *     (ShCaptureKeys), so the game chat box stays open but never sees
 *     the keys we type.
 *   - Characters arrive through the framework's input session - that is
 *     the IME's own channel, so composed Chinese lands in the buffer.
 *   - Chinese text can also be brought in through the clipboard (Ctrl+V),
 *     which works regardless of the game's input handling.
 *   - Enter: release the capture, then PostMessage WM_CHAR for every
 *     character to the game window (the channel GRW-CNChat proved
 *     works on GRW), followed by an Enter key press to submit.
 *   - Esc: release and post Esc so the game closes its chat box.
 */
#include <windows.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include "scripthook.h"
#include "log.h"

#define VK_CHAT     0x54            /* 'T' */
#define POLL_MS     15

/* ---- shared state -------------------------------------------------
 * Text is edited on the poll thread only (paste, backspace) except
 * for the optional WM_CHAR path that some window modes may deliver,
 * so the buffer is guarded by a critical section.  The overlay
 * render thread reads a snapshot under the same lock. */
#define TEXT_MAX    400             /* UTF-16 code units */
typedef struct {
    volatile int open;              /* box visible / typing           */
    volatile int cmd;               /* 0 none 1 commit 2 cancel       */
    wchar_t text[TEXT_MAX];
    int     len;
    uint64_t hwnd;                  /* game window to post text into  */
} ChatState;

static ChatState g_chat;
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

/* Drain what the framework's input session collected since the last
 * poll.  The characters the game window receives - WM_CHAR, and
 * WM_IME_CHAR, which is how a system IME hands over composed text - now
 * arrive through that session (see ShDrawInputTake), and this is the
 * only place they are appended, on the thread that owns the buffer. */
static void TakePendingChars(void) {
    char got[256];
    while (ShDrawInputTake(got, (int)sizeof(got)) > 0) {
        wchar_t wide[128];
        int n = MultiByteToWideChar(CP_UTF8, 0, got, -1, wide, 128);
        int i;
        if (n <= 1) continue;   /* -1 bytes, or a conversion failure */
        Lock();
        for (i = 0; i < n - 1; i++) TextAppendLocked((unsigned)wide[i]);
        Unlock();
    }
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

/* ---- the box, drawn as a drawer --------------------------------------
 *
 * The chat box is drawn the way a plugin draws one: a frameless drawer
 * whose callback calls the input-box primitive.  That is what makes the
 * primitive provable - the framework's own box and a plugin's box are
 * literally the same code path - and it is why the overlay knows nothing
 * about the chat any more: its part ends at the shared widget.
 *
 * The drawer is registered once and shown only while the box is open, so
 * a closed box costs nothing at all (a hidden drawer is never called).
 */
#define DRAW_NAME       "cnchat"
#define VIEW_UTF8_MAX   512
#define ANCHOR_Y_RATIO  0.72f   /* where the box sits, screen fraction */

/* UTF-8 text + hint for one frame, taken under the lock.  Converting
 * the used length only matters: converting the whole buffer with -1
 * fails outright once the UTF-8 form outgrows the view (about 170 CJK
 * chars), which would blank the box mid-sentence. */
static void ChatSnapshot(char *text, int tcap, char *hint, int hcap) {
    int n = 0, cmd, open;

    text[0] = 0;
    hint[0] = 0;
    Lock();
    cmd  = g_chat.cmd;
    open = g_chat.open;
    if (open || cmd) {
        n = WideCharToMultiByte(CP_UTF8, 0, g_chat.text, g_chat.len,
                                text, tcap - 1, NULL, NULL);
        if (n <= 0 && g_chat.len > 0) {
            /* Still too long: keep the longest prefix that fits. */
            int keep = g_chat.len;
            while (keep > 1) {
                keep--;
                n = WideCharToMultiByte(CP_UTF8, 0, g_chat.text, keep,
                                        text, tcap - 1, NULL, NULL);
                if (n > 0) break;
            }
        }
    }
    Unlock();
    if (n > 0) text[n] = 0;   /* success already NULs; be exact */
    else text[0] = 0;

    if (cmd == 1)
        snprintf(hint, (size_t)hcap, "%s", "sending...");
    else if (open)
        snprintf(hint, (size_t)hcap, "%s",
                 "回车发送 · Esc 取消 · Tab 切换频道");
}

/* Called on the render thread, once per frame, while the box is shown. */
static void DrawChat(void *user) {
    char text[VIEW_UTF8_MAX];
    char hint[96];
    ShDrawInput in;

    (void)user;
    ChatSnapshot(text, (int)sizeof(text), hint, (int)sizeof(hint));
    ShDrawInputBox("chat", text, hint, &in);
}

static void ChatDrawerShow(int on) {
    ShDrawShow(DRAW_NAME, on ? 1 : 0);
}

static void ChatDrawerRegister(void) {
    ShDrawOpts o;

    memset(&o, 0, sizeof(o));
    /* Frameless (the box is a panel, not a window), pinned by screen
     * position rather than pixels: centred horizontally, top edge on the
     * 72% line - the same spot this box has always been drawn at. */
    o.flags = SH_DRAW_FRAMELESS | SH_DRAW_NO_MOVE |
              SH_DRAW_POS_CENTER_X | SH_DRAW_POS_Y_RATIO;
    o.y = ANCHOR_Y_RATIO;
    if (!ShDrawAddEx(DRAW_NAME, DrawChat, NULL, &o))
        Log("the chat drawer was refused - see logs\\scripthook_draw.log");
    ChatDrawerShow(0);
}

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
    /* Open the framework's input session for this box.  From here the
     * window hook routes the characters the game window receives into
     * it, the IME probe knows a box is up, and Enter / Esc / Backspace
     * are ours to poll while no composition is live. */
    ShDrawInputOpen("chat");
    ChatDrawerShow(1);   /* the drawer draws the box */
    Log("box opened, session %s, keys captured",
        ShDrawInputIsOpen() ? "live" : "refused");
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
/* The injection runs on its own thread: posting the characters and
 * holding Enter take seconds, and the poll loop must keep servicing
 * keys during all of it.  While g_sending is set the poll loop and the
 * window hook swallow every key, so nothing can start or leak in
 * mid-send. */
typedef struct {
    int     send;
    HWND    hwnd;
    int     len;
    wchar_t txt[TEXT_MAX];
} SendJob;

static SendJob g_sendJob;

static DWORD WINAPI SendThread(LPVOID arg) {
    (void)arg;
    if (g_sendJob.send && g_sendJob.hwnd && g_sendJob.txt[0]) {
        int i;
        /* Feed the text into the game's open chat box, then submit
         * with Enter - the exact channel GRW-CNChat uses. */
        for (i = 0; i < g_sendJob.len; i++) {
            PostMessageW(g_sendJob.hwnd, WM_CHAR,
                         (WPARAM)g_sendJob.txt[i], 1);
            Sleep(3);
        }
        /* Submit with a real Enter (the game reads it through
         * DirectInput, posted WM_KEYDOWN never submits).  DI is
         * un-blocked right after the burst; the press is held 800ms
         * so the down state survives even the stretched frames that
         * follow a long character burst (the game samples the
         * keyboard state per frame, not per event - a short hold is
         * what made long texts sporadically fail to auto-submit). */
        ReleaseKeys();
        Sleep(60);   /* DI re-attach after the capture comes off */
        InjectKeyDown(VK_RETURN);
        Sleep(800);
        InjectKeyUp(VK_RETURN);
    } else {
        /* Cancelled: close the game's chat box too. */
        ReleaseKeys();
        if (g_sendJob.hwnd) InjectKey(VK_ESCAPE);
    }
    g_sending = 0;
    ShDrawInputSetSending(0);   /* the hook may let the keys through again */
    ReleaseKeys();
    return 0;
}

/* Called by the poll thread after a command was set (open still 1). */
static void HandleDone(void) {
    int send;
    HWND hwnd;
    int len, i;

    Lock();
    send = (g_chat.cmd == 1);
    hwnd = (HWND)(uintptr_t)g_chat.hwnd;
    len = g_chat.len;
    for (i = 0; i < len; i++) g_sendJob.txt[i] = g_chat.text[i];
    g_sendJob.txt[len] = 0;
    g_chat.cmd = 0;
    g_chat.open = 0;
    g_chat.len = 0;
    g_chat.text[0] = 0;
    Unlock();
    /* Keep the keyboard CAPTURED while injecting: releasing it here
     * let the user's own physical Enter keyup leak through to the
     * game, which submitted immediately - before the injected
     * characters had all landed (tail truncation).  It is released by
     * the send thread once the submit has landed. */
    g_sending = 1;
    /* The box is gone (open is already 0), so the session ends here:
     * the injected characters now fall through to the game window -
     * that is the injection channel - while the physical keys keep
     * being swallowed until the send thread clears the flag. */
    ShDrawInputClose();
    ShDrawInputSetSending(1);
    ChatDrawerShow(0);   /* the box is gone; sending has no box */
    Log("%s: %d character(s) to the game window",
        send ? "sending" : "cancelled", send ? len : 0);

    g_sendJob.send = send;
    g_sendJob.hwnd = hwnd;
    g_sendJob.len = len;

    {
        HANDLE h = CreateThread(NULL, 0, SendThread, NULL, 0, NULL);
        if (h) CloseHandle(h);
        else {
            /* Nobody else releases the keyboard: with g_sending
             * stuck at 1 every key would stay swallowed. */
            g_sending = 0;
            ShDrawInputSetSending(0);
            ReleaseKeys();
        }
    }
}

/* ---- runtime configuration ------------------------------------------
 * Persisted in the plugin's own ini, plugins\cnchat\cnchat.ini:
 *   [Settings]
 *   enabled  = 0/1   (default 0: the self-drawn box is off)
 *   startkey = VK    (default 0x54 'T'; must match the in-game chat key)
 *   candmode = 0/1   (0 overlay-drawn candidate list [default],
 *                     1 the input method's own candidate window)
 * Loaded once at startup; menu toggles update the globals AND write the
 * ini so the change sticks.  The poll thread reads these globals live,
 * and the candidate mode is also pushed into the framework's input
 * session, which is what draws (or steers) the candidates.
 * Defined before ChatThread, which reads them every poll. */
#define INI_SECTION "Settings"
#define CFG_KEY_CFG  "enabled"
#define CFG_KEY_KEY  "startkey"
#define CFG_KEY_CAND "candmode"

static volatile int g_cfgEnabled = 0;   /* default off */
static volatile int g_cfgKey     = VK_CHAT; /* 'T' */
static volatile int g_cfgCand    = 0;   /* default overlay-drawn */

static HINSTANCE g_inst;
static char      g_iniPath[MAX_PATH];

/* plugins\cnchat\cnchat.ini, derived from this module's own file name -
 * the same convention every plugin uses (see draw_sample.c). */
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
    return g_iniPath[0] ? GetPrivateProfileIntA(INI_SECTION, key, def,
                                                g_iniPath)
                        : def;
}

static void IniSet(const char *key, int v) {
    char buf[16];
    if (!g_iniPath[0]) return;
    snprintf(buf, sizeof(buf), "%d", v);
    WritePrivateProfileStringA(INI_SECTION, key, buf, g_iniPath);
}

/* ---- poll thread ---------------------------------------------------- */

/* Defined with the box's open/close path below; the poll thread is what
 * calls it most (menu opened, feature switched off). */
static void ChatClose(void);

static DWORD WINAPI ChatThread(LPVOID arg) {
    (void)arg;
    int tDown = 0;
    int compLatch = 0;   /* just left a composition; see below   */
    DWORD backNext = 0;  /* when a held Backspace repeats next  */
    for (;;) {
        Sleep(POLL_MS);

        /* Injection in progress on the send thread: swallow every key
         * (poll-side too) so nothing starts or leaks mid-send. */
        if (g_sending) {
            memset(g_keyWas, 0, sizeof(g_keyWas));
            continue;
        }

        if (ShMenuIsOpen()) {
            if (g_chat.open) ChatClose();
            memset(g_keyWas, 0, sizeof(g_keyWas));
            tDown = 0;
            continue;
        }

        /* Feature switched off (default): never touch the chat key. */
        if (!g_cfgEnabled) {
            if (g_chat.open) ChatClose();
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
        TakePendingChars();
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
        if (ShDrawInputComposing()) {
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
    g_cfgEnabled = IniInt(CFG_KEY_CFG, 0) ? 1 : 0;
    g_cfgKey = IniInt(CFG_KEY_KEY, VK_CHAT);
    if (g_cfgKey < 1 || g_cfgKey > 0xFE) g_cfgKey = VK_CHAT;
    g_cfgCand = IniInt(CFG_KEY_CAND, 0) ? 1 : 0;
}

/* The candidate mode is the one setting that is both live and shared:
 * the framework draws (or steers) the candidate window by it, so it goes
 * into the input session as well as staying in this plugin's ini. */
static void ChatSetCandMode(int mode) {
    g_cfgCand = mode ? 1 : 0;
    IniSet(CFG_KEY_CAND, g_cfgCand);
    ShDrawInputSetMode(g_cfgCand);
}

/* ---- the plugin's own menu page -------------------------------------
 * A root page of its own, like every other plugin.  The rows read and
 * write this plugin's ini directly; "Enabled" only takes effect on the
 * next launch (the config is read once at startup), so it is not
 * mirrored into g_cfg* live.  CandMode is applied live through
 * ChatSetCandMode, which keeps the ini in sync. */

static void ChatOnEnabled(uint32_t menu, uint32_t item, int value,
                          void *user) {
    (void)menu; (void)item; (void)user;
    /* Restart to apply: do not touch the running g_cfgEnabled. */
    IniSet(CFG_KEY_CFG, value ? 1 : 0);
}

static void ChatOnCandMode(uint32_t menu, uint32_t item, int value,
                           void *user) {
    (void)menu; (void)item; (void)user;
    ChatSetCandMode(value);
}

static void BuildMenu(void) {
    static const char *kCandOpts[] = { "Self-drawn", "IME native" };
    /* The start key is not user-configurable yet: it is fixed to the
     * game's own text-chat key ("T").  A single-option list shows the
     * current value without arming a key capture. */
    static const char *kKeyOpts[] = { "T" };
    uint32_t m;

    m = ShMenuCreate("Chinese chat box");
    if (!m) return;

    ShMenuToggle(m, "Enabled", IniInt(CFG_KEY_CFG, 0),
                 ChatOnEnabled, NULL);
    ShMenuList(m, "Start chat key", kKeyOpts, 1, 0, NULL, NULL);
    ShMenuList(m, "Candidate window", kCandOpts, 2,
               IniInt(CFG_KEY_CAND, 0), ChatOnCandMode, NULL);
    ShMenuHint(m, "Restart to apply.");
    Log("menu page created");
}

/* ---- plugin entry ---------------------------------------------------- */

static DWORD WINAPI InitThread(LPVOID p) {
    (void)p;

    /* log.h: this translation unit gets its own file and its own Log. */
    LogInit("cnchat.log");
    Log("--- chinese chat box ---");

    ResolveIniPath();
    InitializeCriticalSection(&g_lock);
    g_lockReady = 1;
    ChatCfgLoad();
    ShDrawInputSetMode(g_cfgCand);   /* the framework reads it live */
    ChatDrawerRegister();
    Log("drawer '%s' registered, enabled=%d startkey=0x%02X candmode=%d",
        DRAW_NAME, g_cfgEnabled, g_cfgKey, g_cfgCand);

    /* A text-input utility with nothing game-specific in it: it was a
     * framework module until it became a plugin, and it worked in every
     * mode then.  Say so explicitly - a plugin that stays silent gets the
     * default (no Ghost War, no mercenaries). */
    if (!ShPluginBlacklist(SH_MODE_BLACKLIST_NONE))
        Log("the blacklist declaration was refused");

    if (!Bind()) {
        Log("the game state reader is missing: the box stays off");
        return 0;
    }
    BuildMenu();
    CreateThread(NULL, 0, ChatThread, NULL, 0, NULL);
    Log("poll thread up");
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

/* The F4 menu opens over the input box, or the feature is switched off:
 * drop the box and release the keys.  The framework's session is closed
 * first, so the IME stack is restored and the characters stop being
 * collected even if this runs while the poll thread is between checks;
 * the drawer is hidden so the box stops being drawn the same frame. */
static void ChatClose(void) {
    ShDrawInputClose();
    ChatDrawerShow(0);
    if (!g_chat.open) return;
    Lock();
    g_chat.open = 0;
    g_chat.cmd = 0;
    g_chat.len = 0;
    g_chat.text[0] = 0;
    Unlock();
    ReleaseKeys();
}

/* ---- the box, drawn as a drawer -------------------------------------- */
