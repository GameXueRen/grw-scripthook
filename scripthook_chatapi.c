/* Direct chat send - research module (BUG7). See docs/chat-api.md.
 *
 * Round-3 recon (2026-09-08) corrected the picture: RVA 0xB37620 is
 * NOT a vtable - a single jmp thunk (the heap objects reference one
 * function, not a table) - and no transient instances were caught by
 * the 200ms heap watch.  The registry groups behind the
 * "SilexNetMessageChatMessage.Broadcast/Unicast" names hold ~13
 * function pointers each; uni.t0's target is REAL unpacked code.
 *
 * Round 4 (this file): stop guessing, watch the calls.  MinHook the
 * registry entry functions (the thunks are real code at fixed RVAs);
 * when the player sends a native chat message the fired hooks reveal
 * which function is the actual send, what `this` points to, and where
 * the text lives.  Every hook logs its first hits and dumps the
 * object, then chains to the trampoline - behaviour unchanged.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"
#include "log.h"
#include "third_party/minhook/include/MinHook.h"

#define RQ(a) ShReadQ(a)
extern int  ShReadableAddr(uint64_t addr, size_t len);
extern int  ShReadMem(uint64_t addr, void *out, size_t len);
extern uint64_t ShReadQ(uint64_t addr);
extern int  ShPropRttiOf(uint64_t obj, char *out, int n);

/* registry entry function slots (from the round-2 walk) */
static const struct { const char *name; uint64_t rva; } g_slots[] = {
    { "bcast.t0", 0xE446D0 }, { "bcast.t1", 0xE28FC0 },
    { "bcast.t2", 0xE27070 }, { "bcast.t3", 0xE3E900 },
    { "bcast.t4", 0xE32340 }, { "bcast.t5", 0xE3FFB0 },
    { "uni.t0",   0xB3B9B0 }, { "uni.t1",   0xB42610 },
    { "uni.t2",   0xB48510 }, { "uni.t3",   0xB48BD0 },
    { "uni.t4",   0xB488A0 }, { "uni.t5",   0xB37620 },
};
#define NSLOTS ((int)(sizeof(g_slots) / sizeof(g_slots[0])))

static void *g_orig[NSLOTS];
static volatile int g_hits[NSLOTS];

/* dump the object a hook received, hex+ascii, then follow the
 * heap pointers inside it - the message text sits behind one of
 * them, not in the first 0x60 bytes. */
static void DumpMemAt(const char *tag, uint64_t addr, int n);

static void DumpThis(const char *tag, uint64_t rcx) {
    char nm[200];
    if (!rcx) { Log("%s this=NULL", tag); return; }
    if (!ShReadableAddr(rcx, 8)) {
        Log("%s this=%llx (unreadable)", tag, (unsigned long long)rcx);
        return;
    }
    if (!ShPropRttiOf(rcx, nm, sizeof(nm))) nm[0] = 0;
    Log("%s this=%llx rtti=%s", tag, (unsigned long long)rcx, nm);
    DumpMemAt(tag, rcx, 0x100);
    {
        unsigned char buf[0x100];
        int i;
        if (!ShReadMem(rcx, buf, sizeof(buf))) return;
        for (i = 0; i + 8 <= (int)sizeof(buf); i += 8) {
            uint64_t v;
            char sub[80];
            memcpy(&v, buf + i, 8);
            /* follow plausible heap pointers, skip image + small */
            if (v > 0x100000000ULL && v < 0x800000000000ULL &&
                !ShInImage(v) && ShReadableAddr(v, 0x40)) {
                snprintf(sub, sizeof(sub), "%s.p%02x", tag, i);
                DumpMemAt(sub, v, 0x60);
            }
        }
    }
}

