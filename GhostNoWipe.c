/* Keeping a Ghost Mode save when a full death ends the run.
 *
 * The symptom: in Ghost Mode a complete death - killed outright, or
 * bleeding out after going down - ends the run, the game returns to
 * the save list, and that slot is gone.
 *
 * What is done to a save, measured with GhostWipeProbe on 2026-09-11
 * against a region-locked build, and corrected across several tries:
 *
 *   1. The save is RENAMED, not deleted:
 *          MoveFileExW("...\1771\18.save", "...\1771\18.save.delete", 0xB)
 *      and then the new name is opened for writing. Both slots of a
 *      save pair are renamed, seconds apart.
 *
 *   2. That .delete file is NOT a leftover: it is the record the save
 *      list reads as "this slot was deleted". Leaving one behind - the
 *      first attempt wrote one on purpose, for the game to open - hides
 *      the slot exactly as the wipe would.
 *
 *   3. The other record is the save's own CONTENT. The game writes the
 *      run's end into N.save before it renames anything, and a save
 *      carrying that is not listed. Proved by substitution: a save from
 *      before the death is listed, the one written at the death is not.
 *      The file is a structured, encrypted record - see the GRW save
 *      toolkit: a 552 byte header, then an obfuscated seed, zero-key
 *      TEA, a block shuffle and an xor stream. Decrypting two saves and
 *      diffing them does NOT find the mark: the format is positional,
 *      one inserted byte shifts everything after it, and a pair taken
 *      across a death differed in 94% of its bytes. An earlier note here
 *      claimed the content was untouched across a death, on the strength
 *      of a diff of two saves that both turned out to be post-death
 *      writes; that was wrong.
 *
 *   4. That write lands anywhere from the same instant as the game over
 *      screen to ten seconds after it, and the two slots of a pair are
 *      written seconds apart. So the window has to be exact, and it has
 *      to cover the write rather than the rename: a window still shut
 *      when the first of a pair lands files the marked save away as the
 *      clean copy, and the repair then hands that same marked content
 *      back when the rename comes. Observed directly - of two deaths in
 *      one session, one came back right and one came back marked.
 *
 * The window is kept honest from the file hooks themselves: a call that
 * touches the save folder is judged on a fresh reading of the engine
 * state rather than a rate-limited one, because a sampler a tenth of a
 * second behind is a marked save. The watch thread stays as a second
 * chance, but nothing essential waits on it - a thread that stops used
 * to leave the window shut for good.
 *
 * The rename is not the same event: the game redoes it on every pass
 * over the save list, and a game that has just been restarted never
 * showed the screen, so that one is refused for a marked slot whatever
 * the window says.
 *
 * Which slots are protected
 *
 * Only the ones this plugin has seen a Ghost Mode death on. The game
 * over screen is the one thing a Ghost Mode death produces and no other
 * mode reaches, so a wipe caught while that screen is recent is the real
 * thing - and the slot number goes into ghost_slots.ini at that moment,
 * because by the next launch the window is gone and the game is redoing
 * the rename with nothing left to tell it by. A slot that was never
 * marked, and an ordinary save deleted by hand from the list, are left
 * alone: the game behaves there exactly as it would without the plugin.
 *
 * So three things are held at once, for those slots:
 *
 *   - Two generations of clean copy. A save written while the run is
 *     alive rotates last\X.save into last\X.save.prev and writes the new
 *     one into last\X.save. Nothing is filed away while the window is
 *     open, so the newest copy is always a save the player made while
 *     alive - which is what a repair goes back to. The generation before
 *     it is the fallback.
 *
 *   - Writes during the death are kept out of the file. Once the game
 *     over screen has been seen, a write that opens the save itself is
 *     diverted to temp\ beside the plugin.
 *
 *   - The save's content is held at the last save the player made. The
 *     promotion of X.save.tmp over X.save is where the marked content
 *     would land - measured, ten seconds after the game over screen and
 *     nineteen before the rename - so it is refused and the save written
 *     back from a clean copy. Refusing it without that would leave no
 *     save at all, which the game reports as the slot not existing.
 *
 *   - The wipe is dropped and the save repaired. The rename of N.save to
 *     N.save.delete is refused and the save put back from a clean copy.
 *     The write to the .delete goes to temp\ as well, so no tombstone
 *     lands in the save folder.
 *
 * Both working areas live in the plugin's own folder - last\ for the
 * copies, temp\ for the diverted writes. Nothing goes to %TEMP% or
 * anywhere else outside plugins\GhostNoWipe\. temp\ is emptied at start
 * up so a long session's litter does not pile up.
 *
 * The slot is still shown as deleted for the rest of the session - that
 * state lives in the process, and the probe records no read of the save
 * folder at all after the return to the menu - and comes back on the
 * next launch: the folder is scanned, and the save is there with no
 * .delete beside it. Verified: a death, a restart, the slot is listed
 * again and plays from the last save point.
 *
 * The game's own dialog says otherwise either way, so a status line goes
 * up at the same moment saying what really happened, and comes down once
 * the player is in a game again.
 *
 * 1.save and 2.save are the global profile and need no part of this.
 * They are rewritten for ordinary reasons too - changing a setting
 * writes them - and an attempt to read their rewrites as the wipe's
 * record was a dead end.
 *
 * A wrong mark is undone from the menu ("Forget the ghost slots"), or by
 * editing ghost_slots.ini, or by deleting it. enabled=0 leaves the game
 * exactly as it would be without the plugin.
 *
 * Boundaries: single player only. This changes no difficulty, no death
 * rule and no save content of its own - it declines to move one file and
 * restores another. If cloud sync is turned on later, the kept local
 * save and whatever the cloud holds may disagree; that is for the player
 * to settle. Delete the plugin folder once the game is patched.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

#include "third_party/minhook/include/MinHook.h"

/* From scripthook.h's ShGameState: UNKNOWN, MENU, LOADING, LOBBY,
 * INGAME, RELOADING, PAUSED, GAMEOVER. Kept as a number because a
 * plugin binds the framework by name, not by link. */
