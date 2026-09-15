/* File interception: the one place in the framework that owns the file
 * APIs, and the rules the rest of the world registers with it.
 *
 * Five parts of this repository used to intercept the same kernel32 calls
 * with five private arrangements:
 *
 *   skipintro       patched the main module's import table by hand - a PE
 *                   parser, a rollback table of slots, and a watcher
 *                   thread that takes the patches back out;
 *   GhostNoWipe     MinHook on MoveFileExW, MoveFileW and CreateFileW, to
 *                   turn the ghost save's rename into a copy;
 *   GhostWipeProbe  MinHook on twelve of them, to watch;
 *   forgeprobe      MinHook on thirteen, to watch;
 *   forge_io        MinHook on ReadFile and the completion path, and it
 *                   asks a handle where its file is with
 *                   GetFinalPathNameByHandleW instead of hooking
 *                   CreateFileW, because the first two are already on
 *                   that target.
 *
 * MinHook keeps one hook per target per module, so each of those had to be
 * designed around the others: a target can have one owner, and the owner
 * is whoever gets there first. This file is that owner - once - and the
 * others register rules with it.
 *
 * What owning them means:
 *
 *   - the hooks are inline, on the process' kernel32, so they answer for
 *     the game, for every plugin and for the framework's own modules, and
 *     a pointer taken with GetProcAddress is intercepted exactly like a
 *     static import. That is the reason for inline at all: skipintro's
 *     import table patch could only ever see the main module;
 *   - a rule says which file, which calls, and what to do: hide it,
 *     redirect the call, or decide per call in a callback. A watcher is a
 *     rule with only an after callback;
 *   - nothing is installed until a rule is registered, and the last
 *     unregistration takes every hook and trampoline back out - so a
 *     session that registers nothing pays nothing at all, and a one-shot
 *     user can keep its promise (skipintro releases when it is done);
 *   - because the hooks are process wide, the layer must also know when
 *     NOT to look: its own pass is marked (t_depth), a thread can mark its
 *     own I/O (ShFileOwn), and both kinds of call pass straight through.
 *
 * The hot path is a thread-local read, two counters and a scan of at most
 * FILES_RULES_MAX rule entries; nothing in it allocates, waits, logs per
 * call, or takes a lock that another thread could hold while it makes a
 * file call. Log lines are written on registration, release and install,
 * and for the first few hits of a rule (a counter each), never per call.
 *
 * Rule entries are never freed, only reused: a redirect target handed to a
 * running call stays valid even if the rule is released under it. The
 * prices of that are a fixed table and a stale target in a race, both of
 * which are cheaper than a use-after-free in a file call.
 *
 * See scripthook.h (@defgroup files) for the contract a caller sees, and
 * docs/file-interception.md for the whole story.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#ifdef _MSC_VER
#include <intrin.h>     /* _ReturnAddress, for the owner's name */
#endif

/* SH_BUILD before scripthook.h, like every other framework source: without
 * it SH_API expands to nothing and this module's exports are not in the DLL
 * at all. */
#define SH_BUILD 1
#include "scripthook.h"
#include "log.h"
#include "third_party/minhook/include/MinHook.h"

#define ARRAY_LEN(a)     (sizeof(a) / sizeof((a)[0]))

#define FILES_RULES_MAX  32
#define FILES_NAME_MAX   64
#define FILES_PATH_MAX   260
#define FILES_OWNER_MAX  32

#ifdef _MSC_VER
#define FILE_TLS __declspec(thread)
#else
#define FILE_TLS __thread
#endif

/* ---- per-thread state -------------------------------------------------
 *
 * Two flags, both thread local, both meaning "do not look at this call":
 *
 *   t_depth  we are already inside our own pass - the dispatch, a rule's
 *            callback, or the real function one of them called. A file
 *            call made from in there is not the caller's own and must not
 *            be judged, or a callback that copies a file (which is how
 *            GhostNoWipe keeps a save) would be judged by its own rules.
 *   t_own    this thread has said its I/O is its own business
 *            (ShFileOwn). The forge loader reads archives as part of
 *            serving them, and those reads must not come back through the
 *            rules that serve them.
 */
static FILE_TLS int t_depth;
static FILE_TLS int t_own;

/* A redirect target is handed to the detour as a pointer, and the rule
 * slot that owns it can be released and reused by another module while the
 * call is still in flight - the header's promise that a target "stays
 * valid" only holds if the pointer is not the rule's own buffer. The copy
 * lives in thread-local storage, so it stays valid for the whole call and
 * two threads cannot clobber each other's. */
static FILE_TLS wchar_t t_redirW[FILES_PATH_MAX];
static FILE_TLS char    t_redirA[FILES_PATH_MAX];

/* ---- rules -------------------------------------------------------------
 *
 * A fixed table, entries never freed: `live` is the only thing that goes
 * up and down, so a rule pointer stays valid for the session and a target
 * string can be handed to a running call without a lock.
 */
struct ShFileRule {
    volatile LONG  live;
    int            seq;                    /* registration order        */
    uint32_t       group;                  /* SH_FILE_* bits            */
    int            action;                 /* SH_FILE_* action          */
    int            hasName;                /* nameW/nameA are in use    */
    int            hasSuffix;
    int            hasTo;
    wchar_t        nameW[FILES_NAME_MAX];   /* matched on the file name */
    char           nameA[FILES_NAME_MAX];
    wchar_t        suffixW[FILES_PATH_MAX]; /* and on the path's tail   */
    char           suffixA[FILES_PATH_MAX];
    wchar_t        toW[FILES_PATH_MAX];     /* the redirect target      */
    char           toA[FILES_PATH_MAX];
    ShFileDecideFn before;
    ShFileAfterFn  after;
    void          *user;
    char           owner[FILES_OWNER_MAX];  /* who registered it        */
    volatile LONG  hits;                    /* calls it matched         */
    volatile LONG  noted;                   /* log lines spent on it    */
};

static struct ShFileRule g_rule[FILES_RULES_MAX];
static volatile LONG     g_live;        /* rules in the table          */
static volatile LONG     g_seq;         /* the next registration number */
static volatile LONG     g_mask;        /* union of the live rules' bits */
static volatile LONG     g_lock;        /* registrations, releases      */
static volatile LONG     g_calls;       /* calls the layer looked at    */
static volatile LONG     g_answered;    /* calls a rule answered        */
static volatile LONG     g_installed;   /* targets hooked right now     */
static volatile LONG     g_ready;       /* the log file is open         */

/* Registrations and releases take it; the dispatch never does. Held for a
 * handful of stores, and the string copies, which is why it is a spin lock
 * and not a critical section. */
static void FileLock(void)
{
    while (InterlockedExchange(&g_lock, 1)) Sleep(0);
}

static void FileUnlock(void)
{
    InterlockedExchange(&g_lock, 0);
}

/* Hook installation/removal gets its own lock: the rule table is under
 * FileLock, but SyncTargets runs after FileUnlock, and two plugin threads
 * adding rules in the same group would otherwise drive MinHook's
 * CreateHook/EnableHook on one target at the same time. */
static volatile LONG g_syncLock = 0;

static void SyncLock(void)
{
    while (InterlockedExchange(&g_syncLock, 1)) Sleep(0);
}

static void SyncUnlock(void)
{
    InterlockedExchange(&g_syncLock, 0);
}

/* ---- strings ------------------------------------------------------------
 *
 * Rules are kept in both widths: an A call is matched against the rule's
 * ANSI copy, so the hook path never converts a path (which would be an
 * allocation-free but pointless per-call cost), and the conversions happen
 * once, at registration.
 */

