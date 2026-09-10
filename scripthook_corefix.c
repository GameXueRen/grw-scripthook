/* CPU scheduling, by stage of the game's start up, backported into the
 * ScriptHook loader from
 * https://github.com/kartalbas/wildlands-corecount-fix (v4, MIT).
 *
 * AnvilNext (2017) can stall in two different places, and the community
 * workaround for each is a different set:
 *
 *   the logo screen       - takes forever. Dropping SMT is what makes
 *                           it pass at once.
 *   the window's loading  - hangs on "loading" forever. Dropping the
 *                           E-cores fixes it on some machines; on others
 *                           (Win11 trims the process to the P-cores by
 *                           itself) forcing ALL cores back is what does.
 *   in play               - occasional stutter. Dropping SMT and/or
 *                           processor 0 helps.
 *
 * So there are three dials, all from [loader] of scripthook.ini, and
 * each one is read when its stage begins:
 *
 *   cpu_boot=0      the logo screen
 *   cpu_window=0    the game window: loading, main menu, lobby
 *   cpu_play=0      in play
 *
 * The first two are told apart by the game's own windows, not by a timer
 * or by the engine's state: the logo screen is a window of its own whose
 * title carries the registered mark mis-encoded ("Ghost Recon?Wildlands")
 * while the main window that follows has it right. "The main window is
 * not up yet" is therefore exact, and a dial can be aimed at each stage
 * as precisely as the player sees them.
 *
 *   value  meaning
 *   0      leave alone - set nothing, answer every query with the truth
 *          and let the system schedule it, its own trimming included
 *   1      all cores - force the process onto every processor the
 *          machine has and tell the engine the same; this is the one
 *          that undoes a trim the system did on its own
 *   2      SMT off - one thread per physical core
 *   3      E-cores off - Intel 12th-gen+ hybrid only; anywhere else it
 *          is reported as not applicable and NOTHING is trimmed
 *   4..    combinations; the play dial also has processor 0
 *
 * cpu_cores=N caps the play stage at its lowest N processors (0 or a
 * missing key = no cap). It applies to play alone: trimming the set
 * while the game is still starting is a good way to make it not start,
 * and the stutter it is for is a play-time thing anyway.
 *
 * Why the stages are the point: the engine's start up walks the
 * processors by index - the log has it setting the process mask to 0x1,
 * then 0x2, then 0x4 - so a set with holes in it (SMT off gives 0, 2,
 * 4 ...; a missing processor 0) makes that probe come back somewhere
 * other than where it asked, and the load screen never ends. "SMT off
 * at the logo only" is therefore a real and useful configuration, and
 * keeping it into the window stage is exactly what used to hang.
 *
 * Whatever a stage lands on, two rules hold inside it:
 *   - the count the engine is told is that set's size, always - a count
 *     larger than the set leaves a worker pinned to a processor that is
 *     not there, waited on forever;
 *   - an affinity request naming ONE processor is remapped onto the
 *     index'th processor we allow. Intersecting it would turn the
 *     engine's "probe this core" into "use any of five", which is the
 *     other half of the same hang.
 *
 * An active stage also sets the process affinity for real, so the
 * scheduler enforces it for every thread and Task Manager's own "Set
 * affinity" shows it. Only this process is ever affected: no core is
 * disabled or parked for the rest of the system.
 *
 * Uses MinHook (third_party/minhook, MIT), compiled into dinput8.dll.
 */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <stdlib.h>
#include <stdio.h>
#include <tlhelp32.h>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

#define SH_BUILD 1
#include "scripthook.h"
#include "log.h"
#include "third_party/minhook/include/MinHook.h"

/* ========================================================================= */
/* State.                                                                    */
/* ========================================================================= */
/* The three dials, one per stage, as read from the ini. */
enum {
    STAGE_BOOT = 0,     /* the logo screen                            */
    STAGE_WINDOW,       /* the game window: loading, menu, lobby      */
    STAGE_PLAY,         /* in play                                    */
    STAGE_COUNT
};

/* What a dial can ask for. The order is the ini's order, so the first
 * five values are exactly what every stage offers; the rest are the play
 * dial's processor-0 combinations, which the earlier stages have no use
 * for (the engine needs processor 0 while it starts).
 */
enum {
    D_LEAVE = 0,        /* touch nothing at all                       */
    D_ALL,              /* force every processor the machine has      */
    D_NO_SMT,           /* one thread per physical core               */
    D_NO_ECORE,         /* P-cores only (Intel hybrid)                */
    D_NO_SMT_ECORE,     /* both of those                              */
    D_NO_CPU0,          /* processor 0 out                            */
    D_NO_SMT_CPU0,
    D_NO_ECORE_CPU0,
    D_NO_SMT_ECORE_CPU0
};

