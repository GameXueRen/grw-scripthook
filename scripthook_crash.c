/* Crash reports for the field, and an optional freeze.
 * Report only by default: the log is written and normal
 * handling continues, so nothing behaves differently. */
/* Freezing parks the faulting thread instead, keeping the
 * process alive for a live post mortem. Opt in: a handled
 * exception would hang the game rather than continue. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

extern int ShReadMem(uint64_t addr, void *out, size_t len);
extern void ShSetError(int err);

#define CRASH_LOG_MAX  24
#define CRASH_SEEN_MAX 16
#define CRASH_STACK_N  24
#define CRASH_FILE     "scripthook_crash.log"

/* Static buffers under one guard: a stack overflow must
 * not need 4KB of stack to be reported, and two threads
 * faulting at once must not interleave their reports. */
static volatile LONG g_busy = 0;
static volatile LONG g_caught = 0;
static volatile int  g_freeze = 0;
static uint64_t g_seen[CRASH_SEEN_MAX];
static int      g_nseen = 0;
static int      g_logged = 0;
static int      g_header = 0;
static char     g_buf[4096];
static char     g_first[4096];
static int      g_haveFirst = 0;

static void Emit(const char *text, int len) {
    HANDLE f;
    DWORD wrote = 0;

    f = CreateFileA(CRASH_FILE, FILE_APPEND_DATA, FILE_SHARE_READ,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, text, (DWORD)len, &wrote, NULL);
    CloseHandle(f);
}

/* module+offset, which is what a report needs to be
 * actionable against a disassembly.
 */
static int Where(char *dst, int cap, uint64_t at) {
    HMODULE m = NULL;
    char path[MAX_PATH];
    const char *name;

    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)(uintptr_t)at, &m) || !m)
        return snprintf(dst, cap, "%016llX",
                        (unsigned long long)at);
    path[0] = 0;
    GetModuleFileNameA(m, path, sizeof(path));
    name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    return snprintf(dst, cap, "%s+0x%llX", name,
                    (unsigned long long)(at - (uint64_t)(uintptr_t)m));
}

/* Once per session, so a report carries the build that
 * produced it without the reporter having to say.
 */
static int Header(char *dst, int cap) {
    if (g_header) return 0;
    g_header = 1;
    return snprintf(dst, cap,
                    "\n=== GRW ScriptHook, built %s %s, base %016llX\n",
                    __DATE__, __TIME__,
                    (unsigned long long)ShImageBase());
}

static int Duplicate(uint32_t code, uint64_t at) {
    uint64_t key = ((uint64_t)code << 48) ^ at;
    int i;

    for (i = 0; i < g_nseen; i++)
        if (g_seen[i] == key) return 1;
    if (g_nseen < CRASH_SEEN_MAX) g_seen[g_nseen++] = key;
    return 0;
}

/* fatal says nothing else will handle this, so it is
 * always written. First chance entries are deduped.
 */
