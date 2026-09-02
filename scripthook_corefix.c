/* CPU core-count / affinity fix, backported into the ScriptHook
 * loader from https://github.com/kartalbas/wildlands-corecount-fix
 * (v4, MIT). AnvilNext (2017) dead-locks on the loading screen on
 * CPUs it sees as "too many" or hybrid: the engine sizes internal
 * structures from the reported logical-processor count AND spreads
 * its own workers across every core via
 * SetThreadAffinityMask(0xFFFFFFFF), so its load barrier waits on
 * threads that never finish. Reported triggers include Intel hybrid
 * CPUs (P-core + E-core) and, in some cases, plain CPUs with SMT
 * (Hyper-Threading) enabled.
 *
 * Enabled by:
 *
 *   [loader]
 *   cpu_ecore_off=1        (0 = off, the default)
 *   cpu_ht_off=1           (0 = off, the default; any CPU)
 *   cpu_cores=8            (cap on the reported/clamped CPU count)
 *
 * cpu_ht_off keeps one logical thread per PHYSICAL core (the SMT0 of
 * each core) on ANY CPU -- Intel, AMD, hybrid or not. It is the
 * general fix for "hangs even without E-cores, only with SMT on":
 * the game then sees as many processors as there are physical cores.
 *
 * cpu_ecore_off targets Intel 12th-gen+ hybrid CPUs: the E-cores are
 * dropped so the game only runs on the P-cores. The P-core set is
 * auto-detected via CPUID leaf 0x1A (Core Type) by pinning this
 * thread to each logical processor and reading its type.
 *
 * cpu_cores caps the final set: of the kept CPUs the LOWEST cpu_cores
 * logical processors are reported/clamped to (all of them when there
 * are fewer than cpu_cores, or when cpu_cores=0 is set). Missing or
 * out-of-range cpu_cores falls back to 8.
 *
 * When both switches are off, not one API is touched and the game runs
 * exactly as before. When on, only this process is told the reduced
 * processor count and every affinity call is clamped -- no real core
 * is ever disabled or parked for the rest of the system.
 *
 * Uses MinHook (third_party/minhook, MIT), compiled into dinput8.dll.
 */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

#define SH_BUILD 1
#include "scripthook.h"
#include "log.h"
#include "third_party/minhook/include/MinHook.h"

/* The affinity mask the game is allowed to use. Auto-set to the
 * Intel P-cores when they can be detected, otherwise the low
 * cpu_cores CPUs. */
static ULONG_PTR g_keepmask = 0xFF;
static DWORD     g_max_cores = 8;
/* Explicit cpu_cores=0 in the ini: keep every detected P-core. */
static int       g_autoAll = 0;
static volatile LONG g_installed = 0;

static ULONG_PTR lowmask(DWORD n)
{
    if (n >= (DWORD)(sizeof(ULONG_PTR) * 8))
        return ~(ULONG_PTR)0;
    return ((ULONG_PTR)1 << n) - 1;
}

static ULONG_PTR core_keepmask(void)
{
    return g_keepmask;
}

static DWORD popcount_ptr(ULONG_PTR x)
{
    DWORD c = 0;
    while (x) { c += (DWORD)(x & 1u); x >>= 1; }
    return c;
}

/* Keep only the n lowest-numbered set bits of mask. */
static ULONG_PTR keep_lowest_bits(ULONG_PTR mask, DWORD n)
{
    ULONG_PTR out = 0;
    DWORD i;
    for (i = 0; i < (DWORD)(sizeof(ULONG_PTR) * 8) && n > 0; i++) {
        if (mask & ((ULONG_PTR)1 << i)) {
            out |= (ULONG_PTR)1 << i;
            n--;
        }
    }
    return out;
}

static DWORD clamp_cores(DWORD v) { return (v > g_max_cores) ? g_max_cores : v; }

/* ========================================================================= */
/* CPUID helper (MSVC uses __cpuidex, GCC/Clang use __cpuid_count).          */
/* ========================================================================= */
static void cpuid_raw(unsigned leaf, unsigned sub,
                      unsigned *a, unsigned *b, unsigned *c, unsigned *d)
{
#if defined(_MSC_VER)
    int info[4];
    __cpuidex(info, (int)leaf, (int)sub);
    *a = (unsigned)info[0]; *b = (unsigned)info[1];
    *c = (unsigned)info[2]; *d = (unsigned)info[3];
#else
    __cpuid_count(leaf, sub, *a, *b, *c, *d);
#endif
}

