#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static FILE *g_logFile = NULL;
static char  g_logName[MAX_PATH];
/* 1 while this translation unit's open file is its plugin's own log,
 * which the level does not filter line by line - see LogInitAlways. */
static int   g_logAlways = 0;
/* 1 while this unit has no log of its own at this level and its open file is
 * the session's floor instead (logs\scripthook.log, appended). See LogFloor
 * and the note on the fallback in LogInitMode: only its LOG_ALWAYS lines are
 * written, so a quiet level keeps the milestones and drops the chatter. */
static int   g_logFloor = 0;

/* Build "<gamedir>\logs\<name>", creating the logs directory
 * on first use. The game is free to change the working
 * directory, so the path anchors to the module file, not CWD.
 * Returns 1 on success, 0 on failure. */
static int LogPath(char *buf, size_t n, const char *name) {
    char dir[MAX_PATH], logs[MAX_PATH];
    char *slash;

    if (!GetModuleFileNameA(NULL, dir, MAX_PATH)) return 0;
    slash = strrchr(dir, '\\');
    if (slash) slash[1] = 0;
    else dir[0] = 0;

    if (snprintf(logs, sizeof(logs), "%slogs", dir) < 0) return 0;
    CreateDirectoryA(logs, NULL);

    if (snprintf(buf, n, "%s\\%s", logs, name) < 0) return 0;
    return 1;
}

/* ---- log levels --------------------------------------------------
 *
 * [Settings] LogLevel says how much is written, and which files exist at
 * all. A module's own log costs a file in logs\, so it needs info or
 * debug; the two the support flow asks for - scripthook.log and the
 * crash report - are there whatever the level is. Those two are the
 * floor, so even "none" leaves a session somebody can be asked about.
 *
 * The floor is also *where a module's milestones go* when the level drops
 * its own file. Every module keeps a handful of LOG_ALWAYS lines - the ones
 * that say which step of start up happened (see LogFloor below), and at a
 * quiet level they are appended to logs\scripthook.log instead of being
 * dropped along with the rest of that module's file. That is the change the
 * first public beta's support flow asked for: the field report of
 * 2026-09-17 arrived with the version, the plugin count, the overlay's
 * install route and the loader's own failures all silent, because every one
 * of them was a LOG_INFO line in a file the level had removed, and "did not
 * get there" could not be told apart from "never logged".
 *
 * A plugin's own log is the exception, and a plugin asks for it with
 * LogInitAlways(): it is written at every level except none. Its lines
 * carry no levels of their own, its file is small, and the line that
 * matters is usually the one about something not working - the line a
 * report cannot do without - so it is not the line to drop when a
 * session is quiet.
 *
 * The level is asked of the DLL rather than read here: this header is
 * compiled into every plugin as well, and a plugin's own log has to obey
 * the same setting. It is resolved once per translation unit, and the
 * fallback below is what a unit sees if the DLL cannot answer (which is
 * also the whole story for a build that does not link the DLL at all).
 */
#define LOG_NONE    0
#define LOG_ERR     1
#define LOG_WARN    2
#define LOG_INFO    3
#define LOG_DBG     4
/* Written whatever the level is. Two kinds of line carry it: the loader's
 * own start up lines, and each module's start up milestones - the build, the
 * plugin count, whether the overlay came up and by which route, whether the
 * game's exe answered the site checks. At info and debug a module's own file
 * carries them; at warn and below the floor in logs\scripthook.log does. */
#define LOG_ALWAYS  9

/* A release build starts quieter than a working one: warn keeps the two
 * floor files and drops the module logs, which is the player-facing
 * shape. LogLevel=debug brings the whole set back for one session. */
#ifdef SH_RELEASE
#define LOG_DEFAULT LOG_WARN
#else
#define LOG_DEFAULT LOG_INFO
#endif

static const char *LogLevelName(int l) {
    switch (l) {
    case LOG_NONE: return "none";
    case LOG_ERR:  return "error";
    case LOG_WARN: return "warn";
    case LOG_DBG:  return "debug";
    default:       return "info";
    }
}

