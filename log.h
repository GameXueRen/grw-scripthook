#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

static FILE *g_logFile = NULL;

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

/* Opens <gamedir>\logs\<name> for writing. */
static void LogInit(const char *name) {
    char path[MAX_PATH];
    if (LogPath(path, sizeof(path), name))
        g_logFile = fopen(path, "w");
}

static void LogClose(void) {
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = NULL;
    }
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