static void CopyW(wchar_t *dst, int n, const wchar_t *src)
{
    int i = 0;

    if (src) for (; src[i] && i < n - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

static void CopyA(char *dst, int n, const char *src)
{
    int i = 0;

    if (src) for (; src[i] && i < n - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

static void ToAnsi(char *dst, int n, const wchar_t *src)
{
    dst[0] = 0;
    if (!src || !*src) return;
    if (WideCharToMultiByte(CP_ACP, 0, src, -1, dst, n, NULL, NULL) <= 0)
        dst[0] = 0;
}

/* The file's own name - what a rule's `name` is matched against. A path
 * ending in a separator names the directory, so the answer is empty. */
static const wchar_t *WNamePart(const wchar_t *p)
{
    const wchar_t *f = p;

    for (; *p; p++)
        if (*p == L'\\' || *p == L'/') f = p + 1;
    return f;
}

static const char *ANamePart(const char *p)
{
    const char *f = p;

    for (; *p; p++)
        if (*p == '\\' || *p == '/') f = p + 1;
    return f;
}

/* Does the path end with this tail? Case insensitive, and an empty tail
 * matches everything. */
static int TailIsW(const wchar_t *path, const wchar_t *tail)
{
    size_t n = wcslen(path), m = wcslen(tail);

    if (!m) return 1;
    if (n < m) return 0;
    return _wcsicmp(path + n - m, tail) == 0;
}

static int TailIsA(const char *path, const char *tail)
{
    size_t n = strlen(path), m = strlen(tail);

    if (!m) return 1;
    if (n < m) return 0;
    return _stricmp(path + n - m, tail) == 0;
}

/* ---- the call, as the dispatch sees it --------------------------------
 *
 * The path the rules are matched against: the file part for `name`, the
 * whole path for `suffix`. Exactly one width is filled - the one the call
 * was made in - because the layer never converts a path on the hook path.
 */
typedef struct {
    const wchar_t *fullW;
    const char    *fullA;
    const wchar_t *fileW;
    const char    *fileA;
} FilePath;

static void PathOf(FilePath *p, const void *path, int wide)
{
    memset(p, 0, sizeof(*p));
    if (!path) return;
    if (wide) {
        p->fullW = (const wchar_t *)path;
        p->fileW = WNamePart(p->fullW);
    } else {
        p->fullA = (const char *)path;
        p->fileA = ANamePart(p->fullA);
    }
}

/* ---- the pass a detour makes ------------------------------------------
 *
 * FILES_REAL     call the real function (the path may have been replaced)
 * FILES_HIDDEN   answer "no such file", in the shape this call has
 * FILES_ANSWERED a callback answered: result and error are the answer
 */
#define FILES_REAL     0
#define FILES_HIDDEN   1
#define FILES_ANSWERED 2

/* The groups a HIDE or a REDIRECT can apply to: the ones that name a file.
 * A handle-carrying call cannot be hidden - there is nothing to answer
 * "not found" about - so for those a rule is a decision or a watch only. */
#define FILES_NAMED(g)  ((g) & (SH_FILE_OPEN | SH_FILE_ATTR | SH_FILE_MOVE | \
                                SH_FILE_DELETE | SH_FILE_FIND))

static int RuleMatches(const struct ShFileRule *r, uint32_t group,
                       const FilePath *p)
{
    if (!(r->group & group)) return 0;

    if (r->hasName) {
        if (p->fileW) {
            if (_wcsicmp(p->fileW, r->nameW) != 0) return 0;
        } else if (p->fileA) {
            if (_stricmp(p->fileA, r->nameA) != 0) return 0;
        } else {
            return 0;                   /* no path: a named rule cannot match */
        }
    }
    if (r->hasSuffix) {
        if (p->fullW) {
            if (!TailIsW(p->fullW, r->suffixW)) return 0;
        } else if (p->fullA) {
            if (!TailIsA(p->fullA, r->suffixA)) return 0;
        } else {
            return 0;
        }
    }
    return 1;
}

/* How the actions are weighed: HIDE first, then REDIRECT, then DECIDE.
 * Within one action the earlier registration wins, which is what `best` is
 * carried for. 0 is not an action, so it doubles as "nothing yet". */
static int ActionRank(int action)
{
    switch (action) {
    case SH_FILE_HIDE:     return 3;
    case SH_FILE_REDIRECT: return 2;
    case SH_FILE_DECIDE:   return 1;
    default:               return 0;
    }
}

const char *ShFileActionName(int action)
{
    switch (action) {
    case SH_FILE_HIDE:     return "hide";
    case SH_FILE_REDIRECT: return "redirect";
    case SH_FILE_DECIDE:   return "decide";
    default:               return "";
    }
}

/* Which groups are in a mask, as text, for a log line: one name for a
 * clean single group, "mixed" for several. */
static const char *GroupName(uint32_t g)
{
    switch (g & SH_FILE_ANY) {
    case SH_FILE_OPEN:   return "open";
    case SH_FILE_ATTR:   return "attr";
    case SH_FILE_MOVE:   return "move";
    case SH_FILE_DELETE: return "delete";
    case SH_FILE_FIND:   return "find";
    case SH_FILE_READ:   return "read";
    case SH_FILE_WAIT:   return "wait";
    case SH_FILE_INFO:   return "info";
    case SH_FILE_ANY:    return "any";
    default:             return "mixed";
    }
}

/* ---- the API table -----------------------------------------------------
 *
 * One entry per target, and the only place a hook is ever created: what a
 * target does for the dispatch is in the detour it points at, and the
 * group it belongs to is what a rule's mask is tested against.
 */
typedef struct {
    const char *name;      /* the kernel32 export                   */
    uint32_t    group;     /* its SH_FILE_* group                   */
    void       *detour;    /* what our hook points at               */
    void      **real;      /* where the real pointer is kept        */
    void       *target;    /* GetProcAddress, once resolved         */
    int         hooked;    /* 1 = created and enabled right now     */
} FileApi;

/* ---- what the detours below ask the layer -----------------------------
 *
 * One pass, in three parts, so that every detour has the same shape:
 *
 *   FilesWatch   is anybody interested? (the hot path, and the only part
 *                that runs on every call when nothing is registered)
 *   FilesDecide  weigh the matching rules; hide, redirect, or ask
 *   FilesAfter   tell the rules that wanted the result
 */
static int  FilesWatch(ShFileCall *c, const char *api, uint32_t group,
                       const void *path, const void *path2, int wide,
                       FilePath *p);
static int  FilesDecide(ShFileCall *c, FilePath *p);
static void FilesAfter(ShFileCall *c, FilePath *p, void *result,
                       DWORD err, DWORD bytes);

/* The "no such file" answer each return type wants, so the detours do not
 * repeat the casting. */
static BOOL HideBool(void)
{
    SetLastError(ERROR_FILE_NOT_FOUND);
    return FALSE;
}

static HANDLE HideHandle(void)
{
    SetLastError(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
}

static DWORD HideDword(void)
{
    SetLastError(ERROR_FILE_NOT_FOUND);
    return INVALID_FILE_ATTRIBUTES;
}

/* ---- the real functions ------------------------------------------------- */

static HANDLE (WINAPI *r_CreateFileA)(LPCSTR, DWORD, DWORD,
                                      LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                      HANDLE);
static HANDLE (WINAPI *r_CreateFileW)(LPCWSTR, DWORD, DWORD,
                                      LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                      HANDLE);
static DWORD  (WINAPI *r_GetFileAttributesA)(LPCSTR);
static DWORD  (WINAPI *r_GetFileAttributesW)(LPCWSTR);
static BOOL   (WINAPI *r_GetFileAttributesExA)(LPCSTR, GET_FILEEX_INFO_LEVELS,
                                               LPVOID);
static BOOL   (WINAPI *r_GetFileAttributesExW)(LPCWSTR, GET_FILEEX_INFO_LEVELS,
                                               LPVOID);
static BOOL   (WINAPI *r_MoveFileA)(LPCSTR, LPCSTR);
static BOOL   (WINAPI *r_MoveFileW)(LPCWSTR, LPCWSTR);
static BOOL   (WINAPI *r_MoveFileExA)(LPCSTR, LPCSTR, DWORD);
static BOOL   (WINAPI *r_MoveFileExW)(LPCWSTR, LPCWSTR, DWORD);
static BOOL   (WINAPI *r_DeleteFileA)(LPCSTR);
static BOOL   (WINAPI *r_DeleteFileW)(LPCWSTR);
static BOOL   (WINAPI *r_RemoveDirectoryA)(LPCSTR);
static BOOL   (WINAPI *r_RemoveDirectoryW)(LPCWSTR);
static HANDLE (WINAPI *r_FindFirstFileA)(LPCSTR, LPWIN32_FIND_DATAA);
static HANDLE (WINAPI *r_FindFirstFileW)(LPCWSTR, LPWIN32_FIND_DATAW);
static HANDLE (WINAPI *r_FindFirstFileExA)(LPCSTR, FINDEX_INFO_LEVELS, LPVOID,
                                           FINDEX_SEARCH_OPS, LPVOID, DWORD);
static HANDLE (WINAPI *r_FindFirstFileExW)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID,
                                           FINDEX_SEARCH_OPS, LPVOID, DWORD);
static BOOL   (WINAPI *r_FindNextFileA)(HANDLE, LPWIN32_FIND_DATAA);
static BOOL   (WINAPI *r_FindNextFileW)(HANDLE, LPWIN32_FIND_DATAW);
static BOOL   (WINAPI *r_ReadFile)(HANDLE, LPVOID, DWORD, LPDWORD,
                                   LPOVERLAPPED);
static DWORD  (WINAPI *r_SetFilePointer)(HANDLE, LONG, PLONG, DWORD);
static BOOL   (WINAPI *r_SetFilePointerEx)(HANDLE, LARGE_INTEGER,
                                           PLARGE_INTEGER, DWORD);
static DWORD  (WINAPI *r_GetFileSize)(HANDLE, LPDWORD);
static BOOL   (WINAPI *r_GetFileSizeEx)(HANDLE, PLARGE_INTEGER);
static HANDLE (WINAPI *r_CreateFileMappingA)(HANDLE, LPSECURITY_ATTRIBUTES,
                                             DWORD, DWORD, DWORD, LPCSTR);
static HANDLE (WINAPI *r_CreateFileMappingW)(HANDLE, LPSECURITY_ATTRIBUTES,
                                             DWORD, DWORD, DWORD, LPCWSTR);
static LPVOID (WINAPI *r_MapViewOfFile)(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
static BOOL   (WINAPI *r_UnmapViewOfFile)(LPCVOID);
static BOOL   (WINAPI *r_GetOverlappedResult)(HANDLE, LPOVERLAPPED, LPDWORD,
                                              BOOL);
static BOOL   (WINAPI *r_GetOverlappedResultEx)(HANDLE, LPOVERLAPPED, LPDWORD,
                                                DWORD, BOOL);
static DWORD  (WINAPI *r_WaitForSingleObject)(HANDLE, DWORD);
static DWORD  (WINAPI *r_WaitForSingleObjectEx)(HANDLE, DWORD, BOOL);
static DWORD  (WINAPI *r_WaitForMultipleObjects)(DWORD, const HANDLE *, BOOL,
                                                 DWORD);
static DWORD  (WINAPI *r_WaitForMultipleObjectsEx)(DWORD, const HANDLE *,
                                                   BOOL, DWORD, BOOL);
static BOOL   (WINAPI *r_CloseHandle)(HANDLE);
static BOOL   (WINAPI *r_SetFileInformationByHandle)(HANDLE,
                                                     FILE_INFO_BY_HANDLE_CLASS,
                                                     LPVOID, DWORD);
static BOOL   (WINAPI *r_CopyFileA)(LPCSTR, LPCSTR, BOOL);
static BOOL   (WINAPI *r_CopyFileW)(LPCWSTR, LPCWSTR, BOOL);

/* The offset an OVERLAPPED names, when it is one that carries a position.
 * The engine reads archives asynchronously, so this is the only place a
 * read's offset is knowable without asking the file itself. */
static uint64_t OverlappedOffset(const OVERLAPPED *ov)
{
    if (!ov) return 0;
    return (uint64_t)ov->Offset | ((uint64_t)ov->OffsetHigh << 32);
}

/* ---- open --------------------------------------------------------------- */

static HANDLE WINAPI H_CreateFileA(LPCSTR name, DWORD access, DWORD share,
                                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                   DWORD flags, HANDLE tmpl)
{
    ShFileCall c;
    FilePath   p;
    HANDLE     h;
    DWORD      err;

    if (!FilesWatch(&c, "CreateFileA", SH_FILE_OPEN, name, NULL, 0, &p))
        return r_CreateFileA(name, access, share, sa, disp, flags, tmpl);
    c.access = access;
    c.share  = share;
    c.disp   = disp;
    c.flags  = flags;
    c.handle = NULL;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideHandle();
    case FILES_ANSWERED: SetLastError(c.error); return (HANDLE)c.result;
    }
    h = r_CreateFileA(c.pathA, access, share, sa, disp, flags, tmpl);
    err = GetLastError();
    FilesAfter(&c, &p, h, err, 0);
    SetLastError(c.error);
    return (HANDLE)c.result;
}

static HANDLE WINAPI H_CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                   DWORD flags, HANDLE tmpl)
{
    ShFileCall c;
    FilePath   p;
    HANDLE     h;
    DWORD      err;

    if (!FilesWatch(&c, "CreateFileW", SH_FILE_OPEN, name, NULL, 1, &p))
        return r_CreateFileW(name, access, share, sa, disp, flags, tmpl);
    c.access = access;
    c.share  = share;
    c.disp   = disp;
    c.flags  = flags;
    c.handle = NULL;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideHandle();
    case FILES_ANSWERED: SetLastError(c.error); return (HANDLE)c.result;
    }
    h = r_CreateFileW(c.path, access, share, sa, disp, flags, tmpl);
    err = GetLastError();
    FilesAfter(&c, &p, h, err, 0);
    SetLastError(c.error);
    return (HANDLE)c.result;
}

