#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

static FILE *g_logFile = NULL;

static void LogInit(const char *path) {
    g_logFile = fopen(path, "w");
}

static void LogClose(void) {
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = NULL;
    }
}

static void Log(const char *fmt, ...) {
    if (!g_logFile) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fputc('\n', g_logFile);
    fflush(g_logFile);
}
