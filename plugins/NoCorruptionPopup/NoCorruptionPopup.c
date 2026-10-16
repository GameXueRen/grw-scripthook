/* NoCorruptionPopup - answers the "A corruption of the game files was
 * detected" box so that it is never shown.
 *
 * The box is a plain MessageBoxW (caption "ERROR", type 0x41034) whose
 * CONTINUE button is the No answer: the caller - GRW.exe+0x154EB0 - does
 * `cmp eax, 6` and quits only on Yes. So a matching call is answered with
 * IDNO here and MessageBoxW is not called at all; the caller cannot tell
 * the difference and the box is never created.
 *
 * Only user32's exports are hooked, no game code and no game file is
 * touched, and every switch defaults to off.
 *
 * The box was identified with a read-only probe; the evidence and the
 * addresses are in ../../GameXueRen/documents/scripthook-docs-20261016/corruption-popup-suppression-analysis.md.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "scripthook.h"

/* What this plugin needs of the framework: the shared hook layer (the
 * ShHook* group), which this line carries under version 2 - see the
 * exception in the header's version note. The header is included for that
 * declaration; every entry point this plugin calls - the menu, the text,
 * the hooks - is bound by name at run time, so including it imports nothing
 * and this file links nothing but log.h. */
SH_REQUIRES_API(2);
#include "log.h"

#define STACK_MAX 12

/* Filled in by LoadConfig, but declared here because the watcher threads
 * created at startup read them and the config block sits further down. */
static int g_watchSeconds = 300;
static int g_windowScanMs = 400;
static int g_peekPollMs   = 200;

/* ---- what to do about the box -----------------------------------------
 *
 * All three are off by default: with nothing switched on this plugin
 * installs no hooks and starts no threads, so it does nothing at all to
 * the game. Turn one on in the ini or from the in-game menu.
 *
 * suppress : answer the box instead of showing it.
 * probe    : diagnostic logging and the startup watchers.
 * fallback : click CONTINUE if a box turns up anyway.
 *
 * The markers are narrow on purpose - see the header. match_type is
 * compared exactly; the caption is compared case-insensitively for
 * equality; the text is a case-insensitive substring. An empty caption
 * or text, or a zero type, switches that marker off, and if all three
 * are off nothing is suppressed and the log says so.
 */
static volatile int  g_probe    = 0;
static volatile LONG g_suppress = 0;     /* read by the hook on every call */
static volatile int  g_fallback = 0;
static int  g_matchType    = 0x41034;
static int  g_requireYesNo = 1;
static int  g_returnValue  = 7;          /* IDNO = the CONTINUE button */
static char g_capA[64]     = "ERROR";
static char g_txtA[128]    = "corruption of the game files";
static char g_contA[64]    = "CONTINUE"; /* the button the fallback clicks */
static WCHAR g_capW[64];
static WCHAR g_txtW[128];
static WCHAR g_contW[64];

static volatile LONG g_suppressed;      /* how many boxes were answered */
static volatile LONG g_clicked;         /* how many were clicked instead */

/* The switches, live: the menu writes the ini and flips these, and the
 * detours and the watchers read them on every call - so a flip takes
 * effect on the calls that follow. Every read is on a cold path (a modal
 * dialog API, the watchers' loops), never on a hot one. */
static int PopupSuppress(void) {
    return InterlockedCompareExchange(&g_suppress, 0, 0) ? 1 : 0;
}
static int PopupProbe(void) {
    return g_probe ? 1 : 0;
}
static int PopupFallback(void) {
    return g_fallback ? 1 : 0;
}

/* ---- addresses in the game image -------------------------------------
 *
 * RVAs, not VAs: the image base is 0x140000000 at the link address but
 * an ASLR'd process puts it somewhere else, so everything is expressed
 * relative and added to the base of the main module at runtime.
 *
 * These are the four UIString globals the box is built from, and the
 * one-shot guard byte that flips to 1 the moment the path that shows the
 * box is entered. Read only, and only after a range check.
 */
#define RVA_GUARD     0x04B86E49u   /* byte; 0 -> 1 when the box is asked for */
#define RVA_TXT_TEXT  0x04B86E10u   /* WCHAR* "A corruption of the game..."  */
#define RVA_TXT_TITLE 0x04B86E18u   /* WCHAR* "ERROR"                        */
#define RVA_TXT_CLOSE 0x04B86E20u   /* WCHAR* "CLOSE"                        */
#define RVA_TXT_CONT  0x04B86E28u   /* WCHAR* "CONTINUE"                     */

/* ---- framework bindings ---------------------------------------------- */

typedef int (*LangDeclare_t)(const char *owner, const char *lang,
                             const void *rows, int n);

/* ---- logging --------------------------------------------------------- */

static volatile LONG g_logMade;

static void PLog(const char *fmt, ...) {
    va_list ap;

    if (!g_logMade && InterlockedCompareExchange(&g_logMade, 1, 0) == 0)
        LogInitAlways("NoCorruptionPopup.log");
    va_start(ap, fmt);
    Logv(fmt, ap);
    va_end(ap);
}

/* "GRW.exe+0x1A2B3C", or a bare pointer for anything not in a module. */
static void DescribeAddr(void *addr, char *out, size_t n) {
    HMODULE mod = NULL;
    char file[MAX_PATH];
    const char *base;
    uintptr_t off;

    out[0] = 0;
    if (!addr) {
        snprintf(out, n, "(null)");
        return;
    }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) && mod) {
        if (!GetModuleFileNameA(mod, file, sizeof(file)))
            snprintf(file, sizeof(file), "?");
        base = strrchr(file, '\\');
        base = base ? base + 1 : file;
        off = (uintptr_t)addr - (uintptr_t)mod;
        snprintf(out, n, "%s+0x%llX", base, (unsigned long long)off);
        return;
    }
    snprintf(out, n, "0x%p", addr);
}

/* ---- walking the stack ----------------------------------------------- */

typedef USHORT (WINAPI *CaptureStack_t)(ULONG, ULONG, PVOID *, PULONG);
static CaptureStack_t g_captureStack;

static void BindStackWalk(void) {
    static const char *mods[] = { "ntdll.dll", "kernelbase.dll",
                                  "kernel32.dll" };
    static const char *fns[]  = { "RtlCaptureStackBackTrace",
                                  "CaptureStackBackTrace" };
    int i, j;

    for (i = 0; i < (int)(sizeof(mods) / sizeof(mods[0])) && !g_captureStack;
         i++) {
        HMODULE m = GetModuleHandleA(mods[i]);
        if (!m) continue;
        for (j = 0; j < (int)(sizeof(fns) / sizeof(fns[0])); j++) {
            g_captureStack = (CaptureStack_t)GetProcAddress(m, fns[j]);
            if (g_captureStack) {
                PLog("stack walk: %s!%s", mods[i], fns[j]);
                return;
            }
        }
    }
    PLog("stack walk: not found - callers will not be named");
}