/* ---- attributes --------------------------------------------------------- */

static DWORD WINAPI H_GetFileAttributesA(LPCSTR name)
{
    ShFileCall c;
    FilePath   p;
    DWORD      r, err;

    if (!FilesWatch(&c, "GetFileAttributesA", SH_FILE_ATTR, name, NULL, 0, &p))
        return r_GetFileAttributesA(name);
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideDword();
    case FILES_ANSWERED: SetLastError(c.error); return (DWORD)(uintptr_t)c.result;
    }
    r = r_GetFileAttributesA(c.pathA);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (DWORD)(uintptr_t)c.result;
}

static DWORD WINAPI H_GetFileAttributesW(LPCWSTR name)
{
    ShFileCall c;
    FilePath   p;
    DWORD      r, err;

    if (!FilesWatch(&c, "GetFileAttributesW", SH_FILE_ATTR, name, NULL, 1, &p))
        return r_GetFileAttributesW(name);
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideDword();
    case FILES_ANSWERED: SetLastError(c.error); return (DWORD)(uintptr_t)c.result;
    }
    r = r_GetFileAttributesW(c.path);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (DWORD)(uintptr_t)c.result;
}

static BOOL WINAPI H_GetFileAttributesExA(LPCSTR name,
                                          GET_FILEEX_INFO_LEVELS level,
                                          LPVOID info)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "GetFileAttributesExA", SH_FILE_ATTR, name, NULL, 0, &p))
        return r_GetFileAttributesExA(name, level, info);
    c.buffer = info;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_GetFileAttributesExA(c.pathA, level, info);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

static BOOL WINAPI H_GetFileAttributesExW(LPCWSTR name,
                                          GET_FILEEX_INFO_LEVELS level,
                                          LPVOID info)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "GetFileAttributesExW", SH_FILE_ATTR, name, NULL, 1, &p))
        return r_GetFileAttributesExW(name, level, info);
    c.buffer = info;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_GetFileAttributesExW(c.path, level, info);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

/* ---- move ---------------------------------------------------------------- */

static BOOL WINAPI H_MoveFileA(LPCSTR from, LPCSTR to)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "MoveFileA", SH_FILE_MOVE, from, to, 0, &p))
        return r_MoveFileA(from, to);
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_MoveFileA(c.pathA, c.toA);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

static BOOL WINAPI H_MoveFileW(LPCWSTR from, LPCWSTR to)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "MoveFileW", SH_FILE_MOVE, from, to, 1, &p))
        return r_MoveFileW(from, to);
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_MoveFileW(c.path, c.to);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

static BOOL WINAPI H_MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "MoveFileExA", SH_FILE_MOVE, from, to, 0, &p))
        return r_MoveFileExA(from, to, flags);
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_MoveFileExA(c.pathA, c.toA, flags);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

static BOOL WINAPI H_MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "MoveFileExW", SH_FILE_MOVE, from, to, 1, &p))
        return r_MoveFileExW(from, to, flags);
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_MoveFileExW(c.path, c.to, flags);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

/* ---- delete --------------------------------------------------------------- */

static BOOL WINAPI H_DeleteFileA(LPCSTR name)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "DeleteFileA", SH_FILE_DELETE, name, NULL, 0, &p))
        return r_DeleteFileA(name);
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_DeleteFileA(c.pathA);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