/* Detect the set of Intel P-cores via CPUID leaf 0x1A. Returns 1 and
 * fills *mask with the logical processors whose Core Type is 0x40
 * (Intel Core / P-core), 0 when not applicable. Each logical
 * processor is pinned to this thread in turn so the leaf reports the
 * type of that exact CPU. Runs before any hook is installed, so the
 * real SetThreadAffinityMask is still in effect. */
static int detect_p_cores(ULONG_PTR *mask)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    unsigned maxleaf;
    DWORD total, i;
    DWORD_PTR proc = 0, sys = 0, old;
    ULONG_PTR pm = 0;
    DWORD count = 0;
    HANDLE h = GetCurrentThread();

    /* Vendor: leaf 0 -> EBX 'Genu' EDX 'ineI' ECX 'ntel'. */
    cpuid_raw(0, 0, &a, &b, &c, &d);
    if (!(b == 0x756E6547u && d == 0x49656E69u && c == 0x6C65746Eu))
        return 0;
    maxleaf = a;
    if (maxleaf < 0x1A)
        return 0;                   /* pre-12th-gen: no Core Type leaf */

    total = GetMaximumProcessorCount(0);
    if (total == 0 || total > (DWORD)(sizeof(ULONG_PTR) * 8))
        return 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys))
        proc = (DWORD_PTR)lowmask(total);

    for (i = 0; i < total; i++) {
        DWORD_PTR bit = (DWORD_PTR)1 << i;
        int spin, landed = 0;

        if (!(proc & bit)) continue;
        old = SetThreadAffinityMask(h, bit);
        /* With the mask now a single CPU the scheduler must put us
         * there; wait until it really does so the leaf reports this
         * exact processor's type. */
        for (spin = 0; spin < 100; spin++) {
            if (GetCurrentProcessorNumber() == i) { landed = 1; break; }
            Sleep(1);
        }
        if (landed) {
            cpuid_raw(0x1A, 0, &a, &b, &c, &d);
            if (((a >> 24) & 0xFFu) == 0x40u) { pm |= bit; count++; }
        }
        SetThreadAffinityMask(h, old ? old : proc);
    }

    if (count == 0 || count == total)
        return 0;                   /* no E-cores seen: not a hybrid */
    *mask = pm;
    return 1;
}

/* One logical thread per physical core (SMT0 of each core): the
 * affinity mask that makes the game see/use only physical cores,
 * equivalent to disabling Hyper-Threading. CPU-agnostic -- works on
 * Intel, AMD, with or without E-cores. Runs before any hook, so the
 * real GetLogicalProcessorInformationEx is still in effect.
 * Returns 1 and fills *mask, 0 when the topology cannot be read
 * (single mask word cannot represent a machine with >64 CPUs). */
static int detect_smt0_mask(ULONG_PTR *mask)
{
    DWORD len = 0;
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *buf = NULL, *rec;
    ULONG_PTR m = 0;
    DWORD off;
    BOOL ok;

    ok = GetLogicalProcessorInformationEx(RelationProcessorCore,
                                          NULL, &len);
    if (!ok || len == 0 || len > 0x100000)
        return 0;
    buf = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)malloc(len);
    if (!buf)
        return 0;
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                          buf, &len)) {
        free(buf);
        return 0;
    }

    off = 0;
    while (off + sizeof(*rec) <= len) {
        ULONG_PTR any = 0, low;
        WORD g;
        rec = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)((BYTE *)buf + off);
        if (rec->Size == 0 || off + rec->Size > len)
            break;
        if (rec->Relationship == RelationProcessorCore) {
            /* Lowest-numbered logical processor of the core = its SMT0. */
            for (g = 0; g < rec->Processor.GroupCount; g++) {
                ULONG_PTR gm = rec->Processor.GroupMask[g].Mask;
                if (g == 0) {
                    if (gm) {
                        low = 1;
                        while (!(gm & low)) low <<= 1;
                        any = low;
                    }
                } else if (gm) {
                    /* Core lives in a group beyond word 0; the single
                     * keepmask word cannot represent it, so bail. */
                    free(buf);
                    return 0;
                }
            }
            if (any) m |= any;
        }
        off += rec->Size;
    }
    free(buf);

    if (m == 0)
        return 0;
    *mask = m;
    return 1;
}