/* The caller that matters is the game's own frame, which sits under a
 * couple of runtime frames, so frames are logged whole rather than
 * guessed at. */
static void LogStack(const char *what) {
    void *frames[STACK_MAX];
    char name[96];
    USHORT n = 0, i;

    if (g_captureStack)
        n = g_captureStack(0, STACK_MAX, frames, NULL);
    for (i = 0; i < n; i++) {
        DescribeAddr(frames[i], name, sizeof(name));
        PLog("   %s frame[%u] %s", what, (unsigned)i, name);
    }
    if (!n)
        PLog("   %s (no stack: CaptureStackBackTrace unavailable)", what);
}


/* ---- text helpers ---------------------------------------------------- */

/* Values at or below this are not pointers.
 *
 * CreateWindowExW's lpClassName may be MAKEINTATOM(atom) - a 16-bit
 * registered-class atom standing in for a string pointer - and the first
 * build of this probe read one as a string. It faulted on
 * NoCorruptionPopup.asi+0x14A7 reading 0xC03E, an atom the Uplay overlay
 * passes when it creates its window. Atoms registered by RegisterClass
 * live in 0xC000..0xFFFF, so anything in that range is named, not read. */
#define ATOM_CEILING 0xFFFFu

/* The loop bodies, split out so the callers can wrap them in SEH: a
 * pointer handed over by somebody else can fault part way through a
 * string that looked fine at the first character. */
static void CopyW(const wchar_t *s, char *out, size_t n) {
    size_t i = 0;
    int lastSpace = 0;

    for (; *s && i + 1 < n; s++) {
        wchar_t c = *s;
        if (c == L'\r' || c == L'\n' || c == L'\t' || c == L' ') {
            if (lastSpace) continue;
            out[i++] = ' ';
            lastSpace = 1;
            continue;
        }
        lastSpace = 0;
        out[i++] = (c < 32 || c > 126) ? '?' : (char)c;
    }
    out[i] = 0;
}

static void CopyA(const char *s, char *out, size_t n) {
    size_t i = 0;
    int lastSpace = 0;

    for (; *s && i + 1 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            if (lastSpace) continue;
            out[i++] = ' ';
            lastSpace = 1;
            continue;
        }
        lastSpace = 0;
        out[i++] = (c < 32 || c > 126) ? '?' : (char)c;
    }
    out[i] = 0;
}

/* One line per string, runs of whitespace collapsed: the box's own text
 * runs to three lines and would otherwise split the log. */