static BOOL WINAPI H_DeleteFileW(LPCWSTR name)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "DeleteFileW", SH_FILE_DELETE, name, NULL, 1, &p))
        return r_DeleteFileW(name);
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_DeleteFileW(c.path);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

static BOOL WINAPI H_RemoveDirectoryA(LPCSTR name)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "RemoveDirectoryA", SH_FILE_DELETE, name, NULL, 0, &p))
        return r_RemoveDirectoryA(name);
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_RemoveDirectoryA(c.pathA);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

static BOOL WINAPI H_RemoveDirectoryW(LPCWSTR name)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "RemoveDirectoryW", SH_FILE_DELETE, name, NULL, 1, &p))
        return r_RemoveDirectoryW(name);
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_RemoveDirectoryW(c.path);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

/* ---- find ---------------------------------------------------------------- */

static HANDLE WINAPI H_FindFirstFileA(LPCSTR spec, LPWIN32_FIND_DATAA data)
{
    ShFileCall c;
    FilePath   p;
    HANDLE     h;
    DWORD      err;

    if (!FilesWatch(&c, "FindFirstFileA", SH_FILE_FIND, spec, NULL, 0, &p))
        return r_FindFirstFileA(spec, data);
    c.buffer = data;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideHandle();
    case FILES_ANSWERED: SetLastError(c.error); return (HANDLE)c.result;
    }
    h = r_FindFirstFileA(c.pathA, data);
    err = GetLastError();
    FilesAfter(&c, &p, h, err, 0);
    SetLastError(c.error);
    return (HANDLE)c.result;
}

static HANDLE WINAPI H_FindFirstFileW(LPCWSTR spec, LPWIN32_FIND_DATAW data)
{
    ShFileCall c;
    FilePath   p;
    HANDLE     h;
    DWORD      err;

    if (!FilesWatch(&c, "FindFirstFileW", SH_FILE_FIND, spec, NULL, 1, &p))
        return r_FindFirstFileW(spec, data);
    c.buffer = data;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideHandle();
    case FILES_ANSWERED: SetLastError(c.error); return (HANDLE)c.result;
    }
    h = r_FindFirstFileW(c.path, data);
    err = GetLastError();
    FilesAfter(&c, &p, h, err, 0);
    SetLastError(c.error);
    return (HANDLE)c.result;
}

/* The Ex forms take the same spec and answer the same way; the data is a
 * WIN32_FIND_DATA of the same width, which is what a watcher is after. */
static HANDLE WINAPI H_FindFirstFileExA(LPCSTR spec, FINDEX_INFO_LEVELS level,
                                        LPVOID data, FINDEX_SEARCH_OPS ops,
                                        LPVOID filter, DWORD flags)
{
    ShFileCall c;
    FilePath   p;
    HANDLE     h;
    DWORD      err;

    if (!FilesWatch(&c, "FindFirstFileExA", SH_FILE_FIND, spec, NULL, 0, &p))
        return r_FindFirstFileExA(spec, level, data, ops, filter, flags);
    c.buffer = data;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideHandle();
    case FILES_ANSWERED: SetLastError(c.error); return (HANDLE)c.result;
    }
    h = r_FindFirstFileExA(c.pathA, level, data, ops, filter, flags);
    err = GetLastError();
    FilesAfter(&c, &p, h, err, 0);
    SetLastError(c.error);
    return (HANDLE)c.result;
}

static HANDLE WINAPI H_FindFirstFileExW(LPCWSTR spec, FINDEX_INFO_LEVELS level,
                                        LPVOID data, FINDEX_SEARCH_OPS ops,
                                        LPVOID filter, DWORD flags)
{
    ShFileCall c;
    FilePath   p;
    HANDLE     h;
    DWORD      err;

    if (!FilesWatch(&c, "FindFirstFileExW", SH_FILE_FIND, spec, NULL, 1, &p))
        return r_FindFirstFileExW(spec, level, data, ops, filter, flags);
    c.buffer = data;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideHandle();
    case FILES_ANSWERED: SetLastError(c.error); return (HANDLE)c.result;
    }
    h = r_FindFirstFileExW(c.path, level, data, ops, filter, flags);
    err = GetLastError();
    FilesAfter(&c, &p, h, err, 0);
    SetLastError(c.error);
    return (HANDLE)c.result;
}

/* A find's next entry names no file in the call itself, so it is a watch
 * or a decision and never a hide: the spec belongs to the handle. */
static BOOL WINAPI H_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "FindNextFileA", SH_FILE_FIND, NULL, NULL, 0, &p))
        return r_FindNextFileA(h, data);
    c.handle = h;
    c.buffer = data;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_FindNextFileA(h, data);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

static BOOL WINAPI H_FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW data)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "FindNextFileW", SH_FILE_FIND, NULL, NULL, 1, &p))
        return r_FindNextFileW(h, data);
    c.handle = h;
    c.buffer = data;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_FindNextFileW(h, data);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

/* ---- read, and where a read is ------------------------------------------ */

static BOOL WINAPI H_ReadFile(HANDLE h, LPVOID buf, DWORD want,
                              LPDWORD done, LPOVERLAPPED ov)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "ReadFile", SH_FILE_READ, NULL, NULL, 1, &p))
        return r_ReadFile(h, buf, want, done, ov);
    c.handle     = h;
    c.buffer     = buf;
    c.bytes      = want;
    c.overlapped = ov;
    c.offset     = OverlappedOffset(ov);
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_ReadFile(h, buf, want, done, ov);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err,
               done ? *done : 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

/* SetFilePointer's new position is a return value on one and a 64 bit
 * number in and out on the other; both land in `offset` so a watcher sees
 * the same thing whichever the engine called. */
static DWORD WINAPI H_SetFilePointer(HANDLE h, LONG move, PLONG high,
                                     DWORD method)
{
    ShFileCall c;
    FilePath   p;
    DWORD      r, err;

    if (!FilesWatch(&c, "SetFilePointer", SH_FILE_READ, NULL, NULL, 1, &p))
        return r_SetFilePointer(h, move, high, method);
    c.handle = h;
    c.offset = (uint64_t)(uint32_t)move |
               (high ? ((uint64_t)(uint32_t)*high << 32) : 0);
    c.bytes  = method;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideDword();
    case FILES_ANSWERED: SetLastError(c.error); return (DWORD)(uintptr_t)c.result;
    }
    r = r_SetFilePointer(h, move, high, method);
    err = GetLastError();
    c.offset = (uint64_t)r | (high ? ((uint64_t)(uint32_t)*high << 32) : 0);
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (DWORD)(uintptr_t)c.result;
}

static BOOL WINAPI H_SetFilePointerEx(HANDLE h, LARGE_INTEGER move,
                                      PLARGE_INTEGER here, DWORD method)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "SetFilePointerEx", SH_FILE_READ, NULL, NULL, 1, &p))
        return r_SetFilePointerEx(h, move, here, method);
    c.handle = h;
    c.offset = (uint64_t)move.QuadPart;
    c.bytes  = method;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_SetFilePointerEx(h, move, here, method);
    err = GetLastError();
    if (r && here) c.offset = (uint64_t)here->QuadPart;
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

static DWORD WINAPI H_GetFileSize(HANDLE h, LPDWORD high)
{
    ShFileCall c;
    FilePath   p;
    DWORD      r, err;

    if (!FilesWatch(&c, "GetFileSize", SH_FILE_READ, NULL, NULL, 1, &p))
        return r_GetFileSize(h, high);
    c.handle = h;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideDword();
    case FILES_ANSWERED: SetLastError(c.error); return (DWORD)(uintptr_t)c.result;
    }
    r = r_GetFileSize(h, high);
    err = GetLastError();
    if (r != INVALID_FILE_SIZE)
        c.offset = (uint64_t)r | (high ? ((uint64_t)(uint32_t)*high << 32) : 0);
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (DWORD)(uintptr_t)c.result;
}

static BOOL WINAPI H_GetFileSizeEx(HANDLE h, PLARGE_INTEGER size)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "GetFileSizeEx", SH_FILE_READ, NULL, NULL, 1, &p))
        return r_GetFileSizeEx(h, size);
    c.handle = h;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_GetFileSizeEx(h, size);
    err = GetLastError();
    if (r && size) c.offset = (uint64_t)size->QuadPart;
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