/* --------------------------------------------------------------------- */
/* Light, per-call-site-throttled logger into scripthook_corefix.log.     */
/* --------------------------------------------------------------------- */
static volatile LONG g_log_lines = 0;

static void core_log(const char *fmt, ...)
{
    va_list ap;

    if (InterlockedIncrement(&g_log_lines) > 300)
        return;
    va_start(ap, fmt);
    Logv(fmt, ap);
    va_end(ap);
}

/* Log only the first `cap` firings of a call site. */
static LONG note(volatile LONG *counter, LONG cap, const char *fmt, ...)
{
    LONG idx = InterlockedIncrement(counter);
    if (idx <= cap) {
        char buf[256];
        va_list ap; va_start(ap, fmt);
        _vsnprintf(buf, sizeof(buf) - 1, fmt, ap); buf[sizeof(buf) - 1] = '\0';
        va_end(ap);
        core_log("%s", buf);
    }
    return idx;
}

/* ========================================================================= */
/* Real function pointers.                                                    */
/* ========================================================================= */
typedef void      (WINAPI *pfn_GetSystemInfo)(LPSYSTEM_INFO);
typedef DWORD     (WINAPI *pfn_GetActiveProcessorCount)(WORD);
typedef DWORD     (WINAPI *pfn_GetMaximumProcessorCount)(WORD);
typedef WORD      (WINAPI *pfn_GetActiveProcessorGroupCount)(void);
typedef BOOL      (WINAPI *pfn_GLPI)(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD);
typedef BOOL      (WINAPI *pfn_GLPIEx)(LOGICAL_PROCESSOR_RELATIONSHIP, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, PDWORD);
typedef BOOL      (WINAPI *pfn_GetProcessAffinityMask)(HANDLE, PDWORD_PTR, PDWORD_PTR);
typedef BOOL      (WINAPI *pfn_SetProcessAffinityMask)(HANDLE, DWORD_PTR);
typedef DWORD_PTR (WINAPI *pfn_SetThreadAffinityMask)(HANDLE, DWORD_PTR);
typedef DWORD     (WINAPI *pfn_SetThreadIdealProcessor)(HANDLE, DWORD);
typedef BOOL      (WINAPI *pfn_SetThreadIdealProcessorEx)(HANDLE, PPROCESSOR_NUMBER, PPROCESSOR_NUMBER);
typedef LONG      (WINAPI *pfn_NtQSI)(ULONG, PVOID, ULONG, PULONG);
typedef LONG      (WINAPI *pfn_NtSIP)(HANDLE, ULONG, PVOID, ULONG);

static pfn_GetSystemInfo                real_GetSystemInfo;
static pfn_GetSystemInfo                real_GetNativeSystemInfo;
static pfn_GetActiveProcessorCount      real_GetActiveProcessorCount;
static pfn_GetMaximumProcessorCount     real_GetMaximumProcessorCount;
static pfn_GetActiveProcessorGroupCount real_GetActiveProcessorGroupCount;
static pfn_GLPI                         real_GLPI;
static pfn_GLPIEx                       real_GLPIEx;
static pfn_GetProcessAffinityMask       real_GetProcessAffinityMask;
static pfn_SetProcessAffinityMask       real_SetProcessAffinityMask;
static pfn_SetThreadAffinityMask        real_SetThreadAffinityMask;
static pfn_SetThreadIdealProcessor      real_SetThreadIdealProcessor;
static pfn_SetThreadIdealProcessorEx    real_SetThreadIdealProcessorEx;
static pfn_NtQSI                        real_NtQSI;
static pfn_NtSIP                        real_NtSIP;

static volatile LONG c_GSI, c_GNSI, c_GAPC, c_GMPC, c_GAPGC, c_GLPI, c_GLPIEx,
                     c_GPAM, c_SPAM, c_STAM, c_STIP, c_STIPEx;

