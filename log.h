#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static FILE *g_logFile = NULL;
static char  g_logName[MAX_PATH];

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

/* A release build keeps only the two logs a player can be asked for: the
 * loader's own and the crash report. Every module-level diagnostic is
 * compiled out, which is also what stops the framework from creating a
 * dozen empty files at startup. */
#ifdef SH_RELEASE
static int LogWanted(const char *name) {
    return strcmp(name, "scripthook.log") == 0 ||
           strcmp(name, "scripthook_crash.log") == 0;
}
#else
static int LogWanted(const char *name) { (void)name; return 1; }
#endif

/* Closes whatever this translation unit has open. */
static void LogClose(void) {
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = NULL;
        g_logName[0] = 0;
    }
}

/* Opens <gamedir>\logs\<name> for writing.
 *
 * Idempotent for the name already open. The handle is per translation unit,
 * so a second call for the same file would reopen it with "w": everything
 * the session has already written is truncated and the old handle leaks.
 * scripthook_npc.c works around that by hand today, which is the shape of a
 * bug that has not been hit yet rather than one that cannot be. A call for a
 * different name closes the old file first - the only case where dropping
 * the earlier one is what was asked for. */
static void LogInit(const char *name) {
    char path[MAX_PATH];
    FILE *f;

    if (!LogWanted(name)) return;
    if (g_logFile && strcmp(g_logName, name) == 0) return;
    if (g_logFile) LogClose();
    if (!LogPath(path, sizeof(path), name)) return;
    f = fopen(path, "w");
    if (!f) return;
    g_logFile = f;
    strncpy(g_logName, name, sizeof(g_logName) - 1);
    g_logName[sizeof(g_logName) - 1] = 0;
}

/* The shared printer: every line carries a local timestamp,
 * so the different logs in the logs directory can be lined
 * up against each other. */
static void Logv(const char *fmt, va_list ap) {
    SYSTEMTIME st;

    if (!g_logFile) return;
    GetLocalTime(&st);
    fprintf(g_logFile, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    vfprintf(g_logFile, fmt, ap);
    fputc('\n', g_logFile);
    fflush(g_logFile);
}

static void Log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Logv(fmt, ap);
    va_end(ap);
}

/* The first thing a module has to say: the site it just validated, or the
 * read that came back empty. Its whole reason is that a constant which went
 * stale does not fail loudly on its own - the module refuses, the caller
 * sees a generic error, and finding out why costs a round trip through the
 * game. One line in logs\ is the difference. A module with something to
 * install calls it at the install decision point, in both branches, so the
 * log says which way it went.
 */
static void LogFirstNow(const char *logName, const char *fmt, ...) {
    va_list ap;
    char line[300];

    LogInit(logName);
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    Log(line);
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