static void Report(EXCEPTION_POINTERS *ep, int fatal) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    uint64_t at = (uint64_t)(uintptr_t)er->ExceptionAddress;
    char site[160];
    int n = 0, i;

    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;

    if (g_logged >= CRASH_LOG_MAX) goto done;
    if (!fatal && Duplicate(er->ExceptionCode, at)) goto done;
    g_logged++;

    n += Header(g_buf + n, (int)sizeof(g_buf) - n);
    Where(site, sizeof(site), at);
    n += snprintf(g_buf + n, sizeof(g_buf) - n,
                  "exception %08lX at %s, thread %lu, %s\n",
                  (unsigned long)er->ExceptionCode, site,
                  (unsigned long)GetCurrentThreadId(),
                  fatal == 1 ? "fatal"
                  : fatal == 2 ? "healed, null path resumed"
                  : "first chance");
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        er->NumberParameters >= 2) {
        const char *kind = er->ExceptionInformation[0] == 0 ? "read"
                         : er->ExceptionInformation[0] == 1 ? "write"
                         : "exec";
        n += snprintf(g_buf + n, sizeof(g_buf) - n,
                      "  %s of %016llX\n", kind,
                      (unsigned long long)er->ExceptionInformation[1]);
    }
    n += snprintf(g_buf + n, sizeof(g_buf) - n,
                  "  rip %016llX rsp %016llX rbp %016llX\n"
                  "  rax %016llX rbx %016llX rcx %016llX\n"
                  "  rdx %016llX rsi %016llX rdi %016llX\n"
                  "  r8  %016llX r9  %016llX r10 %016llX\n"
                  "  r11 %016llX r12 %016llX r13 %016llX\n"
                  "  r14 %016llX r15 %016llX\n",
                  (unsigned long long)c->Rip, (unsigned long long)c->Rsp,
                  (unsigned long long)c->Rbp, (unsigned long long)c->Rax,
                  (unsigned long long)c->Rbx, (unsigned long long)c->Rcx,
                  (unsigned long long)c->Rdx, (unsigned long long)c->Rsi,
                  (unsigned long long)c->Rdi, (unsigned long long)c->R8,
                  (unsigned long long)c->R9,  (unsigned long long)c->R10,
                  (unsigned long long)c->R11, (unsigned long long)c->R12,
                  (unsigned long long)c->R13, (unsigned long long)c->R14,
                  (unsigned long long)c->R15);

    /* The raw stack, annotated per module. Return
     * addresses stand out, which is enough to place the
     * fault in a call chain without unwind data. */
    for (i = 0; i < CRASH_STACK_N &&
                n < (int)sizeof(g_buf) - 128; i++) {
        uint64_t v = 0;

        if (!ShReadMem(c->Rsp + (uint64_t)i * 8, &v, 8)) break;
        site[0] = 0;
        if (v > 0x10000) Where(site, sizeof(site), v);
        n += snprintf(g_buf + n, sizeof(g_buf) - n,
                      "  rsp+%02X %016llX %s\n", i * 8,
                      (unsigned long long)v,
                      strchr(site, '+') ? site : "");
    }

    if (!g_haveFirst) {
        memcpy(g_first, g_buf, (size_t)n);
        g_first[n] = 0;
        g_haveFirst = 1;
    }
    Emit(g_buf, n);
    InterlockedIncrement(&g_caught);

done:
    InterlockedExchange(&g_busy, 0);
}

static volatile LONG g_healed = 0;

/* Enough of a plain load to know its length and where it
 * lands. Anything else is refused, and a wrong length
 * fails the null check below rather than resuming. */
static int DecodeLoad(const uint8_t *p, int *len, int *dest) {
    int i = 0, rex = 0, mod, reg, rm;

    if ((p[i] & 0xF0) == 0x40) rex = p[i++];
    if (p[i] != 0x8B) return 0;
    i++;
    mod = p[i] >> 6;
    reg = (p[i] >> 3) & 7;
    rm  = p[i] & 7;
    i++;
    if (mod == 3) return 0;

    /* A SIB with base 101 and mod 00 carries a disp32 of
     * its own, which is the one length trap here.
     */
    if (rm == 4) {
        int sib = p[i];
        i++;
        if (mod == 0 && (sib & 7) == 5) i += 4;
    }
    if (mod == 0 && rm == 5) i += 4;
    else if (mod == 1) i += 1;
    else if (mod == 2) i += 4;

    *len = i;
    *dest = ((rex & 4) ? 8 : 0) | reg;
    return *dest != 4;
}

/* The engine's own null check, in its own bytes: test the
 * destination against itself and branch on the result.
 * Its presence is the proof that a null is expected. */
static int NullChecked(const uint8_t *p, int dest) {
    int rex = 0x48 | ((dest & 8) ? 0x05 : 0);
    int modrm = 0xC0 | ((dest & 7) << 3) | (dest & 7);

    if (p[0] != rex || p[1] != 0x85 || p[2] != modrm) return 0;
    if (p[3] == 0x74 || p[3] == 0x75) return 1;
    if (p[3] == 0x0F && (p[4] == 0x84 || p[4] == 0x85)) return 1;
    return 0;
}

static DWORD64 *RegOf(CONTEXT *c, int i) {
    switch (i) {
    case 0:  return &c->Rax;  case 1:  return &c->Rcx;
    case 2:  return &c->Rdx;  case 3:  return &c->Rbx;
    case 5:  return &c->Rbp;  case 6:  return &c->Rsi;
    case 7:  return &c->Rdi;  case 8:  return &c->R8;
    case 9:  return &c->R9;   case 10: return &c->R10;
    case 11: return &c->R11;  case 12: return &c->R12;
    case 13: return &c->R13;  case 14: return &c->R14;
    case 15: return &c->R15;
    }
    return NULL;
}