#define SH_STATE_INGAME   4
#define SH_STATE_GAMEOVER 7

/* How long after the game over screen writes are still held back. The
 * rewrite of the save was measured one millisecond after the screen
 * appeared, so the poll has to be quick - and the window has to be long,
 * because the rename comes twenty seconds later. */
#define DEATH_WINDOW_MS (300u * 1000u)
#define WATCH_MS 50

typedef int (*GetState_t)(void);
typedef uint32_t (*ToastEx_t)(const char *text, uint32_t rgb, uint32_t ms);
typedef int      (*ToastSet_t)(uint32_t id, const char *text, uint32_t rgb,
                               uint32_t ms);
typedef int      (*ToastHide_t)(uint32_t id);
typedef const char *(*LangForOwned_t)(const char *owner, const char *scope,
                                      const char *text);

static GetState_t     g_getState;
static ToastEx_t      g_toastEx;
static ToastSet_t     g_toastSet;
static ToastHide_t    g_toastHide;
static LangForOwned_t g_langForOwned;

/* Kept up while the slot is listed as gone, taken down once the player
 * is back in a game. The hooks raise it; the state sampling takes it
 * down. */
static uint32_t g_noticeId;

/* This plugin's folder name, which is how the framework finds the
 * translation table kept in its own ini. */
static char g_owner[64];

static volatile LONG   g_enabled = 1;
static volatile LONG64 g_lastGameOver;

static int Enabled(void) {
    return InterlockedCompareExchange(&g_enabled, 0, 0) ? 1 : 0;
}

/* ---- logging ---------------------------------------------------------- */

static FILE *g_log;
static LONG  g_logBusy;