/* Every processor the machine has, whatever this process is currently
 * allowed. A "force all cores" dial needs this rather than the process'
 * own mask, because the system may already have trimmed that one on its
 * own - Win11 does exactly that at start up, which is the thing the
 * dial exists to undo. */
static ULONG_PTR g_sysMask;
/* What the process' own affinity was when we arrived, so a stage that
 * asks for nothing can put it back instead of leaving the last stage's
 * trim in place: "leave alone" means as we found it, not as we left it. */
static ULONG_PTR g_procOrig;
static int       g_touched;                 /* we have set an affinity */
/* The set in force, and the count the engine is told. Zero in both
 * means "leave alone": every hook answers through unhooked. */
static ULONG_PTR g_keepMask;
static DWORD     g_reportCount;
static DWORD     g_coreCap;                 /* cpu_cores, 0 = no cap */
static int       g_dial[STAGE_COUNT];
/* The Intel P-core set and the SMT0 set, detected once at start up: the
 * detections pin this thread to each processor in turn, which is only
 * trustworthy before the hooks exist. */
static ULONG_PTR g_pMask;
static int       g_pMaskOk;
static ULONG_PTR g_smt0Mask;
static int       g_smt0Ok;
static int       g_stage = -1;              /* the dial in force */
static volatile LONG g_winLogged;           /* the window probe spoke once */
static volatile LONG g_logoLogged;          /* the logo window was named once */
static volatile LONG g_sawMain;             /* the main window has been up */
static ShCoreFixStatus g_status;
static volatile LONG g_installed = 0;

/* ---- mask helpers -------------------------------------------------------- */

static ULONG_PTR lowmask(DWORD n)
{
    if (n >= (DWORD)(sizeof(ULONG_PTR) * 8))
        return ~(ULONG_PTR)0;
    return ((ULONG_PTR)1 << n) - 1;
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

/* The index'th set bit of mask (0-based). Used to answer the
 * engine's ideal-processor hints: with processor 0 dropped the
 * allowed set has holes, and clamping an index to "the last core"
 * would pile every thread past the hole onto one processor. */
static DWORD nth_bit(ULONG_PTR mask, DWORD index)
{
    DWORD i;

    for (i = 0; i < (DWORD)(sizeof(ULONG_PTR) * 8); i++) {
        if (!(mask & ((ULONG_PTR)1 << i))) continue;
        if (index == 0) return i;
        index--;
    }
    return 0;
}

static const char *cf_state_name(int st)
{
    switch (st) {
    case SH_CF_OFF:           return "off";
    case SH_CF_APPLIED:       return "applied";
    case SH_CF_NA_NOT_INTEL:  return "not applicable (not an Intel CPU)";
    case SH_CF_NA_NO_ECORE:   return "not applicable (no E-cores seen)";
    case SH_CF_FAILED:        return "detection failed";
    case SH_CF_SKIPPED_EMPTY: return "skipped (it would empty the set)";
    default:                  return "?";
    }
}

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

/* Detect the Intel P-cores via CPUID leaf 0x1A. Returns one of the
 * SH_CF_* states: SH_CF_APPLIED and *mask filled with the logical
 * processors whose Core Type is 0x40 (Intel Core / P-core), or the
 * reason there is nothing to hide. Each logical processor is pinned
 * to this thread in turn so the leaf reports the type of that exact
 * CPU. Runs before any hook is installed, so the real
 * SetThreadAffinityMask is still in effect.
 *
 * The two "not applicable" answers are kept apart on purpose: a
 * Ryzen and an i5-9600 are both CPUs with no E-cores, but only the
 * first means the switch can never do anything here, and the log
 * says which one it saw. */
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
        return SH_CF_NA_NOT_INTEL;
    maxleaf = a;
    if (maxleaf < 0x1A)                 /* pre-12th-gen: no Core Type leaf */
        return SH_CF_NA_NO_ECORE;

    total = GetMaximumProcessorCount(0);
    if (total == 0 || total > (DWORD)(sizeof(ULONG_PTR) * 8))
        return SH_CF_FAILED;
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
        return SH_CF_NA_NO_ECORE;       /* every core is a P-core */
    *mask = pm;
    return SH_CF_APPLIED;
}