/* ========================================================================= */
/* COUNT clamps.                                                              */
/* ========================================================================= */
static void WINAPI hook_GetSystemInfo(LPSYSTEM_INFO si)
{
    real_GetSystemInfo(si);
    if (si && si->dwNumberOfProcessors > g_max_cores) si->dwNumberOfProcessors = g_max_cores;
    note(&c_GSI, 1, "  >> engine called GetSystemInfo");
}
static void WINAPI hook_GetNativeSystemInfo(LPSYSTEM_INFO si)
{
    real_GetNativeSystemInfo(si);
    if (si && si->dwNumberOfProcessors > g_max_cores) si->dwNumberOfProcessors = g_max_cores;
    note(&c_GNSI, 1, "  >> engine called GetNativeSystemInfo");
}
static DWORD WINAPI hook_GetActiveProcessorCount(WORD g)
{
    note(&c_GAPC, 1, "  >> engine called GetActiveProcessorCount");
    return clamp_cores(real_GetActiveProcessorCount(g));
}
static DWORD WINAPI hook_GetMaximumProcessorCount(WORD g)
{
    note(&c_GMPC, 1, "  >> engine called GetMaximumProcessorCount");
    return clamp_cores(real_GetMaximumProcessorCount(g));
}
static WORD WINAPI hook_GetActiveProcessorGroupCount(void)
{
    (void)real_GetActiveProcessorGroupCount;
    note(&c_GAPGC, 1, "  >> engine called GetActiveProcessorGroupCount");
    return 1;
}

/* ========================================================================= */
/* TOPOLOGY trims (keep only CPUs 0..MAX_CORES-1).                            */
/* ========================================================================= */
static BOOL WINAPI hook_GLPI(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buf, PDWORD len)
{
    BOOL ok = real_GLPI(buf, len);
    LONG n = InterlockedIncrement(&c_GLPI);
    if (!ok || !buf || !len) return ok;
    ULONG_PTR keep = core_keepmask();
    DWORD count = *len / (DWORD)sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION), w = 0;
    for (DWORD r = 0; r < count; r++) {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION e = buf[r];
        e.ProcessorMask &= keep;
        if (e.ProcessorMask == 0 && (e.Relationship == RelationProcessorCore || e.Relationship == RelationCache))
            continue;
        buf[w++] = e;
    }
    *len = w * (DWORD)sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    if (n == 1) core_log("  >> GetLogicalProcessorInformation trimmed %lu -> %lu entries", (unsigned long)count, (unsigned long)w);
    return TRUE;
}

static BOOL WINAPI hook_GLPIEx(LOGICAL_PROCESSOR_RELATIONSHIP rel, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buf, PDWORD len)
{
    BOOL ok = real_GLPIEx(rel, buf, len);
    LONG n = InterlockedIncrement(&c_GLPIEx);
    if (!ok || !buf || !len) return ok;
    ULONG_PTR keep = core_keepmask();
    BYTE *base = (BYTE *)buf;
    DWORD total = *len, roff = 0, woff = 0, dropped = 0;
    while (roff < total) {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *rec = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(base + roff);
        DWORD size = rec->Size;
        if (size == 0 || roff + size > total) break;
        BOOL keepRec = TRUE;
        if (rec->Relationship == RelationProcessorCore) {
            ULONG_PTR any = 0;
            for (WORD g = 0; g < rec->Processor.GroupCount; g++) { rec->Processor.GroupMask[g].Mask &= keep; any |= rec->Processor.GroupMask[g].Mask; }
            if (any == 0) keepRec = FALSE;
        } else if (rec->Relationship == RelationGroup) {
            for (WORD g = 0; g < rec->Group.ActiveGroupCount; g++) {
                rec->Group.GroupInfo[g].ActiveProcessorMask &= keep;
                DWORD pc = popcount_ptr(rec->Group.GroupInfo[g].ActiveProcessorMask);
                rec->Group.GroupInfo[g].ActiveProcessorCount = (BYTE)pc;
                rec->Group.GroupInfo[g].MaximumProcessorCount = (BYTE)pc;
            }
        }
        if (keepRec) { if (woff != roff) memmove(base + woff, base + roff, size); woff += size; }
        else dropped++;
        roff += size;
    }
    *len = woff;
    if (n == 1) core_log("  >> GetLogicalProcessorInformationEx dropped %lu core record(s)", (unsigned long)dropped);
    return TRUE;
}