static void GuardLog(const char *fmt, ...) {
    va_list ap;
    char line[1024];
    SYSTEMTIME st;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (!g_log) return;

    while (InterlockedExchange(&g_logBusy, 1)) Sleep(1);
    if (g_log) {
        GetLocalTime(&st);
        fprintf(g_log, "%02u:%02u:%02u.%03u [%lu] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                (unsigned long)GetCurrentProcessId(), line);
        fflush(g_log);
    }
    InterlockedExchange(&g_logBusy, 0);
}

static FILE *OpenLogAt(const char *dir, const char *name) {
    char path[MAX_PATH];
    int len = (int)strlen(dir);

    if (len + 32 >= (int)sizeof(path)) return NULL;
    lstrcpynA(path, dir, sizeof(path));
    strcpy(path + len, "logs");
    CreateDirectoryA(path, NULL);
    strcat(path, "\\");
    strcat(path, name);
    return fopen(path, "a");
}

/* ---- the folders this plugin works with -------------------------------- */

static HINSTANCE g_inst = NULL;
static char      g_dirA[MAX_PATH];        /* the game's working directory */
static wchar_t   g_backupDir[MAX_PATH];   /* <plugin>\last - clean copies */
static wchar_t   g_tempDir[MAX_PATH];     /* <plugin>\temp - diverted writes */
static char      g_slotsPath[MAX_PATH];   /* <plugin>\ghost_slots.ini */

/* Empty a folder of the files an earlier run left there. Failures are
 * logged and otherwise ignored: a stale divert file is harmless. */
static void ClearFolder(const wchar_t *dir) {
    wchar_t pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (!dir[0] || wcslen(dir) + 4 >= MAX_PATH) return;
    wcscpy(pat, dir);
    wcscat(pat, L"\\*");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t victim[MAX_PATH];
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (wcslen(dir) + wcslen(fd.cFileName) + 2 >= MAX_PATH) continue;
        wcscpy(victim, dir);
        wcscat(victim, L"\\");
        wcscat(victim, fd.cFileName);
        DeleteFileW(victim);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* The plugin keeps two working areas in its own folder:
 *
 *   last\   the clean copies to go back to
 *   temp\   the writes that must not land in the save folder
 *
 * The diverted writes used to go to %TEMP%\GhostNoWipe\ instead. They do
 * not any more: a plugin has no business leaving anything outside its
 * own folder, and this way the whole lot goes when the folder does. */
static void ResolveFolders(void) {
    char mod[MAX_PATH];
    char base[MAX_PATH];
    char *slash;
    int len;

    g_dirA[0] = 0;
    g_backupDir[0] = 0;
    g_tempDir[0] = 0;
    g_slotsPath[0] = 0;

    len = GetModuleFileNameA(NULL, mod, sizeof(mod));
    if (!len || len >= (int)sizeof(mod)) return;
    slash = strrchr(mod, '\\');
    if (!slash) return;
    slash[1] = 0;
    lstrcpynA(g_dirA, mod, sizeof(g_dirA));

    if (!g_inst) return;
    if (!GetModuleFileNameA(g_inst, base, sizeof(base))) return;
    slash = strrchr(base, '\\');
    if (!slash) return;
    *slash = 0;                       /* <game>\plugins\GhostNoWipe */
    /* the folder name stands in for the plugin wherever the framework
     * needs to know whose text this is */
    slash = strrchr(base, '\\');
    lstrcpynA(g_owner, slash ? slash + 1 : base, sizeof(g_owner));
    len = (int)strlen(base);
    if (len + 20 >= (int)sizeof(base)) return;

    strcat(base, "\\last");
    CreateDirectoryA(base, NULL);
    MultiByteToWideChar(CP_ACP, 0, base, -1, g_backupDir, MAX_PATH);
    base[len] = 0;

    strcat(base, "\\temp");
    CreateDirectoryA(base, NULL);
    MultiByteToWideChar(CP_ACP, 0, base, -1, g_tempDir, MAX_PATH);
    base[len] = 0;
    ClearFolder(g_tempDir);

    strcat(base, "\\ghost_slots.ini");
    lstrcpynA(g_slotsPath, base, sizeof(g_slotsPath));
    base[len] = 0;
}

/* ---- recognising the files --------------------------------------------- */

static int EndsWithW(const wchar_t *s, const wchar_t *tail) {
    size_t n, m;

    if (!s || !tail) return 0;
    n = wcslen(s);
    m = wcslen(tail);
    if (m > n) return 0;
    return _wcsicmp(s + n - m, tail) == 0;
}

/* The save folder and nothing else: "savegames" appears in plenty of
 * Ubisoft paths, the game id has to follow it. */
static int IsSaveFolderPath(const wchar_t *p) {
    const wchar_t *s;

    if (!p) return 0;
    s = wcsstr(p, L"savegames");
    return s && wcsstr(s, L"1771");
}

/* "...\1771\N.save" and nothing else: not .tmp, not .old, not .delete. */
static int IsPlainSaveName(const wchar_t *p) {
    return IsSaveFolderPath(p) && EndsWithW(p, L".save");
}

/* "...\1771\N.save.delete" - the tombstone. */
static int IsDeleteMarkedSave(const wchar_t *p) {
    return IsSaveFolderPath(p) && EndsWithW(p, L".delete");
}

/* "...\N.save" -> "...\N.save.delete": the wipe. */
static int IsDeathRename(const wchar_t *from, const wchar_t *to) {
    size_t n;

    if (!IsSaveFolderPath(from) || !IsSaveFolderPath(to)) return 0;
    if (!EndsWithW(from, L".save")) return 0;
    if (!EndsWithW(to, L".delete")) return 0;

    n = wcslen(from);
    return wcslen(to) == n + 7 && _wcsnicmp(from, to, n) == 0;
}

/* "...\N.save.tmp" -> "...\N.save": the last step of an ordinary save. */
static int IsPromotion(const wchar_t *from, const wchar_t *to) {
    size_t n;

    if (!IsSaveFolderPath(from) || !IsSaveFolderPath(to)) return 0;
    if (!EndsWithW(from, L".save.tmp") || !IsPlainSaveName(to)) return 0;

    n = wcslen(to);
    return wcslen(from) == n + 4 && _wcsnicmp(from, to, n) == 0;
}

static int InDeathWindow(void) {
    LONG64 t = InterlockedCompareExchange64(&g_lastGameOver, 0, 0);

    if (!t) return 0;
    return (GetTickCount64() - (uint64_t)t) < DEATH_WINDOW_MS;
}

/* ---- sampling the engine state ---------------------------------------- */

/* Done on the file hooks, which run on the game's own thread many times
 * a frame. The watch thread is kept as a second chance, but nothing
 * essential waits on it - and that matters, because a watch thread that
 * stops leaves the death window shut for good. A window that never opens
 * is a marked write taken for an ordinary save: it gets filed away as
 * the clean copy, and handed straight back by the repair when the rename
 * finally comes. That is how a slot comes back marked after a restart
 * even though nothing of the wipe was left in the save folder.
 *
 * Transitions are logged once each. The sampling is rate limited, so a
 * frame's worth of file calls costs one clock read. */
static volatile LONG   g_prevState = -1;
static volatile LONG   g_hideWanted;
static volatile LONG64 g_lastSample;

static void SampleStateNow(void) {
    LONG64 now;
    int st;

    if (!g_getState) return;
    now = (LONG64)GetTickCount64();
    InterlockedExchange64(&g_lastSample, now);

    st = g_getState();
    if (st != (int)InterlockedCompareExchange(&g_prevState, 0, 0)) {
        InterlockedExchange(&g_prevState, st);
        GuardLog("state -> %d", st);
    }

    if (st == SH_STATE_GAMEOVER)
        InterlockedExchange64(&g_lastGameOver, now);
    else if (st == SH_STATE_INGAME)
        InterlockedExchange(&g_hideWanted, 1);
}

static void SampleState(void) {
    LONG64 now;

    if (!g_getState) return;
    now = (LONG64)GetTickCount64();
    if (now - InterlockedCompareExchange64(&g_lastSample, 0, 0) < 100)
        return;
    SampleStateNow();
}

/* The state, sampled for a call that is about to be judged. The rate
 * limit above is there so a frame's worth of file traffic costs one
 * clock read - but a call inside the save folder is rare, and it is the
 * one call whose answer changes the outcome.
 *
 * It has to be exact. Measured twice in one session: the marked write
 * lands anywhere from the same instant as the game over screen to ten
 * seconds after it, and the two slots of a pair are written seconds
 * apart. A window that is still shut when the first of them lands is a
 * marked save filed away as the clean copy - and then handed straight
 * back by the repair when the rename comes, which is why the slot came
 * back marked on some deaths and not others. */
static void SampleFor(const wchar_t *a, const wchar_t *b) {
    if (IsSaveFolderPath(a) || (b && IsSaveFolderPath(b)))
        SampleStateNow();
    else
        SampleState();
}

static void ShowKeptNotice(void);
static void HideKeptNotice(void);

/* Defined further down, where the slot marks are kept. */
static int  SlotOfName(const wchar_t *path, int *out);
static int  IsGhostSlot(int slot);
static void MarkGhostSlot(int slot);

/* Whether a slot is one this plugin may touch at all.
 *
 * A Ghost Mode death is the only thing that raises the game over screen,
 * so a wipe caught while that screen is recent is the real thing - and
 * that is how a slot is recognised in the first place. Once recognised it
 * is written down, because by the next launch the window is gone and the
 * game is redoing the rename with nothing left to tell it by.
 *
 * Everything else is left alone: an ordinary save deleted by hand, a slot
 * belonging to another mode, all of it. */
static int ShouldTouchSlot(int slot) {
    return IsGhostSlot(slot) || InDeathWindow();
}

/* ---- the clean copy ---------------------------------------------------- */

typedef BOOL   (WINAPI *MoveFileExW_t)(LPCWSTR from, LPCWSTR to, DWORD flags);
typedef BOOL   (WINAPI *MoveFileW_t)(LPCWSTR from, LPCWSTR to);
typedef BOOL   (WINAPI *CopyFileW_t)(LPCWSTR from, LPCWSTR to,
                                     BOOL failIfExists);
typedef HANDLE (WINAPI *CreateFileW_t)(LPCWSTR name, DWORD access,
                                       DWORD share,
                                       LPSECURITY_ATTRIBUTES sa,
                                       DWORD disposition, DWORD flags,
                                       HANDLE tmpl);

static MoveFileExW_t g_realMoveFileExW;
static MoveFileW_t   g_realMoveFileW;
static CopyFileW_t   g_realCopyFileW;
static CreateFileW_t g_realCreateFileW;

static int BackupPath(const wchar_t *save, wchar_t *out, size_t cap) {
    const wchar_t *base;

    out[0] = 0;
    if (!g_backupDir[0] || !save) return 0;
    base = wcsrchr(save, L'\\');
    if (!base) base = wcsrchr(save, L'/');
    base = base ? base + 1 : save;
    if (wcslen(g_backupDir) + wcslen(base) + 16 >= cap) return 0;

    wcscpy(out, g_backupDir);
    wcscat(out, L"\\");
    wcscat(out, base);
    return 1;
}

/* The copy to go back to, and the one before it.
 *
 * The death is not announced when it is written. Measured: the game
 * writes the save that carries the mark 9 seconds BEFORE the game over
 * screen appears, so the death window is still shut and an earlier
 * attempt filed that marked content away as the clean copy - which is
 * exactly what got put back, so the slot stayed gone.
 *
 * The wipe writes once. So the copy before the newest one is the last
 * ordinary save the player made while alive, and that is the one to go
 * back to. Two copies, rolled on every ordinary save. */
static int BackupPrevPath(const wchar_t *save, wchar_t *out, size_t cap) {
    if (!BackupPath(save, out, cap)) return 0;
    if (wcslen(out) + 6 >= cap) return 0;
    wcscat(out, L".prev");
    return 1;
}

/* After every ordinary save, and on the first read of a save that has
 * none yet: the content to go back to. */
/* 1.save and 2.save are the global profile: where the game records which
 * slots exist. Everything else here is about N.save, and none of it is
 * enough on its own - measured, the wipe leaves N.save alone entirely
 * and rewrites the global profile instead, on every pass over the list,
 * including the one after a restart. Keeping N.save while that goes
 * through changes nothing: the list reads the profile.
 *
 * So the profile is put back too, at the same moment the rename is
 * dropped, from a copy taken while the slot was alive. The copy is NOT
 * refreshed from a promote that happens after a death: those carries
 * the mark, and the whole point of the copy is to predate it. */
static int IsGlobalProfileName(const wchar_t *p) {
    if (!IsSaveFolderPath(p)) return 0;
    return EndsWithW(p, L"\\1.save") || EndsWithW(p, L"/1.save") ||
           EndsWithW(p, L"\\2.save") || EndsWithW(p, L"/2.save");
}

static void RestoreSave(const wchar_t *save);

/* Roll the copies: what is there becomes the one before it, and the new
 * content becomes the current one. Two deep, because the wipe writes
 * once - so one step back is always a save made while the player was
 * alive. */
static void BackupSave(const wchar_t *save) {
    wchar_t dst[MAX_PATH];
    wchar_t prev[MAX_PATH];

    if (!g_realCopyFileW || !BackupPath(save, dst, MAX_PATH)) return;
    if (BackupPrevPath(save, prev, MAX_PATH) &&
        GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES)
        g_realCopyFileW(dst, prev, FALSE);

    if (g_realCopyFileW(save, dst, FALSE)) {
        GuardLog("copy  %ls", save);
        GuardLog("      kept; the one before it is what a repair goes back "
                 "to");
    } else {
        GuardLog("copy of %ls failed (err=%lu)", save, GetLastError());
    }
}

static void BackupSaveIfMissing(const wchar_t *save) {
    wchar_t dst[MAX_PATH];

    if (!BackupPath(save, dst, MAX_PATH)) return;
    if (GetFileAttributesW(dst) == INVALID_FILE_ATTRIBUTES) BackupSave(save);
}

/* Write the save back from the clean copy, and say which copy was used.
 *
 * The newest copy is the one to use, now that nothing is filed away
 * while the run is ending - a copy is only ever taken from a save the
 * player made while alive, so the newest is the closest one to the
 * death. The copy before it is the fallback for the first death after
 * an upgrade, when the newest may still be a marked write.
 *
 * Returns 0 if there is no copy at all. The caller must then let the
 * real call through: a save that cannot be repaired is better replaced
 * by the game's own write than left missing. */
static int RestoreSaveAs(const wchar_t *save) {
    wchar_t cur[MAX_PATH];
    wchar_t prev[MAX_PATH];
    const wchar_t *src;

    if (!g_realCopyFileW || !BackupPath(save, cur, MAX_PATH)) return 0;

    src = NULL;
    if (GetFileAttributesW(cur) != INVALID_FILE_ATTRIBUTES)
        src = cur;
    else if (BackupPrevPath(save, prev, MAX_PATH) &&
             GetFileAttributesW(prev) != INVALID_FILE_ATTRIBUTES)
        src = prev;

    if (!src) {
        GuardLog("no copy of %ls to put back - it keeps whatever the game "
                 "left in it", save);
        return 0;
    }

    if (!g_realCopyFileW(src, save, FALSE)) {
        GuardLog("putting %ls back from %ls failed (err=%lu)", save, src,
                 GetLastError());
        return 0;
    }

    GuardLog("put back %ls from %ls", save,
             _wcsicmp(src, cur) == 0 ? L"the newest copy"
                                     : L"the copy before the newest one");
    return 1;
}

/* Same, for a caller that just wants the save repaired and does not act
 * on whether it worked. */
static void RestoreSave(const wchar_t *save) {
    RestoreSaveAs(save);
}

/* ---- keeping the wipe out of the save folder --------------------------- */

/* A write to a path this plugin must not let land in the save folder is
 * sent to the plugin's own temp\ folder instead, filled from `source` if
 * it is not there yet, so the game opens something and reads back what
 * it expects. `source` is the save itself for a write to the save, and
 * the save the tombstone was renamed from for a write to a .delete.
 *
 * A failure here returns 0 and the caller falls through to the real
 * API: no folder to divert into means the write goes where it would
 * have gone without the plugin, which is the safe way to be wrong. */
static int RedirectToTemp(const wchar_t *name, const wchar_t *source,
                          wchar_t *out, size_t cap) {
    const wchar_t *base;

    if (!name || !source) return 0;
    if (!g_tempDir[0]) return 0;
    base = wcsrchr(name, L'\\');
    if (!base) base = wcsrchr(name, L'/');
    base = base ? base + 1 : name;

    if (wcslen(g_tempDir) + wcslen(base) + 4 >= cap) return 0;

    wcscpy(out, g_tempDir);
    wcscat(out, L"\\");
    wcscat(out, base);

    if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES && g_realCopyFileW)
        g_realCopyFileW(source, out, FALSE);

    return 1;
}

static int StripDeleteSuffix(const wchar_t *name, wchar_t *out, size_t cap) {
    size_t len = wcslen(name);

    if (len <= 7 || len >= cap) return 0;
    wcsncpy(out, name, len - 7);
    out[len - 7] = 0;
    return 1;
}

/* ---- the hooks --------------------------------------------------------- */

static BOOL WINAPI HookMoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
    if (Enabled()) {
        SampleFor(from, to);
        /* The wipe, at whatever moment it comes - the game redoes it on
         * every pass over the save list. Dropped, and the save repaired
         * in case the mark was written before the screen was noticed. */
        if (IsDeathRename(from, to)) {
            int slot = 0;
            if (SlotOfName(from, &slot) && ShouldTouchSlot(slot)) {
                GuardLog("keep  %ls", from);
                GuardLog("      the rename is dropped and nothing takes its "
                         "place; the .delete write goes to the temp folder");
                MarkGhostSlot(slot);
                RestoreSave(from);
                ShowKeptNotice();
                return TRUE;
            }
            GuardLog("pass  %ls", from);
            GuardLog("      a slot this plugin is not watching; the rename "
                     "goes through");
        }
        /* The last step of a save: the .tmp takes the save's place.
         *
         * While the run is ending this is where the mark would land, and
         * waiting for the rename to drop it is far too late. Measured:
         * the game writes the marked save ten seconds after the game over
         * screen and nineteen seconds before it renames the file - by
         * then it has read its own write, and the next launch reads it
         * too. Dropping the rename alone is what left a restart showing
         * the slot as gone.
         *
         * So the promotion is dropped and the save put back from the
         * clean copy. It keeps its name and the content it had while the
         * player was alive, which is the one state the game reads as a
         * slot that is still there. With no copy to go back to, the real
         * call goes through: a save the game overwrote beats a missing
         * one. */
        if (IsPromotion(from, to)) {
            int slot = 0;

            if (InDeathWindow() && SlotOfName(to, &slot) &&
                ShouldTouchSlot(slot) && RestoreSaveAs(to)) {
                GuardLog("hold  %ls", to);
                GuardLog("      the run has ended; it keeps the content it "
                         "had before it, from the clean copy");
                ShowKeptNotice();
                return TRUE;
            }
            BOOL ok = g_realMoveFileExW(from, to, flags);
            if (ok) {
                if (InDeathWindow())
                    GuardLog("note  %ls was written as the run ended - the "
                             "copy to go back to is left as it was", to);
                else if (IsGlobalProfileName(to))
                    GuardLog("note  %ls was rewritten - the profile copy "
                             "keeps the version from before the run ended",
                             to);
                else
                    BackupSave(to);
            }
            return ok;
        }
    }
    return g_realMoveFileExW(from, to, flags);
}

