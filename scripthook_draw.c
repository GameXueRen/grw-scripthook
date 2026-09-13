/* The drawing registry and the input session - the C half of the
 * plugin UI layer (the primitives are ImGui and live in
 * scripthook_ovl.cpp; see scripthook_draw.h for why it is split).
 *
 * What lives here:
 *   - the drawer table: a plugin registers a callback by name
 *     (ShDrawAdd) and the renderer calls it once per frame, inside an
 *     ImGui window titled with that name.  Nothing registered, nothing
 *     drawn, nothing paid.
 *   - the input session: one named text box at a time may own the
 *     keyboard (ShDrawInputOpen).  The session is what the window hook
 *     routes text into, what the IME probe keys off, and what the
 *     "Enter / Esc / Backspace belong to the IME" test answers for.
 *   - every exported wrapper: they forward to the renderer's vtable and
 *     are no-ops when no renderer is attached, so plugin code is the
 *     same in both build chains.
 *
 * The split of responsibility for a text box is deliberate and is the
 * one the game's input model forces: the framework owns the characters
 * (they arrive as WM_CHAR / WM_IME_CHAR on the game window and are the
 * only path a system IME can deliver text through) and the IME session
 * (composition string, candidate list, anchoring).  The OWNER owns the
 * buffer: it appends what ShDrawInputTake hands it, deletes, pastes and
 * decides what Enter means.  The framework never edits text and never
 * interprets a command key - so there is exactly one writer per byte.
 *
 * Threads: registration and ShDrawInputTake/Open/Close come from plugin
 * threads, the primitives and the frame dispatch run on the render
 * thread, WM_CHAR and the IME messages arrive on the window's thread.
 * One critical section guards the table and the session; no callback is
 * ever invoked with it held.
 */
#include <windows.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "scripthook_draw.h"
#include "log.h"

#define DRAW_LOG "scripthook_draw.log"

/* ---- one-time init and logging -------------------------------------- */

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_lock;

static BOOL CALLBACK DrawOnce(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_lock);
    LogInit(DRAW_LOG);
    return TRUE;
}

static void Ready(void)
{
    InitOnceExecuteOnce(&g_once, DrawOnce, NULL, NULL);
}

static void DrawLog(const char *fmt, ...)
{
    va_list ap;
    Ready();
    va_start(ap, fmt);
    Logv(fmt, ap);
    va_end(ap);
}

/* ---- the drawer table ----------------------------------------------- */

static ShDrawItem g_items[SH_DRAW_MAX];
/* Published by the renderer once ImGui is up.  Read on every primitive
 * call, so it is exchanged atomically rather than assigned. */
static const ShDrawVtbl *g_vt = NULL;

static const ShDrawVtbl *Vt(void)
{
    return (const ShDrawVtbl *)InterlockedCompareExchangePointer(
        (PVOID volatile *)&g_vt, NULL, NULL);
}

void ShDrawSetVtbl(const ShDrawVtbl *vt)
{
    if (!vt) return;
    if (Vt() == vt) return;
    InterlockedExchangePointer((PVOID volatile *)&g_vt, (PVOID)vt);
    DrawLog("renderer attached: the drawing primitives are live");
}

int ShDrawReady(void)
{
    return Vt() != NULL ? 1 : 0;
}

/* Both called with the lock held. */
static ShDrawItem *FindLocked(const char *name)
{
    int i;
    for (i = 0; i < SH_DRAW_MAX; i++)
        if (g_items[i].used && _stricmp(g_items[i].name, name) == 0)
            return &g_items[i];
    return NULL;
}

static ShDrawItem *FreeLocked(void)
{
    int i;
    for (i = 0; i < SH_DRAW_MAX; i++)
        if (!g_items[i].used) return &g_items[i];
    return NULL;
}