/* ========================================================================= */
/* AFFINITY: keep this process' threads on CPUs 0..MAX_CORES-1 and stop the    */
/* game from resetting itself back onto the E-cores. This is the key fix.      */
/* ========================================================================= */
static BOOL WINAPI hook_GetProcessAffinityMask(HANDLE h, PDWORD_PTR proc, PDWORD_PTR sys)
{
    BOOL ok = real_GetProcessAffinityMask(h, proc, sys);
    ULONG_PTR keep = core_keepmask();
    if (proc) *proc = keep;
    if (sys)  *sys  = keep;
    note(&c_GPAM, 3, "  >> GetProcessAffinityMask -> 0x%zX", (size_t)keep);
    (void)ok;
    return TRUE;
}
static BOOL WINAPI hook_SetProcessAffinityMask(HANDLE h, DWORD_PTR mask)
{
    ULONG_PTR keep = core_keepmask();
    DWORD_PTR m = (DWORD_PTR)(mask & keep);
    if (m == 0) m = (DWORD_PTR)keep;
    note(&c_SPAM, 3, "  >> SetProcessAffinityMask requested=0x%zX forced=0x%zX", (size_t)mask, (size_t)m);
    return real_SetProcessAffinityMask(h, m);
}
static DWORD_PTR WINAPI hook_SetThreadAffinityMask(HANDLE h, DWORD_PTR mask)
{
    ULONG_PTR keep = core_keepmask();
    DWORD_PTR m = (DWORD_PTR)(mask & keep);
    if (m == 0) m = (DWORD_PTR)keep;
    note(&c_STAM, 5, "  >> SetThreadAffinityMask requested=0x%zX forced=0x%zX", (size_t)mask, (size_t)m);
    return real_SetThreadAffinityMask(h, m);
}
static DWORD WINAPI hook_SetThreadIdealProcessor(HANDLE h, DWORD idp)
{
    DWORD v = (idp >= 64) ? idp : (idp > g_max_cores - 1 ? g_max_cores - 1 : idp);
    note(&c_STIP, 3, "  >> SetThreadIdealProcessor %lu -> %lu", (unsigned long)idp, (unsigned long)v);
    return real_SetThreadIdealProcessor(h, v);
}
static BOOL WINAPI hook_SetThreadIdealProcessorEx(HANDLE h, PPROCESSOR_NUMBER ideal, PPROCESSOR_NUMBER prev)
{
    PROCESSOR_NUMBER local;
    if (ideal) { local = *ideal; local.Group = 0; if (local.Number > (BYTE)(g_max_cores - 1)) local.Number = (BYTE)(g_max_cores - 1); }
    note(&c_STIPEx, 3, "  >> SetThreadIdealProcessorEx clamped");
    return real_SetThreadIdealProcessorEx(h, ideal ? &local : NULL, prev);
}

/* ========================================================================= */
/* ntdll direct-call path (bypasses kernel32).                                */
/* ========================================================================= */
typedef struct _SBI {
    ULONG Reserved, TimerResolution, PageSize, NumberOfPhysicalPages,
          LowestPhysicalPageNumber, HighestPhysicalPageNumber, AllocationGranularity;
    ULONG_PTR MinimumUserModeAddress, MaximumUserModeAddress, ActiveProcessorsAffinityMask;
    CCHAR NumberOfProcessors;
} SBI;

#define SYS_BASIC_INFO 0
#define SYS_LPI        73
static BOOL  g_ntqsi_seen[512];

static LONG WINAPI hook_NtQSI(ULONG cls, PVOID buf, ULONG len, PULONG retlen)
{
    LONG st = real_NtQSI(cls, buf, len, retlen);
    if (cls < 512 && !g_ntqsi_seen[cls]) { g_ntqsi_seen[cls] = TRUE; core_log("  NtQuerySystemInformation class=%lu (first seen)", (unsigned long)cls); }

    if (st == 0 && buf) {
        if (cls == SYS_BASIC_INFO && len >= sizeof(SBI)) {
            SBI *sbi = (SBI *)buf;
            if ((DWORD)(unsigned char)sbi->NumberOfProcessors > g_max_cores) sbi->NumberOfProcessors = (CCHAR)g_max_cores;
            sbi->ActiveProcessorsAffinityMask = core_keepmask();
        } else if (cls == SYS_LPI) {
            ULONG_PTR keep = core_keepmask();
            DWORD count = (retlen ? *retlen : len) / (DWORD)sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION), w = 0;
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION *a = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION *)buf;
            for (DWORD r = 0; r < count; r++) {
                SYSTEM_LOGICAL_PROCESSOR_INFORMATION e = a[r];
                e.ProcessorMask &= keep;
                if (e.ProcessorMask == 0 && (e.Relationship == RelationProcessorCore || e.Relationship == RelationCache)) continue;
                a[w++] = e;
            }
            if (retlen) *retlen = w * (DWORD)sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
        }
    }
    return st;
}