static BOOL WINAPI HookMoveFileW(LPCWSTR from, LPCWSTR to) {
    if (Enabled()) {
        SampleFor(from, to);
        if (IsDeathRename(from, to)) {
            int slot = 0;
            if (SlotOfName(from, &slot) && ShouldTouchSlot(slot)) {
                GuardLog("keep  %ls", from);
                GuardLog("      the rename is dropped and nothing takes its "
                         "place");
                MarkGhostSlot(slot);
                RestoreSave(from);
                ShowKeptNotice();
                return TRUE;
            }
            GuardLog("pass  %ls", from);
            GuardLog("      a slot this plugin is not watching; the rename "
                     "goes through");
        }
        if (IsPromotion(from, to)) {
            int slot = 0;

            if (InDeathWindow() && SlotOfName(to, &slot) &&
                ShouldTouchSlot(slot) && RestoreSaveAs(to)) {
                GuardLog("hold  %ls", to);
                GuardLog("      the run has ended; it keeps the content it "
                         "had before it, from the clean copy");
                ShowKeptNotice();
                return TRUE;
            }
            BOOL ok = g_realMoveFileW(from, to);
            if (ok) {
                if (InDeathWindow())
                    GuardLog("note  %ls was written as the run ended - the "
                             "copy to go back to is left as it was", to);
                else if (IsGlobalProfileName(to))
                    GuardLog("note  %ls was rewritten - the profile copy "
                             "keeps the version from before the run ended",
                             to);
                else
                    BackupSave(to);
            }
            return ok;
        }
    }
    return g_realMoveFileW(from, to);
}