int ShDrawAddEx(const char *name, ShDrawFn fn, void *user,
                const ShDrawOpts *opts)
{
    ShDrawItem *it;

    Ready();
    if (!name || !name[0] || !fn) return 0;
    if (strlen(name) >= SH_DRAW_NAME_MAX) {
        DrawLog("drawer '%s' refused: the name is longer than %d",
                name, SH_DRAW_NAME_MAX - 1);
        return 0;
    }

    EnterCriticalSection(&g_lock);
    /* Registering an existing name is a replace, not a second entry:
     * a plugin that reloads its own callback would otherwise leak a
     * slot per attempt. */
    it = FindLocked(name);
    if (it) {
        it->fn = fn;
        it->user = user;
        it->shown = 1;
        if (opts) it->opts = *opts;
        LeaveCriticalSection(&g_lock);
        DrawLog("drawer '%s' replaced", name);
        return 1;
    }
    it = FreeLocked();
    if (!it) {
        LeaveCriticalSection(&g_lock);
        DrawLog("drawer '%s' refused: all %d slots are taken",
                name, SH_DRAW_MAX);
        return 0;
    }
    memset(it, 0, sizeof(*it));
    strncpy(it->name, name, SH_DRAW_NAME_MAX - 1);
    it->fn = fn;
    it->user = user;
    it->shown = 1;
    if (opts) it->opts = *opts;
    it->used = 1;
    LeaveCriticalSection(&g_lock);
    DrawLog("drawer '%s' registered (callback %p, user %p, flags 0x%x)",
            name, (void *)fn, user, it->opts.flags);
    return 1;
}

int ShDrawAdd(const char *name, ShDrawFn fn, void *user)
{
    return ShDrawAddEx(name, fn, user, NULL);
}

int ShDrawDel(const char *name)
{
    ShDrawItem *it;

    Ready();
    if (!name || !name[0]) return 0;
    EnterCriticalSection(&g_lock);
    it = FindLocked(name);
    if (it) {
        memset(it, 0, sizeof(*it));
    }
    LeaveCriticalSection(&g_lock);
    if (!it) return 0;
    DrawLog("drawer '%s' gone", name);
    return 1;
}

int ShDrawCount(void)
{
    int i, n = 0;
    Ready();
    EnterCriticalSection(&g_lock);
    for (i = 0; i < SH_DRAW_MAX; i++)
        if (g_items[i].used) n++;
    LeaveCriticalSection(&g_lock);
    return n;
}

void ShDrawSetShown(const char *name, int shown)
{
    ShDrawItem *it;
    Ready();
    if (!name || !name[0]) return;
    EnterCriticalSection(&g_lock);
    it = FindLocked(name);
    if (it && it->shown != (shown ? 1 : 0)) {
        it->shown = shown ? 1 : 0;
        LeaveCriticalSection(&g_lock);
        DrawLog("drawer '%s' %s", name, shown ? "shown" : "hidden");
        return;
    }
    LeaveCriticalSection(&g_lock);
}

int ShDrawShow(const char *name, int on)
{
    ShDrawItem *it;
    int found;
    Ready();
    if (!name || !name[0]) return 0;
    EnterCriticalSection(&g_lock);
    it = FindLocked(name);
    found = it != NULL;
    if (it) it->shown = on ? 1 : 0;
    LeaveCriticalSection(&g_lock);
    return found;
}

int ShDrawShown(const char *name)
{
    ShDrawItem *it;
    int shown = 0;
    Ready();
    if (!name || !name[0]) return 0;
    EnterCriticalSection(&g_lock);
    it = FindLocked(name);
    if (it) shown = it->shown;
    LeaveCriticalSection(&g_lock);
    return shown;
}

int ShDrawWantFrame(void)
{
    int i, any = 0;
    Ready();
    EnterCriticalSection(&g_lock);
    for (i = 0; i < SH_DRAW_MAX; i++)
        if (g_items[i].used && g_items[i].shown && g_items[i].fn) {
            any = 1;
            break;
        }
    LeaveCriticalSection(&g_lock);
    return any;
}

void ShDrawFrame(void)
{
    ShDrawItem items[SH_DRAW_MAX];
    const ShDrawVtbl *vt = Vt();
    int i, n = 0;

    if (!vt || !vt->begin || !vt->end) return;

    Ready();
    /* Snapshot first: a callback is free to register or drop drawers
     * while it runs, and it must not do so while we hold the lock (a
     * callback can be slow - it is on the Present path). */
    EnterCriticalSection(&g_lock);
    for (i = 0; i < SH_DRAW_MAX; i++)
        if (g_items[i].used && g_items[i].shown && g_items[i].fn)
            items[n++] = g_items[i];
    LeaveCriticalSection(&g_lock);

    for (i = 0; i < n; i++) {
        if (vt->begin(items[i].name, &items[i].opts))
            items[i].fn(items[i].user);
        vt->end();
    }
}