/* The mapping calls name no file either - they take a handle - but they
 * are how a caller would read one without ReadFile, so a watcher that has
 * to prove "this archive was read, not mapped" needs to see them. */
static HANDLE WINAPI H_CreateFileMappingA(HANDLE h,
                                          LPSECURITY_ATTRIBUTES sa,
                                          DWORD protect, DWORD hi, DWORD lo,
                                          LPCSTR name)
{
    ShFileCall c;
    FilePath   p;
    HANDLE     r;
    DWORD      err;

    if (!FilesWatch(&c, "CreateFileMappingA", SH_FILE_READ, NULL, NULL, 0, &p))
        return r_CreateFileMappingA(h, sa, protect, hi, lo, name);
    c.handle = h;
    c.bytes  = protect;
    c.offset = (uint64_t)lo | ((uint64_t)hi << 32);
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideHandle();
    case FILES_ANSWERED: SetLastError(c.error); return (HANDLE)c.result;
    }
    r = r_CreateFileMappingA(h, sa, protect, hi, lo, name);
    err = GetLastError();
    FilesAfter(&c, &p, r, err, 0);
    SetLastError(c.error);
    return (HANDLE)c.result;
}

static HANDLE WINAPI H_CreateFileMappingW(HANDLE h,
                                          LPSECURITY_ATTRIBUTES sa,
                                          DWORD protect, DWORD hi, DWORD lo,
                                          LPCWSTR name)
{
    ShFileCall c;
    FilePath   p;
    HANDLE     r;
    DWORD      err;

    if (!FilesWatch(&c, "CreateFileMappingW", SH_FILE_READ, NULL, NULL, 1, &p))
        return r_CreateFileMappingW(h, sa, protect, hi, lo, name);
    c.handle = h;
    c.bytes  = protect;
    c.offset = (uint64_t)lo | ((uint64_t)hi << 32);
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideHandle();
    case FILES_ANSWERED: SetLastError(c.error); return (HANDLE)c.result;
    }
    r = r_CreateFileMappingW(h, sa, protect, hi, lo, name);
    err = GetLastError();
    FilesAfter(&c, &p, r, err, 0);
    SetLastError(c.error);
    return (HANDLE)c.result;
}

static LPVOID WINAPI H_MapViewOfFile(HANDLE h, DWORD access, DWORD hi,
                                     DWORD lo, SIZE_T bytes)
{
    ShFileCall c;
    FilePath   p;
    LPVOID     r;
    DWORD      err;

    if (!FilesWatch(&c, "MapViewOfFile", SH_FILE_READ, NULL, NULL, 1, &p))
        return r_MapViewOfFile(h, access, hi, lo, bytes);
    c.handle = h;
    c.bytes  = (DWORD)bytes;
    c.offset = (uint64_t)lo | ((uint64_t)hi << 32);
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return NULL;
    case FILES_ANSWERED: SetLastError(c.error); return (LPVOID)c.result;
    }
    r = r_MapViewOfFile(h, access, hi, lo, bytes);
    err = GetLastError();
    FilesAfter(&c, &p, r, err, 0);
    SetLastError(c.error);
    return (LPVOID)c.result;
}

static BOOL WINAPI H_UnmapViewOfFile(LPCVOID base)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "UnmapViewOfFile", SH_FILE_READ, NULL, NULL, 1, &p))
        return r_UnmapViewOfFile(base);
    c.buffer = (void *)base;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_UnmapViewOfFile(base);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

/* ---- the completion path --------------------------------------------------
 *
 * Everything an asynchronous read needs to be told it has finished, plus
 * CloseHandle. This is where a read with an OVERLAPPED can be looked at
 * again after the fact: the engine's own thread is the one that waits, and
 * it is the only one that can be holding the buffer at that moment.
 */
static BOOL WINAPI H_GetOverlappedResult(HANDLE h, LPOVERLAPPED ov,
                                         LPDWORD done, BOOL wait)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "GetOverlappedResult", SH_FILE_WAIT, NULL, NULL, 1, &p))
        return r_GetOverlappedResult(h, ov, done, wait);
    c.handle     = h;
    c.overlapped = ov;
    c.offset     = OverlappedOffset(ov);
    c.bytes      = done ? *done : 0;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_GetOverlappedResult(h, ov, done, wait);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, done ? *done : 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

static BOOL WINAPI H_GetOverlappedResultEx(HANDLE h, LPOVERLAPPED ov,
                                           LPDWORD done, DWORD ms,
                                           BOOL alertable)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "GetOverlappedResultEx", SH_FILE_WAIT, NULL, NULL, 1, &p))
        return r_GetOverlappedResultEx(h, ov, done, ms, alertable);
    c.handle     = h;
    c.overlapped = ov;
    c.offset     = OverlappedOffset(ov);
    c.bytes      = done ? *done : 0;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_GetOverlappedResultEx(h, ov, done, ms, alertable);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, done ? *done : 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

static DWORD WINAPI H_WaitForSingleObject(HANDLE h, DWORD ms)
{
    ShFileCall c;
    FilePath   p;
    DWORD      r, err;

    if (!FilesWatch(&c, "WaitForSingleObject", SH_FILE_WAIT, NULL, NULL, 1, &p))
        return r_WaitForSingleObject(h, ms);
    c.handle = h;
    c.bytes  = ms;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return WAIT_FAILED;
    case FILES_ANSWERED: SetLastError(c.error); return (DWORD)(uintptr_t)c.result;
    }
    r = r_WaitForSingleObject(h, ms);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (DWORD)(uintptr_t)c.result;
}

static DWORD WINAPI H_WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
    ShFileCall c;
    FilePath   p;
    DWORD      r, err;

    if (!FilesWatch(&c, "WaitForSingleObjectEx", SH_FILE_WAIT, NULL, NULL, 1, &p))
        return r_WaitForSingleObjectEx(h, ms, alertable);
    c.handle = h;
    c.bytes  = ms;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return WAIT_FAILED;
    case FILES_ANSWERED: SetLastError(c.error); return (DWORD)(uintptr_t)c.result;
    }
    r = r_WaitForSingleObjectEx(h, ms, alertable);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (DWORD)(uintptr_t)c.result;
}

/* The multiple forms carry an array: the first handle is `handle`, and the
 * array and its count are handed on so a watcher can look at all of them
 * (a read's OVERLAPPED is still resolved from the handle it waits on). */
static DWORD WINAPI H_WaitForMultipleObjects(DWORD n, const HANDLE *hs,
                                             BOOL all, DWORD ms)
{
    ShFileCall c;
    FilePath   p;
    DWORD      r, err;

    if (!FilesWatch(&c, "WaitForMultipleObjects", SH_FILE_WAIT, NULL, NULL, 1, &p))
        return r_WaitForMultipleObjects(n, hs, all, ms);
    c.handle = n ? hs[0] : NULL;
    c.buffer = (void *)hs;
    c.bytes  = ms;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return WAIT_FAILED;
    case FILES_ANSWERED: SetLastError(c.error); return (DWORD)(uintptr_t)c.result;
    }
    r = r_WaitForMultipleObjects(n, hs, all, ms);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (DWORD)(uintptr_t)c.result;
}

static DWORD WINAPI H_WaitForMultipleObjectsEx(DWORD n, const HANDLE *hs,
                                               BOOL all, DWORD ms,
                                               BOOL alertable)
{
    ShFileCall c;
    FilePath   p;
    DWORD      r, err;

    if (!FilesWatch(&c, "WaitForMultipleObjectsEx", SH_FILE_WAIT, NULL, NULL, 1, &p))
        return r_WaitForMultipleObjectsEx(n, hs, all, ms, alertable);
    c.handle = n ? hs[0] : NULL;
    c.buffer = (void *)hs;
    c.bytes  = ms;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return WAIT_FAILED;
    case FILES_ANSWERED: SetLastError(c.error); return (DWORD)(uintptr_t)c.result;
    }
    r = r_WaitForMultipleObjectsEx(n, hs, all, ms, alertable);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (DWORD)(uintptr_t)c.result;
}

static BOOL WINAPI H_CloseHandle(HANDLE h)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "CloseHandle", SH_FILE_WAIT, NULL, NULL, 1, &p))
        return r_CloseHandle(h);
    c.handle = h;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_CloseHandle(h);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

/* ---- metadata and copies -------------------------------------------------- */