static HANDLE WINAPI HookCreateFileW(LPCWSTR name, DWORD access, DWORD share,
                                     LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                     DWORD flags, HANDLE tmpl) {
    wchar_t target[MAX_PATH];

    if (Enabled()) {
        /* Every file call is a chance to look at the engine state, and
         * there are a great many of them: this is what keeps the death
         * window honest, with the watch thread only as a second chance.
         * A call inside the save folder is judged without the rate
         * limit - see SampleFor. The notice is taken down from here too,
         * on the game's own thread, which is the one that may touch the
         * HUD. */
        SampleFor(name, NULL);
        if (InterlockedExchange(&g_hideWanted, 0)) HideKeptNotice();

        /* A write to the tombstone: out of the save folder, for a slot
         * this plugin is watching. */
        if (IsDeleteMarkedSave(name)) {
            wchar_t source[MAX_PATH];
            int slot = 0;
            if (StripDeleteSuffix(name, source, MAX_PATH) &&
                SlotOfName(name, &slot) && ShouldTouchSlot(slot) &&
                RedirectToTemp(name, source, target, MAX_PATH)) {
                GuardLog("write %ls", name);
                GuardLog("      goes to %ls - no tombstone lands in the save "
                         "folder", target);
                return g_realCreateFileW(target, access, share, sa, disp,
                                         flags, tmpl);
            }
        } else if (IsPlainSaveName(name)) {
            /* A write into the save itself while the run is ending is
             * kept out of the file. Any right that can write counts, not
             * just GENERIC_WRITE: the game asks for FILE_WRITE_DATA
             * rather than the generic one, and checking only the generic
             * right is how one attempt let the mark through.
             *
             * The global profile (1.save / 2.save) is not a slot and gets
             * none of this: it is rewritten for ordinary reasons as well -
             * changing a setting writes it - so an attempt to read its
             * rewrites as the wipe's record was a dead end. */
            int slot = 0;
            if ((access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA |
                           FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES)) &&
                SlotOfName(name, &slot) && ShouldTouchSlot(slot) &&
                RedirectToTemp(name, name, target, MAX_PATH)) {
                GuardLog("write %ls", name);
                GuardLog("      the run has ended; it goes to %ls instead",
                         target);
                return g_realCreateFileW(target, access, share, sa, disp,
                                         flags, tmpl);
            }
            /* Reading it is how the game loads a slot: a good moment to
             * take a copy if there is none yet. */
            if (!IsGlobalProfileName(name))
                BackupSaveIfMissing(name);
        }
    }
    return g_realCreateFileW(name, access, share, sa, disp, flags, tmpl);
}