/* ShLogLevel, resolved once for this translation unit. */
static int LogLevel(void) {
    typedef int (*LevelFn)(void);
    static int cached = -1;
    LevelFn fn = NULL;
    HMODULE di;

    if (cached >= 0) return cached;
    cached = LOG_DEFAULT;
    di = GetModuleHandleA("dinput8.dll");
    if (di)
        *(FARPROC *)&fn = GetProcAddress(di, "ShLogLevel");
    if (fn) {
        int v = fn();
        if (v >= LOG_NONE && v <= LOG_DBG) cached = v;
    }
    return cached;
}

static int LogLevelOn(int level) {
    return level == LOG_ALWAYS || level <= LogLevel();
}

/* Whether this translation unit's log exists at the level in force.
 *
 * scripthook.log and scripthook_crash.log are the floor: a report is
 * always asked for both. Everything else is a module's own log - another
 * file in logs\ - and costs info or debug.
 *
 * `always` is the plugin's answer: written at every level but none. None
 * is the one setting that means "write nothing", so it is the one that
 * turns these off as well. */
static int LogWanted(const char *name, int always) {
    if (strcmp(name, "scripthook.log") == 0 ||
        strcmp(name, "scripthook_crash.log") == 0)
        return 1;
    if (always) return LogLevel() > LOG_NONE;
    return LogLevel() >= LOG_INFO;
}

/* Closes whatever this translation unit has open. */
static void LogClose(void) {
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = NULL;
        g_logName[0] = 0;
    }
    g_logAlways = 0;
    g_logFloor = 0;
}

/* "<gamedir>\<name>": where a log goes when logs\ cannot be created or
 * written. The game folder is where the DLL already lives, so it is the one
 * place whose availability is not in question. */
static int LogFallbackPath(char *buf, size_t n, const char *name) {
    char dir[MAX_PATH];
    char *slash;

    if (!GetModuleFileNameA(NULL, dir, MAX_PATH)) return 0;
    slash = strrchr(dir, '\\');
    if (slash) slash[1] = 0;
    else dir[0] = 0;
    return snprintf(buf, n, "%s%s", dir, name) >= 0;
}

/* Opens <gamedir>\logs\<name> for writing.
 *
 * Idempotent for the name already open. The handle is per translation unit,
 * so a second call for the same file would reopen it with "w": everything
 * the session has already written is truncated and the old handle leaks.
 * scripthook_npc.c works around that by hand today, which is the shape of a
 * bug that has not been hit yet rather than one that cannot be. A call for a
 * different name closes the old file first - the only case where dropping
 * the earlier one is what was asked for.
 *
 * If logs\ cannot be created - an install under Program Files, a read-only
 * drive, a scanner holding the folder - the file goes to the game folder
 * instead and says so in its first line. Before this, that failure was
 * completely silent: a session with no logs and nothing anywhere saying why,
 * which is the hardest kind of report to act on.
 *
 * The session argument is LogRotate's: see the sessions note below for what it
 * decides, and why the file a previous run left is renamed rather than thrown
 * away. */
/* ---- sessions -----------------------------------------------------------
 *
 * Every file here belongs to one session, and a few sessions are kept.
 *
 * A log is read for the run that went wrong - a crash, a freeze, a mod that
 * misbehaved - and that run is nearly always the one that just ended, so a log
 * thrown away at start up throws the evidence away. The mainstream shape
 * (logrotate's `create`, spdlog's rotating sink, Docker's max-file) is the one
 * used here: the session running now keeps the plain name a support request
 * asks for, and the session before it is renamed aside, dated with the moment
 * it started. Nothing is rewritten in place - a rename is metadata - so a file
 * that is still being appended to, or tailed by a player, never sees a torn or
 * half-copied log.
 *
 * What says "this file is someone else's" is its first line: every file opens
 * with a session marker, and the marker carries the process start time as well
 * as the pid, so two runs cannot be confused even when the pid comes round
 * again. That check needs no shared state, which is what makes it possible
 * here: log.h is compiled into every translation unit, and each has its own
 * copy of everything below. A file written before markers existed has none and
 * is kept the same way - and it is the one most likely to hold the crash
 * somebody is asking about, so it is the one that must not be dropped.
 *
 * Kept per name: the session in progress and LOG_SESSIONS - 1 before it, so a
 * folder gains files rather than unbounded space. The archives sort by name,
 * so the oldest is the one that goes.
 */