static BOOL WINAPI H_SetFileInformationByHandle(HANDLE h,
                                                FILE_INFO_BY_HANDLE_CLASS cls,
                                                LPVOID info, DWORD size)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "SetFileInformationByHandle", SH_FILE_INFO, NULL, NULL, 1, &p))
        return r_SetFileInformationByHandle(h, cls, info, size);
    c.handle = h;
    c.buffer = info;
    c.bytes  = size;
    c.offset = (uint64_t)cls;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_SetFileInformationByHandle(h, cls, info, size);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

static BOOL WINAPI H_CopyFileA(LPCSTR from, LPCSTR to, BOOL failIfExists)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "CopyFileA", SH_FILE_INFO, from, to, 0, &p))
        return r_CopyFileA(from, to, failIfExists);
    c.bytes = failIfExists ? 1u : 0u;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_CopyFileA(c.pathA, c.toA, failIfExists);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

static BOOL WINAPI H_CopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists)
{
    ShFileCall c;
    FilePath   p;
    BOOL       r;
    DWORD      err;

    if (!FilesWatch(&c, "CopyFileW", SH_FILE_INFO, from, to, 1, &p))
        return r_CopyFileW(from, to, failIfExists);
    c.bytes = failIfExists ? 1u : 0u;
    switch (FilesDecide(&c, &p)) {
    case FILES_HIDDEN:   return HideBool();
    case FILES_ANSWERED: SetLastError(c.error); return (BOOL)(uintptr_t)c.result;
    }
    r = r_CopyFileW(c.path, c.to, failIfExists);
    err = GetLastError();
    FilesAfter(&c, &p, (void *)(uintptr_t)r, err, 0);
    SetLastError(c.error);
    return (BOOL)(uintptr_t)c.result;
}

/* ---- the table ------------------------------------------------------------
 *
 * The order is the order the log lists them in, and nothing depends on it.
 * A target that is not there (GetOverlappedResultEx before Windows 8) is
 * skipped when it is asked for, with the reason in the log.
 */
static FileApi g_api[] = {
    { "CreateFileA",   SH_FILE_OPEN,   (void *)H_CreateFileA,   (void **)&r_CreateFileA,   NULL, 0 },
    { "CreateFileW",   SH_FILE_OPEN,   (void *)H_CreateFileW,   (void **)&r_CreateFileW,   NULL, 0 },
    { "GetFileAttributesA",   SH_FILE_ATTR, (void *)H_GetFileAttributesA,   (void **)&r_GetFileAttributesA,   NULL, 0 },
    { "GetFileAttributesW",   SH_FILE_ATTR, (void *)H_GetFileAttributesW,   (void **)&r_GetFileAttributesW,   NULL, 0 },
    { "GetFileAttributesExA", SH_FILE_ATTR, (void *)H_GetFileAttributesExA, (void **)&r_GetFileAttributesExA, NULL, 0 },
    { "GetFileAttributesExW", SH_FILE_ATTR, (void *)H_GetFileAttributesExW, (void **)&r_GetFileAttributesExW, NULL, 0 },
    { "MoveFileA",     SH_FILE_MOVE,   (void *)H_MoveFileA,     (void **)&r_MoveFileA,     NULL, 0 },
    { "MoveFileW",     SH_FILE_MOVE,   (void *)H_MoveFileW,     (void **)&r_MoveFileW,     NULL, 0 },
    { "MoveFileExA",   SH_FILE_MOVE,   (void *)H_MoveFileExA,   (void **)&r_MoveFileExA,   NULL, 0 },
    { "MoveFileExW",   SH_FILE_MOVE,   (void *)H_MoveFileExW,   (void **)&r_MoveFileExW,   NULL, 0 },
    { "DeleteFileA",   SH_FILE_DELETE, (void *)H_DeleteFileA,   (void **)&r_DeleteFileA,   NULL, 0 },
    { "DeleteFileW",   SH_FILE_DELETE, (void *)H_DeleteFileW,   (void **)&r_DeleteFileW,   NULL, 0 },
    { "RemoveDirectoryA", SH_FILE_DELETE, (void *)H_RemoveDirectoryA, (void **)&r_RemoveDirectoryA, NULL, 0 },
    { "RemoveDirectoryW", SH_FILE_DELETE, (void *)H_RemoveDirectoryW, (void **)&r_RemoveDirectoryW, NULL, 0 },
    { "FindFirstFileA",   SH_FILE_FIND, (void *)H_FindFirstFileA,   (void **)&r_FindFirstFileA,   NULL, 0 },
    { "FindFirstFileW",   SH_FILE_FIND, (void *)H_FindFirstFileW,   (void **)&r_FindFirstFileW,   NULL, 0 },
    { "FindFirstFileExA", SH_FILE_FIND, (void *)H_FindFirstFileExA, (void **)&r_FindFirstFileExA, NULL, 0 },
    { "FindFirstFileExW", SH_FILE_FIND, (void *)H_FindFirstFileExW, (void **)&r_FindFirstFileExW, NULL, 0 },
    { "FindNextFileA",    SH_FILE_FIND, (void *)H_FindNextFileA,    (void **)&r_FindNextFileA,    NULL, 0 },
    { "FindNextFileW",    SH_FILE_FIND, (void *)H_FindNextFileW,    (void **)&r_FindNextFileW,    NULL, 0 },
    { "ReadFile",         SH_FILE_READ, (void *)H_ReadFile,         (void **)&r_ReadFile,         NULL, 0 },
    { "SetFilePointer",   SH_FILE_READ, (void *)H_SetFilePointer,   (void **)&r_SetFilePointer,   NULL, 0 },
    { "SetFilePointerEx", SH_FILE_READ, (void *)H_SetFilePointerEx, (void **)&r_SetFilePointerEx, NULL, 0 },
    { "GetFileSize",      SH_FILE_READ, (void *)H_GetFileSize,      (void **)&r_GetFileSize,      NULL, 0 },
    { "GetFileSizeEx",    SH_FILE_READ, (void *)H_GetFileSizeEx,    (void **)&r_GetFileSizeEx,    NULL, 0 },
    { "CreateFileMappingA", SH_FILE_READ, (void *)H_CreateFileMappingA, (void **)&r_CreateFileMappingA, NULL, 0 },
    { "CreateFileMappingW", SH_FILE_READ, (void *)H_CreateFileMappingW, (void **)&r_CreateFileMappingW, NULL, 0 },
    { "MapViewOfFile",      SH_FILE_READ, (void *)H_MapViewOfFile,      (void **)&r_MapViewOfFile,      NULL, 0 },
    { "UnmapViewOfFile",    SH_FILE_READ, (void *)H_UnmapViewOfFile,    (void **)&r_UnmapViewOfFile,    NULL, 0 },
    { "GetOverlappedResult",   SH_FILE_WAIT, (void *)H_GetOverlappedResult,   (void **)&r_GetOverlappedResult,   NULL, 0 },
    { "GetOverlappedResultEx", SH_FILE_WAIT, (void *)H_GetOverlappedResultEx, (void **)&r_GetOverlappedResultEx, NULL, 0 },
    { "WaitForSingleObject",     SH_FILE_WAIT, (void *)H_WaitForSingleObject,     (void **)&r_WaitForSingleObject,     NULL, 0 },
    { "WaitForSingleObjectEx",   SH_FILE_WAIT, (void *)H_WaitForSingleObjectEx,   (void **)&r_WaitForSingleObjectEx,   NULL, 0 },
    { "WaitForMultipleObjects",  SH_FILE_WAIT, (void *)H_WaitForMultipleObjects,  (void **)&r_WaitForMultipleObjects,  NULL, 0 },
    { "WaitForMultipleObjectsEx", SH_FILE_WAIT, (void *)H_WaitForMultipleObjectsEx, (void **)&r_WaitForMultipleObjectsEx, NULL, 0 },
    { "CloseHandle",     SH_FILE_WAIT, (void *)H_CloseHandle,     (void **)&r_CloseHandle,     NULL, 0 },
    { "SetFileInformationByHandle", SH_FILE_INFO, (void *)H_SetFileInformationByHandle, (void **)&r_SetFileInformationByHandle, NULL, 0 },
    { "CopyFileA",       SH_FILE_INFO, (void *)H_CopyFileA,       (void **)&r_CopyFileA,       NULL, 0 },
    { "CopyFileW",       SH_FILE_INFO, (void *)H_CopyFileW,       (void **)&r_CopyFileW,       NULL, 0 }
};