/* ---- watching for the death screen ------------------------------------ */

/* The rewrite of the save lands within a millisecond of the game over
 * screen, so the poll is deliberately quick. Transitions are logged once
 * each, not polled into the log. */
/* ---- the line that says what really happened --------------------------- */

/* The game's dialog says the save was deleted, and the slot is missing
 * from the list as well, so on its own that dialog is the last word. A
 * line goes up at the same moment saying otherwise: the save is still
 * there, play resumes from the last checkpoint, and a restart brings the
 * slot back. It stays until the player is in a game again.
 *
 * The text goes through the same table as the menu, so a translation
 * shipped in this plugin's ini applies here too. */
static const char *NOTICE_TEXT =
    "The save was kept - the run is over, and play resumes from the last "
    "checkpoint. Restart the game to carry on.";

#define NOTICE_RGB 0xFFCC33u

static void ShowKeptNotice(void) {
    const char *text = NOTICE_TEXT;

    if (!g_toastEx) return;
    if (g_langForOwned)
        text = g_langForOwned(g_owner[0] ? g_owner : NULL,
                              "Ghost save guard", NOTICE_TEXT);

    /* ms 0 keeps it up until it is dismissed: this is not a message that
     * should fade while the player is still reading the save list. */
    if (g_noticeId && g_toastSet &&
        g_toastSet(g_noticeId, text, NOTICE_RGB, 0))
        return;
    g_noticeId = g_toastEx(text, NOTICE_RGB, 0);
}

