/* Keeping a Ghost Mode save when a full death ends the run.
 *
 * The symptom: in Ghost Mode a complete death - killed outright, or
 * bleeding out after going down - ends the run, the game returns to
 * the save list, and that slot is gone.
 *
 * What is done to a save, measured with GhostWipeProbe on 2026-09-11
 * against a region-locked build, and corrected across three tries:
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
 *   4. That write lands NINE SECONDS BEFORE the game over screen, so
 *      the moment a death is noticed is already too late to set it
 *      aside, and a window keyed on the screen cannot cover the rename
 *      either - the game redoes the rename on every pass over the list,
 *      and a game that has just been restarted never showed the screen.
 *
 * So three things are held at once:
 *
 *   - Two generations of clean copy. Every ordinary save rotates
 *     last\X.save into last\X.save.prev and writes the new one into
 *     last\X.save. The wipe writes once, so the .prev generation is
 *     always a save made while the player was alive.
 *
 *   - Writes during the death are kept out of the file. Once the game
 *     over screen has been seen, a write that opens the save itself is
 *     diverted to the temp folder. The .tmp promotion is left alone: an
 *     attempt that held it back left no save at all - the old save has
 *     already moved aside as .old and been deleted by then - and the
 *     game reports that as the slot not existing.
 *
 *   - The wipe is dropped and the save repaired. Every rename of N.save
 *     to N.save.delete is refused, whatever the moment, and the save is
 *     put back from the .prev generation - not the newest copy, which
 *     may be the wipe's own write. The write to the .delete is sent to
 *     the temp folder, so no tombstone ever lands in the save folder.
 *
 * The slot is still shown as deleted for the rest of the session - that
 * state lives in the process - and comes back on the next launch: the
 * folder is scanned, and the save is there with no .delete beside it.
 * Verified: a death, a restart, and the slot is listed again and plays
 * from the last save point.
 *
 * 1.save and 2.save are the global profile and need no part of this.
 * They are rewritten for ordinary reasons too - changing a setting
 * writes them - and an attempt to read their rewrites as the wipe's
 * record was a dead end.
 *
 * A player who wants a slot actually gone turns the plugin off - from
 * the menu, or enabled=0 in its ini - deletes it, and turns it back on.
 *
 * Boundaries: single player only. This changes no difficulty, no death
 * rule and no save content of its own - it declines to move one file
 * and restores another. If cloud sync is turned on later, the kept
 * local save and whatever the cloud holds may disagree; that is for the
 * player to settle. Delete the plugin folder once the game is patched.
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
#define SH_STATE_GAMEOVER 7

/* How long after the game over screen writes are still held back. The
 * rewrite of the save was measured one millisecond after the screen
 * appeared, so the poll has to be quick - and the window has to be long,
 * because the rename comes twenty seconds later. */
#define DEATH_WINDOW_MS (300u * 1000u)
#define WATCH_MS 50

typedef int (*GetState_t)(void);