#define PROC_AFFINITY_MASK_CLASS 21
static LONG WINAPI hook_NtSIP(HANDLE proc, ULONG cls, PVOID info, ULONG len)
{
    if (cls == PROC_AFFINITY_MASK_CLASS && info && len >= sizeof(ULONG_PTR)) {
        ULONG_PTR keep = core_keepmask();
        ULONG_PTR m = (*(ULONG_PTR *)info) & keep;
        if (m == 0) m = keep;
        return real_NtSIP(proc, cls, &m, (ULONG)sizeof(m));
    }
    return real_NtSIP(proc, cls, info, len);
}

/* ========================================================================= */
/* Configuration + installation.                                             */
/* ========================================================================= */
/* cpu_cores from the main scripthook.ini: the logical-processor count
 * the game is told (the report target).
 *   - key missing / not a number  -> default 8
 *   - explicit 0                  -> keep every detected P-core (no trim)
 *   - 1..256                      -> that many
 * The cpu_ecore_off / cpu_ht_off switches are read directly in
 * ShCoreFixStartup; this function only parses cpu_cores.
 * (ShConfigGetInt with def=-1 tells the three apart: missing or junk
 * returns -1, a real "0" returns 0.) */
static void load_config(void)
{
    int v;

    g_autoAll = 0;
    v = ShConfigGetInt("loader", "cpu_cores", -1);
    if (v == 0) {
        g_autoAll = 1;
        g_max_cores = 8;          /* fallback until auto-detect runs */
    } else if (v >= 1 && v <= 256) {
        g_max_cores = (DWORD)v;
    } else {
        g_max_cores = 8;
    }
    g_keepmask = lowmask(g_max_cores);
}

static int hook_api(const wchar_t *mod, const char *name, LPVOID detour, LPVOID *real_out)
{
    MH_STATUS s = MH_CreateHookApi(mod, name, detour, real_out);
    if (s != MH_OK) { core_log("  hook %-32s FAILED (MH_STATUS=%d)", name, (int)s); return 0; }
    return 1;
}

static int install_hooks(void)
{
    if (MH_Initialize() != MH_OK) { core_log("MH_Initialize failed"); return 0; }
    int ok = 1;
    /* Count */
    ok &= hook_api(L"kernel32", "GetSystemInfo",                (LPVOID)hook_GetSystemInfo,                (LPVOID *)&real_GetSystemInfo);
    ok &= hook_api(L"kernel32", "GetNativeSystemInfo",          (LPVOID)hook_GetNativeSystemInfo,          (LPVOID *)&real_GetNativeSystemInfo);
    ok &= hook_api(L"kernel32", "GetActiveProcessorCount",      (LPVOID)hook_GetActiveProcessorCount,      (LPVOID *)&real_GetActiveProcessorCount);
    ok &= hook_api(L"kernel32", "GetMaximumProcessorCount",     (LPVOID)hook_GetMaximumProcessorCount,     (LPVOID *)&real_GetMaximumProcessorCount);
    ok &= hook_api(L"kernel32", "GetActiveProcessorGroupCount", (LPVOID)hook_GetActiveProcessorGroupCount, (LPVOID *)&real_GetActiveProcessorGroupCount);
    /* Topology */
    ok &= hook_api(L"kernel32", "GetLogicalProcessorInformation",   (LPVOID)hook_GLPI,   (LPVOID *)&real_GLPI);
    ok &= hook_api(L"kernel32", "GetLogicalProcessorInformationEx", (LPVOID)hook_GLPIEx, (LPVOID *)&real_GLPIEx);
    /* Affinity (the key fix) */
    ok &= hook_api(L"kernel32", "GetProcessAffinityMask",   (LPVOID)hook_GetProcessAffinityMask,   (LPVOID *)&real_GetProcessAffinityMask);
    ok &= hook_api(L"kernel32", "SetProcessAffinityMask",   (LPVOID)hook_SetProcessAffinityMask,   (LPVOID *)&real_SetProcessAffinityMask);
    ok &= hook_api(L"kernel32", "SetThreadAffinityMask",    (LPVOID)hook_SetThreadAffinityMask,    (LPVOID *)&real_SetThreadAffinityMask);
    ok &= hook_api(L"kernel32", "SetThreadIdealProcessor",  (LPVOID)hook_SetThreadIdealProcessor,  (LPVOID *)&real_SetThreadIdealProcessor);
    ok &= hook_api(L"kernel32", "SetThreadIdealProcessorEx",(LPVOID)hook_SetThreadIdealProcessorEx,(LPVOID *)&real_SetThreadIdealProcessorEx);
    /* ntdll direct path (best-effort) */
    hook_api(L"ntdll", "NtQuerySystemInformation", (LPVOID)hook_NtQSI, (LPVOID *)&real_NtQSI);
    hook_api(L"ntdll", "NtSetInformationProcess",  (LPVOID)hook_NtSIP, (LPVOID *)&real_NtSIP);

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { core_log("MH_EnableHook failed"); ok = 0; }
    return ok;
}