/* ---- primitives: thin forwarders to the renderer -------------------- */

#define VT_CALL(f, ...)                                            \
    do {                                                           \
        const ShDrawVtbl *v_ = Vt();                               \
        if (v_ && v_->f) v_->f(__VA_ARGS__);                       \
    } while (0)

#define VT_GET(ret, f, def, ...)                                   \
    (Vt() && Vt()->f ? Vt()->f(__VA_ARGS__) : (def))

float ShDrawScale(void)              { return VT_GET(float, scale, 1.0f); }
void  ShDrawText(const char *t)      { VT_CALL(text, t); }
void  ShDrawTextColored(const char *t, unsigned rgb, int a)
                                     { VT_CALL(text_colored, t, rgb, a); }
void  ShDrawTextWrapped(const char *t) { VT_CALL(text_wrapped, t); }
void  ShDrawHint(const char *t)      { VT_CALL(hint, t); }
void  ShDrawSpacing(void)            { VT_CALL(spacing); }
void  ShDrawSameLine(void)           { VT_CALL(same_line); }
void  ShDrawSeparator(void)          { VT_CALL(separator); }
void  ShDrawPanel(float w, float h, unsigned rgb, int a)
                                     { VT_CALL(panel, w, h, rgb, a); }
void  ShDrawRect(float w, float h, unsigned rgb, int a)
                                     { VT_CALL(rect, w, h, rgb, a); }
int   ShDrawButton(const char *label) { return VT_GET(int, button, 0, label); }
int   ShDrawToggle(const char *label, int *v)
                                     { return VT_GET(int, toggle, 0, label, v); }
int   ShDrawNumber(const char *label, int *v, int step, int mn, int mx)
                                     { return VT_GET(int, number, 0, label, v,
                                                     step, mn, mx); }
int   ShDrawSlider(const char *label, float *v, float mn, float mx)
                                     { return VT_GET(int, slider, 0, label, v,
                                                     mn, mx); }
int   ShDrawList(const char *label, int *idx, const char *const *items, int n)
                                     { return VT_GET(int, list, 0, label, idx,
                                                     items, n); }
void  ShDrawPushFont(int which)      { VT_CALL(push_font, which); }
void  ShDrawPopFont(void)            { VT_CALL(pop_font); }

/* ---- the input session ---------------------------------------------
 * One named box owns the keyboard at a time.  The framework does not
 * edit anything: it collects what the system delivers and hands it to
 * the owner, which is the only writer of its own buffer. */

#define IN_PEND_MAX 512            /* UTF-16 code units waiting to be read */

static char          g_inId[SH_DRAW_NAME_MAX];
static volatile LONG g_inOpen = 0;
static volatile LONG g_inMode = 0;    /* 0 self-drawn, 1 IME native  */
static volatile LONG g_inSending = 0; /* owner is injecting text     */
static wchar_t       g_pend[IN_PEND_MAX];
static int           g_pendLen = 0;
static int           g_pendFullLogged = 0;

int ShDrawInputOpen(const char *id)
{
    Ready();
    if (!id || !id[0] || strlen(id) >= SH_DRAW_NAME_MAX) return 0;
    EnterCriticalSection(&g_lock);
    strncpy(g_inId, id, SH_DRAW_NAME_MAX - 1);
    g_pendLen = 0;
    g_pendFullLogged = 0;
    LeaveCriticalSection(&g_lock);
    InterlockedExchange(&g_inOpen, 1);
    DrawLog("input session opened for box '%s'", id);
    return 1;
}

void ShDrawInputClose(void)
{
    Ready();
    if (InterlockedCompareExchange(&g_inOpen, 0, 0) == 0) return;
    InterlockedExchange(&g_inOpen, 0);
    EnterCriticalSection(&g_lock);
    g_pendLen = 0;
    g_inId[0] = 0;
    LeaveCriticalSection(&g_lock);
    DrawLog("input session closed");
}