static GetState_t g_getState;

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
        fprintf(g_log, "%02u:%02u:%02u.%03u  %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
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
static char      g_dirA[MAX_PATH];     /* the game's working directory */
static wchar_t   g_backupDir[MAX_PATH];

/* The plugin's own folder, which is where the clean copies live:
 * <game>\plugins\GhostNoWipe\last\. */
static void ResolveFolders(void) {
    char mod[MAX_PATH];
    char *slash;
    int len;

    g_dirA[0] = 0;
    g_backupDir[0] = 0;

    len = GetModuleFileNameA(NULL, mod, sizeof(mod));
    if (!len || len >= (int)sizeof(mod)) return;
    slash = strrchr(mod, '\\');
    if (!slash) return;
    slash[1] = 0;
    lstrcpynA(g_dirA, mod, sizeof(g_dirA));

    if (g_inst) {
        char own[MAX_PATH];
        int n;
        if (GetModuleFileNameA(g_inst, own, sizeof(own))) {
            char *p = strrchr(own, '\\');
            if (p) {
                *p = 0;
                n = (int)strlen(own);
                if (n + 16 < (int)sizeof(own)) {
                    strcat(own, "\\last");
                    CreateDirectoryA(own, NULL);
                    MultiByteToWideChar(CP_ACP, 0, own, -1, g_backupDir,
                                        MAX_PATH);
                }
            }
        }
    }
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

/* Put the profile back. The saves are found from the paths already seen
 * rather than guessed: the folder is the one N.save lives in. */
static void RestoreGlobalProfile(const wchar_t *save) {
    wchar_t dir[MAX_PATH];
    wchar_t path[MAX_PATH];
    const wchar_t *slash;
    size_t n;

    slash = wcsrchr(save, L'\\');
    if (!slash) slash = wcsrchr(save, L'/');
    if (!slash) return;
    n = (size_t)(slash - save);
    if (n + 16 >= MAX_PATH) return;
    wcsncpy(dir, save, n);
    dir[n] = 0;

    wcscpy(path, dir);
    wcscat(path, L"\\1.save");
    RestoreSave(path);
    wcscpy(path, dir);
    wcscat(path, L"\\2.save");
    RestoreSave(path);
}

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

/* The wipe is undone: whatever the game wrote into the save on its way
 * out is replaced. The copy BEFORE the newest one is used, because the
 * newest may be the wipe's own write - the game makes that one nine
 * seconds before the death screen is up, so nothing at that moment can
 * tell it from an ordinary save. The one before it is a save the player
 * made while alive. */
static void RestoreSave(const wchar_t *save) {
    wchar_t cur[MAX_PATH];
    wchar_t prev[MAX_PATH];
    wchar_t src[MAX_PATH];

    if (!g_realCopyFileW || !BackupPath(save, cur, MAX_PATH)) return;
    if (!BackupPrevPath(save, prev, MAX_PATH)) return;

    if (GetFileAttributesW(prev) != INVALID_FILE_ATTRIBUTES)
        wcscpy(src, prev);
    else if (GetFileAttributesW(cur) != INVALID_FILE_ATTRIBUTES)
        wcscpy(src, cur);
    else {
        GuardLog("no copy of %ls to put back - the save keeps whatever the "
                 "game left in it", save);
        return;
    }

    if (g_realCopyFileW(src, save, FALSE))
        GuardLog("put back %ls from %ls", save,
                 _wcsicmp(src, prev) == 0 ? L"the copy before the newest one"
                                          : L"the newest copy");
    else
        GuardLog("putting %ls back failed (err=%lu)", save, GetLastError());
}

/* ---- keeping the wipe out of the save folder --------------------------- */

/* A write to a path this plugin must not let land in the save folder is
 * sent to %TEMP%\GhostNoWipe\<name> instead, filled from `source` if it
 * is not there yet, so the game opens something and reads back what it
 * expects. `source` is the save itself for a write to the save, and the
 * save the tombstone was renamed from for a write to a .delete. */
static int RedirectToTemp(const wchar_t *name, const wchar_t *source,
                          wchar_t *out, size_t cap) {
    wchar_t temp[MAX_PATH];
    const wchar_t *base;

    if (!name || !source) return 0;
    base = wcsrchr(name, L'\\');
    if (!base) base = wcsrchr(name, L'/');
    base = base ? base + 1 : name;

    if (!GetTempPathW(MAX_PATH, temp)) return 0;
    if (wcslen(temp) + wcslen(base) + 16 >= cap) return 0;

    wcscpy(out, temp);
    wcscat(out, L"GhostNoWipe");
    CreateDirectoryW(out, NULL);
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
        /* The wipe, at whatever moment it comes - the game redoes it on
         * every pass over the save list. Dropped, and the save repaired
         * in case the mark was written before the screen was noticed. */
        if (IsDeathRename(from, to)) {
            GuardLog("keep  %ls", from);
            GuardLog("      the rename is dropped and nothing takes its "
                     "place; the .delete write goes to the temp folder");
            RestoreSave(from);
            RestoreGlobalProfile(from);
            return TRUE;
        }
        /* The last step of a save: the .tmp takes the save's place. Left
         * alone, always. Holding this back is what broke an earlier
         * attempt: by the time it runs, the old save has already moved
         * aside as .old and been deleted, so refusing it leaves no save
         * at all - which the game reports as the slot not existing.
         *
         * A save written while the run is ending carries the mark, so it
         * is not kept as the copy to go back to; the point of the copy is
         * to predate it. The repair happens at the rename instead. */
        if (IsPromotion(from, to)) {
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
        if (IsDeathRename(from, to)) {
            GuardLog("keep  %ls", from);
            GuardLog("      the rename is dropped and nothing takes its "
                     "place");
            RestoreSave(from);
            RestoreGlobalProfile(from);
            return TRUE;
        }
        if (IsPromotion(from, to)) {
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
        /* A write to the tombstone: out of the save folder, always. */
        if (IsDeleteMarkedSave(name)) {
            wchar_t source[MAX_PATH];
            if (StripDeleteSuffix(name, source, MAX_PATH) &&
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
             * The global profile (1.save / 2.save) needs none of this. It
             * is rewritten for ordinary reasons as well - changing a
             * setting writes it - so an attempt to read its rewrites as
             * the wipe's record was a dead end. */
            if ((access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA |
                           FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES)) &&
                InDeathWindow() &&
                RedirectToTemp(name, name, target, MAX_PATH)) {
                GuardLog("write %ls", name);
                GuardLog("      the run has ended; it goes to %ls instead",
                         target);
                return g_realCreateFileW(target, access, share, sa, disp,
                                         flags, tmpl);
            }
            /* Reading it is how the game loads a slot: a good moment to
             * take a copy if there is none yet. */
            BackupSaveIfMissing(name);
        }
    }
    return g_realCreateFileW(name, access, share, sa, disp, flags, tmpl);
}

/* ---- watching for the death screen ------------------------------------ */

/* The rewrite of the save lands within a millisecond of the game over
 * screen, so the poll is deliberately quick. Transitions are logged once
 * each, not polled into the log. */
static DWORD WINAPI WatchThread(LPVOID p) {
    int wasOver = 0;

    (void)p;
    for (;;) {
        if (Enabled() && g_getState) {
            int over = (g_getState() == SH_STATE_GAMEOVER);
            if (over) {
                InterlockedExchange64(&g_lastGameOver,
                                      (LONG64)GetTickCount64());
                if (!wasOver) GuardLog("death screen seen");
            }
            wasOver = over;
        }
        Sleep(WATCH_MS);
    }
    return 0;
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

static void BuildMenu(HMODULE m) {
    uint32_t (*menuCreate)(const char *) = NULL;
    int (*menuToggle)(uint32_t, const char *, int, MenuFn, void *) = NULL;
    int (*menuHint)(uint32_t, const char *) = NULL;

    *(FARPROC *)&menuCreate = GetProcAddress(m, "ShMenuCreate");
    *(FARPROC *)&menuToggle = GetProcAddress(m, "ShMenuToggle");
    *(FARPROC *)&menuHint   = GetProcAddress(m, "ShMenuHint");
    if (!menuCreate || !menuToggle) return;

    {
        uint32_t menu = menuCreate("Ghost save guard");
        menuToggle(menu, "Keep the save when the run ends", Enabled(),
                   OnEnable, NULL);
        if (menuHint)
            menuHint(menu,
                     "A death in Ghost Mode makes the game rewrite the save "
                     "to mark the run over, then rename it to .save.delete - "
                     "the rename is the tombstone the save list reads. The "
                     "rewrite is held out of the file, the rename is "
                     "dropped, and the save is put back from a clean copy "
                     "kept at the last ordinary save. Deleting a slot by "
                     "hand is caught too: turn the plugin off to remove "
                     "one.");
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

    GuardLog("--- GhostNoWipe: keeping the save when a run ends ---");
    GuardLog("build " __DATE__ " " __TIME__);
    GuardLog("config: guard=%s, ini=%s", Enabled() ? "on" : "off",
             g_iniPath[0] ? g_iniPath : "(none)");
    GuardLog("clean copies: %ls", g_backupDir[0] ? g_backupDir : L"(nowhere)");

    {
        HMODULE di = GetModuleHandleA("dinput8.dll");
        if (di) {
            *(FARPROC *)&g_getState = GetProcAddress(di, "ShGetGameState");
            BuildMenu(di);
        }
    }
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