static void HideKeptNotice(void) {
    if (!g_noticeId || !g_toastHide) return;
    g_toastHide(g_noticeId);
    g_noticeId = 0;
}

/* The second chance at the state, for the moments the game is not
 * opening files - which is most of the menu. It only samples; anything
 * that touches the HUD is left to the game's own thread. */
static DWORD WINAPI WatchThread(LPVOID p) {
    (void)p;
    GuardLog("watch: thread up");
    for (;;) {
        if (Enabled()) SampleState();
        Sleep(WATCH_MS);
    }
    return 0;
}

/* ---- ghost slots ------------------------------------------------------- */

/* Which slots this plugin has seen a Ghost Mode death on.
 *
 * The game over screen is the one thing a Ghost Mode death produces and
 * no other mode ever reaches, so a wipe caught while that screen is
 * recent is the real thing. The window closes when the game restarts,
 * though, and the rename is redone on every pass over the list, so what
 * was learned has to be written down or it goes with the process.
 *
 * Kept as a plain list of slot numbers under [slots] in the plugin's
 * own folder. Editing a line out, deleting the file, or the menu item
 * below all undo a wrong mark. */

#define SLOT_MAX 64

static unsigned char g_ghost[SLOT_MAX];

/* "...\1771\17.save", "...\17.save.delete" and "...\17.save.tmp" all
 * give 17. The global profile (1.save / 2.save) is not a slot. */
static int SlotOfName(const wchar_t *path, int *out) {
    const wchar_t *base, *dot, *slash;
    wchar_t name[24];
    wchar_t *end = NULL;
    size_t n;
    long v;

    if (!IsSaveFolderPath(path)) return 0;
    slash = wcsrchr(path, L'\\');
    if (!slash) slash = wcsrchr(path, L'/');
    base = slash ? slash + 1 : path;

    dot = wcschr(base, L'.');
    if (!dot) return 0;
    n = (size_t)(dot - base);
    if (!n || n >= sizeof(name) / sizeof(name[0])) return 0;
    wcsncpy(name, base, n);
    name[n] = 0;

    v = wcstol(name, &end, 10);
    if (!end || *end || v <= 1 || v >= SLOT_MAX) return 0;
    if (v == 2) return 0;                 /* the global profile */
    *out = (int)v;
    return 1;
}

static void LoadGhostSlots(void) {
    int i;

    memset(g_ghost, 0, sizeof(g_ghost));
    if (!g_slotsPath[0]) return;
    for (i = 3; i < SLOT_MAX; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%d", i);
        if (GetPrivateProfileIntA("slots", key, 0, g_slotsPath))
            g_ghost[i] = 1;
    }
}

static int IsGhostSlot(int slot) {
    if (slot <= 2 || slot >= SLOT_MAX) return 0;
    return g_ghost[slot] != 0;
}

static void MarkGhostSlot(int slot) {
    char key[16];

    if (slot <= 2 || slot >= SLOT_MAX || g_ghost[slot]) return;
    g_ghost[slot] = 1;
    if (!g_slotsPath[0]) return;
    snprintf(key, sizeof(key), "%d", slot);
    WritePrivateProfileStringA("slots", key, "1", g_slotsPath);
    GuardLog("slot %d is a Ghost Mode slot from now on", slot);
}

static void ForgetGhostSlots(void) {
    memset(g_ghost, 0, sizeof(g_ghost));
    if (g_slotsPath[0])
        WritePrivateProfileStringA("slots", NULL, NULL, g_slotsPath);
    GuardLog("menu: ghost slot marks cleared");
}

/* ---- plugin ini -------------------------------------------------------- */

static char g_iniPath[MAX_PATH];

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

static void LoadConfig(void) {
    int on = 1;

    if (g_iniPath[0])
        on = GetPrivateProfileIntA("Settings", "enabled", 1, g_iniPath);
    InterlockedExchange(&g_enabled, on ? 1 : 0);
}

static void SaveIni(void) {
    char buf[8];

    if (!g_iniPath[0]) return;
    snprintf(buf, sizeof(buf), "%d", Enabled());
    WritePrivateProfileStringA("Settings", "enabled", buf, g_iniPath);
}

/* ---- menu -------------------------------------------------------------- */

typedef void (*MenuFn)(uint32_t menu, uint32_t item, int value, void *user);

static void OnEnable(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu; (void)item; (void)user;
    InterlockedExchange(&g_enabled, value ? 1 : 0);
    GuardLog("menu: guard=%s", Enabled() ? "on" : "off");
    SaveIni();
}

