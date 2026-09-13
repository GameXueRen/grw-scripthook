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
 * The stages are told apart by two signals, both the game's own, and both
 * latched the moment they arrive so that a stage only ever moves forwards.
 *
 * The boundary between the first two is the game's own windows, and each
 * one carries two features that say which it is: the class the game gives
 * it - the splash screen is "ScimitarSplashScreenWindow", the window that
 * follows is "ScimitarEngineWindowClass" - and the title, where the
 * registered mark of the splash screen comes through mis-encoded ("Ghost
 * Recon?Wildlands") while the main window has it right. The class is asked
 * first, because a class name is structural where the mark is an encoding
 * accident; the title stays as the fallback, so a build that renames its
 * windows is judged exactly as it was before. The first stage is then
 * simply "the main window is not up yet", which is exact, and it ends the
 * moment the game's own window appears.
 *
 * The front end is the game's own state saying MenuOrLobby - the main menu
 * and every lobby in one bucket - and the third stage is everything from
 * that first main menu on: the menu, a lobby, a later load screen, the
 * pause menu and the world are all play, for the rest of the session.
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
 * The play stage also carries a priority dial, cpu_prio_play: 0 leave
 * alone, 1 normal, 2 above normal, 3 high. The two loading stages have one
 * switch between them instead - cpu_eco_boot, off by default - and when it
 * is on they hold the efficiency mode, the Windows 11 EcoQoS hint that lets
 * the scheduler run the process slower and on the efficiency cores. That is
 * exactly what a logo screen and a loading screen are, and on a machine
 * that cannot do efficiency mode the switch resolves to the low class
 * rather than quietly doing nothing (see LoadingPrio). Whichever of the two
 * it lands on, the play stage puts it back: what we changed is restored to
 * the state the process arrived in, and one the process arrived with is
 * never touched at all. What the dials are doing right now is public,
 * read-only: ShCpuGetStatus / ShCpuStage / ShCpuAllowedMask /
 * ShCpuReportedCount and ShCpuOnStageChange, in scripthook.h under
 * @defgroup cpu.
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
#pragma intrinsic(_ReturnAddress)
#else
#include <cpuid.h>
#endif

#define SH_BUILD 1
#include "scripthook.h"
#include "log.h"
#include "third_party/minhook/include/MinHook.h"

/* The framework's single error channel lives in scripthook_api.c, and the
 * modules that use it declare it themselves - the same line the blacklist
 * and the rest carry. */
extern void ShSetError(int err);

/* The caller of an exported function, for the one API here that has to
 * know which plugin is talking. Same definition as the blacklist's. */
#ifdef _MSC_VER
#define SH_CALLER_ADDR() _ReturnAddress()
#else
#define SH_CALLER_ADDR() __builtin_return_address(0)
#endif

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

/* The priority a stage can hold. Only the play stage has a dial now: the
 * two loading stages have one switch - the efficiency mode - and what that
 * switch turns into on a machine which cannot do efficiency mode is the low
 * class, which is the same intent (get out of the way while nothing is
 * being played) said with what is there.
 *
 * Realtime is deliberately not among them - a game at realtime priority can
 * starve the desktop, the audio and the input threads, which is the
 * opposite of what any of this is for.
 *
 * The order is the ini's order for the four values the play dial stores:
 * 0 leave alone, 1 normal, 2 above normal, 3 high. P_ECO and P_IDLE are
 * markers for what the loading stages' switch resolved to - never written
 * to the ini and never offered as a choice, which is what keeps the two
 * scales from having to agree on anything. */
enum {
    P_LEAVE = 0,        /* touch nothing                       */
    P_NORMAL,           /* NORMAL_PRIORITY_CLASS               */
    P_ABOVE,            /* ABOVE_NORMAL_PRIORITY_CLASS         */
    P_HIGH,             /* HIGH_PRIORITY_CLASS                 */
    P_ECO,              /* efficiency mode (EcoQoS): no class  */
    P_IDLE              /* where P_ECO falls back to: the low class */
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
/* The play priority dial, the one switch the two loading stages have, and
 * what the process' class was when we arrived - so a stage that holds
 * nothing can give the class back instead of leaving the last stage's in
 * place. */
static int       g_prioPlay;                /* the play dial, 0..3 */
static int       g_ecoBoot;                 /* the loading stages' switch */
static DWORD     g_prioOrig;
static int       g_prioTouched;
/* The class the current stage wants held, 0 while it wants none: the
 * SetPriorityClass hook answers the engine with this one, because the
 * engine sets its own class during start up and would undo the dial. */
static volatile LONG g_prioNow;
/* The efficiency mode. This is the one place in the framework that reads
 * or writes the process' power-throttling class, and the snapshot below
 * only means anything because of that: a second writer anywhere would
 * decide for itself what "as we found it" was. */
static volatile LONG g_ecoState = SH_ECO_OFF;  /* SH_ECO_*, for the log and the page */
static volatile LONG g_ecoOurs;         /* 1 while the switch on is one we turned on */
static volatile LONG g_ecoApi;          /* 0 unprobed, 1 usable, 2 no call, 3 not Win11 */
static volatile LONG g_ecoFailed;       /* the setter failed; logged once, not retried */
static int   g_ecoSaveValid;            /* the arrival state could be read */
static ULONG g_ecoSaveControl;          /* ... and what it said */
static ULONG g_ecoSaveState;
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
static volatile LONG g_winMismatch;         /* the class/title clash was reported */
static volatile LONG g_sawMain;             /* the main window has been up */
static volatile LONG g_pastFront;           /* the front end has been reached */
static volatile LONG g_stateSeen;           /* the state machine has answered */
static volatile LONG g_stageFloor;          /* a watchdog moved the stage on */
static ShCpuStatus g_status;
static volatile LONG g_installed = 0;

/* The stage-change subscribers, one slot per plugin: the caller's own
 * module is the identity, the way the blacklist registry does it. A spin
 * lock, not a critical section - it is held for a handful of stores, by
 * registration (once per plugin, from the loader thread) and by the
 * dispatcher, and it is never held while a plugin runs. */
#define STAGE_FN_MAX 8
static ShCpuStageFn  g_stageFn[STAGE_FN_MAX];
static void         *g_stageUser[STAGE_FN_MAX];
static char          g_stageOwner[STAGE_FN_MAX][32];
static volatile LONG g_stageLock;

static void StageLock(void)
{
    while (InterlockedExchange(&g_stageLock, 1)) Sleep(0);
}
static void StageUnlock(void)
{
    InterlockedExchange(&g_stageLock, 0);
}

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
typedef BOOL      (WINAPI *pfn_SetPriorityClass)(HANDLE, DWORD);
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
static pfn_SetPriorityClass            real_SetPriorityClass;
static pfn_NtQSI                        real_NtQSI;
static pfn_NtSIP                        real_NtSIP;

static volatile LONG c_GSI, c_GNSI, c_GAPC, c_GMPC, c_GAPGC, c_GLPI, c_GLPIEx,
                     c_GPAM, c_SPAM, c_STAM, c_STIP, c_STIPEx, c_SPC, c_PHOLD,
                     c_ECO;

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
/* PRIORITY: the engine sets its own class while it starts, which would       */
/* quietly undo a priority dial. While a stage holds one, its request is      */
/* answered with ours; with no dial in force it goes through untouched, which */
/* is what "leave alone" has to mean here too.                                */
/* ========================================================================= */
static BOOL WINAPI hook_SetPriorityClass(HANDLE h, DWORD cls)
{
    note(&c_SPC, 3, "  >> SetPriorityClass requested=0x%lX",
         (unsigned long)cls);
    if (g_prioNow)
        return real_SetPriorityClass(h, (DWORD)g_prioNow);
    return real_SetPriorityClass(h, cls);
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
#define PROC_PRIORITY_CLASS      18
static LONG WINAPI hook_NtSIP(HANDLE proc, ULONG cls, PVOID info, ULONG len)
{
    if (cls == PROC_AFFINITY_MASK_CLASS && info &&
        len >= sizeof(ULONG_PTR) && g_keepMask) {
        ULONG_PTR m = (*(ULONG_PTR *)info) & g_keepMask;
        if (m == 0) m = g_keepMask;
        return real_NtSIP(proc, cls, &m, (ULONG)sizeof(m));
    }
    /* The direct route to the same thing: an engine that calls ntdll
     * instead of kernel32 would otherwise walk straight past a dial. */
    if (cls == PROC_PRIORITY_CLASS && info && len >= sizeof(ULONG) &&
        g_prioNow) {
        ULONG v = (ULONG)g_prioNow;
        note(&c_SPC, 5, "  >> NtSetInformationProcess priority forced=0x%lX",
             (unsigned long)v);
        return real_NtSIP(proc, cls, &v, (ULONG)sizeof(v));
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
    ok &= hook_api(L"kernel32", "SetPriorityClass",         (LPVOID)hook_SetPriorityClass,         (LPVOID *)&real_SetPriorityClass);
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

/* ========================================================================= */
/* The public API.                                                            */
/* ========================================================================= */
/* What the dials are doing, for a plugin or for the settings page. The
 * fields are written by the stage thread and read from anywhere, so this
 * is a snapshot: each field is an aligned 32 or 64 bit value, read and
 * written whole on x64, so a reader may see two neighbouring fields from
 * two ticks a quarter of a second apart but never a torn one - and each is
 * true of some moment, which is what a status query is for. The stage lock
 * guards the subscriber table below, not this.
 *
 * Everything here is a read. No plugin can move a dial, and that is on
 * purpose: the process' class and affinity have one owner, and a second
 * one would be a fight the player would feel.
 */
static void CopyStatus(ShCpuStatus *out)
{
    out->active      = g_status.active;
    out->stage       = g_status.stage;
    out->dial[0]     = g_status.dial[0];
    out->dial[1]     = g_status.dial[1];
    out->dial[2]     = g_status.dial[2];
    out->prio[0]     = g_status.prio[0];
    out->prio[1]     = g_status.prio[1];
    out->prio[2]     = g_status.prio[2];
    out->ecoreState  = g_status.ecoreState;
    out->origCount   = g_status.origCount;
    out->sysCount    = g_status.sysCount;
    out->reportCount = g_status.reportCount;
    out->keepCount   = g_status.keepCount;
    out->mask        = g_status.mask;
    out->eco         = (int)InterlockedCompareExchange(&g_ecoState, 0, 0);
    out->ecoOurs     = (int)InterlockedCompareExchange(&g_ecoOurs, 0, 0);
}

SH_API int ShCpuGetStatus(ShCpuStatus *out)
{
    if (!out) return 0;
    CopyStatus(out);
    return 1;
}

SH_API int ShCpuStage(void)
{
    int stage = g_status.stage;

    return (stage >= 0 && stage < STAGE_COUNT) ? stage : STAGE_BOOT;
}

SH_API uint64_t ShCpuAllowedMask(void)
{
    return (uint64_t)g_keepMask;
}

SH_API unsigned ShCpuReportedCount(void)
{
    return (unsigned)g_reportCount;
}

/* One slot per plugin, told when the stage actually changes and not
 * otherwise. Registering again from the same module replaces that module's
 * own slot; NULL clears it. 1 when accepted, 0 with ShLastError saying why
 * - SH_ERR_BAD_ARG when the caller is not a plugin, SH_ERR_REGISTRY_FULL
 * when eight plugins are already subscribed. */
SH_API int ShCpuOnStageChange(ShCpuStageFn fn, void *user)
{
    char me[32];
    int i, free = -1;

    ShPluginOwnerFromAddress(SH_CALLER_ADDR(), me, sizeof(me));
    if (!me[0]) {
        ShSetError(SH_ERR_BAD_ARG);     /* only a plugin can subscribe */
        return 0;
    }
    StageLock();
    for (i = 0; i < STAGE_FN_MAX; i++) {
        if (!strcmp(g_stageOwner[i], me)) {
            g_stageFn[i] = fn;
            g_stageUser[i] = user;
            StageUnlock();
            return 1;
        }
        if (free < 0 && !g_stageFn[i] && !g_stageOwner[i][0]) free = i;
    }
    if (!fn) {                          /* nothing of ours left to clear */
        StageUnlock();
        return 1;
    }
    if (free < 0) {
        StageUnlock();
        ShSetError(SH_ERR_REGISTRY_FULL);
        return 0;
    }
    g_stageFn[free] = fn;
    g_stageUser[free] = user;
    snprintf(g_stageOwner[free], sizeof(g_stageOwner[free]), "%s", me);
    StageUnlock();
    return 1;
}

/* Handed the new stage after the lock is dropped, so a plugin that calls
 * back into the framework from its own callback cannot deadlock against
 * us - the same rule the blacklist's watcher keeps. */
static void ReportStage(int stage)
{
    ShCpuStageFn fn[STAGE_FN_MAX];
    void        *user[STAGE_FN_MAX];
    int i, n = 0;

    StageLock();
    for (i = 0; i < STAGE_FN_MAX; i++)
        if (g_stageFn[i]) { fn[n] = g_stageFn[i]; user[n] = g_stageUser[i]; n++; }
    StageUnlock();
    for (i = 0; i < n; i++)
        fn[i](stage, user[i]);
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

/* The class a value names, or 0 for the two that name none: leave alone
 * sets nothing, and the efficiency mode is not a class at all - it goes
 * through the process' power-throttling class, not SetPriorityClass. */
static DWORD prio_class(int p)
{
    switch (p) {
    case P_IDLE:   return IDLE_PRIORITY_CLASS;
    case P_NORMAL: return NORMAL_PRIORITY_CLASS;
    case P_ABOVE:  return ABOVE_NORMAL_PRIORITY_CLASS;
    case P_HIGH:   return HIGH_PRIORITY_CLASS;
    default:       return 0;
    }
}

static const char *prio_name(int p)
{
    switch (p) {
    case P_LEAVE:  return "leave alone";
    case P_NORMAL: return "normal";
    case P_ABOVE:  return "above normal";
    case P_HIGH:   return "high";
    case P_ECO:    return "efficiency mode";
    case P_IDLE:   return "low";
    default:       return "?";
    }
}

/* ---- the efficiency mode (EcoQoS) ---------------------------------------
 *
 * SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, ...)
 * with PROCESS_POWER_THROTTLING_EXECUTION_SPEED is the documented way in.
 * Three states are kept apart, because giving the process back is part of
 * the job:
 *
 *   on        ControlMask = StateMask = EXECUTION_SPEED
 *   off       ControlMask = EXECUTION_SPEED, StateMask = 0
 *   managed   ControlMask = StateMask = 0 - handed back to the system,
 *             which may then throttle us on its own while we are in the
 *             background, exactly as it would without us
 *
 * The structure and the constants are declared here rather than taken from
 * the SDK, and the two calls are resolved with GetProcAddress: the class
 * does not exist before Windows 10 1709 / 11, and importing the functions
 * would make dinput8.dll fail to load on an older system instead of
 * degrading. A system without them gets one log line and the dial reads as
 * not applicable - never as applied.
 *
 * The trap met while writing this, worth keeping: the size passed must be
 * exactly sizeof(state), the structure must be zeroed and Version must be
 * 1, or the call fails with ERROR_INVALID_PARAMETER (87). An 87 in this
 * log means that and not a permission problem.
 *
 * The engine sets its own priority class during start up, which is why
 * SetPriorityClass is hooked; nothing in it touches the power-throttling
 * class, so this switch needs no hook to stay in force.
 */
#define PROC_POWER_THROTTLING_CLASS 4       /* ProcessPowerThrottling */
#define PTT_VERSION                 1UL     /* PROCESS_POWER_THROTTLING_CURRENT_VERSION */
#define PTT_EXECUTION_SPEED         0x1UL   /* PROCESS_POWER_THROTTLING_EXECUTION_SPEED */

typedef struct { ULONG Version; ULONG ControlMask; ULONG StateMask; } PttState;
typedef BOOL (WINAPI *pfn_SetProcessInformation)(HANDLE, int, LPVOID, DWORD);
typedef BOOL (WINAPI *pfn_GetProcessInformation)(HANDLE, int, LPVOID, DWORD);

/* RTL_OSVERSIONINFOW's shape, declared here for the same reason the state
 * above is: the check has to compile on an SDK that may not carry it. */
typedef struct {
    ULONG dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber,
          dwPlatformId;
    WCHAR szCSDVersion[128];
} OsVersionInfoW;
typedef LONG (WINAPI *pfn_RtlGetVersion)(OsVersionInfoW *);

/* Windows 11's first build. The efficiency mode is a Windows 11 feature:
 * Microsoft's own page for SetProcessInformation says the EcoQoS level does
 * not exist before Windows 11 and that such a process was marked LowQoS
 * instead - so on Windows 10 the same call can succeed and mean something
 * else, which is exactly the "applied" lie this module refuses to tell. */
#define WIN11_BUILD 22000

static pfn_SetProcessInformation real_SetProcessInformation;
static pfn_GetProcessInformation real_GetProcessInformation;

/* Resolved and judged once. 1 while the dial can be honoured on this
 * machine: the two calls exist AND this is Windows 11. Anything else gets
 * one log line with the build number and leaves the dial reading as not
 * applicable - never as applied - while the classes and the affinity carry
 * on working (they have no such requirement).
 *
 * RtlGetVersion rather than GetVersionEx: the latter reports 6.2 to a
 * process whose manifest does not claim a newer system, and the game's own
 * manifest is not ours to rely on. If the probe cannot be made at all, the
 * version is assumed to be new enough: refusing a feature on a machine
 * that is probably fine would be the worse mistake. */
static int EcoAvailable(void)
{
    LONG st = InterlockedCompareExchange(&g_ecoApi, 0, 0);

    if (st == 0) {
        HMODULE k = GetModuleHandleA("kernel32.dll");
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        pfn_RtlGetVersion rtl = nt ? (pfn_RtlGetVersion)(void *)
            GetProcAddress(nt, "RtlGetVersion") : NULL;
        OsVersionInfoW os;
        int win11 = 1;

        if (!k) k = LoadLibraryA("kernel32.dll");
        if (k) {
            real_SetProcessInformation = (pfn_SetProcessInformation)(void *)
                GetProcAddress(k, "SetProcessInformation");
            real_GetProcessInformation = (pfn_GetProcessInformation)(void *)
                GetProcAddress(k, "GetProcessInformation");
        }
        memset(&os, 0, sizeof(os));
        os.dwOSVersionInfoSize = sizeof(os);
        if (rtl && rtl(&os) == 0 && os.dwBuildNumber)
            win11 = os.dwBuildNumber >= WIN11_BUILD;

        if (!real_SetProcessInformation || !real_GetProcessInformation) {
            st = 2;
            Log("corefix: efficiency mode: not applicable - kernel32 has no "
                "Set/GetProcessInformation (build %lu)",
                (unsigned long)os.dwBuildNumber);
        } else if (!win11) {
            st = 3;
            Log("corefix: efficiency mode: not applicable - it is a Windows "
                "11 feature and this is build %lu; before Windows 11 the "
                "same call only marks the process LowQoS, which is not what "
                "this dial says, so nothing is set",
                (unsigned long)os.dwBuildNumber);
        } else {
            st = 1;
            Log("corefix: efficiency mode: available (Windows build %lu)",
                (unsigned long)os.dwBuildNumber);
        }
        InterlockedExchange(&g_ecoApi, st);
    }
    return st == 1;
}

/* 1 on, 0 off (a handed-back process counts as off), -1 unreadable. */
static int EcoIsOn(void)
{
    PttState st;

    if (!EcoAvailable()) return -1;
    memset(&st, 0, sizeof(st));
    st.Version = PTT_VERSION;
    if (!real_GetProcessInformation(GetCurrentProcess(),
                                    PROC_POWER_THROTTLING_CLASS, &st,
                                    (DWORD)sizeof(st)))
        return -1;
    return (st.ControlMask & st.StateMask & PTT_EXECUTION_SPEED) ? 1 : 0;
}

/* The state the process arrived in, read once before any dial applies.
 * "Off" gives this back - the same rule the affinity and the class follow:
 * as we found it, not as we left it. */
static void EcoSnapshot(void)
{
    PttState st;

    if (!EcoAvailable()) {
        g_ecoState = SH_ECO_NA;
        return;
    }
    memset(&st, 0, sizeof(st));
    st.Version = PTT_VERSION;
    if (!real_GetProcessInformation(GetCurrentProcess(),
                                    PROC_POWER_THROTTLING_CLASS, &st,
                                    (DWORD)sizeof(st))) {
        g_ecoState = SH_ECO_FAILED;
        Log("corefix: efficiency mode: could not be read (err %lu); a dial "
            "can still set it, but nothing can be put back exactly",
            (unsigned long)GetLastError());
        return;
    }
    g_ecoSaveControl = st.ControlMask;
    g_ecoSaveState   = st.StateMask;
    g_ecoSaveValid   = 1;
    g_ecoState = (st.ControlMask & st.StateMask & PTT_EXECUTION_SPEED)
               ? SH_ECO_ON : SH_ECO_OFF;
    Log("corefix: efficiency mode: found it %s (control 0x%lX, state "
        "0x%lX); it is left alone unless a dial asks for it",
        g_ecoState == SH_ECO_ON ? "ON" : "off",
        (unsigned long)st.ControlMask, (unsigned long)st.StateMask);
}

/* Turn it on (1), or give the process back (0). 1 when the call went
 * through; a failure is logged once, with the error, and never retried in
 * a loop - a dial that cannot be honoured must not fill the log. */
static int EcoSet(int on)
{
    PttState st;
    DWORD err;

    if (!EcoAvailable()) return 0;
    memset(&st, 0, sizeof(st));
    st.Version = PTT_VERSION;
    if (on) {
        st.ControlMask = PTT_EXECUTION_SPEED;
        st.StateMask   = PTT_EXECUTION_SPEED;
    } else if (g_ecoSaveValid &&
               !((g_ecoSaveControl & g_ecoSaveState) & PTT_EXECUTION_SPEED)) {
        st.ControlMask = g_ecoSaveControl;
        st.StateMask   = g_ecoSaveState;
    } else {
        /* The one case that is not the snapshot: the process arrived
         * throttled, and this stage asks for it off. Handing that state
         * back would restore exactly the throttling the dial drops, so it
         * is turned off explicitly instead - and said so in the log. */
        st.ControlMask = PTT_EXECUTION_SPEED;
        st.StateMask   = 0;
    }
    if (real_SetProcessInformation(GetCurrentProcess(),
                                   PROC_POWER_THROTTLING_CLASS, &st,
                                   (DWORD)sizeof(st)))
        return 1;
    err = GetLastError();
    if (!InterlockedExchange(&g_ecoFailed, 1))
        Log("corefix: efficiency mode: SetProcessInformation failed (err "
            "%lu%s)", (unsigned long)err,
            err == 87 ? " - 87 means a bad parameter: the version or the "
                        "size of the state is wrong" : "");
    return 0;
}

/* Put the efficiency mode where this stage wants it. want 1 = the stage's
 * resolved priority is the efficiency mode (the loading switch, on a
 * machine that has it); want 0 = it is not, so one an earlier stage turned
 * on goes back. A switch nobody here turned on is never touched: the state
 * a process arrived in is not ours to change. A stage whose resolved
 * priority is not the mode costs not one call - the "ours" flag is clear
 * and this returns at once. */
static void EcoWant(int stage, int want)
{
    if (!EcoAvailable()) {
        g_ecoState = SH_ECO_NA;
        return;
    }
    if (want) {
        if (EcoIsOn() == 1) {
            /* Already on, whoever set it: the dial asked for this state,
             * so from here on it is what turns it off again. */
            InterlockedExchange(&g_ecoOurs, 1);
            g_ecoState = SH_ECO_ON;
            return;
        }
        if (!EcoSet(1)) {
            g_ecoState = SH_ECO_FAILED;
            return;
        }
        InterlockedExchange(&g_ecoOurs, 1);
        g_ecoState = SH_ECO_ON;
        /* Read back and say what the system reports: the call returning
         * success is not the same as the state being in force, and this is
         * the line a player checks when the switch "does nothing". */
        note(&c_ECO, 3, "corefix: %s: efficiency mode ON (asked for by this "
                        "stage's priority dial; the system reports it %s)",
             stage_name(stage), EcoIsOn() == 1 ? "on" : "NOT set");
        return;
    }
    if (!InterlockedCompareExchange(&g_ecoOurs, 0, 0))
        return;                         /* not ours: as we found it */
    if (EcoIsOn() != 1) {
        InterlockedExchange(&g_ecoOurs, 0);
        g_ecoState = SH_ECO_OFF;
        return;
    }
    if (!EcoSet(0)) {
        g_ecoState = SH_ECO_FAILED;
        return;
    }
    InterlockedExchange(&g_ecoOurs, 0);
    g_ecoState = SH_ECO_OFF;
    Log("corefix: %s: efficiency mode OFF - an earlier stage turned it on "
        "and this one does not ask for it (%s; the system reports it %s)",
        stage_name(stage),
        (g_ecoSaveValid &&
         !((g_ecoSaveControl & g_ecoSaveState) & PTT_EXECUTION_SPEED))
            ? "put back as it was found"
            : "as it arrived throttled, it is turned off rather than handed "
              "that state back",
        EcoIsOn() == 0 ? "off" : "STILL ON");
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
/* What the two loading stages hold, resolved into what will actually be in
 * force on this machine - and it is a resolution rather than a reading
 * because the answer has to be true even when the machine cannot do what
 * the switch asks:
 *
 *   off                                -> leave alone
 *   on, and the system has it          -> efficiency mode
 *   on, and it has not (or the call
 *   has already failed)                -> the low class instead
 *
 * The last line is the point: a loading screen yielding the machine to
 * whatever else wants it is the same intent the efficiency mode has, said
 * with what this machine actually has. Giving up on the switch instead
 * would leave it reading as "on" while doing nothing at all. */
static int LoadingPrio(void)
{
    if (!g_ecoBoot) return P_LEAVE;
    if (!EcoAvailable()) return P_IDLE;
    if (InterlockedCompareExchange(&g_ecoFailed, 0, 0)) return P_IDLE;
    return P_ECO;
}

/* One place that answers "what should this stage hold", so the applier, the
 * recheck and the status line can never disagree. */
static int EffectivePrio(int stage)
{
    return stage == STAGE_PLAY ? g_prioPlay : LoadingPrio();
}

/* Priority and affinity are both per-process and both restated whenever
 * the stage changes. A stage that asks for no priority gives back the
 * class the process came with, for the same reason the affinity goes
 * back: the stage before it must not quietly stay in force. */
static void ApplyPriority(int stage)
{
    int p = EffectivePrio(stage);
    DWORD was = (DWORD)g_prioNow;       /* what the stage before us held */
    DWORD cls;

    /* The efficiency mode first, in both directions. Entering play that is
     * the whole point: the loading stages' mode is gone before the play
     * priority lands, so the process is never left running on a class this
     * stage did not choose. And the switch is not a class, so nothing below
     * here can be it. */
    EcoWant(stage, p == P_ECO);

    /* Two of the values hold no class, and both give back the one the
     * process came with: the efficiency mode is not a class at all, and
     * leave alone means the class as we found it rather than the one the
     * stage before left in force. That second case is also what the loading
     * switch's fallback needs - there the class really was changed, to the
     * low one, and "leave alone" in play has to hand it back. */
    if (p == P_LEAVE || p == P_ECO) {
        g_prioNow = 0;
        if (g_prioTouched) {
            SetPriorityClass(GetCurrentProcess(), g_prioOrig);
            g_prioTouched = 0;
            Log("corefix: %s: priority put back as it was found (0x%lX)%s",
                stage_name(stage), (unsigned long)g_prioOrig,
                was == IDLE_PRIORITY_CLASS
                    ? " - the low the loading switch fell back to is given "
                      "back with it" : "");
        }
    } else {
        cls = prio_class(p);
        if (!cls)
            return;
        g_prioNow = (LONG)cls;          /* the hook holds it there */
        SetPriorityClass(GetCurrentProcess(), cls);
        g_prioTouched = 1;
        /* Read it back: someone outside this process - a launcher or the
         * anti-cheat service, whose calls our hooks cannot see - may
         * already have put it where it was, and then the log has to say
         * so. */
        Log("corefix: %s: priority %s (0x%lX), now 0x%lX", stage_name(stage),
            prio_name(p), (unsigned long)cls,
            (unsigned long)GetPriorityClass(GetCurrentProcess()));
    }
}

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
        ApplyPriority(stage);
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
    ApplyPriority(stage);
}

/* A dial reads as one of the enum values. A value its stage does not
 * offer - the logo and window dials have no processor-0 choice - reads
 * as "leave alone" rather than as something arbitrary. */
static int read_dial(const char *key, int maxDial)
{
    int v = ShConfigGetInt("loader", key, 0);

    return (v >= 0 && v <= maxDial) ? v : D_LEAVE;
}

/* A priority dial reads as one of the P_* values at or below `hi`. A value
 * above it reads as "leave alone" rather than as something arbitrary: the
 * play stage is capped at P_HIGH, because the efficiency mode is not
 * offered there - a hand-written ini that asks for it in play gets the
 * class left alone rather than a game throttled on purpose. */
static int read_prio(const char *key, int hi)
{
    int v = ShConfigGetInt("loader", key, P_LEAVE);

    return (v >= P_LEAVE && v <= hi) ? v : P_LEAVE;
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

    /* One priority dial, the play stage's. The two loading stages have a
     * switch instead - the efficiency mode, or the low class where this
     * machine cannot do efficiency mode; see LoadingPrio. */
    g_prioPlay = read_prio("cpu_prio_play", P_HIGH);
    g_ecoBoot  = ShConfigGetBool("loader", "cpu_eco_boot", 0) ? 1 : 0;

    /* The cap is a ceiling: 0, a missing key and junk all mean "no cap",
     * and one processor group's worth of bits is as far as a single mask
     * word reaches. */
    v = ShConfigGetInt("loader", "cpu_cores", 0);
    g_coreCap = (v > 0 && v <= 64) ? (DWORD)v : 0;

    memset(&g_status, 0, sizeof(g_status));
    g_status.dial[0] = g_dial[STAGE_BOOT];
    g_status.dial[1] = g_dial[STAGE_WINDOW];
    g_status.dial[2] = g_dial[STAGE_PLAY];
    /* The two loading stages report what their switch resolves to on this
     * machine rather than what the ini asks for - that resolution is what
     * is actually in force, and the page shows it as it is. */
    g_status.prio[0] = LoadingPrio();
    g_status.prio[1] = g_status.prio[0];
    g_status.prio[2] = g_prioPlay;
    g_status.ecoBoot = g_ecoBoot;

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
    g_prioOrig = GetPriorityClass(GetCurrentProcess());
    /* The efficiency mode's arrival state, read here: after the class above
     * and before any dial applies, so both snapshots describe the process
     * as it was handed to us. A stage whose dial asks for the mode counts
     * as a dial that does something, so it keeps this launch out of the
     * branch below by itself. */
    EcoSnapshot();

    if (g_dial[STAGE_BOOT] == D_LEAVE && g_dial[STAGE_WINDOW] == D_LEAVE &&
        g_dial[STAGE_PLAY] == D_LEAVE && g_coreCap == 0 &&
        g_prioPlay == P_LEAVE && !g_ecoBoot) {
        /* Nothing is applied on this path: no hook goes in and not one
         * scheduling API is touched. The one call made above is the read
         * of the efficiency mode's arrival state - a getter, not a
         * scheduling API, and the only way the status can report that
         * switch honestly. The stage thread still starts (see
         * ShCoreFixLateStartup), because the stage is part of the public
         * CPU API whether or not a dial asks for anything. */
        Log("corefix: disabled - every dial is leave-alone and no cap is "
            "set, so no hook and not one scheduling API is touched (the "
            "efficiency mode's arrival state was read once, so the status "
            "can report it honestly)");
        return;
    }

    Log("corefix: dials boot=%d(%s) window=%d(%s) play=%d(%s) "
        "play-max-cores=%lu",
        g_dial[STAGE_BOOT], dial_name(g_dial[STAGE_BOOT]),
        g_dial[STAGE_WINDOW], dial_name(g_dial[STAGE_WINDOW]),
        g_dial[STAGE_PLAY], dial_name(g_dial[STAGE_PLAY]),
        (unsigned long)g_coreCap);
    Log("corefix: priorities loading=%s (the switch is %s) play=%s "
        "(found 0x%lX)", prio_name(LoadingPrio()), g_ecoBoot ? "on" : "off",
        prio_name(g_prioPlay), (unsigned long)g_prioOrig);
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
/* Three of them, and they only ever move forwards - the logo screen, the
 * load that follows it into the front end, and then everything else:
 *
 *   logo     a window of its own, and the game names it apart: class
 *            "ScimitarSplashScreenWindow", title "Ghost Recon?Wildlands"
 *            (the registered mark mis-encoded). The stage is simply "the
 *            main window is not up yet", which is exact, and it ends the
 *            moment the game's own window appears.
 *   window   "ScimitarEngineWindowClass", titled "Ghost Recon(R)
 *            Wildlands", up and the front end not yet reached: the first
 *            load, which is where the engine does its own start-up work.
 *   play     from the first main menu on. The game's own state says
 *            MenuOrLobby - the main menu and every lobby in one bucket -
 *            and once the player can choose a mode the machine belongs to
 *            play: the menu, a lobby, a later load screen, the pause menu
 *            and the world are all this one stage, for the rest of the
 *            session.
 *
 * Each step is latched, so a stage is entered once and never left, and a
 * dial is applied by a step rather than by a state that can flap. That is
 * the shape: two of the three questions a stage could answer - is the
 * engine still starting, is any of the world up - stop being interesting
 * the moment the player reaches the front end, and the dials for them stop
 * being in force with it.
 *
 * Two watchdogs keep a missing signal from freezing a stage forever: the
 * logo one for a build whose two windows are renamed or re-titled, and the
 * front end one
 * for a build where the state hook never armed. Both raise the floor the
 * stage is read through rather than returning a stage for one tick, so
 * progress is monotone even then. */
typedef struct {
    int      logo;              /* a window the class or title calls the logo */
    int      main;              /* ... and the game's own window              */
    int      logoByCls;         /* the class, not the title, identified it    */
    int      mainByCls;
    int      seen;              /* titled windows of this process       */
    wchar_t  cls[64];           /* the last one seen, for the log       */
    wchar_t  title[192];
    /* The two are named apart, each with its own title and class. Both can
     * be up in the same enumeration - the logo screen can linger while the
     * game's own window comes up - and a log that prints "the last one seen"
     * for both would read as if one window had changed its title. */
    wchar_t  logoTitle[192];
    wchar_t  logoCls[64];
    wchar_t  mainTitle[192];
    wchar_t  mainCls[64];
    /* The first window whose class and title disagreed, if there was one:
     * reported once, empty the rest of the time. */
    wchar_t  mmTitle[192];
    wchar_t  mmCls[64];
} WinProbe;

/* The two windows, matched loosely on the part of the class name that says
 * what the window is: "ScimitarSplashScreenWindow" and
 * "ScimitarEngineWindowClass" are what has been seen on this build, and the
 * exact class of each goes into the log every session, so a rename shows up
 * there before it can matter. A class matching neither votes for nothing
 * and leaves the title deciding, which is how this worked before classes
 * were read at all. */
static int ClsIsSplash(const wchar_t *cls)
{
    return wcsstr(cls, L"Splash") != NULL;
}

static int ClsIsEngine(const wchar_t *cls)
{
    return wcsstr(cls, L"Engine") != NULL;
}

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
        wchar_t cls[64] = L"";
        int     splash, engine, marked;

        GetClassNameW(h, cls, 63);
        splash = ClsIsSplash(cls);
        engine = !splash && ClsIsEngine(cls);
        marked = wcschr(title, L'?') != NULL;   /* the mark came through as '?' */

        /* The class first and the title as the fallback, so that the window
         * is named by the feature that says what it is rather than by how a
         * string survived the engine's own text handling - and a build
         * whose classes are renamed is still judged the way it always was.
         * Where the class of the engine's window is known it settles the
         * window even if the title looks mis-encoded, and a window with the
         * game's name in it that matches neither class falls to the title,
         * which reads a plain title as the main window. */
        if (splash || (!engine && marked)) {
            p->logo = 1;
            p->logoByCls = splash;
            wcsncpy(p->logoTitle, title, 191);
            p->logoTitle[191] = 0;
            wcsncpy(p->logoCls, cls, 63);
            p->logoCls[63] = 0;
        } else {
            p->main = 1;
            p->mainByCls = engine;
            wcsncpy(p->mainTitle, title, 191);
            p->mainTitle[191] = 0;
            wcsncpy(p->mainCls, cls, 63);
            p->mainCls[63] = 0;
        }
        /* Kept for the one log line about the two features disagreeing - a
         * class saying "splash screen" over a clean title, or the engine's
         * window over a mis-encoded one - the first such window only. */
        if (!p->mmCls[0] && ((splash && !marked) || (engine && marked))) {
            wcsncpy(p->mmTitle, title, 191);
            p->mmTitle[191] = 0;
            wcsncpy(p->mmCls, cls, 63);
            p->mmCls[63] = 0;
        }
        wcsncpy(p->title, title, 191);
        p->title[191] = 0;
        wcsncpy(p->cls, cls, 63);
        p->cls[63] = 0;
    }
    return TRUE;
}

static void ProbeWindows(WinProbe *p)
{
    memset(p, 0, sizeof(*p));
    EnumWindows(WinEnumProc, (LPARAM)p);
}

/* The world being up is proof that the front end was passed: nothing loads
 * the world without going through the menu first. It is the second half of
 * the front end signal, for a session that came in through a launcher's
 * "continue" and never showed the menu long enough to be caught - and it
 * can only ever agree with the first, never contradict it. */
static int InWorld(int st)
{
    switch (st) {
    case SH_STATE_INGAME:
    case SH_STATE_PAUSED:
    case SH_STATE_DRONE:
    case SH_STATE_BINOCULAR:
    case SH_STATE_CINEMATIC:
    case SH_STATE_GAMEOVER:
        return 1;
    default:
        return 0;
    }
}

/* Which stage the game is in right now. Every signal is latched where it
 * arrives and the answer is read through the watchdog floor, so this only
 * ever returns a stage at or after the one before it: three steps,
 * forwards, once each. */
static int StageFromNow(void)
{
    int st, floor;

    if (!g_sawMain) {
        WinProbe w;

        ProbeWindows(&w);
        if (!g_winLogged) {
            g_winLogged = 1;
            Log("corefix: window probe: %d titled window(s), main=%d "
                "logo=%d; logo '%ls' class '%ls'; main '%ls' class '%ls'",
                w.seen, w.main, w.logo,
                w.logoTitle, w.logoCls, w.mainTitle, w.mainCls);
        }
        /* Both windows are named once, each with its own title and class,
         * and with the feature that named it: this is the evidence the
         * stage boundary is where the player sees it, and the thing to read
         * first if a title or a class ever changes. */
        if (w.logo && !g_logoLogged) {
            g_logoLogged = 1;
            Log("corefix: the logo window is up ('%ls', class '%ls') - %s",
                w.logoTitle, w.logoCls,
                w.logoByCls ? "named by its class"
                            : "named by the mis-encoded mark in its title");
        }
        if (w.main) {
            g_sawMain = 1;
            Log("corefix: the game's main window is up ('%ls', class '%ls') "
                "- the logo stage is over (%s)", w.mainTitle, w.mainCls,
                w.mainByCls ? "named by its class" : "named by its title");
        }
        /* The two features disagreeing is not an error here - the class is
         * believed and the title is the fallback - but it is exactly what a
         * build that renamed its windows, or one that fixed the mark, looks
         * like, and it is worth one line before a stage lands early. */
        if (w.mmCls[0] && !g_winMismatch) {
            g_winMismatch = 1;
            Log("corefix: window '%ls' (class '%ls'): the class and the title "
                "disagree about which window this is - the class is being "
                "believed", w.mmTitle, w.mmCls);
        }
    }

    if (!g_sawMain) {
        st = STAGE_BOOT;
    } else {
        /* The front end, latched by whichever of the two signals arrives
         * first: the game's own flow state saying MenuOrLobby - the main
         * menu and every lobby in one bucket, which is why a lobby is not a
         * stage of its own - or the world being up, which proves the front
         * end was passed even when the menu was never caught. A state that
         * cannot be read leaves the stage where it is: the thread's second
         * watchdog is what moves it on then, not a guess here. */
        if (!g_pastFront) {
            int s = ShGetGameState();

            if (s != SH_STATE_UNKNOWN)
                g_stateSeen = 1;
            if (s == SH_STATE_MENU) {
                g_pastFront = 1;
                Log("corefix: the main menu is up (state MenuOrLobby) - from "
                    "here every dial is the play one: the menu, a lobby, a "
                    "later load screen and the world are all play");
            } else if (InWorld(s)) {
                char nm[32];

                g_pastFront = 1;
                nm[0] = 0;
                ShGetGameStateName(nm, (int)sizeof(nm));
                Log("corefix: the world is up already (state %s) - the front "
                    "end must have been passed, so every dial from here on "
                    "is the play one", nm[0] ? nm : "?");
            }
        }
        st = g_pastFront ? STAGE_PLAY : STAGE_WINDOW;
    }

    /* Read through the floor a watchdog may have raised, so that a stage
     * never goes backwards even when a signal never arrived. */
    floor = (int)InterlockedCompareExchange(&g_stageFloor, 0, 0);
    return st < floor ? floor : st;
}

/* A change of stage always re-applies, even when two stages carry identical
 * dials: "leave alone" and "all cores" differ only in whether the system's
 * own trimming is undone, so the step itself is the event.
 *
 * Two 120 second guards, for the two signals that could never arrive: a
 * build whose windows no longer match the logo probe, and one where
 * the state module failed to hook. Each raises the floor the stage is read
 * through and says so - a dial stuck in force for a whole session would be
 * worse than one that starts a little early - and each counts within its
 * own stage, so neither can cut the other short. The second only runs while
 * the state machine has never answered: where it does answer, the front end
 * signal is real and the stage waits for it however long the first load
 * takes. */
#define STAGE_POLL_MS   250
#define STAGE_STATE_MS  120000

static DWORD WINAPI StageThread(LPVOID p)
{
    uint64_t stageAt = GetTickCount64();    /* when this stage went in force */

    (void)p;
    for (;;) {
        int stage = StageFromNow();

        if (stage == STAGE_BOOT &&
            GetTickCount64() - stageAt >= STAGE_STATE_MS) {
            InterlockedExchange(&g_stageFloor, STAGE_WINDOW);
            stage = STAGE_WINDOW;
            Log("corefix: the main window never came up after %lu s - the "
                "logo stage is over anyway",
                (unsigned long)(STAGE_STATE_MS / 1000));
        } else if (stage == STAGE_WINDOW && !g_stateSeen &&
                   GetTickCount64() - stageAt >= STAGE_STATE_MS) {
            InterlockedExchange(&g_stageFloor, STAGE_PLAY);
            stage = STAGE_PLAY;
            Log("corefix: the game's state has never been readable after %lu "
                "s in the window stage - the play stage takes over, since "
                "nothing here can tell the front end apart without it",
                (unsigned long)(STAGE_STATE_MS / 1000));
        }
        if (stage != g_stage) {
            ApplyDial(stage);
            ReportStage(stage);
            stageAt = GetTickCount64();
        }
        /* A held priority is checked rather than assumed. The class can
         * be changed by a process outside this one, where no hook of
         * ours is in the path - an anti-cheat or launcher service can do
         * it with its own handle - and a dial that quietly stops being
         * in force is worse than one that never was. */
        if (g_prioNow &&
            (DWORD)GetPriorityClass(GetCurrentProcess()) != (DWORD)g_prioNow) {
            DWORD was = GetPriorityClass(GetCurrentProcess());

            SetPriorityClass(GetCurrentProcess(), (DWORD)g_prioNow);
            note(&c_PHOLD, 3, "  priority class was 0x%lX, held back to "
                 "0x%lX", (unsigned long)was, (unsigned long)g_prioNow);
        }
        /* The switch is checked the same way and for the same reason, and
         * EcoWant is idempotent - it adopts a switch that is already on and
         * sets one that is missing - so one call per tick is the whole
         * check. No call at all while no dial asks for the mode: the "ours"
         * flag is clear and the second test short-circuits. */
        if (g_stage >= 0) {
            if (EffectivePrio(g_stage) == P_ECO)
                EcoWant(g_stage, 1);
            else if (InterlockedCompareExchange(&g_ecoOurs, 0, 0) &&
                     EcoIsOn() == 1)
                EcoWant(g_stage, 0);
        }
        Sleep(STAGE_POLL_MS);
    }
    return 0;
}

/* Started from the loader thread, not from DllMain: creating a thread
 * under the loader lock is how a start up deadlocks, and nothing here is
 * needed before the loader thread exists anyway.
 *
 * Always, not only when a dial asks for something: the stage is part of
 * the public CPU API (ShCpuStage, ShCpuOnStageChange), and a plugin must
 * not be told "boot" for a whole session because nobody asked for a trim.
 * The thread costs one EnumWindows and one state read per quarter second,
 * and it stops probing windows the moment the game's own window is up. */
void ShCoreFixLateStartup(void)
{
    static volatile LONG up;

    if (InterlockedExchange(&up, 1))
        return;
    CreateThread(NULL, 0, StageThread, NULL, 0, NULL);
}