/* ---- installing and taking the hooks back out ---------------------------
 *
 * Called on the thread that registered or released a rule, never from
 * inside a hook, and never from DllMain: creating a hook allocates and
 * writes code pages. Every target is enabled and disabled by its own
 * address - never with MH_ALL_HOOKS, which would also switch on hooks
 * another module of this DLL deliberately created but has not enabled
 * yet (scripthook_corefix.c, scripthook_forge_io.c and forgeprobe all
 * call it, and a future one that wants a hook created-but-off would be
 * broken by it).
 */
static void FileLogInit(void)
{
    if (InterlockedExchange(&g_ready, 1)) return;
    LogInit("scripthook_files.log");
    Log("--- file interception layer ---");
}

static int MinHookReady(void)
{
    static volatile LONG tried = 0;
    MH_STATUS s;

    if (InterlockedCompareExchange(&tried, 0, 0)) return 1;
    s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        Log("files: MH_Initialize failed (%s) - the layer stays out",
            MH_StatusToString(s));
        return 0;
    }
    InterlockedExchange(&tried, 1);
    return 1;
}

static void SyncTargets(void)
{
    uint32_t need;
    int      was, have = 0, changed = 0, i;

    SyncLock();
    need = (uint32_t)InterlockedCompareExchange(&g_mask, 0, 0);
    was  = (int)InterlockedCompareExchange(&g_installed, 0, 0);

    if (need && !MinHookReady()) { SyncUnlock(); return; }

    for (i = 0; i < (int)ARRAY_LEN(g_api); i++) {
        FileApi *a = &g_api[i];
        int want = (need & a->group) != 0;

        if (want && !a->hooked) {
            MH_STATUS s;

            if (!a->target) {
                a->target = (void *)GetProcAddress(
                    GetModuleHandleA("kernel32.dll"), a->name);
                if (!a->target) {
                    Log("files: %s is not on this system - not hooked",
                        a->name);
                    continue;
                }
            }
            s = MH_CreateHook(a->target, a->detour, a->real);
            if (s != MH_OK) {
                Log("files: hooking %s failed (%s)", a->name,
                    MH_StatusToString(s));
                continue;
            }
            s = MH_EnableHook(a->target);
            if (s != MH_OK) {
                Log("files: enabling %s failed (%s)", a->name,
                    MH_StatusToString(s));
                MH_RemoveHook(a->target);
                *(a->real) = NULL;  /* the trampoline went with it */
                continue;
            }
            a->hooked = 1;
            changed++;
        } else if (!want && a->hooked) {
            MH_DisableHook(a->target);
            MH_RemoveHook(a->target);
            a->hooked = 0;
            *(a->real) = NULL;      /* never leave a freed trampoline set */
            changed++;
        }
        if (a->hooked) have++;
    }

    InterlockedExchange(&g_installed, have ? 1 : 0);
    if (changed)
        Log("files: %d of %d target(s) hooked, the rest released",
            have, (int)ARRAY_LEN(g_api));
    if (was && !have)
        Log("files: the layer is out - not one file call is intercepted");
    SyncUnlock();
}

/* ---- the pass -----------------------------------------------------------
 *
 * Watch: the whole hot path when nothing is registered - a thread-local
 * read, two counters and a bit test, and the call goes straight through
 * without a context being built at all.
 */
static int FilesWatch(ShFileCall *c, const char *api, uint32_t group,
                      const void *path, const void *path2, int wide,
                      FilePath *p)
{
    if (t_depth || t_own) return 0;

    if (!InterlockedCompareExchange(&g_live, 0, 0)) return 0;
    if (!((uint32_t)InterlockedCompareExchange(&g_mask, 0, 0) & group))
        return 0;

    memset(c, 0, sizeof(*c));
    c->api   = api;
    c->group = group;
    c->wide  = wide;
    if (wide) {
        c->path  = (const wchar_t *)path;
        c->asked  = c->path;
        c->to    = (const wchar_t *)path2;
    } else {
        c->pathA  = (const char *)path;
        c->askedA = c->pathA;
        c->toA    = (const char *)path2;
    }
    PathOf(p, path, wide);

    InterlockedIncrement(&g_calls);
    return 1;
}

/* One line per rule, for its first few hits, then silence: what it did and
 * which rule did it, which is what a session's log has to answer. */
static void NoteHit(struct ShFileRule *r, const char *what)
{
    LONG n = InterlockedIncrement(&r->noted);

    if (n > 3) return;
    Log("files: rule %d (%s) %s %s '%ls'%ls%ls - hit %ld%s",
        r->seq, r->owner[0] ? r->owner : "?", what,
        GroupName(r->group),
        r->hasName ? r->nameW : L"*",
        (r->hasName && r->hasSuffix) ? L" ..." : L"",
        r->hasSuffix ? r->suffixW : L"",
        (long)n, n == 3 ? " (the rest is only counted)" : "");
}

/* Weigh the rules that match this call. The winner decides it, and a hide
 * wins over everything - the most conservative answer a rule can give is
 * the one that cannot be wrong. */
static int FilesDecide(ShFileCall *c, FilePath *p)
{
    struct ShFileRule *best = NULL;
    int bestRank = 0, i;

    for (i = 0; i < (int)ARRAY_LEN(g_rule); i++) {
        struct ShFileRule *r = &g_rule[i];
        int rank;

        if (!InterlockedCompareExchange(&r->live, 0, 0)) continue;
        rank = ActionRank(r->action);
        if (!rank || bestRank > rank) continue;
        if (bestRank == rank && best && best->seq < r->seq) continue;
        if (!RuleMatches(r, c->group, p)) continue;

        best = r;
        bestRank = rank;
        if (rank == 3) break;                   /* nothing beats a hide */
    }

    if (!best) return FILES_REAL;
    c->matched = 1;
    InterlockedIncrement(&best->hits);

    if (best->action == SH_FILE_HIDE) {
        /* Only a call that names a file can be answered "not there"; a
         * handle-carrying call has nothing to hide. */
        if (!p->fileW && !p->fileA) return FILES_REAL;
        c->answered = 1;
        c->result   = NULL;
        c->error    = ERROR_FILE_NOT_FOUND;
        InterlockedIncrement(&g_answered);
        NoteHit(best, "hid");
        FilesAfter(c, p, NULL, ERROR_FILE_NOT_FOUND, 0);
        return FILES_HIDDEN;
    }

    if (best->action == SH_FILE_REDIRECT) {
        if (c->wide) {
            CopyW(t_redirW, FILES_PATH_MAX, best->toW);
            c->path = t_redirW;
            PathOf(p, c->path, 1);
        } else {
            ToAnsi(t_redirA, FILES_PATH_MAX, best->toW);
            c->pathA = t_redirA;
            PathOf(p, c->pathA, 0);
        }
        NoteHit(best, "redirected");
        return FILES_REAL;      /* the detour uses the replaced path */
    }

    /* SH_FILE_DECIDE: unlike a hide or a redirect, one decider does not
     * end the question - there can be more than one on a path, and each of
     * them is entitled to be asked. They are asked in registration order
     * and the first to answer ends it. That ordering is also what keeps a
     * watcher (a decide rule with no before callback at all) from
     * swallowing a call some other module's decider was written for.
     *
     * The list is walked with a repeated scan for the next registration
     * after the last one asked: at most 32 entries, no allocation, and
     * only on the calls a decider actually matched. */
    if (bestRank == 1) {
        int lastSeq = 0;

        for (;;) {
            struct ShFileRule *pick = NULL;
            int i;

            for (i = 0; i < (int)ARRAY_LEN(g_rule); i++) {
                struct ShFileRule *r = &g_rule[i];

                if (!InterlockedCompareExchange(&r->live, 0, 0)) continue;
                if (r->action != SH_FILE_DECIDE || !r->before) continue;
                if (r->seq <= lastSeq) continue;
                if (pick && pick->seq < r->seq) continue;
                if (!RuleMatches(r, c->group, p)) continue;
                pick = r;
            }
            if (!pick) break;
            lastSeq = pick->seq;

            t_depth++;
            {
                int answered = pick->before(c, pick->user);
                t_depth--;
                if (answered) {
                    c->answered = 1;
                    InterlockedIncrement(&g_answered);
                    NoteHit(pick, "answered");
                    return FILES_ANSWERED;
                }
            }
            NoteHit(pick, "let through");
        }
    }
    return FILES_REAL;
}