/* One logical thread per physical core (the SMT0 of each core): the
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

    /* First call sizes the buffer: FALSE with len set is the
     * documented answer, so a FALSE here is not a failure - reading
     * it as one is why this function never found a single core on
     * any machine and HT trimming never happened. */
    ok = GetLogicalProcessorInformationEx(RelationProcessorCore,
                                          NULL, &len);
    if (ok || len == 0 || len > 0x100000)
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
    /* Walk by each record's own Size, not by the union's: the union is
     * 80 bytes while a processor record is 48 plus its group masks, so
     * a bound of sizeof(*rec) stops before the last record - which is
     * how a six-core machine reported five physical cores and CPU 10
     * was never in the mask at all. */
    while (off + 8 <= len) {
        ULONG_PTR any = 0, low;
        WORD g;
        rec = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)((BYTE *)buf + off);
        if (rec->Size < 8 || off + rec->Size > len)
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
/* Every one of them answers with the count the engine is told, which is     */
/* the size of the set as it stood before the CPU0 cut.                       */
/* ========================================================================= */
/* A count is only ever lowered, and only once the trims have run:
 * zero would mean "not decided yet", and answering zero processors
 * is worse than answering the truth. */
static DWORD report_count(DWORD real)
{
    if (g_reportCount == 0 || real <= g_reportCount)
        return real;
    return g_reportCount;
}

static void WINAPI hook_GetSystemInfo(LPSYSTEM_INFO si)
{
    real_GetSystemInfo(si);
    if (si)
        si->dwNumberOfProcessors = report_count(si->dwNumberOfProcessors);
    note(&c_GSI, 1, "  >> engine called GetSystemInfo");
}
static void WINAPI hook_GetNativeSystemInfo(LPSYSTEM_INFO si)
{
    real_GetNativeSystemInfo(si);
    if (si)
        si->dwNumberOfProcessors = report_count(si->dwNumberOfProcessors);
    note(&c_GNSI, 1, "  >> engine called GetNativeSystemInfo");
}
static DWORD WINAPI hook_GetActiveProcessorCount(WORD g)
{
    note(&c_GAPC, 1, "  >> engine called GetActiveProcessorCount");
    return report_count(real_GetActiveProcessorCount(g));
}
static DWORD WINAPI hook_GetMaximumProcessorCount(WORD g)
{
    note(&c_GMPC, 1, "  >> engine called GetMaximumProcessorCount");
    return report_count(real_GetMaximumProcessorCount(g));
}
static WORD WINAPI hook_GetActiveProcessorGroupCount(void)
{
    (void)real_GetActiveProcessorGroupCount;
    note(&c_GAPGC, 1, "  >> engine called GetActiveProcessorGroupCount");
    return 1;
}