#define LOG_SESSIONS 3                  /* the run in progress plus two */

/* The process start time, which together with the pid names this session: a
 * later run can be given the same pid after enough uptime, and the pair cannot
 * repeat. Asked once, and a unit that never opens a log never asks. */
static unsigned long long LogSession(void) {
    static unsigned long long cached;
    FILETIME created, exited, kernel, user;

    if (cached) return cached;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        cached = ((unsigned long long)created.dwHighDateTime << 32) |
                 created.dwLowDateTime;
    if (!cached) cached = 1;            /* cached even when it cannot be had */
    return cached;
}

/* "pid=1234 start=1F2A3B4C5D6E7F80": what a session's marker is recognised by.
 * Built once per translation unit, and a race that builds it twice builds the
 * same string. */
static const char *LogIdentity(void) {
    static char id[64];

    if (!id[0])
        snprintf(id, sizeof(id), "pid=%lu start=%016llX",
                 (unsigned long)GetCurrentProcessId(),
                 (unsigned long long)LogSession());
    return id;
}

/* The first line of every file: what it is, when the run began, and which
 * session wrote it, so the file a player sends says all three by itself. */
static void LogMarker(FILE *f) {
    SYSTEMTIME st;

    GetLocalTime(&st);
    fprintf(f, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] === session "
               "%04u-%02u-%02u %02u:%02u:%02u, %s ===\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds,
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            LogIdentity());
    fflush(f);
}

/* Is the file on disk the one the session running now is writing? Its first
 * line carries the marker. An empty file, an unreadable one, and one from
 * before markers existed are all "not ours" - the last two on purpose: the
 * caller then tries to move it aside, and if it cannot, appends to it rather
 * than truncating evidence it could not read. */
static int LogIsOurs(const char *path) {
    char line[192];
    FILE *f = fopen(path, "r");
    int ours;

    if (!f) return 0;
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
    ours = strstr(line, LogIdentity()) != NULL;
    fclose(f);
    return ours;
}

/* When the file on disk was created - the moment the session that wrote it
 * began, which is what its archive is named after. */
static int LogCreated(const char *path, SYSTEMTIME *out) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    FILETIME local;

    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return 0;
    if (!FileTimeToLocalFileTime(&fa.ftCreationTime, &local)) return 0;
    return FileTimeToSystemTime(&local, out);
}

/* 1 for "<stem>-YYYYMMDD-HHMMSS.log" and nothing else. The strictness is what
 * keeps the pruning below from ever touching a file that is not an archive of
 * this log. */
static int LogIsArchive(const char *name, const char *stem) {
    size_t n = strlen(stem);
    int i;

    if (strlen(name) != n + 20) return 0;
    if (strncmp(name, stem, n) != 0 || name[n] != '-') return 0;
    for (i = 0; i < 8; i++)
        if (name[n + 1 + i] < '0' || name[n + 1 + i] > '9') return 0;
    if (name[n + 9] != '-') return 0;
    for (i = 0; i < 6; i++)
        if (name[n + 10 + i] < '0' || name[n + 10 + i] > '9') return 0;
    return strcmp(name + n + 16, ".log") == 0;
}

/* Drops the oldest archives of this name until LOG_SESSIONS - 1 are left. One
 * at a time, found in the folder itself, so nothing has to be held in memory
 * and a folder that already holds more than this build makes is settled in a
 * few passes. The names sort, so the smallest is the oldest. */