/* Called from loader DllMain after the real dinput8 is loaded and before
 * any plugin loads. Reads scripthook.ini itself so no ordering between
 * this and the loader thread matters. No-op unless cpu_ecore_off=1
 * or cpu_ht_off=1. */
void ShCoreFixStartup(void)
{
    int wantEcore, wantHt;
    ULONG_PTR keep = 0;

    if (InterlockedCompareExchange(&g_installed, 1, 0))
        return;

    LogInit("scripthook_corefix.log");

    ShConfigInit();
    wantEcore = ShConfigGetBool("loader", "cpu_ecore_off", 0);
    wantHt    = ShConfigGetBool("loader", "cpu_ht_off", 0);
    if (!wantEcore && !wantHt) {
        Log("corefix: disabled (set [loader] cpu_ecore_off=1 and/or "
            "cpu_ht_off=1 to enable)");
        return;
    }
    load_config();

    /* Two independent trims, applied in order:
     *   1. cpu_ht_off    -> one logical thread per physical core (SMT0),
     *                       works on any CPU (AMD included).
     *   2. cpu_ecore_off -> Intel 12th-gen+ hybrid: keep only P-cores
     *                       (the E-cores are hidden from the game).
     * With both on, the result is P-core SMT0 threads (strictest).
     * When no trim succeeds the low cpu_cores CPUs are the fallback.
     * cpu_cores caps the final set (cpu_cores=0 keeps all of it). */
    if (wantHt) {
        ULONG_PTR sm = 0;
        if (detect_smt0_mask(&sm)) {
            keep = sm;
            Log("corefix: ht_off: one thread per physical core, "
                "mask 0x%zX", (size_t)sm);
        } else {
            keep = lowmask(g_max_cores);
            Log("corefix: ht_off: topology read failed, keeping low %lu "
                "logical processors", (unsigned long)g_max_cores);
        }
    }
    if (wantEcore) {
        ULONG_PTR pm = 0;
        if (detect_p_cores(&pm)) {
            if (keep) keep &= pm;   /* both: SMT0 within P-cores */
            else      keep = pm;
            Log("corefix: ecore_off: P-core mask 0x%zX", (size_t)pm);
        } else if (!keep) {
            keep = lowmask(g_max_cores);
            Log("corefix: ecore_off: no hybrid CPU detected, keeping "
                "low %lu logical processors (cpu_cores)",
                (unsigned long)g_max_cores);
        }
    }

    /* cpu_cores caps the result; 0 means keep everything found. */
    if (keep && !g_autoAll) {
        DWORD n = popcount_ptr(keep);
        if (n > g_max_cores) {
            keep = keep_lowest_bits(keep, g_max_cores);
            Log("corefix: capped to lowest %lu of the kept set "
                "(cpu_cores)", (unsigned long)g_max_cores);
        }
    }
    g_keepmask = keep ? keep : lowmask(g_max_cores);
    g_max_cores = popcount_ptr(g_keepmask);
    Log("corefix: final keep mask 0x%zX, reporting %lu processors",
        (size_t)g_keepmask, (unsigned long)g_max_cores);

    {
        int ok = install_hooks();
        Log("corefix: affinity forced to 0x%zX, hooks=%s",
            (size_t)g_keepmask, ok ? "ENABLED" : "INCOMPLETE");
    }
}