/* ========================================================================= */
/* TOPOLOGY trims (keep only what the affinity mask allows).                  */
/* ========================================================================= */
static BOOL WINAPI hook_GLPI(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buf, PDWORD len)
{
    BOOL ok = real_GLPI(buf, len);
    LONG n = InterlockedIncrement(&c_GLPI);
    if (!ok || !buf || !len || !g_keepMask) return ok;
    DWORD count = *len / (DWORD)sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION), w = 0;
    for (DWORD r = 0; r < count; r++) {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION e = buf[r];
        e.ProcessorMask &= g_keepMask;
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
    if (!ok || !buf || !len || !g_keepMask) return ok;
    BYTE *base = (BYTE *)buf;
    DWORD total = *len, roff = 0, woff = 0, dropped = 0;
    while (roff < total) {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *rec = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(base + roff);
        DWORD size = rec->Size;
        if (size == 0 || roff + size > total) break;
        BOOL keepRec = TRUE;
        if (rec->Relationship == RelationProcessorCore) {
            ULONG_PTR any = 0;
            for (WORD g = 0; g < rec->Processor.GroupCount; g++) { rec->Processor.GroupMask[g].Mask &= g_keepMask; any |= rec->Processor.GroupMask[g].Mask; }
            if (any == 0) keepRec = FALSE;
        } else if (rec->Relationship == RelationGroup) {
            for (WORD g = 0; g < rec->Group.ActiveGroupCount; g++) {
                rec->Group.GroupInfo[g].ActiveProcessorMask &= g_keepMask;
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
/* AFFINITY: keep the game's threads on the allowed set and stop the engine   */
/* from spreading itself back over everything.                                */
/* ========================================================================= */
static BOOL WINAPI hook_GetProcessAffinityMask(HANDLE h, PDWORD_PTR proc, PDWORD_PTR sys)
{
    BOOL ok = real_GetProcessAffinityMask(h, proc, sys);
    /* The system mask is a fact about the machine and is passed
     * through as it came; only the process' own answer is ours, and
     * only when the real call did answer. */
    if (ok && proc && g_keepMask) *proc = (DWORD_PTR)g_keepMask;
    note(&c_GPAM, 3, "  >> GetProcessAffinityMask -> 0x%zX", (size_t)g_keepMask);
    return ok;
}
/* A requested affinity mask, mapped onto the set we allow.
 *
 * A mask naming ONE processor is an index - the engine saying "put
 * this thread on processor i", which is how it probes cores and how
 * it pins its workers. Intersecting it with the allowed set instead
 * turns "processor 0" into "any of five", and the engine's start up
 * never finishes: the log caught SetProcessAffinityMask(0x1) coming
 * back as five processors. An index is therefore REMAPPED to the
 * index'th processor we allow, so one stays one.
 *
 * A wider mask is a set to spread over, and intersecting is exactly
 * right there - an empty result falls back to everything allowed,
 * never to nothing. */
static DWORD_PTR trim_mask(DWORD_PTR mask)
{
    if (!g_keepMask) return mask;
    if (mask && (mask & (mask - 1)) == 0) {     /* a single processor */
        DWORD idx = 0, n = popcount_ptr(g_keepMask);

        while (idx < 64 && !((mask >> idx) & 1)) idx++;
        if (n)
            return (DWORD_PTR)1 << nth_bit(g_keepMask,
                                           (idx < n) ? idx : n - 1);
        return mask;
    }
    {
        DWORD_PTR m = (DWORD_PTR)(mask & g_keepMask);
        return m ? m : (DWORD_PTR)g_keepMask;
    }
}
static BOOL WINAPI hook_SetProcessAffinityMask(HANDLE h, DWORD_PTR mask)
{
    DWORD_PTR m = trim_mask(mask);
    note(&c_SPAM, 3, "  >> SetProcessAffinityMask requested=0x%zX forced=0x%zX", (size_t)mask, (size_t)m);
    return real_SetProcessAffinityMask(h, m);
}
static DWORD_PTR WINAPI hook_SetThreadAffinityMask(HANDLE h, DWORD_PTR mask)
{
    DWORD_PTR m = trim_mask(mask);
    note(&c_STAM, 5, "  >> SetThreadAffinityMask requested=0x%zX forced=0x%zX", (size_t)mask, (size_t)m);
    return real_SetThreadAffinityMask(h, m);
}
/* Ideal-processor hints are indexes into the set the engine believes
 * in, so they are mapped onto the index'th processor we allow -
 * clamping them to the highest number instead would crowd every
 * thread past a hole (CPU 0 dropped, or an E-core trim) onto the
 * same core. */
static DWORD map_ideal(DWORD idp)
{
    DWORD n = popcount_ptr(g_keepMask);

    if (idp >= 64 || n == 0)
        return idp;
    return nth_bit(g_keepMask, (idp < n) ? idp : n - 1);
}
static DWORD WINAPI hook_SetThreadIdealProcessor(HANDLE h, DWORD idp)
{
    DWORD v = map_ideal(idp);
    note(&c_STIP, 3, "  >> SetThreadIdealProcessor %lu -> %lu", (unsigned long)idp, (unsigned long)v);
    return real_SetThreadIdealProcessor(h, v);
}
static BOOL WINAPI hook_SetThreadIdealProcessorEx(HANDLE h, PPROCESSOR_NUMBER ideal, PPROCESSOR_NUMBER prev)
{
    PROCESSOR_NUMBER local;
    if (ideal) {
        local = *ideal;
        local.Group = 0;
        local.Number = (BYTE)map_ideal(local.Number);
    }
    note(&c_STIPEx, 3, "  >> SetThreadIdealProcessorEx mapped");
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

    if (st == 0 && buf && g_keepMask) {
        if (cls == SYS_BASIC_INFO && len >= sizeof(SBI)) {
            SBI *sbi = (SBI *)buf;
            sbi->NumberOfProcessors = (CCHAR)report_count(
                (DWORD)(unsigned char)sbi->NumberOfProcessors);
            sbi->ActiveProcessorsAffinityMask = g_keepMask;
        } else if (cls == SYS_LPI) {
            DWORD count = (retlen ? *retlen : len) / (DWORD)sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION), w = 0;
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION *a = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION *)buf;
            for (DWORD r = 0; r < count; r++) {
                SYSTEM_LOGICAL_PROCESSOR_INFORMATION e = a[r];
                e.ProcessorMask &= g_keepMask;
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
    if (cls == PROC_AFFINITY_MASK_CLASS && info &&
        len >= sizeof(ULONG_PTR) && g_keepMask) {
        ULONG_PTR m = (*(ULONG_PTR *)info) & g_keepMask;
        if (m == 0) m = g_keepMask;
        return real_NtSIP(proc, cls, &m, (ULONG)sizeof(m));
    }
    return real_NtSIP(proc, cls, info, len);
}

/* ========================================================================= */
/* Installation.                                                              */
/* ========================================================================= */
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

/* Set the process mask with the real API: the hooks answer the
 * engine's questions, and asking ourselves through one of them here
 * would be answering our own question. */
static BOOL apply_mask(ULONG_PTR mask)
{
    if (real_SetProcessAffinityMask)
        return real_SetProcessAffinityMask(GetCurrentProcess(),
                                           (DWORD_PTR)mask);
    return SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)mask);
}

/* The launch's result, for the mod settings page. The fields are
 * written once, on the attach path, and read later from the loader
 * thread, so no lock is needed. */
int ShCoreFixGetStatus(ShCoreFixStatus *out)
{
    if (!out) return 0;
    *out = g_status;
    return 1;
}

/* ---- what a dial asks for ------------------------------------------------ */

static int dial_wants_ecore(int d)
{
    return d == D_NO_ECORE || d == D_NO_SMT_ECORE ||
           d == D_NO_ECORE_CPU0 || d == D_NO_SMT_ECORE_CPU0;
}
static int dial_wants_smt(int d)
{
    return d == D_NO_SMT || d == D_NO_SMT_ECORE ||
           d == D_NO_SMT_CPU0 || d == D_NO_SMT_ECORE_CPU0;
}
static int dial_wants_cpu0(int d)
{
    return d == D_NO_CPU0 || d == D_NO_SMT_CPU0 ||
           d == D_NO_ECORE_CPU0 || d == D_NO_SMT_ECORE_CPU0;
}

static const char *dial_name(int d)
{
    switch (d) {
    case D_LEAVE:             return "leave alone";
    case D_ALL:               return "all cores";
    case D_NO_SMT:            return "SMT off";
    case D_NO_ECORE:          return "E-cores off";
    case D_NO_CPU0:           return "processor 0 off";
    case D_NO_SMT_ECORE:      return "SMT + E-cores off";
    case D_NO_SMT_CPU0:       return "SMT + CPU0 off";
    case D_NO_ECORE_CPU0:     return "E-cores + CPU0 off";
    case D_NO_SMT_ECORE_CPU0: return "SMT + E-cores + CPU0 off";
    default:                  return "?";
    }
}

static const char *stage_name(int s)
{
    switch (s) {
    case STAGE_BOOT:   return "boot";
    case STAGE_WINDOW: return "window";
    case STAGE_PLAY:   return "play";
    default:           return "?";
    }
}

/* Every processor the machine has, whatever this process is currently
 * allowed to touch. A dial works from this and not from the process' own
 * mask, because the system may already have trimmed that one by itself -
 * which is the thing an "all cores" dial exists to undo. */
static ULONG_PTR system_mask(void)
{
    DWORD_PTR proc = 0, sys = 0;

    if (GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && sys)
        return (ULONG_PTR)sys;
    {
        DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        return n ? lowmask(n) : lowmask(1);
    }
}

/* Put one stage's dial in force.
 *
 * "leave alone" sets nothing and leaves every hook answering through
 * unhooked, so the machine behaves exactly as it would without us -
 * including any trimming the system decided on its own. Everything else
 * works from the machine's own set and sets the process affinity for
 * real, so the scheduler enforces it and Task Manager can show it. */
static void ApplyDial(int stage)
{
    int d = g_dial[stage];
    /* The core ceiling belongs to play alone: trimming the set while the
     * game is still starting is a good way to make it not start, and it
     * is not what it is for - the stutter is a play-time thing. A stage
     * that asks for nothing and has no ceiling to apply therefore does
     * nothing at all, even when a ceiling is set for the play stage. */
    DWORD cap = (stage == STAGE_PLAY) ? g_coreCap : 0;
    ULONG_PTR m;
    DWORD n;
    BOOL set;

    if (d == D_LEAVE && cap == 0) {
        /* As we found it, not as we left it: a stage that asks for
         * nothing gives back the affinity the process came with, or the
         * trim from the stage before would quietly stay in force. */
        g_keepMask = 0;
        g_reportCount = 0;
        if (g_touched) {
            apply_mask(g_procOrig);
            g_touched = 0;
            Log("corefix: stage %s: leave alone - the process affinity is "
                "put back as it was found (0x%zX), and every query answers "
                "as it came", stage_name(stage), (size_t)g_procOrig);
        } else {
            Log("corefix: stage %s: leave alone - nothing is set and every "
                "query answers as it came", stage_name(stage));
        }
        g_stage = stage;
        g_status.stage = stage;
        g_status.keepCount = 0;
        g_status.reportCount = 0;
        g_status.mask = 0;
        return;
    }

    m = g_sysMask;
    if (dial_wants_ecore(d)) {
        if (g_pMaskOk)
            m &= g_pMask;
        else
            Log("corefix: %s: E-cores off does not apply to this CPU, "
                "left as it is", stage_name(stage));
    }
    if (dial_wants_smt(d)) {
        if (g_smt0Ok)
            m &= g_smt0Mask;
        else
            Log("corefix: %s: SMT off: topology unreadable, no trim",
                stage_name(stage));
    }
    if (dial_wants_cpu0(d)) {
        ULONG_PTR t = m & ~(ULONG_PTR)1;
        if (t) m = t;                   /* never an empty set */
    }
    if (cap) {
        n = popcount_ptr(m);
        if (n > cap) {
            m = keep_lowest_bits(m, cap);
            Log("corefix: %s: max cores %lu trims the set from %lu",
                stage_name(stage), (unsigned long)cap, (unsigned long)n);
        }
    }
    if (!m)
        m = g_sysMask;

    g_keepMask = m;
    g_reportCount = popcount_ptr(m);
    set = apply_mask(m);
    g_touched = 1;
    g_stage = stage;
    g_status.stage = stage;
    g_status.keepCount = g_reportCount;
    g_status.reportCount = g_reportCount;
    g_status.mask = (unsigned long long)m;
    Log("corefix: stage %s: %s -> mask 0x%zX, engine told %lu processors, "
        "affinity %s",
        stage_name(stage), dial_name(d), (size_t)m,
        (unsigned long)g_reportCount, set ? "applied" : "FAILED");
}

/* A dial reads as one of the enum values. A value its stage does not
 * offer - the logo and window dials have no processor-0 choice - reads
 * as "leave alone" rather than as something arbitrary. */
static int read_dial(const char *key, int maxDial)
{
    int v = ShConfigGetInt("loader", key, 0);

    return (v >= 0 && v <= maxDial) ? v : D_LEAVE;
}

/* Who we are and who started us. A child process inherits its parent's
 * affinity mask, so "this process started on six of twelve" usually
 * means the launcher handed it over that way - and naming that process
 * is the difference between a mystery and a fact. */
static void LogProcessTree(void)
{
    PROCESSENTRY32 pe;
    HANDLE snap;
    DWORD pid = GetCurrentProcessId();
    DWORD ppid = 0;
    const char *pname = "?";
    char path[MAX_PATH] = "";

    GetModuleFileNameA(NULL, path, MAX_PATH);

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        Log("corefix: process %lu (%s)", (unsigned long)pid, path);
        return;
    }
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                ppid = pe.th32ParentProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));

        if (ppid) {
            pe.dwSize = sizeof(pe);
            if (Process32First(snap, &pe)) {
                do {
                    if (pe.th32ProcessID == ppid) {
                        pname = pe.szExeFile;
                        break;
                    }
                } while (Process32Next(snap, &pe));
            }
        }
    }
    CloseHandle(snap);

    Log("corefix: process %lu (%s), started by %lu (%s)",
        (unsigned long)pid, path, (unsigned long)ppid, pname);
}