int ShDrawInputIsOpen(void)
{
    return InterlockedCompareExchange(&g_inOpen, 0, 0) ? 1 : 0;
}

void ShDrawInputSetSending(int on)
{
    InterlockedExchange(&g_inSending, on ? 1 : 0);
}

int ShDrawInputSending(void)
{
    return InterlockedCompareExchange(&g_inSending, 0, 0) ? 1 : 0;
}

void ShDrawInputSetMode(int mode)
{
    InterlockedExchange(&g_inMode, mode ? 1 : 0);
}

int ShDrawInputGetMode(void)
{
    return (int)InterlockedCompareExchange(&g_inMode, 0, 0);
}

int ShDrawInputComposing(void)
{
    return VT_GET(int, composing, 0);
}

int ShDrawInputTake(char *out, int cap)
{
    int n = 0, keep;

    Ready();
    if (!out || cap <= 0) return 0;
    out[0] = 0;

    EnterCriticalSection(&g_lock);
    keep = g_pendLen;
    /* Convert the longest prefix that fits.  A unit that does not fit
     * stays queued, so a short caller buffer slows the drain down but
     * never loses a character - and a cut through a surrogate pair is
     * rejected by the converter and retried one unit shorter. */
    while (keep > 0) {
        int w = WideCharToMultiByte(CP_UTF8, 0, g_pend, keep,
                                    out, cap - 1, NULL, NULL);
        if (w > 0) {
            out[w] = 0;
            n = w;
            break;
        }
        keep--;
    }
    if (keep > 0) {
        if (keep < g_pendLen)
            memmove(g_pend, g_pend + keep,
                    (size_t)(g_pendLen - keep) * sizeof(wchar_t));
        g_pendLen -= keep;
        g_pend[g_pendLen] = 0;
    }
    LeaveCriticalSection(&g_lock);
    return n;
}

int ShDrawInputWndMsg(uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp)
{
    int open;
    (void)hwnd; (void)lp;

    open = ShDrawInputIsOpen();
    if (!open && !ShDrawInputSending()) return 0;

    if (msg == WM_CHAR || msg == WM_IME_CHAR) {
        /* While typing the character feeds the owner's buffer; while
         * sending it MUST fall through to the game window procedure -
         * that is the injection channel. */
        if (!open || wp < 0x20 || wp == 0x7F || wp >= 0x10000) return 0;
        EnterCriticalSection(&g_lock);
        if (g_pendLen < IN_PEND_MAX - 1) {
            g_pend[g_pendLen++] = (wchar_t)wp;
            g_pend[g_pendLen] = 0;
            LeaveCriticalSection(&g_lock);
            return 1;
        }
        if (!g_pendFullLogged) {
            g_pendFullLogged = 1;
            LeaveCriticalSection(&g_lock);
            DrawLog("input queue is full: text is being dropped - the "
                    "owner of the box is not calling ShDrawInputTake()");
            return 1;
        }
        LeaveCriticalSection(&g_lock);
        return 1;
    }

    /* Swallow the rest of the keyboard while the box is up (typing) OR
     * while the owner is injecting.  A physical Enter keyup that leaks
     * through during the injection makes the game submit before the
     * injected characters have all landed - the tail-truncation bug.
     * Tab still reaches the game while typing (channel switch). */
    if (msg == WM_KEYDOWN || msg == WM_KEYUP ||
        msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) {
        if ((uint32_t)wp == VK_TAB && open) return 0;
        return 1;
    }
    return 0;
}

int ShDrawInputBox(const char *id, const char *text, const char *hint,
                   ShDrawInput *out)
{
    const ShDrawVtbl *vt;
    int focused;

    Ready();
    if (out) memset(out, 0, sizeof(*out));

    EnterCriticalSection(&g_lock);
    focused = (g_inOpen && id && _stricmp(id, g_inId) == 0) ? 1 : 0;
    LeaveCriticalSection(&g_lock);

    if (out) {
        out->focused = focused;
        out->mode = (int)InterlockedCompareExchange(&g_inMode, 0, 0);
    }

    vt = Vt();
    if (!vt || !vt->input_box) return 0;
    /* The renderer fills in `composing` as it draws, because that state
     * lives with the IME and is only readable on the render thread. */
    return vt->input_box(id, text, hint, focused, out);
}