/* A read that faulted into a load the engine immediately
 * null checks: give it the null it already handles and
 * step over. Resuming takes a path the engine wrote. */
static int CanHeal(EXCEPTION_POINTERS *ep, int *len, int *dest) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    uint8_t code[24];

    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return 0;
    if (er->NumberParameters < 2) return 0;
    if (er->ExceptionInformation[0] != 0) return 0;

    if (!ShReadMem(ep->ContextRecord->Rip, code, sizeof(code)))
        return 0;
    if (!DecodeLoad(code, len, dest)) return 0;
    if (*len < 2 || *len > 15) return 0;
    if (!NullChecked(code + *len, *dest)) return 0;
    return RegOf(ep->ContextRecord, *dest) != NULL;
}

static void Heal(CONTEXT *c, int len, int dest) {
    *RegOf(c, dest) = 0;
    c->Rip += (uint64_t)len;
    InterlockedIncrement(&g_healed);
}

static int Fatal(uint32_t code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return 1;
    }
    return 0;
}

/* Vectored, so a crash the game's own handlers swallow on
 * the way down is still recorded. Reporting only: normal
 * handling continues unless freezing is on. */
static LONG CALLBACK CrashVeh(EXCEPTION_POINTERS *ep) {
    int len = 0, dest = 0, heal;

    if (!Fatal(ep->ExceptionRecord->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;

    /* Decided before reporting, applied after, so the
     * entry describes the fault and not our repair.
     */
    heal = CanHeal(ep, &len, &dest);
    Report(ep, heal ? 2 : 0);
    if (heal) {
        Heal(ep->ContextRecord, len, dest);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_freeze) for (;;) Sleep(60000);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Nothing handled it, so this one really was the end. */
static LONG WINAPI CrashUef(EXCEPTION_POINTERS *ep) {
    Report(ep, 1);
    if (g_freeze) for (;;) Sleep(60000);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* Reporting only by default. A first chance handler sees
 * every fault before the game does, including the ones it
 * throws on purpose, and healing those breaks it. */
static int g_intercept = 0;
static void *g_veh = NULL;

void ShCrashStartup(void) {
    SetUnhandledExceptionFilter(CrashUef);
}

/** Watch faults first and resume ones with a null path.
 *  Off by default: it breaks titles that fault on purpose.
 */
SH_API int ShSetCrashIntercept(int on) {
    if (on && !g_veh) {
        g_veh = AddVectoredExceptionHandler(1, CrashVeh);
        g_intercept = g_veh ? 1 : 0;
        return g_intercept;
    }
    if (!on && g_veh) {
        RemoveVectoredExceptionHandler(g_veh);
        g_veh = NULL;
        g_intercept = 0;
    }
    return 1;
}

SH_API int ShCrashInterceptOn(void) {
    return g_intercept;
}

/* Crash reporters install their own filter, so the slot
 * is claimed back on the state watcher's cadence.
 */
void ShCrashRearm(void) {
    SetUnhandledExceptionFilter(CrashUef);
}

/** How many crashes have been caught this session. */
SH_API int ShCrashCount(void) {
    return (int)g_caught;
}

/** How many were resumed on the engine's own null path. */
SH_API int ShCrashHealed(void) {
    return (int)g_healed;
}

/** Park the faulting thread instead of letting it die. */
SH_API int ShSetCrashFreeze(int on) {
    g_freeze = on ? 1 : 0;
    ShSetError(SH_OK);
    return 1;
}

SH_API int ShCrashFreezeOn(void) {
    return g_freeze;
}

/** The first report of the session, as text. */
SH_API int ShCrashReport(char *buf, int len) {
    int n;

    if (!buf || len < 2) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (!g_haveFirst) { ShSetError(SH_ERR_NO_CANDIDATE); return 0; }
    n = (int)strlen(g_first);
    if (n > len - 1) n = len - 1;
    memcpy(buf, g_first, (size_t)n);
    buf[n] = 0;
    ShSetError(SH_OK);
    return 1;
}