/* Called from loader DllMain after the real dinput8 is loaded and before
 * any plugin loads. The logo stage is already under way at that moment,
 * so its dial goes into force right here; the other two are put in force
 * by StageThread as the game moves. With all three dials left alone and
 * no cap, not one API is touched. */
void ShCoreFixStartup(void)
{
    DWORD_PTR proc = 0, sys = 0;
    ULONG_PTR procMask;
    int v, needEcore, needSmt, ok;

    if (InterlockedCompareExchange(&g_installed, 1, 0))
        return;

    LogInit("scripthook_corefix.log");

    ShConfigInit();
    g_dial[STAGE_BOOT]   = read_dial("cpu_boot",   D_NO_SMT_ECORE);
    g_dial[STAGE_WINDOW] = read_dial("cpu_window", D_NO_SMT_ECORE);
    g_dial[STAGE_PLAY]   = read_dial("cpu_play",   D_NO_SMT_ECORE_CPU0);

    /* The cap is a ceiling: 0, a missing key and junk all mean "no cap",
     * and one processor group's worth of bits is as far as a single mask
     * word reaches. */
    v = ShConfigGetInt("loader", "cpu_cores", 0);
    g_coreCap = (v > 0 && v <= 64) ? (DWORD)v : 0;

    memset(&g_status, 0, sizeof(g_status));
    g_status.dial[0] = g_dial[STAGE_BOOT];
    g_status.dial[1] = g_dial[STAGE_WINDOW];
    g_status.dial[2] = g_dial[STAGE_PLAY];

    g_sysMask = system_mask();
    if (GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys))
        procMask = (ULONG_PTR)proc;
    else
        procMask = g_sysMask;
    if (!procMask)
        procMask = g_sysMask;
    g_procOrig = procMask;
    g_status.origCount = popcount_ptr(procMask);
    g_status.sysCount = popcount_ptr(g_sysMask);

    if (g_dial[STAGE_BOOT] == D_LEAVE && g_dial[STAGE_WINDOW] == D_LEAVE &&
        g_dial[STAGE_PLAY] == D_LEAVE && g_coreCap == 0) {
        Log("corefix: disabled - all three dials are leave-alone and no "
            "cap is set, so not one API is touched");
        return;
    }

    Log("corefix: dials boot=%d(%s) window=%d(%s) play=%d(%s) "
        "play-max-cores=%lu",
        g_dial[STAGE_BOOT], dial_name(g_dial[STAGE_BOOT]),
        g_dial[STAGE_WINDOW], dial_name(g_dial[STAGE_WINDOW]),
        g_dial[STAGE_PLAY], dial_name(g_dial[STAGE_PLAY]),
        (unsigned long)g_coreCap);
    Log("corefix: the machine has %lu processors, this process started on "
        "%lu (mask 0x%zX)",
        (unsigned long)g_status.sysCount, (unsigned long)g_status.origCount,
        (size_t)procMask);
    if (g_status.origCount < g_status.sysCount) {
        /* A child inherits its parent's affinity, so this usually means
         * whoever launched us handed it over this way. Naming that
         * process turns a mystery into a fact. */
        LogProcessTree();
        Log("corefix: note: something outside this process already trimmed "
            "it - a launcher or a system policy - and an 'all cores' dial "
            "is what undoes that");
    }

    /* Both detections pin this thread to each processor in turn, so they
     * run before the hooks exist - afterwards they would be measuring
     * their own answers. They are done whenever any dial might ask for
     * them, not only for the boot one. */
    needEcore = dial_wants_ecore(g_dial[STAGE_BOOT]) ||
                dial_wants_ecore(g_dial[STAGE_WINDOW]) ||
                dial_wants_ecore(g_dial[STAGE_PLAY]);
    needSmt = dial_wants_smt(g_dial[STAGE_BOOT]) ||
              dial_wants_smt(g_dial[STAGE_WINDOW]) ||
              dial_wants_smt(g_dial[STAGE_PLAY]);

    if (needEcore) {
        g_status.ecoreState = detect_p_cores(&g_pMask);
        if (g_status.ecoreState == SH_CF_APPLIED)
            g_pMaskOk = 1;
        Log("corefix: E-cores: %s (%s)", cf_state_name(g_status.ecoreState),
            g_pMaskOk ? "a dial may use them" : "no dial can trim for them");
    } else {
        g_status.ecoreState = SH_CF_OFF;
    }

    if (needSmt) {
        if (detect_smt0_mask(&g_smt0Mask))
            g_smt0Ok = 1;
        Log("corefix: SMT: %s", g_smt0Ok
            ? "one thread per physical core is available"
            : "topology unreadable, no dial can trim for it");
    }

    g_status.active = 1;

    ok = install_hooks();
    ApplyDial(STAGE_BOOT);
    Log("corefix: hooks=%s", ok ? "ENABLED" : "INCOMPLETE");
}