static void OnForgetSlots(uint32_t menu, uint32_t item, int value,
                          void *user) {
    (void)menu; (void)item; (void)value; (void)user;
    ForgetGhostSlots();
}

static void BuildMenu(HMODULE m) {
    uint32_t (*menuCreate)(const char *) = NULL;
    int (*menuToggle)(uint32_t, const char *, int, MenuFn, void *) = NULL;
    int (*menuAction)(uint32_t, const char *, MenuFn, void *) = NULL;
    int (*menuHint)(uint32_t, const char *) = NULL;

    *(FARPROC *)&menuCreate = GetProcAddress(m, "ShMenuCreate");
    *(FARPROC *)&menuToggle = GetProcAddress(m, "ShMenuToggle");
    *(FARPROC *)&menuAction = GetProcAddress(m, "ShMenuAction");
    *(FARPROC *)&menuHint   = GetProcAddress(m, "ShMenuHint");
    if (!menuCreate || !menuToggle) return;

    {
        uint32_t menu = menuCreate("Ghost save guard");
        menuToggle(menu, "Keep the save when the run ends", Enabled(),
                   OnEnable, NULL);
        if (menuAction)
            menuAction(menu, "Forget the ghost slots", OnForgetSlots, NULL);
        if (menuHint)
            menuHint(menu,
                     "Stops a Ghost Mode save being wiped when the run ends. "
                     "Only slots this plugin has seen a Ghost Mode death on "
                     "are protected; every other slot is left alone.");
    }
    GuardLog("menu created");
}

/* ---- startup ----------------------------------------------------------- */

static void InstallHooks(void) {
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    MH_STATUS st;

    st = MH_Initialize();
    if (st != MH_OK) {
        GuardLog("install: MH_Initialize failed (%d) - the wipe goes ahead "
                 "as usual", (int)st);
        return;
    }

    MH_CreateHookApi(L"kernel32.dll", "MoveFileExW",
                     (LPVOID)HookMoveFileExW, (LPVOID *)&g_realMoveFileExW);
    MH_CreateHookApi(L"kernel32.dll", "MoveFileW",
                     (LPVOID)HookMoveFileW, (LPVOID *)&g_realMoveFileW);
    MH_CreateHookApi(L"kernel32.dll", "CreateFileW",
                     (LPVOID)HookCreateFileW, (LPVOID *)&g_realCreateFileW);

    /* CopyFileW is called, not hooked: the pointer is all that is needed,
     * and an untouched entry point is one less thing in the way of a save
     * being written. */
    if (k32)
        *(FARPROC *)&g_realCopyFileW = GetProcAddress(k32, "CopyFileW");
    if (!g_realCopyFileW)
        GuardLog("install: CopyFileW not found - nothing can be copied");

    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK) {
        GuardLog("install: MH_EnableHook failed (%d)", (int)st);
        return;
    }
    GuardLog("install: MoveFileExW=%p MoveFileW=%p CreateFileW=%p "
             "CopyFileW=%p",
             (void *)g_realMoveFileExW, (void *)g_realMoveFileW,
             (void *)g_realCreateFileW, (void *)g_realCopyFileW);
}

static DWORD WINAPI InitThread(LPVOID p) {
    (void)p;
    ResolveFolders();
    if (g_dirA[0]) g_log = OpenLogAt(g_dirA, "GhostNoWipe.log");
    ResolveIniPath();
    LoadConfig();
    LoadGhostSlots();

    GuardLog("--- GhostNoWipe: keeping the save when a run ends ---");
    GuardLog("build " __DATE__ " " __TIME__);
    GuardLog("config: guard=%s, ini=%s", Enabled() ? "on" : "off",
             g_iniPath[0] ? g_iniPath : "(none)");
    GuardLog("clean copies: %ls", g_backupDir[0] ? g_backupDir : L"(nowhere)");
    GuardLog("diverted writes: %ls",
             g_tempDir[0] ? g_tempDir : L"(nowhere)");
    GuardLog("ghost slots: %s", g_slotsPath[0] ? g_slotsPath : "(nowhere)");

    {
        HMODULE di = GetModuleHandleA("dinput8.dll");
        if (di) {
            *(FARPROC *)&g_getState = GetProcAddress(di, "ShGetGameState");
            /* Optional: an older dinput8 simply has no status line, and
             * the log says so rather than the plugin failing. */
            *(FARPROC *)&g_toastEx = GetProcAddress(di, "ShToastEx");
            *(FARPROC *)&g_toastSet = GetProcAddress(di, "ShToastSet");
            *(FARPROC *)&g_toastHide = GetProcAddress(di, "ShToastHide");
            *(FARPROC *)&g_langForOwned =
                GetProcAddress(di, "ShLangForOwned");
            BuildMenu(di);
        }
    }
    GuardLog("notice: toast=%p set=%p hide=%p lang=%p owner=%s",
             (void *)g_toastEx, (void *)g_toastSet, (void *)g_toastHide,
             (void *)g_langForOwned, g_owner[0] ? g_owner : "(none)");
    if (!g_getState)
        GuardLog("install: ShGetGameState not found - writes cannot be tied "
                 "to the death screen, only the rename is caught");

    if (Enabled()) {
        InstallHooks();
        CreateThread(NULL, 0, WatchThread, NULL, 0, NULL);
    } else {
        GuardLog("guard is off: nothing is hooked, the game behaves as if "
                 "this plugin were not installed");
    }
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