static void OneLineW(const wchar_t *s, char *out, size_t n) {
    out[0] = 0;
    if (!s) { snprintf(out, n, "(null)"); return; }
    if ((uintptr_t)s <= ATOM_CEILING) {
        snprintf(out, n, "<atom 0x%04X>", (unsigned)(uintptr_t)s);
        return;
    }
    __try {
        CopyW(s, out, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(out, n, "<unreadable @0x%llX>",
                 (unsigned long long)(uintptr_t)s);
    }
}

static void OneLineA(const char *s, char *out, size_t n) {
    out[0] = 0;
    if (!s) { snprintf(out, n, "(null)"); return; }
    if ((uintptr_t)s <= ATOM_CEILING) {
        snprintf(out, n, "<atom 0x%04X>", (unsigned)(uintptr_t)s);
        return;
    }
    __try {
        CopyA(s, out, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(out, n, "<unreadable @0x%llX>",
                 (unsigned long long)(uintptr_t)s);
    }
}

/* ---- matching the box --------------------------------------------------
 *
 * Case-insensitive and ASCII-only: every marker is ASCII, and a caption
 * that is not ASCII could not equal one anyway. The comparisons run
 * inside SEH because the strings belong to the caller - a faulting
 * compare must not take the game with it, and "did not match" is the
 * safe answer, since that only means the box is passed through.
 */
static int LowerA(int c) {
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static WCHAR LowerW(WCHAR c) {
    return (c >= L'A' && c <= L'Z') ? (WCHAR)(c - L'A' + L'a') : c;
}

static int EqualNA(const char *s, const char *pat) {
    if (!pat || !*pat || !s) return 0;
    for (; *s && *pat; s++, pat++)
        if (LowerA((unsigned char)*s) != LowerA((unsigned char)*pat)) return 0;
    return *s == 0 && *pat == 0;
}

static int EqualNW(const WCHAR *s, const WCHAR *pat) {
    if (!pat || !*pat || !s) return 0;
    for (; *s && *pat; s++, pat++)
        if (LowerW(*s) != LowerW(*pat)) return 0;
    return *s == 0 && *pat == 0;
}

static int HasNA(const char *s, const char *pat) {
    if (!pat || !*pat || !s) return 0;
    for (; *s; s++) {
        const char *a = s, *b = pat;
        while (*a && *b && LowerA((unsigned char)*a) == LowerA((unsigned char)*b)) {
            a++; b++;
        }
        if (!*b) return 1;
    }
    return 0;
}

static int HasNW(const WCHAR *s, const WCHAR *pat) {
    if (!pat || !*pat || !s) return 0;
    for (; *s; s++) {
        const WCHAR *a = s, *b = pat;
        while (*a && *b && LowerW(*a) == LowerW(*b)) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

/* The decision, shared by both message-box encodings: the button field
 * must be MB_YESNO, and at least one configured marker must hit. */
static int MatchMarkers(UINT type, int capEq, int txtHas, char *why,
                        size_t wn) {
    int byType = 0, byCap = 0, byText = 0;

    why[0] = 0;
    if (g_requireYesNo && (type & 0xF) != 0x4) return 0;   /* MB_YESNO */
    if (g_matchType && type == (UINT)g_matchType) byType = 1;
    if (capEq) byCap = 1;
    if (txtHas) byText = 1;
    if (!byType && !byCap && !byText) return 0;

    snprintf(why, wn, "%s%s%s%s%s",
             byType ? "type" : "",
             byCap ? (byType ? "+caption" : "caption") : "",
             byText ? ((byType || byCap) ? "+text" : "text") : "",
             "", "");
    return 1;
}

static int MatchBoxW(LPCWSTR caption, LPCWSTR text, UINT type, char *why,
                     size_t wn) {
    int capEq = 0, txtHas = 0;

    __try {
        capEq  = EqualNW(caption, g_capW);
        txtHas = HasNW(text, g_txtW);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        capEq = 0;
        txtHas = 0;
    }
    return MatchMarkers(type, capEq, txtHas, why, wn);
}

static int MatchBoxA(LPCSTR caption, LPCSTR text, UINT type, char *why,
                     size_t wn) {
    int capEq = 0, txtHas = 0;

    __try {
        capEq  = EqualNA(caption, g_capA);
        txtHas = HasNA(text, g_txtA);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        capEq = 0;
        txtHas = 0;
    }
    return MatchMarkers(type, capEq, txtHas, why, wn);
}

/* True when no marker is configured, which would make suppression a
 * "swallow every Yes/No box" switch - warned about at startup. */
static int NoMarkersConfigured(void) {
    return (!g_matchType && !g_capA[0] && !g_txtA[0]) ? 1 : 0;
}

static void ClassOf(HWND h, char *out, size_t n) {
    if (!h || !GetClassNameA(h, out, (int)n)) snprintf(out, n, "?");
}

static void TitleOf(HWND h, char *out, size_t n) {
    if (!h || !GetWindowTextA(h, out, (int)n)) snprintf(out, n, "(none)");
}

/* ---- window scanning -------------------------------------------------
 *
 * Deliberately independent of the API hooks: whatever puts the box up,
 * if it owns a window this notices. Only windows of this process are
 * listed - the box belongs to us and the desktop's own windows are
 * noise - and children are walked as well as top levels, because an
 * engine that draws its own dialogs often parents them.
 */

#define WIN_LOG_MAX 256

typedef struct {
    HWND  seen[WIN_LOG_MAX];
    int   n;
    int   over;      /* the table filled up: stop listing, stop growing */
} Seen;

static int SeenHas(const Seen *s, HWND h) {
    int i;
    for (i = 0; i < s->n; i++)
        if (s->seen[i] == h) return 1;
    return 0;
}

/* Returns 1 when the window was taken into the table and should be
 * described. A full table returns 0 rather than letting the same window
 * be described on every scan for the rest of the session. */
static int SeenAdd(Seen *s, HWND h) {
    if (s->n < WIN_LOG_MAX) { s->seen[s->n++] = h; return 1; }
    s->over = 1;
    return 0;
}

static void DescribeWindow(HWND h, const char *tag) {
    char cls[64], title[160];
    DWORD pid = 0, tid;
    RECT r = { 0, 0, 0, 0 };

    tid = GetWindowThreadProcessId(h, &pid);
    ClassOf(h, cls, sizeof(cls));
    TitleOf(h, title, sizeof(title));
    GetWindowRect(h, &r);
    PLog("%s hwnd=%p pid=%lu tid=%lu cls=\"%s\" title=\"%s\" "
         "style=0x%08lX ex=0x%08lX parent=%p vis=%d rect=%ld,%ld,%ld,%ld",
         tag, (void *)h, (unsigned long)pid, (unsigned long)tid, cls, title,
         (unsigned long)GetWindowLongPtrA(h, GWL_STYLE),
         (unsigned long)GetWindowLongPtrA(h, GWL_EXSTYLE),
         (void *)GetParent(h), (int)IsWindowVisible(h),
         (long)r.left, (long)r.top, (long)r.right, (long)r.bottom);
}

/* Quiet by design: a scan that finds nothing new says nothing. The first
 * scan reports what it walked so the log shows the scan is alive, and
 * the tag is "startup" then. */
static void ScanOwnWindows(Seen *seen, const char *tag) {
    DWORD me = GetCurrentProcessId();
    HWND h;
    int i = 0, found = 0;
    static volatile LONG capWarned;

    for (h = GetTopWindow(NULL); h && i < 4000; h = GetWindow(h, GW_HWNDNEXT)) {
        DWORD pid = 0;
        HWND c;

        i++;
        GetWindowThreadProcessId(h, &pid);
        if (pid != me) continue;
        if (!SeenHas(seen, h) && SeenAdd(seen, h)) {
            DescribeWindow(h, tag);
            found++;
        }
        for (c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            DWORD cpid = 0;
            GetWindowThreadProcessId(c, &cpid);
            if (cpid != me) continue;
            if (!SeenHas(seen, c) && SeenAdd(seen, c)) {
                DescribeWindow(c, "  child");
                found++;
            }
        }
    }
    if (found || tag[0] == 's')
        PLog("%s: walked %d top-level windows, %d new for this process",
             tag, i, found);
    if (seen->over && InterlockedExchange(&capWarned, 1) == 0)
        PLog("note: the window table is full (%d); later windows of this "
             "process are not listed", WIN_LOG_MAX);
}

/* ---- the fallback: click CONTINUE -------------------------------------
 *
 * Independent of the MessageBox hook and of matching by text, so it
 * covers the case the hook cannot: a box that was put up by some path
 * the hook never saw, or a build whose text did not match. It looks for
 * a dialog of the system class wearing the box's caption, carrying the
 * box's text in one of its children, with a button labelled CONTINUE -
 * all three, or it does nothing. Under a working suppression the box is
 * never created and this never fires.
 *
 * BM_CLICK is posted rather than sent: the box runs a modal loop on the
 * game's own thread, and posting keeps this thread out of it.
 */
static HWND g_lastBox;

static void ClickContinueIfBoxed(void) {
    DWORD me = GetCurrentProcessId();
    HWND h;

    for (h = GetTopWindow(NULL); h; h = GetWindow(h, GW_HWNDNEXT)) {
        char cls[64], cap[128];
        DWORD pid = 0;
        HWND c, cont = NULL;
        int hasText = 0;

        GetWindowThreadProcessId(h, &pid);
        if (pid != me) continue;
        ClassOf(h, cls, sizeof(cls));
        if (_stricmp(cls, "#32770") != 0) continue;   /* the dialog class */
        TitleOf(h, cap, sizeof(cap));
        if (g_capA[0] && !EqualNA(cap, g_capA)) continue;

        for (c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            char t[280], ccls[64];

            ClassOf(c, ccls, sizeof(ccls));
            TitleOf(c, t, sizeof(t));
            if (_stricmp(ccls, "Button") == 0 && g_contA[0] &&
                EqualNA(t, g_contA))
                cont = c;
            else if (g_txtA[0] && HasNA(t, g_txtA))
                hasText = 1;
        }
        if (!cont) continue;
        if (g_txtA[0] && !hasText) continue;   /* the caption alone is not enough */
        if (h == g_lastBox) continue;          /* this one is already pressed */

        g_lastBox = h;
        InterlockedIncrement(&g_clicked);
        PLog("** FALLBACK: clicking \"%s\" on #32770 \"%s\" hwnd=%p "
             "(the hook did not answer this one)", g_contA, cap, (void *)h);
        PostMessageA(cont, BM_CLICK, 0, 0);
    }
}

/* ---- read-only peeks at the game image ------------------------------- */

static HMODULE g_game;          /* GRW.exe / GRW_Upp.exe */
static uintptr_t g_gameBase;
static size_t g_gameSize;
static int g_peekOk;

static size_t ImageSize(HMODULE m) {
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)m;
    IMAGE_NT_HEADERS *nt;

    if (!m || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS *)((char *)m + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->OptionalHeader.SizeOfImage;
}

static void BindGameImage(void) {
    char name[MAX_PATH] = "";
    const char *base;

    if (g_game) return;                  /* bound once; the base never moves */
    g_game = GetModuleHandleA(NULL);
    if (!g_game || !GetModuleFileNameA(g_game, name, sizeof(name))) {
        PLog("peek: main module unknown - the guard byte will not be watched");
        return;
    }
    base = strrchr(name, '\\');
    base = base ? base + 1 : name;
    g_gameBase = (uintptr_t)g_game;
    g_gameSize = ImageSize(g_game);
    PLog("peek: main module %s base=0x%llX size=0x%llX",
         base, (unsigned long long)g_gameBase, (unsigned long long)g_gameSize);

    /* The RVAs are only meaningful in the two builds they were read
     * from; anything else and the peek is skipped rather than trusted. */
    if ((_stricmp(base, "GRW.exe") == 0 ||
         _stricmp(base, "GRW_Upp.exe") == 0) && g_gameSize > 0x5000000u) {
        g_peekOk = 1;
        PLog("peek: recognised build - guard byte and UIString globals are "
             "watched (read only)");
    } else {
        PLog("peek: unrecognised build \"%s\" - peek disabled", base);
    }
}

/* A read is only attempted for an address that lies inside the image and
 * is committed, so the probe cannot fault on a build it does not know -
 * the names check above is belt and braces on top of this. */
static int InsideImage(uintptr_t a, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;

    if (!g_peekOk) return 0;
    if (a < g_gameBase || a + n > g_gameBase + g_gameSize) return 0;
    if (VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) != sizeof(mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return 1;
}

static unsigned char PeekByte(uintptr_t rva) {
    uintptr_t a = g_gameBase + rva;
    if (!InsideImage(a, 1)) return 0;
    return *(volatile unsigned char *)a;
}

/* The globals hold WCHAR*, or zero before the constructor has run. The
 * pointer is followed only when it lands back inside the image, which is
 * where the literals live. */
static void PeekString(uintptr_t rva, const char *label) {
    uintptr_t slot = g_gameBase + rva;
    uintptr_t p;
    char text[220];

    if (!InsideImage(slot, sizeof(void *))) return;
    p = *(uintptr_t *)slot;
    if (!p) { PLog("   %-8s slot=0x%llX -> null", label,
                   (unsigned long long)slot); return; }
    if (!InsideImage(p, 2)) {
        PLog("   %-8s slot=0x%llX -> 0x%llX (outside the image)",
             label, (unsigned long long)slot, (unsigned long long)p);
        return;
    }
    OneLineW((const wchar_t *)p, text, sizeof(text));
    PLog("   %-8s slot=0x%llX -> 0x%llX \"%s\"",
         label, (unsigned long long)slot, (unsigned long long)p, text);
}

static void DumpGlobals(const char *why) {
    PLog("globals (%s):", why);
    PeekString(RVA_TXT_TEXT,  "text");
    PeekString(RVA_TXT_TITLE, "title");
    PeekString(RVA_TXT_CLOSE, "close");
    PeekString(RVA_TXT_CONT,  "continue");
}

/* ---- the API hooks ---------------------------------------------------
 *
 * Every one of these records and then calls the original. The return
 * value of the MessageBox pair is logged because it is the cheapest
 * possible answer to "is the box a MessageBox" - if it is, the text will
 * appear here and the button the player pressed will be logged with it.
 */

typedef int   (WINAPI *MessageBoxW_t)(HWND, LPCWSTR, LPCWSTR, UINT);
typedef int   (WINAPI *MessageBoxA_t)(HWND, LPCSTR, LPCSTR, UINT);
typedef HWND  (WINAPI *CreateWindowExW_t)(DWORD, LPCWSTR, LPCWSTR, DWORD,
                                          int, int, int, int, HWND, HMENU,
                                          HINSTANCE, LPVOID);
typedef BOOL  (WINAPI *ShowWindow_t)(HWND, int);
typedef BOOL  (WINAPI *TerminateProcess_t)(HANDLE, UINT);
typedef VOID  (WINAPI *ExitProcess_t)(UINT);

static MessageBoxW_t      g_realMBW;
static MessageBoxA_t      g_realMBA;
static CreateWindowExW_t  g_realCW;
static ShowWindow_t       g_realSW;
static TerminateProcess_t g_realTP;
static ExitProcess_t      g_realEP;

/* A hook can run again on the same thread before it returns: reading a
 * window's text sends WM_GETTEXT, the game's own window procedure
 * handles it and may create or show a window, and that lands back in
 * these hooks. Naming that window is what would recurse, so a hook that
 * finds this flag already set records nothing and returns. */
static __declspec(thread) int t_inHook;

static int HookEnter(void) {
    if (t_inHook) return 0;
    t_inHook = 1;
    return 1;
}

static void HookLeave(void) {
    t_inHook = 0;
}

/* One shot each: the game boxes things more than once and the log only
 * needs the first sighting of each kind to be useful. */
static volatile LONG g_exitSeen, g_guardFired;

static int WINAPI HookMessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption,
                                  UINT type) {
    char t[220], c[120], why[48];
    int r, log = (PopupSuppress() || PopupProbe());

    if (log) {
        OneLineW(text, t, sizeof(t));
        OneLineW(caption, c, sizeof(c));
        /* The one hook that does not simply record: a matched box is
         * answered here, with the value the CONTINUE button would have
         * produced, and MessageBoxW is not called at all. The caller
         * compares the return against IDYES, so this is indistinguishable
         * from the player having pressed CONTINUE - minus the box. */
        if (PopupSuppress() && MatchBoxW(caption, text, type, why, sizeof(why))) {
            LONG n = InterlockedIncrement(&g_suppressed);
            PLog("** SUPPRESSED MessageBoxW (matched by %s) -> returning %d, "
                 "the box is never shown", why, g_returnValue);
            if (n <= 8) {
                PLog("   suppressed caption=\"%s\" text=\"%s\"", c, t);
                LogStack("sup");
            } else if (n == 9) {
                PLog("   (further suppressions are counted, not printed)");
            }
            return g_returnValue;
        }
        PLog("** MessageBoxW FIRED owner=%p type=0x%X text=\"%s\" caption=\"%s\"",
             (void *)owner, type, t, c);
        LogStack("mbw");
    }
    r = g_realMBW(owner, text, caption, type);
    if (log) PLog("   MessageBoxW returned %d", r);
    return r;
}

static int WINAPI HookMessageBoxA(HWND owner, LPCSTR text, LPCSTR caption,
                                  UINT type) {
    char t[220], c[120], why[48];
    int r, log = (PopupSuppress() || PopupProbe());

    if (log) {
        OneLineA(text, t, sizeof(t));
        OneLineA(caption, c, sizeof(c));
        if (PopupSuppress() && MatchBoxA(caption, text, type, why, sizeof(why))) {
            LONG n = InterlockedIncrement(&g_suppressed);
            PLog("** SUPPRESSED MessageBoxA (matched by %s) -> returning %d, "
                 "the box is never shown", why, g_returnValue);
            if (n <= 8) {
                PLog("   suppressed caption=\"%s\" text=\"%s\"", c, t);
                LogStack("sup");
            } else if (n == 9) {
                PLog("   (further suppressions are counted, not printed)");
            }
            return g_returnValue;
        }
        PLog("** MessageBoxA FIRED owner=%p type=0x%X text=\"%s\" caption=\"%s\"",
             (void *)owner, type, t, c);
        LogStack("mba");
    }
    r = g_realMBA(owner, text, caption, type);
    if (log) PLog("   MessageBoxA returned %d", r);
    return r;
}

static HWND WINAPI HookCreateWindowExW(DWORD exstyle, LPCWSTR cls,
                                       LPCWSTR name, DWORD style,
                                       int x, int y, int w, int h,
                                       HWND parent, HMENU menu,
                                       HINSTANCE inst, LPVOID param) {
    HWND hwnd = g_realCW(exstyle, cls, name, style, x, y, w, h,
                         parent, menu, inst, param);
    if (PopupProbe() && HookEnter()) {
        char c[120], t[160];
        DWORD err = GetLastError();

        /* cls is a string here, or an atom, or nothing - OneLineW names
         * whichever it turns out to be. The resolved class and title are
         * deliberately not asked for: the window scanner lists every
         * window of this process with both, moments later, and querying
         * inside this hook is what could recurse back into it. */
        OneLineW(cls, c, sizeof(c));
        OneLineW(name, t, sizeof(t));
        PLog("CreateWindowExW cls=\"%s\" name=\"%s\" style=0x%08lX ex=0x%08lX "
             "parent=%p -> hwnd=%p err=%lu",
             c, t, (unsigned long)style, (unsigned long)exstyle,
             (void *)parent, (void *)hwnd, (unsigned long)err);
        HookLeave();
    }
    return hwnd;
}

/* ShowWindow runs all game long - the engine shows and hides panels
 * constantly - and a line per call would both bloat the log and cost
 * frames. Only the first show of each window is worth a line; the dialog
 * is a window that has never been seen before, so the one sighting that
 * matters is never the one dropped. */
#define SW_LOG_MAX 256
static HWND g_swSeen[SW_LOG_MAX];
static volatile LONG g_swN, g_swOver;

/* 1 = first sighting, log it. Also 1 when the table is full? No: a full
 * table means "stop narrating", so the same window is not described on
 * every call for the rest of the session. */
static int SwFirstSight(HWND h) {
    LONG i, n = InterlockedCompareExchange(&g_swN, 0, 0);

    for (i = 0; i < n && i < SW_LOG_MAX; i++)
        if (g_swSeen[i] == h) return 0;
    i = InterlockedIncrement(&g_swN) - 1;
    if (i < SW_LOG_MAX) { g_swSeen[i] = h; return 1; }
    InterlockedExchange(&g_swOver, 1);
    return 0;
}

static BOOL WINAPI HookShowWindow(HWND hwnd, int cmd) {
    BOOL r = g_realSW(hwnd, cmd);
    /* The re-entrancy test comes first: taking the window into the table
     * and then declining to describe it would drop that sighting for
     * good, and the box we are after is shown exactly once. */
    if (PopupProbe() && !t_inHook && SwFirstSight(hwnd)) {
        char c[64], t[160];
        DWORD pid = 0;
        HookEnter();
        GetWindowThreadProcessId(hwnd, &pid);
        ClassOf(hwnd, c, sizeof(c));
        TitleOf(hwnd, t, sizeof(t));
        PLog("ShowWindow hwnd=%p cmd=%d cls=\"%s\" title=\"%s\" pid=%lu -> %d",
             (void *)hwnd, cmd, c, t, (unsigned long)pid, (int)r);
        HookLeave();
    }
    return r;
}

static BOOL WINAPI HookTerminateProcess(HANDLE proc, UINT code) {
    if (PopupProbe() && proc && proc == GetCurrentProcess()) {
        PLog("** TerminateProcess(self, 0x%X)%s",
             (unsigned)code,
             code == 0x63u ? "   <-- the code the corruption box exits with" : "");
        LogStack("term");
    }
    return g_realTP(proc, code);
}

static VOID WINAPI HookExitProcess(UINT code) {
    if (PopupProbe() && !InterlockedExchange(&g_exitSeen, 1)) {
        PLog("ExitProcess(0x%X)", (unsigned)code);
        LogStack("exit");
    }
    g_realEP(code);
}

/* ---- background watchers --------------------------------------------- */

static volatile LONG g_stop;
static Seen  g_seen;
static unsigned char g_lastGuard;
/* One watcher of each kind at a time. A watcher stops when its window is
 * over or its switch goes off, and clears its flag so that turning the
 * switch back on can start a fresh one. */
static volatile LONG g_watchGuard, g_watchWindow;

static DWORD WINAPI GuardThread(LPVOID p) {
    ULONGLONG stopAt;

    (void)p;
    if (!g_peekOk) {
        PLog("guard watch: peek is disabled on this build - nothing to poll");
        InterlockedExchange(&g_watchGuard, 0);
        return 0;
    }
    g_lastGuard = PeekByte(RVA_GUARD);
    PLog("guard byte at startup = %u", (unsigned)g_lastGuard);
    /* The one thing that decides how to read this whole log: if the guard
     * is already set, the box was asked for before this module was even
     * loaded and the API hooks were never in a position to see it. */
    if (g_lastGuard)
        PLog("NOTE: guard already 1 - the corruption path ran BEFORE this "
             "module loaded, so the API hooks were too late. The window "
             "list below is then the only useful record.");
    else
        PLog("NOTE: guard is 0 - this module is in place ahead of the "
             "corruption path, so the API hooks should catch it.");
    DumpGlobals("at startup");

    /* Bounded on purpose: the guard byte is a one-shot that flips as the
     * game starts, so once the watch window is over there is nothing left
     * to see and this stops instead of polling for the whole session. */
    stopAt = GetTickCount64() + (ULONGLONG)g_watchSeconds * 1000u;
    while (!g_stop && PopupProbe() && GetTickCount64() < stopAt) {
        unsigned char g = PeekByte(RVA_GUARD);

        if (g != g_lastGuard) {
            PLog("** dialog path: FIRED - guard byte %u -> %u",
                 (unsigned)g_lastGuard, (unsigned)g);
            g_lastGuard = g;
            if (!InterlockedExchange(&g_guardFired, 1)) {
                DumpGlobals("when the box was asked for");
                ScanOwnWindows(&g_seen, "at the box");
            }
        }
        Sleep(g_peekPollMs);
    }
    PLog("guard watch: stopped after %d s", g_watchSeconds);
    InterlockedExchange(&g_watchGuard, 0);
    return 0;
}

static DWORD WINAPI WindowThread(LPVOID p) {
    ULONGLONG stopAt = GetTickCount64() + (ULONGLONG)g_watchSeconds * 1000u;

    (void)p;
    /* A first pass right away: if the box is already up by the time this
     * module runs, the API hooks missed it and only the window list - or
     * the fallback's click - can do anything about it. */
    if (PopupProbe()) ScanOwnWindows(&g_seen, "startup");
    if (PopupFallback()) ClickContinueIfBoxed();

    /* Stops with the watch window: the box only ever appears while the
     * game starts, so scanning for it for the rest of the session would
     * be work with nothing to find. */
    while (!g_stop && (PopupProbe() || PopupFallback()) &&
           GetTickCount64() < stopAt) {
        if (PopupProbe()) ScanOwnWindows(&g_seen, "watch");
        if (PopupFallback()) ClickContinueIfBoxed();
        Sleep(g_windowScanMs);
    }
    PLog("window watch: stopped after %d s - the box only appears at startup, "
         "so nothing is watched for the rest of the session", g_watchSeconds);
    InterlockedExchange(&g_watchWindow, 0);
    return 0;
}


/* ---- config ----------------------------------------------------------
 *
 * The ini beside the .asi, written out when it is not there. The
 * switches are what the menu owns and writes back; the markers are
 * reverse-engineering tuning rather than runtime switches. */

static HINSTANCE g_inst;
static char      g_ini[MAX_PATH];

/* plugins\NoCorruptionPopup\NoCorruptionPopup.ini, from our module
 * name. */
static void ResolveIni(void) {
    char mod[MAX_PATH];
    const char *dot;
    size_t n;

    g_ini[0] = 0;
    if (!g_inst || !GetModuleFileNameA(g_inst, mod, sizeof(mod))) return;
    dot = strrchr(mod, '.');
    n = dot ? (size_t)(dot - mod) : strlen(mod);
    if (n >= sizeof(g_ini)) n = sizeof(g_ini) - 1;
    memcpy(g_ini, mod, n);
    g_ini[n] = 0;
    strncat(g_ini, ".ini", sizeof(g_ini) - n - 1);
}

/* The ini, as it is written out when it is not there. Every switch in it
 * is off, which is also what the compiled-in defaults are, so a fresh
 * install does nothing until a switch is turned on here or in the menu. */
static const char kIniTemplate[] =
    "; NoCorruptionPopup - answers the \"A corruption of the game files\" box\n"
    "; so that it is never shown. Nothing here is about the game itself.\n"
    ";\n"
    "; Every switch below defaults to 0 = off. With all of them off this\n"
    "; plugin installs no hooks, starts no threads and does nothing to the\n"
    "; game. Deleting this plugin's folder undoes all of it.\n"
    "[Settings]\n"
    "; 1 = answer the box instead of showing it (the point of the plugin).\n"
    "suppress=0\n"
    "\n"
    "; 1 = diagnostic logging and the startup watchers (troubleshooting).\n"
    "probe=0\n"
    "\n"
    "; 1 = click CONTINUE if a box turns up anyway. Only runs while watching.\n"
    "fallback_click=0\n"
    "\n"
    "; ---- what counts as \"this box\" ----------------------------------------\n"
    "; The text is localised in some builds, so matching is by any of three\n"
    "; markers. An empty caption/text or a 0 type switches that marker off.\n"
    "; 0x41034 = MB_YESNO | MB_ICONWARNING | MB_SYSTEMMODAL | MB_TOPMOST.\n"
    "match_type=0x41034\n"
    "match_caption=ERROR\n"
    "match_text=corruption of the game files\n"
    "\n"
    "; The button the fallback presses.\n"
    "continue_button=CONTINUE\n"
    "\n"
    "; Refuse to answer anything that is not a Yes/No box.\n"
    "require_yesno=1\n"
    "\n"
    "; What the hook returns instead of showing the box.\n"
    ";   7 = IDNO  = the CONTINUE button = carry on   (the intended setting)\n"
    ";   6 = IDYES = the CLOSE button    = the caller quits with code 99\n"
    "; Anything else is refused at startup and 7 is used.\n"
    "return_value=7\n"
    "\n"
    "; ---- diagnostics ------------------------------------------------------\n"
    "; How long after startup to keep watching, in seconds. The box only ever\n"
    "; appears during start up, so nothing runs after this.\n"
    "watch_seconds=300\n"
    "; Window scan interval, ms.\n"
    "window_scan_ms=400\n"
    "; How often the one-shot guard byte in the game image is polled, ms.\n"
    "pointer_poll_ms=200\n";

/* Read once at start up. A missing key, a missing file and junk all
 * read as the compiled-in default, and every switch defaults to off -
 * so a fresh install does nothing until a switch is turned on. */
static void LoadConfig(void) {
    /* A fresh install gets the template written out first, every
     * switch in it off, which is also what the defaults are: reading a
     * missing file and reading a fresh one agree. */
    if (g_ini[0] &&
        GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES) {
        FILE *f = fopen(g_ini, "w");

        if (f) {
            fwrite(kIniTemplate, 1, sizeof(kIniTemplate) - 1, f);
            fclose(f);
            PLog("config: wrote the default ini at %s", g_ini);
        }
    }

    if (g_ini[0]) {
        g_suppress = GetPrivateProfileIntA("Settings", "suppress", 0,
                                           g_ini) ? 1 : 0;
        g_probe    = GetPrivateProfileIntA("Settings", "probe", 0,
                                           g_ini) ? 1 : 0;
        g_fallback = GetPrivateProfileIntA("Settings",
                                           "fallback_click", 0,
                                           g_ini) ? 1 : 0;
        g_matchType = GetPrivateProfileIntA("Settings", "match_type",
                                            0x41034, g_ini);
        g_requireYesNo = GetPrivateProfileIntA("Settings",
                                                "require_yesno", 1,
                                                g_ini);
        g_returnValue = GetPrivateProfileIntA("Settings",
                                               "return_value", 7,
                                               g_ini);
        g_watchSeconds = GetPrivateProfileIntA("Settings",
                                                "watch_seconds", 300,
                                                g_ini);
        g_windowScanMs = GetPrivateProfileIntA("Settings",
                                                "window_scan_ms", 400,
                                                g_ini);
        g_peekPollMs = GetPrivateProfileIntA("Settings",
                                              "pointer_poll_ms", 200,
                                              g_ini);
        GetPrivateProfileStringA("Settings", "match_caption",
                                 "ERROR", g_capA, sizeof(g_capA),
                                 g_ini);
        GetPrivateProfileStringA("Settings", "match_text",
                                 "corruption of the game files",
                                 g_txtA,
                                 sizeof(g_txtA), g_ini);
        GetPrivateProfileStringA("Settings", "continue_button", "CONTINUE",
                                 g_contA, sizeof(g_contA), g_ini);
    }

    if (g_watchSeconds < 10) g_watchSeconds = 10;
    if (g_windowScanMs < 100) g_windowScanMs = 100;
    if (g_peekPollMs < 50) g_peekPollMs = 50;

    /* Only the two answers the caller understands: it compares against
     * IDYES, so anything else would silently mean "continue" - better to
     * say so than to let a typo pick the meaning. */
    if (g_returnValue != 6 && g_returnValue != 7) {
        PLog("config: return_value=%d is neither IDYES(6) nor IDNO(7); using 7",
             g_returnValue);
        g_returnValue = 7;
    }

    /* The wide copies are what MessageBoxW is matched against. Always
     * built, ini or no ini, because the defaults are compiled in. */
    MultiByteToWideChar(CP_ACP, 0, g_capA, -1, g_capW,
                        (int)(sizeof(g_capW) / sizeof(g_capW[0])));
    MultiByteToWideChar(CP_ACP, 0, g_txtA, -1, g_txtW,
                        (int)(sizeof(g_txtW) / sizeof(g_txtW[0])));
    MultiByteToWideChar(CP_ACP, 0, g_contA, -1, g_contW,
                        (int)(sizeof(g_contW) / sizeof(g_contW[0])));
}

/* Writes back the two switches the menu owns - and only those, so the ini
 * stays the two-line file the plugin writes when it is missing. A key a
 * player added by hand (fallback_click and the rest) is left alone. */
static void SaveIni(void) {
    char buf[16];

    if (!g_ini[0]) return;
    snprintf(buf, sizeof(buf), "%d", g_suppress ? 1 : 0);
    WritePrivateProfileStringA("Settings", "suppress", buf, g_ini);
    snprintf(buf, sizeof(buf), "%d", g_probe ? 1 : 0);
    WritePrivateProfileStringA("Settings", "probe", buf, g_ini);
}

/* Defined with the startup code below; the menu calls it when a switch is
 * turned on after the game is already running. */
static void EnsureInstalled(void);

/* ---- menu / text ----------------------------------------------------- */

typedef void (*MenuFn)(uint32_t menu, uint32_t item, int value, void *user);
typedef struct { const char *key; const char *text; } TextRow;

static const TextRow kEn[] = {
    { "@cp.page", "Corruption popup" },
    { "@cp.sup",  "Answer the box (skip it)" },
    { "@cp.log",  "Diagnostic log (startup only)" },
    { "@cp.hint", "Auto-answers the game-files corruption box at start-up, so it "
                  "is skipped" }
};

static const TextRow kZh[] = {
    { "@cp.page", "文件损坏弹窗" },
    { "@cp.sup",  "自动应答（跳过弹窗）" },
    { "@cp.log",  "诊断日志（仅启动时）" },
    { "@cp.hint", "自动应答跳过启动游戏时的文件损坏弹窗" }
};

/* Both switches take effect on the spot: the hook reads its flag on every
 * call, the watchers read theirs on every pass, and a watcher that is not
 * running yet is started here. */
static void OnSuppress(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    InterlockedExchange(&g_suppress, value ? 1 : 0);
    if (g_suppress) EnsureInstalled();
    PLog("menu: suppress=%s", g_suppress ? "on" : "off");
    SaveIni();
}

static void OnProbe(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    g_probe = value ? 1 : 0;
    if (g_probe) EnsureInstalled();
    PLog("menu: probe=%s", g_probe ? "on" : "off");
    SaveIni();
}

static void BuildMenu(HMODULE fw) {
    uint32_t (*menuCreate)(const char *) = NULL;
    int (*menuToggle)(uint32_t, const char *, int, MenuFn, void *) = NULL;
    int (*menuHint)(uint32_t, const char *) = NULL;
    static int textDone;
    LangDeclare_t langDeclare = NULL;

    *(FARPROC *)&menuCreate = GetProcAddress(fw, "ShMenuCreate");
    *(FARPROC *)&menuToggle = GetProcAddress(fw, "ShMenuToggle");
    *(FARPROC *)&menuHint   = GetProcAddress(fw, "ShMenuHint");
    if (!menuCreate) return;

    if (!textDone) {
        *(FARPROC *)&langDeclare = GetProcAddress(fw, "ShLangDeclare");
        if (langDeclare) {
            langDeclare("NoCorruptionPopup", "en-US", kEn,
                        (int)(sizeof(kEn) / sizeof(kEn[0])));
            langDeclare("NoCorruptionPopup", "zh-CN", kZh,
                        (int)(sizeof(kZh) / sizeof(kZh[0])));
            textDone = 1;
        }
    }

    if (menuToggle) {
        uint32_t menu = menuCreate("@cp.page");
        menuToggle(menu, "@cp.sup", g_suppress ? 1 : 0, OnSuppress, NULL);
        menuToggle(menu, "@cp.log", g_probe ? 1 : 0, OnProbe, NULL);
        if (menuHint) menuHint(menu, "@cp.hint");
        PLog("menu created");
    }
}

/* ---- startup --------------------------------------------------------- */

static volatile LONG g_hooksMade;

/* The shared hook layer's two entry points, bound by name: this plugin
 * links nothing but log.h, so a framework old enough not to carry them
 * reads as null here and the install is skipped with a line saying so -
 * the plugin stays loadable, it just does nothing, which is the same
 * contract as every other late binding in this file. */
typedef int (*HookAdd_t)(const char *, HMODULE, const char *, void *,
                         void **);
typedef int (*HookRemove_t)(const char *);
static HookAdd_t    pHookAdd;
static HookRemove_t pHookRemove;

/* Returns 1 once the hooks are in place, 0 when there was nothing to do
 * or the layer refused - so a later attempt can pick it up. The gate
 * counts the two box hooks and nothing else: they are what the plugin
 * is for. Counting them rather than borrowing an initializer's return
 * value is what keeps a supervisor from retrying a session that is
 * already working - that mistake cost a session of retry spam once
 * already. */
static int InstallHooks(void) {
    /* probeOnly = the hook is diagnostics, and is skipped when
     * probe=0. The MessageBox pair is not: it is what answers the
     * box, so it is installed whenever there is anything to do at
     * all. */
    struct { const char *mod; const char *fn; void *det; void **orig;
             const char *label; int probeOnly; } want[] = {
        { "user32.dll",  "MessageBoxW",      (void *)HookMessageBoxW,
          (void **)&g_realMBW, "MessageBoxW",      0 },
        { "user32.dll",  "MessageBoxA",      (void *)HookMessageBoxA,
          (void **)&g_realMBA, "MessageBoxA",      0 },
        { "user32.dll",  "CreateWindowExW",
          (void *)HookCreateWindowExW,  (void **)&g_realCW,
          "CreateWindowExW",  1 },
        { "user32.dll",  "ShowWindow",       (void *)HookShowWindow,
          (void **)&g_realSW,  "ShowWindow",       1 },
        { "kernel32.dll","TerminateProcess",
          (void *)HookTerminateProcess, (void **)&g_realTP,
          "TerminateProcess", 1 },
        { "kernel32.dll","ExitProcess",      (void *)HookExitProcess,
          (void **)&g_realEP,  "ExitProcess",      1 },
    };
    int i, made = 0, tried = 0, core = 0;

    if (!g_suppress && !g_probe) {
        PLog("nothing to do: suppress and probe are both off");
        return 0;
    }
    if (!pHookAdd) {
        PLog("install: the framework carries no shared hook layer - "
             "nothing is recorded");
        return 0;
    }

    /* One ShHookAdd per target: the layer owns MinHook's state, keeps
     * one hook per target for the whole DLL, and enables each target
     * by its own address. DUP means somebody else owns that target -
     * this detour is not in the chain, and the layer's log says whose
     * is. */
    for (i = 0; i < (int)(sizeof(want) / sizeof(want[0])); i++) {
        char tag[64];
        HMODULE mod;
        void *orig = NULL;
        int rc;

        if (want[i].probeOnly && !g_probe) continue;
        mod = GetModuleHandleA(want[i].mod);
        if (!mod) {
            PLog("install: %s is not loaded - %s not asked for",
                 want[i].mod, want[i].label);
            continue;
        }
        tried++;
        snprintf(tag, sizeof(tag), "ncp:%s", want[i].label);
        rc = pHookAdd(tag, mod, want[i].fn, want[i].det, &orig);
        *(want[i].orig) = (rc == SH_HOOK_OK) ? orig : NULL;
        if (rc == SH_HOOK_OK) {
            made++;
            if (!want[i].probeOnly) core++;
            PLog("install: hook %-17s -> asked for (the framework "
                 "installs it)", want[i].label);
        } else if (rc == SH_HOOK_DUP) {
            PLog("install: hook %-17s -> taken by somebody else",
                 want[i].label);
        } else {
            PLog("install: hook %-17s -> refused (the reason is in "
                 "scripthook_hooks.log)", want[i].label);
        }
    }
    PLog("install: done at tick %llu ms, %d of %d asked for",
         (unsigned long long)GetTickCount64(), made, tried);
    return core == 2 ? 1 : 0;
}
/* Installs whatever the switches ask for, and only once: called from
 * startup, and again from the menu when a switch is turned on later, so a
 * switch flipped in game works without a restart. */
static void EnsureInstalled(void) {
    if ((g_suppress || g_probe) &&
        InterlockedCompareExchange(&g_hooksMade, 0, 0) == 0 &&
        InterlockedCompareExchange(&g_hooksMade, -1, 0) == 0) {
        if (InstallHooks())
            InterlockedExchange(&g_hooksMade, 1);
        else
            InterlockedExchange(&g_hooksMade, 0);
    }
    if (g_probe && InterlockedCompareExchange(&g_watchGuard, 1, 0) == 0) {
        BindGameImage();                 /* no-op if it is already bound */
        CreateThread(NULL, 0, GuardThread, NULL, 0, NULL);
    }
    if ((g_probe || g_fallback) &&
        InterlockedCompareExchange(&g_watchWindow, 1, 0) == 0)
        CreateThread(NULL, 0, WindowThread, NULL, 0, NULL);
}

static DWORD WINAPI InitThread(LPVOID p) {
    HMODULE fw;

    (void)p;
    PLog("--- NoCorruptionPopup: the corruption box, answered ---");
    PLog("build " __DATE__ " " __TIME__);
    PLog("start: tick=%llu ms pid=%lu cmdline=%s",
         (unsigned long long)GetTickCount64(),
         (unsigned long)GetCurrentProcessId(), GetCommandLineA());

    ResolveIni();
    LoadConfig();
    PLog("config: suppress=%s probe=%s fallback_click=%s "
         "return_value=%d(%s) watch=%ds window_scan=%dms guard_poll=%dms",
         g_suppress ? "on" : "off",
         g_probe ? "on" : "off", g_fallback ? "on" : "off", g_returnValue,
         g_returnValue == 7 ? "IDNO=CONTINUE" : "IDYES=CLOSE",
         g_watchSeconds, g_windowScanMs, g_peekPollMs);
    PLog("match: type=0x%X caption=\"%s\" text=\"%s\" continue_button=\"%s\" "
         "require_yesno=%d ini=%s",
         g_matchType, g_capA, g_txtA, g_contA, g_requireYesNo,
         g_ini[0] ? g_ini : "(none)");
    if (g_suppress && NoMarkersConfigured())
        PLog("WARNING: suppress is on with no marker configured - every "
             "MB_YESNO box would be answered with %d. Set match_type, "
             "match_caption or match_text.", g_returnValue);

    fw = GetModuleHandleA("dinput8.dll");
    if (fw) BuildMenu(fw);
    else PLog("framework dinput8.dll not found - no menu page");

    /* Everything below is only worth doing when a switch is on: with all
     * three off there is nothing to hook, nothing to watch and nothing to
     * look up, and the log above is the whole record. */
    if (fw) {
        *(FARPROC *)&pHookAdd    = GetProcAddress(fw, "ShHookAdd");
        *(FARPROC *)&pHookRemove = GetProcAddress(fw,
                                                "ShHookRemove");
    }
    if (!g_suppress && !g_probe && !g_fallback) {
        PLog("plugin is off: suppress, probe and fallback_click are "
             "all 0 - nothing is hooked, nothing is watched");
        return 0;
    }

    BindStackWalk();
    BindGameImage();
    DumpGlobals("early");

    EnsureInstalled();

    if (g_suppress)
        PLog("the box should NOT appear - it is answered before it is "
             "drawn. Nothing to press; just read this log.");
    else
        PLog("suppression is off: the box will appear as usual. Press "
             "CONTINUE, then read this log.");
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