/* ========================================================================= */
/* The stages: which dial is in force.                                        */
/* ========================================================================= */
/* The two start-up stages have a window signature of their own, and it is
 * a far better clock than anything the engine will tell us:
 *
 *   the logo screen  a separate window whose title carries the registered
 *                    mark mis-encoded, "Ghost Recon?Wildlands"
 *   the main window  "Ghost Recon(R) Wildlands", the mark intact
 *
 * So the logo stage is simply "the main window is not up yet", which is
 * exact, and it ends the moment the game's own window appears - no
 * guessing about when the engine has finished finding its way. */
static int StageFromState(int st);

typedef struct {
    int      logo;              /* a window titled like the logo screen */
    int      main;              /* a window titled like the main one     */
    int      seen;              /* titled windows of this process       */
    wchar_t  cls[64];           /* the last one seen, for the log       */
    wchar_t  title[192];
} WinProbe;

static BOOL CALLBACK WinEnumProc(HWND h, LPARAM l)
{
    WinProbe *p = (WinProbe *)l;
    wchar_t title[192];
    DWORD pid = 0;

    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    if (!GetWindowTextW(h, title, 191) || !title[0]) return TRUE;

    p->seen++;
    if (wcsstr(title, L"Ghost Recon")) {
        if (wcschr(title, L'?'))
            p->logo = 1;        /* the mark came through as '?' */
        else
            p->main = 1;
        GetClassNameW(h, p->cls, 63);
        wcsncpy(p->title, title, 191);
        p->title[191] = 0;
    }
    return TRUE;
}