static void DumpMemAt(const char *tag, uint64_t addr, int n) {
    unsigned char buf[0x100];
    int i, got = n < (int)sizeof(buf) ? n : (int)sizeof(buf);
    if (!ShReadMem(addr, buf, got)) return;
    for (i = 0; i < got; i += 16) {
        char line[100];
        int j, p = 0;
        p += snprintf(line + p, sizeof(line) - p, "%s +%03x ", tag, i);
        for (j = i; j < i + 16 && j < got; j++)
            p += snprintf(line + p, sizeof(line) - p, "%02x", buf[j]);
        p += snprintf(line + p, sizeof(line) - p, " ");
        for (j = i; j < i + 16 && j < got; j++) {
            unsigned char c = buf[j];
            line[p++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        line[p] = 0;
        Log("%s", line);
    }
}

/* generic detour: logs then jumps to the trampoline.  One body per
 * arity would be overkill - all registry functions take (this, ...)
 * and we only forward RCX/RDX/R8/R9 through a matching prototype. */
typedef uint64_t (__attribute__((ms_abi)) *Fn4_t)(uint64_t, uint64_t,
                                                  uint64_t, uint64_t);

#define DEFINE_HOOK(idx)                                                   \
static uint64_t __attribute__((ms_abi)) Hook##idx(uint64_t a, uint64_t b, \
                                 uint64_t c, uint64_t d) {                 \
    if (g_hits[idx]++ < 6) {                                               \
        Log("HIT %s args %llx %llx %llx %llx", g_slots[idx].name,          \
            (unsigned long long)a, (unsigned long long)b,                  \
            (unsigned long long)c, (unsigned long long)d);                 \
        DumpThis(g_slots[idx].name, a);                                    \
    }                                                                      \
    return ((Fn4_t)g_orig[idx])(a, b, c, d);                               \
}

DEFINE_HOOK(0)  DEFINE_HOOK(1)  DEFINE_HOOK(2)  DEFINE_HOOK(3)
DEFINE_HOOK(4)  DEFINE_HOOK(5)  DEFINE_HOOK(6)  DEFINE_HOOK(7)
DEFINE_HOOK(8)  DEFINE_HOOK(9)  DEFINE_HOOK(10) DEFINE_HOOK(11)

typedef uint64_t __attribute__((ms_abi)) (*HookFn_t)(uint64_t, uint64_t,
                                                     uint64_t, uint64_t);
static HookFn_t g_hooks[NSLOTS] = {
    Hook0,  Hook1,  Hook2,  Hook3,
    Hook4,  Hook5,  Hook6,  Hook7,
    Hook8,  Hook9,  Hook10, Hook11,
};

static void InstallHooks(void) {
    int i, ok = 0;
    if (MH_Initialize() != MH_OK) { Log("MH_Initialize failed"); return; }
    for (i = 0; i < NSLOTS; i++) {
        void *tgt = (void *)(uintptr_t)SH_IMG(g_slots[i].rva);
        MH_STATUS s = MH_CreateHook(tgt, g_hooks[i], &g_orig[i]);
        if (s != MH_OK) {
            Log("hook %s rva %llx FAILED (%d)", g_slots[i].name,
                (unsigned long long)g_slots[i].rva, (int)s);
            continue;
        }
        if (MH_EnableHook(tgt) != MH_OK) {
            Log("enable %s FAILED", g_slots[i].name);
            continue;
        }
        Log("hook %s rva %llx armed", g_slots[i].name,
            (unsigned long long)g_slots[i].rva);
        ok++;
    }
    Log("hooks armed: %d/%d", ok, NSLOTS);
}

/* ---- thread ---------------------------------------------------------- */

static DWORD WINAPI ChatApiThread(LPVOID arg) {
    (void)arg;
    Sleep(8000);
    Log("chatapi round-4 (hooks) start base=%llx",
        (unsigned long long)ShImageBase());
    InstallHooks();
    for (;;) {
        int i, total = 0;
        Sleep(2000);
        for (i = 0; i < NSLOTS; i++) total += g_hits[i];
        if (total) Log("hook heartbeat: %d total hits", total);
    }
    return 0;
}

void ShChatApiStartup(void) {
    LogInit("scripthook_chatapi.log");
    Log("chatapi module loaded");
    CreateThread(NULL, 0, ChatApiThread, NULL, 0, NULL);
}