/* Tell the rules that wanted the result. Runs for a call that ran and for
 * one a rule answered, and for every matching rule with an after callback,
 * in registration order. A hidden call stays hidden: what an after
 * callback changes is what the caller sees, and the layer has already
 * decided that a hide is the answer. */
static void FilesAfter(ShFileCall *c, FilePath *p, void *result, DWORD err,
                       DWORD bytes)
{
    int i;

    c->result = result;
    c->error  = err;
    c->done   = bytes;
    if (!c->matched) return;

    for (i = 0; i < (int)ARRAY_LEN(g_rule); i++) {
        struct ShFileRule *r = &g_rule[i];

        if (!r->after) continue;
        if (!InterlockedCompareExchange(&r->live, 0, 0)) continue;
        if (!RuleMatches(r, c->group, p)) continue;

        t_depth++;
        r->after(c, r->user);
        t_depth--;
    }
}

/* ---- the owner's name ---------------------------------------------------
 *
 * Which module registered a rule, asked of the caller's own address rather
 * than by any registry: a plugin that calls ShFileRuleAdd from its init
 * thread is named the same way the blacklist names it. A caller inside the
 * framework DLL - forge_io is one - is named "dinput8", which is the
 * honest answer: the code is the framework's own.
 */
static void OwnerFromAddress(void *addr, char *out, int n)
{
    HMODULE     mod = NULL;
    char        path[MAX_PATH];
    const char *base, *dot;
    int         i;

    out[0] = 0;
    if (!addr || !n) return;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &mod) || !mod)
        return;
    if (!GetModuleFileNameA(mod, path, sizeof(path))) return;

    base = ANamePart(path);
    dot  = strrchr(base, '.');
    for (i = 0; base[i] && (!dot || base + i < dot) && i < n - 1; i++)
        out[i] = base[i];
    out[i] = 0;
}

/* ---- the registry ------------------------------------------------------- */

ShFileRule *ShFileRuleAdd(const ShFileRuleDesc *desc)
{
    struct ShFileRule *r = NULL;
    uint32_t group;
    int      i;

    FileLogInit();
    if (!desc) return NULL;

    group = desc->group & SH_FILE_ANY;
    if (!group) {
        Log("files: refused: a rule with no group covers nothing");
        return NULL;
    }
    if (desc->action != SH_FILE_HIDE && desc->action != SH_FILE_REDIRECT &&
        desc->action != SH_FILE_DECIDE) {
        Log("files: refused: %d is not a SH_FILE_ action", desc->action);
        return NULL;
    }
    if (desc->action == SH_FILE_REDIRECT && (!desc->to || !desc->to[0])) {
        Log("files: refused: a redirect with no target");
        return NULL;
    }
    if (desc->action == SH_FILE_DECIDE && !desc->before && !desc->after) {
        Log("files: refused: a decide rule with neither callback");
        return NULL;
    }
    if ((desc->name && wcslen(desc->name) >= FILES_NAME_MAX) ||
        (desc->suffix && wcslen(desc->suffix) >= FILES_PATH_MAX) ||
        (desc->to && wcslen(desc->to) >= FILES_PATH_MAX)) {
        Log("files: refused: a name, suffix or target longer than the "
            "table holds (%d/%d chars)", FILES_NAME_MAX, FILES_PATH_MAX);
        return NULL;
    }
    if (desc->action == SH_FILE_HIDE && !FILES_NAMED(group)) {
        Log("files: refused: a hide needs a group that names a file "
            "(open, attr, move, delete, find)");
        return NULL;
    }

    FileLock();
    for (i = 0; i < FILES_RULES_MAX; i++) {
        if (!InterlockedCompareExchange(&g_rule[i].live, 0, 0)) {
            r = &g_rule[i];
            break;
        }
    }
    if (!r) {
        FileUnlock();
        Log("files: refused: all %d slots are taken", FILES_RULES_MAX);
        return NULL;
    }

    memset(r, 0, sizeof(*r));
    r->group  = group;
    r->action = desc->action;
    r->before = desc->before;
    r->after  = desc->after;
    r->user   = desc->user;
    CopyW(r->nameW, FILES_NAME_MAX, desc->name);
    ToAnsi(r->nameA, FILES_NAME_MAX, r->nameW);
    r->hasName = r->nameW[0] != 0;
    CopyW(r->suffixW, FILES_PATH_MAX, desc->suffix);
    ToAnsi(r->suffixA, FILES_PATH_MAX, r->suffixW);
    r->hasSuffix = r->suffixW[0] != 0;
    CopyW(r->toW, FILES_PATH_MAX, desc->to);
    ToAnsi(r->toA, FILES_PATH_MAX, r->toW);
    r->hasTo = r->toW[0] != 0;
    r->seq   = (int)InterlockedIncrement(&g_seq);

#ifdef _MSC_VER
    OwnerFromAddress(_ReturnAddress(), r->owner, FILES_OWNER_MAX);
#else
    OwnerFromAddress(__builtin_return_address(0), r->owner, FILES_OWNER_MAX);
#endif

    InterlockedIncrement(&g_live);
    InterlockedExchange(&r->live, 1);            /* published last */
    InterlockedExchange(&g_mask, (LONG)((uint32_t)g_mask | group));
    FileUnlock();

    Log("files: rule %d (%s) %s %s '%ls'%ls%ls -> %d of %d slots in use",
        r->seq, r->owner[0] ? r->owner : "?", ShFileActionName(r->action),
        GroupName(r->group),
        r->hasName ? r->nameW : L"*",
        (r->hasName && r->hasSuffix) ? L" ..." : L"",
        r->hasSuffix ? r->suffixW : L"",
        (int)InterlockedCompareExchange(&g_live, 0, 0), FILES_RULES_MAX);

    SyncTargets();
    return (ShFileRule *)r;
}

int ShFileRuleDel(ShFileRule *rule)
{
    struct ShFileRule *r = (struct ShFileRule *)rule;
    uint32_t mask = 0;
    int      i;

    if (!r) return 0;
    if ((uintptr_t)r < (uintptr_t)g_rule ||
        (uintptr_t)r >= (uintptr_t)(g_rule + FILES_RULES_MAX))
        return 0;
    if (((uintptr_t)r - (uintptr_t)g_rule) % sizeof(g_rule[0])) return 0;

    FileLock();
    if (!InterlockedExchange(&r->live, 0)) {
        FileUnlock();
        return 0;                               /* already out */
    }
    for (i = 0; i < FILES_RULES_MAX; i++)
        if (InterlockedCompareExchange(&g_rule[i].live, 0, 0))
            mask |= g_rule[i].group;
    InterlockedExchange(&g_mask, (LONG)mask);
    InterlockedDecrement(&g_live);
    FileUnlock();

    Log("files: rule %d (%s) is out after %ld hit(s) - %d left",
        r->seq, r->owner[0] ? r->owner : "?",
        (long)InterlockedCompareExchange(&r->hits, 0, 0),
        (int)InterlockedCompareExchange(&g_live, 0, 0));

    SyncTargets();
    return 1;
}

int ShFileMatchCount(void)
{
    return (int)InterlockedCompareExchange(&g_live, 0, 0);
}

int ShFileInstalled(void)
{
    return (int)InterlockedCompareExchange(&g_installed, 0, 0);
}

uint32_t ShFileCallCount(void)
{
    return (uint32_t)InterlockedCompareExchange(&g_calls, 0, 0);
}

void ShFileOwn(int on)
{
    t_own = on ? 1 : 0;
}

int ShFileStatus(char *buf, int n)
{
    int i, hooked = 0, live = (int)InterlockedCompareExchange(&g_live, 0, 0);
    int w;

    if (!buf || n <= 0) return 0;
    for (i = 0; i < (int)ARRAY_LEN(g_api); i++)
        if (g_api[i].hooked) hooked++;

    w = snprintf(buf, n, "%d rule(s), %d target(s) hooked, %u call(s) "
                 "looked at, %ld answered",
                 live, hooked,
                 (unsigned)InterlockedCompareExchange(&g_calls, 0, 0),
                 (long)InterlockedCompareExchange(&g_answered, 0, 0));
    if (w < 0 || w >= n) {
        buf[0] = 0;
        return 0;
    }
    return w;
}