static void ProbeWindows(WinProbe *p)
{
    memset(p, 0, sizeof(*p));
    EnumWindows(WinEnumProc, (LPARAM)p);
}

/* Which stage the game is in right now: the windows decide between the
 * logo screen and everything after it, and the engine's own flow state
 * separates the window stage from play. Seeing the main window is
 * latched: it is the point of no return, and a full-screen switch or a
 * moment with the window hidden must not be read as a step backwards
 * into the boot dial. */
static int StageFromNow(void)
{
    WinProbe w;

    if (!g_sawMain) {
        ProbeWindows(&w);
        if (!g_winLogged) {
            g_winLogged = 1;
            Log("corefix: window probe: %d titled window(s), main=%d "
                "logo=%d, last '%ls' class '%ls'",
                w.seen, w.main, w.logo, w.title, w.cls);
        }
        /* Both windows are named once, with their real titles: this is
         * the evidence the stage boundary is where the player sees it,
         * and the thing to read first if a title ever changes. */
        if (w.logo && !g_logoLogged) {
            g_logoLogged = 1;
            Log("corefix: the logo window is up ('%ls', class '%ls')",
                w.title, w.cls);
        }
        if (w.main) {
            g_sawMain = 1;
            Log("corefix: the game's main window is up ('%ls', class '%ls') "
                "- the logo stage is over", w.title, w.cls);
        }
    }
    if (!g_sawMain)
        return STAGE_BOOT;
    return StageFromState(ShGetGameState());
}