static void LogPrune(const char *dir, const char *stem) {
    int passes = 0;

    for (;;) {
        WIN32_FIND_DATAA fd;
        char pat[MAX_PATH], oldest[MAX_PATH];
        HANDLE h;
        int n = 0;

        oldest[0] = 0;
        if (snprintf(pat, sizeof(pat), "%s\\%s-*.log", dir, stem) >=
            (int)sizeof(pat))
            return;
        h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (!LogIsArchive(fd.cFileName, stem)) continue;
            n++;
            if (!oldest[0] || strcmp(fd.cFileName, oldest) < 0)
                snprintf(oldest, sizeof(oldest), "%s", fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        /* A delete that keeps failing must not spin here. */
        if (n <= LOG_SESSIONS - 1 || !oldest[0] || ++passes > 64) return;
        if (snprintf(pat, sizeof(pat), "%s\\%s", dir, oldest) <
            (int)sizeof(pat))
            DeleteFileA(pat);
    }
}

/* Moves the session before this one aside as "<stem>-<date>.log" and settles
 * the folder to the kept set. Returns 1 when what is on disk is this session's
 * own file or nothing at all, 0 when it is another session's and could not be
 * moved - the caller then appends rather than truncates, because a rename that
 * failed must not become the loss of a log.
 *
 * A rename, not a copy, and failure is not fatal for the same reason: not being
 * able to keep the previous log is no reason to refuse to start one now. The
 * loader's first call runs on the attach path, before the file interception
 * layer holds a single rule (its hooks go up with the first one), so no rule of
 * ours can see the call; later opens are the game's own file APIs like any
 * other. */
static int LogRotate(const char *name, const char *path) {
    char dir[MAX_PATH], stem[MAX_PATH], arc[MAX_PATH];
    const char *slash, *dot;
    SYSTEMTIME st;
    size_t dn, sn;

    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return 1;
    if (LogIsOurs(path)) return 1;      /* this session's file already */

    slash = strrchr(path, '\\');
    dn = slash ? (size_t)(slash - path) : 0;
    if (dn == 0 || dn >= sizeof(dir)) return 0;   /* no folder: leave it alone */
    memcpy(dir, path, dn);
    dir[dn] = 0;

    dot = strrchr(name, '.');
    sn = dot ? (size_t)(dot - name) : strlen(name);
    if (sn >= sizeof(stem)) return 0;
    memcpy(stem, name, sn);
    stem[sn] = 0;

    if (!LogCreated(path, &st))
        GetLocalTime(&st);              /* a name that is merely close is fine */
    if (snprintf(arc, sizeof(arc), "%s\\%s-%04u%02u%02u-%02u%02u%02u.log",
                 dir, stem, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond) < (int)sizeof(arc))
        MoveFileExA(path, arc, MOVEFILE_REPLACE_EXISTING);
    LogPrune(dir, stem);
    /* Gone means it was moved, and the caller may start a new one. */
    return GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES;
}

static FILE *LogOpen(const char *name, const char *path, int fresh) {
    /* scripthook.log has more than one writer, and they do not share a file
     * pointer: the loader owns this handle, and every module whose own log
     * the level dropped appends to the same file (the floor branch of
     * LogInitMode, one handle per translation unit).
     *
     * "w" leaves this handle writing at its own tracked offset, so the next
     * line written here lands on top of whatever a floor writer appended in
     * between. The session of 2026-09-17 23:51 has that scar: the overlay's
     * "factory capture installed (...)" line survives in logs\scripthook.log
     * as nothing but its tail, "d while it is up", one line later - and it
     * was the line that said which capture route the overlay had taken.
     *
     * So: truncate with "w", then write with "a" like every other writer,
     * which is the one mode where the OS itself puts each write at the end
     * of the file whatever the other handles are doing.
     *
     * `fresh` is LogRotate's answer: 1 means this session's own file, which is
     * started here and given the session marker as its first line, 0 means
     * another session's file that could not be moved aside - and that is
     * appended to, never truncated, so a failed rename cannot cost a log.
     *
     * A session that appends says so with the same marker, so the lines that
     * follow can be told apart from the ones already in the file. This is not
     * hypothetical: a launch runs GRW.exe as two processes (a wrapper and the
     * game, seconds apart), the second one finds the first one's file still
     * open and holds a handle that a rename cannot move, and without the marker
     * its block reads as the first process's - the 2026-09-25 22:07 session in
     * logs\scripthook_ovl.log showed one banner and a doubled "overlay: off"
     * line until the second process's own line in logs\scripthook.log
     * (loader: attach pid=22320) explained it.
     *
     * Not for scripthook.log: every module whose own log the level dropped
     * appends to it, and it would fill with markers. */
    FILE *f;

    if (!fresh)
    {
        f = fopen(path, "a");
        if (f && strcmp(name, "scripthook.log") != 0) LogMarker(f);
        return f;
    }
    f = fopen(path, "w");
    if (!f) return NULL;
    LogMarker(f);                       /* the line that names this session */
    if (strcmp(name, "scripthook.log") != 0) return f;
    fclose(f);
    return fopen(path, "a");
}

static void LogInitMode(const char *name, int always) {
    char path[MAX_PATH];
    FILE *f;

    if (!LogWanted(name, always)) {
        /* The level dropped this unit's own log. Its LOG_ALWAYS lines are the
         * milestones a report is read for, so they keep somewhere to go: the
         * session's floor file, appended - the loader owns its own handle on
         * the same name, and fopen shares. A plugin (always) is left alone:
         * its own file exists at every level but none, so for it this branch
         * only ever means "none", which means nothing is written. */
        if (always) return;
        if (g_logFile) return;          /* already routed */
        if (!LogPath(path, sizeof(path), "scripthook.log")) return;
        f = fopen(path, "a");
        if (!f) return;
        g_logFile = f;
        g_logFloor = 1;
        strncpy(g_logName, "scripthook.log", sizeof(g_logName) - 1);
        g_logName[sizeof(g_logName) - 1] = 0;
        return;
    }
    if (g_logFile && strcmp(g_logName, name) == 0) return;
    if (g_logFile) LogClose();
    if (!LogPath(path, sizeof(path), name)) return;
    /* The run before this one keeps its own file rather than being overwritten
     * - see the sessions note above - and the folder is settled to the kept
     * set on the way past. */
    f = LogOpen(name, path, LogRotate(name, path));
    if (!f) {
        if (!LogFallbackPath(path, sizeof(path), name)) return;
        f = LogOpen(name, path, LogRotate(name, path));
        if (!f) return;
        fprintf(f, "[note] logs\\ could not be written to; this file is "
                   "beside the game executable instead\n");
    }
    g_logFile = f;
    g_logAlways = always;
    strncpy(g_logName, name, sizeof(g_logName) - 1);
    g_logName[sizeof(g_logName) - 1] = 0;
}

/* A module's own log: written at info and debug. */
static void LogInit(const char *name) { LogInitMode(name, 0); }

/* A plugin's own log: written at every level except none, and its lines
 * are not filtered - they carry no levels of their own, and the line a
 * report needs is often the one about something not working, which an
 * info threshold would drop. */
static void LogInitAlways(const char *name) { LogInitMode(name, 1); }

/* The shared printer: every line carries a local timestamp,
 * so the different logs in the logs directory can be lined
 * up against each other. */
static void LogWrite(const char *fmt, va_list ap) {
    SYSTEMTIME st;

    if (!g_logFile) return;
    GetLocalTime(&st);
    if (g_logFloor) {
        /* This unit has no log of its own at this level: it is sharing the
         * session's scripthook.log with the loader and with every other
         * module in the same position, each through its own handle. One
         * write per line, then, or two units' lines interleave in the middle
         * of each other. */
        char line[1024];
        int  n = snprintf(line, sizeof(line),
                          "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
                          st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                          st.wSecond, st.wMilliseconds);

        if (n < 0) n = 0;
        if (n < (int)sizeof(line) - 1) {
            int m = vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
            if (m > 0) n += m;
        }
        if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
        if (line[n - 1] != '\n') { line[n] = '\n'; line[n + 1] = 0; }
        fwrite(line, 1, strlen(line), g_logFile);
        fflush(g_logFile);
        return;
    }
    fprintf(g_logFile, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    vfprintf(g_logFile, fmt, ap);
    fputc('\n', g_logFile);
    fflush(g_logFile);
}

/* The raw entry point, for the modules that have a wrapper of their own
 * (draw, corefix, api, config, the overlay): their lines carry no level,
 * because their file is gated as a whole by LogWanted. At a quiet level that
 * file is not there and this unit writes into the floor instead - where an
 * untagged line is exactly the chatter the floor is not for. So nothing is
 * written, and the unit's milestones arrive through LogAt(LOG_ALWAYS). */
static void Logv(const char *fmt, va_list ap) {
    if (g_logFloor) return;
    LogWrite(fmt, ap);
}

/* One line at one level; the level is what [Settings] LogLevel filters
 * on. Log() stays the plain call every existing site uses - it means
 * info, which is what those lines are. */
static void LogAt(int level, const char *fmt, ...) {
    va_list ap;

    if (!g_logFile) return;
    /* A plugin's own log is not filtered line by line - see LogInitAlways.
     * Every other file here belongs to a module and follows the level - and
     * a unit that is on the floor keeps only its milestones. */
    if (g_logFloor) {
        if (level != LOG_ALWAYS) return;
    } else if (!g_logAlways && !LogLevelOn(level)) {
        return;
    }
    va_start(ap, fmt);
    LogWrite(fmt, ap);
    va_end(ap);
}

#define Log(...)        LogAt(LOG_INFO, __VA_ARGS__)
#define LogErr(...)     LogAt(LOG_ERR, __VA_ARGS__)
#define LogWarn(...)    LogAt(LOG_WARN, __VA_ARGS__)
#define LogDbg(...)     LogAt(LOG_DBG, __VA_ARGS__)
#define LogAlways(...)  LogAt(LOG_ALWAYS, __VA_ARGS__)

/* The first thing a module has to say: the site it just validated, or the
 * read that came back empty. Its whole reason is that a constant which went
 * stale does not fail loudly on its own - the module refuses, the caller
 * sees a generic error, and finding out why costs a round trip through the
 * game. One line in logs\ is the difference. A module with something to
 * install calls it at the install decision point, in both branches, so the
 * log says which way it went.
 *
 * Where that one line goes depends on the level: into the module's own file
 * at info and debug, and into logs\scripthook.log - as a milestone, at
 * LOG_ALWAYS - when the level has dropped that file. It has to: a released
 * package runs at warn, and this is the line that tells "the site did not
 * match this game build" apart from "the module never got that far". Every
 * one of these modules (entity, havok, stealth, input, fov, blur, hit, npc,
 * reflect) refuses on a stale constant, and with the line filtered out, a
 * player's report could not say which module refused or that any had.
 * Nine files' worth of these lines is nothing next to not being able to
 * read the report; the call sites are once-per-site by construction.
 */
static void LogFirstNow(const char *logName, const char *fmt, ...) {
    va_list ap;
    char line[300];

    LogInit(logName);
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    /* Through LogAt with the text as an argument: the line is data here,
     * and a '%' in it must not be read as a conversion. */
    LogAt(g_logFloor ? LOG_ALWAYS : LOG_INFO, "%s", line);
}

/* Once per CALL SITE, not once per translation unit.
 *
 * The flag used to be a function-level static, which every call site in a
 * file shared: entity.c has five of them, havok.c four, stealth/input/fov
 * three each - and only the first ever reached the log. So the branch that
 * says "and this is the one that did not match" was silent in exactly the
 * session it was needed for. The static now sits inside the macro's own
 * block, so each expansion gets its own flag, while the call site reads the
 * same as before. */
#define LogFirst(name, ...)                                            \
    do {                                                               \
        static volatile LONG once_;                                    \
        if (InterlockedExchange(&once_, 1) == 0)                       \
            LogFirstNow((name), __VA_ARGS__);                          \
    } while (0)