/* Everything loaded but not yet played - a load screen, the main menu, a
 * lobby - is the window stage; play is the world being played, the pause
 * menu included (the world stays up behind it). An unrecognised state
 * reads as the window stage, the most conservative of the two. */
static int StageFromState(int st)
{
    switch (st) {
    case SH_STATE_INGAME:
    case SH_STATE_PAUSED:
    case SH_STATE_DRONE:
    case SH_STATE_BINOCULAR:
    case SH_STATE_CINEMATIC:
    case SH_STATE_GAMEOVER:
        return STAGE_PLAY;
    default:
        return STAGE_WINDOW;
    }
}

/* Two stages can carry identical dials and still mean different things -
 * "leave alone" and "all cores" differ only in whether the system's own
 * trimming is undone - so a change of stage always re-applies.
 *
 * The 120 second guard: if no state ever arrives (the state module failed
 * to hook, or the engine never reaches GameFlow), the boot dial must not
 * stay in force forever, because it is the one most likely to be a set
 * with holes in it. The window dial takes over and the log says so. */
#define STAGE_POLL_MS   250
#define STAGE_STATE_MS  120000

static DWORD WINAPI StageThread(LPVOID p)
{
    uint64_t start = GetTickCount64();
    int timedOut = 0;

    (void)p;
    for (;;) {
        int stage = StageFromNow();

        if (!timedOut && stage == STAGE_BOOT &&
            GetTickCount64() - start >= STAGE_STATE_MS) {
            timedOut = 1;
            stage = STAGE_WINDOW;
            Log("corefix: the main window never came up after %lu s - "
                "leaving the boot dial for the window one",
                (unsigned long)(STAGE_STATE_MS / 1000));
        }
        if (stage != g_stage)
            ApplyDial(stage);
        Sleep(STAGE_POLL_MS);
    }
    return 0;
}

/* Started from the loader thread, not from DllMain: creating a thread
 * under the loader lock is how a start up deadlocks, and nothing here is
 * needed before the loader thread exists anyway. */
void ShCoreFixLateStartup(void)
{
    if (g_status.active)
        CreateThread(NULL, 0, StageThread, NULL, 0, NULL);
}
