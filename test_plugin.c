#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "image.h"

#define REPL_PORT 9999
#define RESP_MAX  (1024 * 2048)
#define CMD_TIMEOUT_MS 5000

static FILE    *g_log = NULL;
static uint8_t *g_imageBase = NULL;

typedef struct {
    char      name[9];
    uint8_t  *base;
    size_t    size;
    uint32_t  chars;
} Section;

static Section g_sections[64];
static int     g_nsections = 0;

static void PLog(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void MapSections(void) {
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_imageBase;
    IMAGE_NT_HEADERS *nt =
        (IMAGE_NT_HEADERS *)(g_imageBase + dos->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    int n = nt->FileHeader.NumberOfSections;
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) {
        memcpy(g_sections[i].name, sec[i].Name, 8);
        g_sections[i].name[8] = 0;
        g_sections[i].base  = g_imageBase + sec[i].VirtualAddress;
        g_sections[i].size  = sec[i].Misc.VirtualSize;
        g_sections[i].chars = sec[i].Characteristics;
        g_nsections++;
    }
}

/* ---- response buffer ---- */

typedef struct {
    char  *buf;
    int    len;
    int    cap;
} Resp;

static void RAppend(Resp *r, const char *fmt, ...) {
    if (r->len >= r->cap - 2) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(r->buf + r->len, r->cap - r->len, fmt, ap);
    va_end(ap);
    if (n > 0) r->len += n;
}

/* ---- safe memory access ---- */

/* cached readable ranges; avoids per-read VirtualQuery */
typedef struct { uintptr_t lo, hi; } Range;
static Range  *g_ranges = NULL;
static int     g_nranges = 0;

static void RefreshRanges(void) {
    if (g_ranges) free(g_ranges);
    int cap = 262144;
    g_ranges = (Range *)malloc(cap * sizeof(Range));
    g_nranges = 0;
    if (!g_ranges) return;

    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = NULL;
    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;
        if (mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        {
            if (g_nranges < cap) {
                g_ranges[g_nranges].lo = (uintptr_t)mbi.BaseAddress;
                g_ranges[g_nranges].hi =
                    (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
                g_nranges++;
            }
        }
        scan = next;
    }
}

static int CanRead(const void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr;
    if (a < 0x1000) return 0;
    if (!g_ranges) RefreshRanges();
    int lo = 0, hi = g_nranges - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (a < g_ranges[mid].lo) hi = mid - 1;
        else if (a >= g_ranges[mid].hi) lo = mid + 1;
        else return (a + len) <= g_ranges[mid].hi;
    }
    return 0;
}

static uint64_t SafeReadPtr(const void *addr) {
    if (!CanRead(addr, 8)) return 0;
    uint64_t val;
    memcpy(&val, addr, 8);
    return val;
}

static int SafeReadFloat3(const void *addr, float *out) {
    if (!CanRead(addr, 12)) return 0;
    memcpy(out, addr, 12);
    return 1;
}

static int SafeReadByte(const void *addr, uint8_t *out) {
    if (!CanRead(addr, 1)) return 0;
    *out = *(const uint8_t *)addr;
    return 1;
}

static int IsHeapPtr(uint64_t v) {
    return v >= 0x10000 && v < 0x7FF000000000ULL;
}

/* ---- crash reports ---- */

/* The handler lives in dinput8 now, so every user gets
 * reports. This is the control surface for it.
 */
static void CmdCrash(Resp *r, const char *line) {
    HMODULE m = GetModuleHandleA("dinput8.dll");
    int (*count)(void);
    int (*healed)(void);
    int (*report)(char *, int);
    int (*setFreeze)(int);
    int (*freezeOn)(void);
    char arg[32] = {0};
    char buf[4096];

    if (!m) { RAppend(r, "dinput8 not loaded\n"); return; }
    *(FARPROC *)&count = GetProcAddress(m, "ShCrashCount");
    *(FARPROC *)&healed = GetProcAddress(m, "ShCrashHealed");
    *(FARPROC *)&report = GetProcAddress(m, "ShCrashReport");
    *(FARPROC *)&setFreeze = GetProcAddress(m, "ShSetCrashFreeze");
    *(FARPROC *)&freezeOn = GetProcAddress(m, "ShCrashFreezeOn");
    if (!count || !report || !setFreeze) {
        RAppend(r, "no crash API, rebuild dinput8\n");
        return;
    }

    sscanf(line, "%*s %31s", arg);
    if (arg[0]) {
        setFreeze(strcmp(arg, "off") != 0);
        RAppend(r, "freeze %s\n", freezeOn() ? "on" : "off");
        return;
    }

    RAppend(r, "caught %d, healed %d, freeze %s\n",
            count(), healed ? healed() : -1,
            freezeOn && freezeOn() ? "on" : "off");
    if (count() > 0 && report(buf, (int)sizeof(buf)))
        RAppend(r, "first:\n%s", buf);
}

/* ---- commands ---- */

static void CmdHelp(Resp *r) {
    RAppend(r, "commands:\n");
    RAppend(r, "  crash [on|off]     crash report, freeze toggle\n");
    RAppend(r, "  base               image base address\n");
    RAppend(r, "  sections           list PE sections\n");
    RAppend(r, "  scan <string>      find string in memory\n");
    RAppend(r, "  xref <hex_addr>    find LEA refs to address\n");
    RAppend(r, "  read <hex> [len]   hex dump (default 128)\n");
    RAppend(r, "  readhex <hex> <len> raw hex, up to 512K, one line\n");
    RAppend(r, "  findq <start> <len> <val> <mask> [stride] [max]\n");
    RAppend(r, "                     qwords where (q & mask) == val\n");
    RAppend(r, "  readstr <hex>      read null-terminated string\n");
    RAppend(r, "  dis <hex> [len]    raw bytes for disasm (default 64)\n");
    RAppend(r, "  strings <hex> <len> printable strings in range\n");
    RAppend(r, "  silexlist          list SilexNetMessage names\n");
    RAppend(r, "  scanfloat <v> [t]  find float in all RW memory\n");
    RAppend(r, "  scanvec3 <x y z>   find 3 consecutive floats\n");
    RAppend(r, "  writefloat <a> <v> write float at address\n");
    RAppend(r, "  findplayer         find player via CT chain\n");
    RAppend(r, "  fault [go]         report a caught fault\n");
    RAppend(r, "Entities:\n");
    RAppend(r, "  ents [radius] [all] world registry by distance\n");
    RAppend(r, "  enthp <entity>     health sub component\n");
    RAppend(r, "  entmark [r|off]    label what you look at\n");
    RAppend(r, "ScriptHook API:\n");
    RAppend(r, "  api                state, player, health\n");
    RAppend(r, "  apitp <x y z>      ShTeleportPlayer\n");
    RAppend(r, "  apihop <x y z> [hop] [delayMs] [ox oy oz]\n");
    RAppend(r, "  apiground <x y> [c] ShTeleportPlayerToGround\n");
    RAppend(r, "  apiheight <x y>    ShGroundHeight\n");
    RAppend(r, "  apiplace <ent x y z> ShPlaceEntity\n");
    RAppend(r, "  apihp [set|god|nodie] [v]\n");
    RAppend(r, "  apifind [r] [kind] [all]  ShFindEntities\n");
    RAppend(r, "  apikind <entity>   ShGetEntityKind\n");
    RAppend(r, "  help               this message\n");
}

static void CmdBase(Resp *r) {
    RAppend(r, "image base: %p\n", g_imageBase);
}

static void CmdSections(Resp *r) {
    RAppend(r, "%-10s %-18s %-12s %s\n",
            "name", "base", "size", "flags");
    for (int i = 0; i < g_nsections; i++) {
        Section *s = &g_sections[i];
        RAppend(r, "%-10s %p  0x%08zx   0x%08x",
                s->name, s->base, s->size, s->chars);
        if (s->chars & IMAGE_SCN_MEM_EXECUTE) RAppend(r, " X");
        if (s->chars & IMAGE_SCN_MEM_READ)    RAppend(r, " R");
        if (s->chars & IMAGE_SCN_MEM_WRITE)   RAppend(r, " W");
        RAppend(r, "\n");
    }
}

static uint64_t ParseHex(const char *s) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    return strtoull(s, NULL, 16);
}

static void CmdScan(Resp *r, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) { RAppend(r, "usage: scan <string>\n"); return; }
    int found = 0;
    for (int s = 0; s < g_nsections; s++) {
        uint8_t *base = g_sections[s].base;
        size_t   sz   = g_sections[s].size;
        for (size_t i = 0; i + nlen <= sz; i++) {
            if (memcmp(base + i, needle, nlen) == 0) {
                char preview[80];
                size_t plen = 72;
                if (i + plen > sz) plen = sz - i;
                memcpy(preview, base + i, plen);
                preview[plen] = 0;
                for (size_t j = 0; j < plen; j++)
                    if (preview[j] < 32 || preview[j] > 126)
                        preview[j] = '.';
                RAppend(r, "  %p  %.8s+0x%zx  \"%s\"\n",
                        base + i, g_sections[s].name, i, preview);
                found++;
                if (found >= 50) {
                    RAppend(r, "  (truncated at 50)\n");
                    return;
                }
            }
        }
    }
    RAppend(r, "found %d matches\n", found);
}

/* search PE sections for raw bytes given as hex digits */
static void CmdScanHex(Resp *r, const char *hexs) {
    uint8_t pat[64];
    int n = 0;
    for (const char *p = hexs; p[0] && p[1] && n < 64; ) {
        if (p[0] == ' ') { p++; continue; }
        int hi = -1, lo = -1;
        for (int i = 0; i < 16; i++) {
            if ("0123456789abcdef"[i] == (p[0] | 32)) hi = i;
            if ("0123456789abcdef"[i] == (p[1] | 32)) lo = i;
        }
        if (hi < 0 || lo < 0) break;
        pat[n++] = (uint8_t)((hi << 4) | lo);
        p += 2;
    }
    if (n == 0) { RAppend(r, "usage: scanhex <hexbytes>\n"); return; }
    RAppend(r, "searching %d bytes...\n", n);

    int found = 0;
    for (int s = 0; s < g_nsections; s++) {
        uint8_t *base = g_sections[s].base;
        size_t sz = g_sections[s].size;
        if (sz < (size_t)n) continue;
        for (size_t i = 0; i + n <= sz; i++) {
            if (memcmp(base + i, pat, n) != 0) continue;
            RAppend(r, "  %p  %.8s+0x%zx\n",
                    base + i, g_sections[s].name, i);
            found++;
            if (found >= 40) goto done_sh;
        }
    }
done_sh:
    RAppend(r, "found %d\n", found);
}

/* write raw bytes (hex) into code or data */
static uint8_t g_patchOrig[64];
static uint8_t *g_patchAt = NULL;
static int g_patchLen = 0;

static void CmdPatch(Resp *r, const char *line) {
    char as[64] = {0}, hs[160] = {0};
    sscanf(line, "%*s %63s %159s", as, hs);
    if (!as[0] || !hs[0]) {
        RAppend(r, "usage: patch <addr> <hexbytes>\n");
        return;
    }
    uint8_t *addr = (uint8_t *)ParseHex(as);
    uint8_t buf[64];
    int n = 0;
    for (const char *p = hs; p[0] && p[1] && n < 64; p += 2) {
        int hi = -1, lo = -1;
        for (int i = 0; i < 16; i++) {
            if ("0123456789abcdef"[i] == (p[0] | 32)) hi = i;
            if ("0123456789abcdef"[i] == (p[1] | 32)) lo = i;
        }
        if (hi < 0 || lo < 0) break;
        buf[n++] = (uint8_t)((hi << 4) | lo);
    }
    if (n == 0) { RAppend(r, "bad hex\n"); return; }

    DWORD old;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &old)) {
        RAppend(r, "VirtualProtect failed %lu\n", GetLastError());
        return;
    }
    memcpy(g_patchOrig, addr, n);
    g_patchAt = addr;
    g_patchLen = n;
    memcpy(addr, buf, n);
    VirtualProtect(addr, n, old, &old);
    RAppend(r, "patched %d bytes at %p\n", n, addr);
}

static void CmdUnpatch(Resp *r) {
    if (!g_patchAt) { RAppend(r, "nothing patched\n"); return; }
    DWORD old;
    VirtualProtect(g_patchAt, g_patchLen, PAGE_EXECUTE_READWRITE, &old);
    memcpy(g_patchAt, g_patchOrig, g_patchLen);
    VirtualProtect(g_patchAt, g_patchLen, old, &old);
    RAppend(r, "restored %d bytes at %p\n", g_patchLen, g_patchAt);
    g_patchAt = NULL;
}

static void CmdXref(Resp *r, const char *addrStr) {
    uint8_t *target = (uint8_t *)ParseHex(addrStr);
    RAppend(r, "scanning for LEA refs to %p...\n", target);
    int found = 0;
    for (int s = 0; s < g_nsections; s++) {
        uint32_t ch = g_sections[s].chars;
        if (!(ch & IMAGE_SCN_MEM_EXECUTE) &&
            !(ch & IMAGE_SCN_CNT_CODE)) continue;
        uint8_t *base = g_sections[s].base;
        size_t   sz   = g_sections[s].size;
        if (sz < 7) continue;
        for (size_t i = 0; i <= sz - 7; i++) {
            uint8_t b0 = base[i];
            if (b0 != 0x48 && b0 != 0x4c) continue;
            if (base[i+1] != 0x8d) continue;
            uint8_t modrm = base[i+2];
            if ((modrm & 0xc7) != 0x05) continue;
            int32_t disp;
            memcpy(&disp, base + i + 3, 4);
            uint8_t *ref = base + i + 7 + disp;
            if (ref == target) {
                int reg = (modrm >> 3) & 7;
                if (b0 == 0x4c) reg += 8;
                static const char *rnames[] = {
                    "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                    "r8","r9","r10","r11","r12","r13","r14","r15"
                };
                RAppend(r, "  LEA %s, [rip+0x%x] at %p "
                        "(%.8s+0x%zx)\n",
                        rnames[reg], (unsigned)disp,
                        base + i, g_sections[s].name,
                        i);
                found++;
                if (found >= 30) goto done;
            }
        }
    }
done:
    RAppend(r, "found %d refs\n", found);
}

static void CmdRead(Resp *r, uint8_t *addr, int len) {
    static uint8_t copy[4096];
    SIZE_T got = 0;

    if (len > 4096) len = 4096;
    /* Through the kernel: a page released between the check
     * and the copy then fails here instead of faulting. */
    if (!CanRead(addr, len) ||
        !ReadProcessMemory(GetCurrentProcess(), addr, copy, len, &got) ||
        (int)got < len) {
        RAppend(r, "cannot read %d bytes at %p (unmapped)\n", len, addr);
        return;
    }
    RAppend(r, "dumping %d bytes at %p:\n", len, addr);
    for (int row = 0; row < len; row += 16) {
        RAppend(r, "  %p: ", addr + row);
        char ascii[17];
        int cols = (len - row < 16) ? len - row : 16;
        for (int c = 0; c < cols; c++) {
            uint8_t b = copy[row + c];
            RAppend(r, "%02x ", b);
            ascii[c] = (b >= 32 && b <= 126) ? (char)b : '.';
        }
        ascii[cols] = 0;
        for (int c = cols; c < 16; c++) RAppend(r, "   ");
        RAppend(r, " %s\n", ascii);
    }
}

static void CmdReadStr(Resp *r, uint8_t *addr) {
    char buf[1024];
    int i = 0;
    while (i < 1023 && addr[i] != 0) {
        buf[i] = addr[i];
        i++;
    }
    buf[i] = 0;
    RAppend(r, "%p: \"%s\" (%d bytes)\n", addr, buf, i);
}

static void CmdDis(Resp *r, uint8_t *addr, int len) {
    if (len > 512) len = 512;
    RAppend(r, "%d bytes at %p for disassembly:\n", len, addr);
    for (int row = 0; row < len; row += 16) {
        int cols = (len - row < 16) ? len - row : 16;
        RAppend(r, "%p: ", addr + row);
        for (int c = 0; c < cols; c++)
            RAppend(r, "%02x ", addr[row + c]);
        RAppend(r, "\n");
    }
}

static void CmdStrings(Resp *r, uint8_t *addr, size_t len) {
    if (len > 0x1000000) len = 0x1000000;
    int found = 0;
    size_t i = 0;
    while (i < len && found < 200) {
        if (addr[i] >= 32 && addr[i] <= 126) {
            size_t start = i;
            while (i < len && addr[i] >= 32 && addr[i] <= 126) i++;
            size_t slen = i - start;
            if (slen >= 4) {
                char tmp[256];
                size_t cpy = slen > 255 ? 255 : slen;
                memcpy(tmp, addr + start, cpy);
                tmp[cpy] = 0;
                RAppend(r, "  %p: \"%s\"\n", addr + start, tmp);
                found++;
            }
        } else {
            i++;
        }
    }
    RAppend(r, "found %d strings\n", found);
}

static void CmdSilexList(Resp *r) {
    RAppend(r, "scanning for SilexNetMessage names...\n");
    const char *prefix = "SilexNetMessage";
    size_t plen = strlen(prefix);
    int found = 0;
    for (int s = 0; s < g_nsections; s++) {
        uint8_t *base = g_sections[s].base;
        size_t   sz   = g_sections[s].size;
        for (size_t i = 0; i + plen <= sz; i++) {
            if (memcmp(base + i, prefix, plen) != 0) continue;
            if (i > 0 && base[i-1] >= 32 && base[i-1] <= 126)
                continue;
            char name[256];
            size_t n = 0;
            while (n < 255 && (i + n) < sz &&
                   base[i + n] >= 32 && base[i + n] <= 126) {
                name[n] = base[i + n];
                n++;
            }
            name[n] = 0;
            if (strchr(name, '.') == NULL) {
                RAppend(r, "  %p  %s\n", base + i, name);
                found++;
            }
            i += n;
        }
    }
    RAppend(r, "found %d message types\n", found);
}

/* ---- float/heap scanning ---- */

static void CmdScanFloat(Resp *r, const char *valStr, const char *tolStr) {
    float target = (float)atof(valStr);
    float tol = tolStr[0] ? (float)atof(tolStr) : 0.01f;
    if (target == 0.0f && valStr[0] != '0') {
        RAppend(r, "usage: scanfloat <value> [tolerance=0.01]\n");
        return;
    }
    RAppend(r, "scanning for float %.4f +/- %.4f...\n", target, tol);

    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *addr = NULL;
    int found = 0;

    while (VirtualQuery(addr, &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD))
        {
            uint8_t *base = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize;
            for (size_t i = 0; i + 4 <= sz; i += 4) {
                float val;
                memcpy(&val, base + i, 4);
                float diff = val - target;
                if (diff < 0) diff = -diff;
                if (diff <= tol) {
                    float nearby[4] = {0};
                    if (i >= 8)
                        memcpy(&nearby[0], base + i - 8, 4);
                    if (i >= 4)
                        memcpy(&nearby[1], base + i - 4, 4);
                    if (i + 8 <= sz)
                        memcpy(&nearby[2], base + i + 4, 4);
                    if (i + 12 <= sz)
                        memcpy(&nearby[3], base + i + 8, 4);
                    RAppend(r, "  %p: %.6f "
                            "[..%.2f, %.2f, HERE, %.2f, %.2f..]\n",
                            base + i, val,
                            nearby[0], nearby[1],
                            nearby[2], nearby[3]);
                    found++;
                    if (found >= 100) {
                        RAppend(r, "  (truncated at 100)\n");
                        goto done;
                    }
                }
            }
        }
        addr = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (addr < (uint8_t *)mbi.BaseAddress) break;
    }
done:
    RAppend(r, "found %d matches\n", found);
}

static void CmdScanFloat3(Resp *r, const char *v1s,
                          const char *v2s, const char *v3s,
                          uint64_t lo, uint64_t hi)
{
    float t1 = (float)atof(v1s);
    float t2 = (float)atof(v2s);
    float t3 = (float)atof(v3s);
    float tol = 0.5f;
    if (!hi) hi = ~0ULL;
    RAppend(r, "scanning %p..%p for vec3(%.2f, %.2f, %.2f) +/- %.2f\n",
            (void *)lo, (void *)hi, t1, t2, t3, tol);

    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *addr = (uint8_t *)lo;
    int found = 0;

    while (VirtualQuery(addr, &mbi, sizeof(mbi))) {
        if ((uint64_t)mbi.BaseAddress >= hi) break;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD))
        {
            uint8_t *base = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize;
            for (size_t i = 0; i + 12 <= sz; i += 4) {
                if ((uint64_t)(base + i) < lo) continue;
                if ((uint64_t)(base + i) >= hi) break;
                float v[3];
                memcpy(v, base + i, 12);
                float d0 = v[0]-t1; if (d0<0) d0=-d0;
                float d1 = v[1]-t2; if (d1<0) d1=-d1;
                float d2 = v[2]-t3; if (d2<0) d2=-d2;
                if (d0 <= tol && d1 <= tol && d2 <= tol) {
                    RAppend(r, "  %p: (%.4f, %.4f, %.4f)\n",
                            base + i, v[0], v[1], v[2]);
                    found++;
                    if (found >= 100) goto done3;
                }
            }
        }
        addr = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (addr < (uint8_t *)mbi.BaseAddress) break;
    }
done3:
    RAppend(r, "found %d matches\n", found);
}

static uint8_t *PlayerPosPtr(void);

/* CT entity chain, validated against live position */
static void CmdFindEnt(Resp *r) {
    uint8_t *pp = PlayerPosPtr();
    if (!pp) { RAppend(r, "player pos unknown\n"); return; }
    float tgt[3];
    memcpy(tgt, pp, 12);
    RAppend(r, "matching root pos (%.2f, %.2f, %.2f)\n",
            tgt[0], tgt[1], tgt[2]);

    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = (uint8_t *)0x1000000;
    int found = 0;

    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & PAGE_READWRITE) &&
            !(mbi.Protect & PAGE_GUARD))
        {
            uint8_t *b = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize;
            for (size_t o = 0; o + 8 <= sz; o += 8) {
                uint64_t list = SafeReadPtr(b + o);
                if (!IsHeapPtr(list)) continue;
                uint64_t arr = SafeReadPtr((void *)(list + 0x50));
                if (!IsHeapPtr(arr)) continue;

                for (int i = 0; i < 64; i++) {
                    uint64_t elem = SafeReadPtr((void *)(arr + i * 8));
                    if (!IsHeapPtr(elem)) continue;
                    uint64_t v = SafeReadPtr((void *)(elem + 0x18));
                    if (!IsHeapPtr(v)) continue;
                    uint8_t flag;
                    if (!SafeReadByte((void *)(v + 0x3FC), &flag)) continue;
                    if (!flag) continue;

                    uint64_t e2 = SafeReadPtr((void *)(elem + 0x10));
                    if (!IsHeapPtr(e2)) continue;
                    uint64_t p = SafeReadPtr((void *)(e2 + 0xE0));
                    if (!IsHeapPtr(p)) continue;
                    uint64_t q = SafeReadPtr((void *)(p + 0x10C0));
                    if (!IsHeapPtr(q)) continue;

                    float rp[3];
                    if (!SafeReadFloat3((void *)(q + 0x50), rp)) continue;
                    int ok = 1;
                    for (int k = 0; k < 3; k++) {
                        float d = rp[k] - tgt[k];
                        if (d < 0) d = -d;
                        if (!(d <= 3.0f)) ok = 0;
                    }
                    if (!ok) continue;

                    RAppend(r, "HIT list=%p arr=%p [%d] elem=%p\n",
                            b + o, (void *)arr, i, (void *)elem);
                    RAppend(r, "   e2=%p +E0=%p +10C0=%p\n",
                            (void *)e2, (void *)p, (void *)q);
                    RAppend(r, "   rootpos=%p (%.2f, %.2f, %.2f)\n",
                            (void *)(q + 0x50), rp[0], rp[1], rp[2]);
                    if (++found >= 6) return;
                }
            }
        }
        scan = next;
    }
    RAppend(r, "found %d\n", found);
}

/* shift every float triple matching the player position */
static void CmdTpShift(Resp *r, const char *line) {
    float dx;
    if (sscanf(line, "%*s %f", &dx) != 1) {
        RAppend(r, "usage: tpshift <dx>\n");
        return;
    }
    uint8_t *pp = PlayerPosPtr();
    if (!pp) { RAppend(r, "player not found\n"); return; }
    float p[3];
    memcpy(p, pp, 12);
    RAppend(r, "current (%.3f, %.3f, %.3f), shifting X by %.1f\n",
            p[0], p[1], p[2], dx);

    float zup[3] = {p[0], p[1], p[2]};
    float yup[3] = {p[0], p[2], p[1]};
    const float TOL = 0.35f;
    int n = 0;

    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = NULL;
    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & PAGE_READWRITE) &&
            !(mbi.Protect & PAGE_GUARD))
        {
            uint8_t *b = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize;
            for (size_t i = 0; i + 12 <= sz; i += 4) {
                float v[3];
                memcpy(v, b + i, 12);
                /* reject NaN and absurd values before comparing */
                int sane = 1;
                for (int k = 0; k < 3; k++) {
                    if (v[k] != v[k]) sane = 0;
                    float m = v[k] < 0 ? -v[k] : v[k];
                    if (m > 40000.0f) sane = 0;
                }
                if (!sane) continue;
                int okz = 1, oky = 1;
                for (int k = 0; k < 3; k++) {
                    float dz = v[k] - zup[k]; if (dz < 0) dz = -dz;
                    float dy = v[k] - yup[k]; if (dy < 0) dy = -dy;
                    if (!(dz <= TOL)) okz = 0;
                    if (!(dy <= TOL)) oky = 0;
                }
                if (!okz && !oky) continue;
                if (n > 5000) {
                    RAppend(r, "ABORT: >5000 matches, refusing\n");
                    return;
                }
                float nv = v[0] + dx;
                memcpy(b + i, &nv, 4);
                n++;
            }
        }
        scan = next;
    }
    RAppend(r, "shifted %d copies\n", n);
}

/* three consecutive doubles */
static void CmdScanVec3d(Resp *r, const char *line) {
    double t1, t2, t3;
    if (sscanf(line, "%*s %lf %lf %lf", &t1, &t2, &t3) != 3) {
        RAppend(r, "usage: scanvec3d <x> <y> <z>\n");
        return;
    }
    double tol = 0.5;
    RAppend(r, "scanning doubles (%.2f, %.2f, %.2f)...\n", t1, t2, t3);

    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = NULL;
    int found = 0;
    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD))
        {
            uint8_t *b = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize;
            for (size_t i = 0; i + 24 <= sz; i += 8) {
                double v[3];
                memcpy(v, b + i, 24);
                double d0 = v[0]-t1; if (d0<0) d0=-d0;
                double d1 = v[1]-t2; if (d1<0) d1=-d1;
                double d2 = v[2]-t3; if (d2<0) d2=-d2;
                if (d0 <= tol && d1 <= tol && d2 <= tol) {
                    RAppend(r, "  %p: (%.4f, %.4f, %.4f)\n",
                            b + i, v[0], v[1], v[2]);
                    if (++found >= 60) goto done_d;
                }
            }
        }
        scan = next;
    }
done_d:
    RAppend(r, "found %d\n", found);
}

static void CmdWriteDouble(Resp *r, const char *line) {
    char as[64] = {0};
    double val;
    if (sscanf(line, "%*s %63s %lf", as, &val) != 2) {
        RAppend(r, "usage: writedouble <addr> <val>\n");
        return;
    }
    uint8_t *addr = (uint8_t *)ParseHex(as);
    DWORD old;
    if (VirtualProtect(addr, 8, PAGE_READWRITE, &old)) {
        double prev;
        memcpy(&prev, addr, 8);
        memcpy(addr, &val, 8);
        VirtualProtect(addr, 8, old, &old);
        RAppend(r, "wrote %.4f at %p (was %.4f)\n", val, addr, prev);
    } else {
        RAppend(r, "protect failed\n");
    }
}

static void CmdWriteFloat(Resp *r, const char *addrStr,
                          const char *valStr)
{
    uint8_t *addr = (uint8_t *)ParseHex(addrStr);
    float val = (float)atof(valStr);
    DWORD oldProtect;
    if (VirtualProtect(addr, 4, PAGE_READWRITE, &oldProtect)) {
        float old;
        memcpy(&old, addr, 4);
        memcpy(addr, &val, 4);
        VirtualProtect(addr, 4, oldProtect, &oldProtect);
        RAppend(r, "wrote %.4f at %p (was %.4f)\n", val, addr, old);
    } else {
        RAppend(r, "VirtualProtect failed at %p (err %lu)\n",
                addr, GetLastError());
    }
}


/* writeq <addr> <hexvalue> */
static void CmdWriteQ(Resp *r, const char *line) {
    char as[64] = {0}, vs[64] = {0};
    uint8_t *addr;
    uint64_t val, old = 0;
    DWORD prot;

    if (sscanf(line, "%*s %63s %63s", as, vs) != 2) {
        RAppend(r, "usage: writeq <addr> <hexvalue>\n");
        return;
    }
    addr = (uint8_t *)ParseHex(as);
    val = ParseHex(vs);
    if (!VirtualProtect(addr, 8, PAGE_READWRITE, &prot)) {
        RAppend(r, "VirtualProtect failed at %p (err %lu)\n",
                addr, GetLastError());
        return;
    }
    memcpy(&old, addr, 8);
    memcpy(addr, &val, 8);
    VirtualProtect(addr, 8, prot, &prot);
    RAppend(r, "wrote %p at %p (was %p)\n",
            (void *)val, addr, (void *)old);
}

/* Arbitrary bytes, for building structs in game memory. */
/* Native UI through the ShUi* exports. */
static void CmdUi(Resp *r, const char *line) {
    HMODULE m = GetModuleHandleA("dinput8.dll");
    int (*ready)(void);
    uint32_t (*panel)(float, float, float, float, uint32_t, float);
    uint32_t (*label)(uint32_t, float, float, float, float,
                      const char *, uint32_t);
    uint32_t (*image)(uint32_t, float, float, float, float,
                      uint32_t, float);
    int (*setText)(uint32_t, const char *);
    int (*setPos)(uint32_t, float, float);
    int (*setSize)(uint32_t, float, float);
    int (*setColour)(uint32_t, uint32_t);
    int (*setAlpha)(uint32_t, float);
    int (*show)(uint32_t, int);
    int (*destroy)(uint32_t);
    int (*lastErr)(void);
    char sub[32] = {0};
    unsigned id = 0, parent = 0, rgb = 0xFFFFFF;
    float x = 0, y = 0, w = 0, h = 0, a = 1.0f;
    const char *text;
    int ok = 0;

    if (!m) { RAppend(r, "dinput8 missing\n"); return; }
    *(FARPROC *)&ready = GetProcAddress(m, "ShUiReady");
    *(FARPROC *)&panel = GetProcAddress(m, "ShUiPanel");
    *(FARPROC *)&label = GetProcAddress(m, "ShUiLabel");
    *(FARPROC *)&image = GetProcAddress(m, "ShUiImage");
    *(FARPROC *)&setText = GetProcAddress(m, "ShUiSetText");
    *(FARPROC *)&setPos = GetProcAddress(m, "ShUiSetPos");
    *(FARPROC *)&setSize = GetProcAddress(m, "ShUiSetSize");
    *(FARPROC *)&setColour = GetProcAddress(m, "ShUiSetColour");
    *(FARPROC *)&setAlpha = GetProcAddress(m, "ShUiSetAlpha");
    *(FARPROC *)&show = GetProcAddress(m, "ShUiShow");
    *(FARPROC *)&destroy = GetProcAddress(m, "ShUiDestroy");
    *(FARPROC *)&lastErr = GetProcAddress(m, "ShLastError");
    if (!ready || !panel || !label || !destroy) {
        RAppend(r, "ShUi* exports missing\n");
        return;
    }
    sscanf(line, "%*s %31s", sub);
    if (strcmp(sub, "enable") == 0) {
        void (*enable)(int);
        int on = 1;
        *(FARPROC *)&enable = GetProcAddress(m, "ShUiEnable");
        sscanf(line, "%*s %*s %d", &on);
        if (!enable) { RAppend(r, "ShUiEnable missing\n"); return; }
        enable(on);
        RAppend(r, "native ui %s\n", on ? "enabled" : "disabled");
        return;
    }
    if (strcmp(sub, "ready") == 0) {
        ok = ready();
        RAppend(r, "ready %d err %d\n", ok, lastErr ? lastErr() : -1);
        return;
    }
    if (strcmp(sub, "scene") == 0) {
        uint32_t (*screate)(const char *, int);
        int (*sorder)(uint32_t, int);
        int (*sshow)(uint32_t, int);
        int (*sdestroy)(uint32_t);
        char what[16] = {0};
        int a = 0, b = 0;
        *(FARPROC *)&screate = GetProcAddress(m, "ShUiSceneCreate");
        *(FARPROC *)&sorder = GetProcAddress(m, "ShUiSceneSetOrder");
        *(FARPROC *)&sshow = GetProcAddress(m, "ShUiSceneShow");
        *(FARPROC *)&sdestroy = GetProcAddress(m, "ShUiSceneDestroy");
        sscanf(line, "%*s %*s %15s %d %d", what, &a, &b);
        if (!screate || !sorder || !sshow || !sdestroy) {
            RAppend(r, "scene exports missing\n");
        } else if (strcmp(what, "new") == 0) {
            RAppend(r, "scene id %u\n", screate("repl", a));
        } else if (strcmp(what, "order") == 0) {
            RAppend(r, "order ok %d\n", sorder((uint32_t)a, b));
        } else if (strcmp(what, "show") == 0) {
            RAppend(r, "show ok %d\n", sshow((uint32_t)a, b));
        } else if (strcmp(what, "destroy") == 0) {
            RAppend(r, "destroy ok %d\n", sdestroy((uint32_t)a));
        } else {
            RAppend(r, "usage: ui scene new order | order id o | "
                       "show id v | destroy id\n");
        }
        return;
    }
    if (strcmp(sub, "createin") == 0) {
        uint32_t (*createin)(uint32_t, uint32_t, int, float, float,
                             float, float);
        unsigned sid = 0, parent = 0; int cls = 0;
        float x = 0, y = 0, w = 0, h = 0;
        *(FARPROC *)&createin = GetProcAddress(m, "ShUiCreateIn");
        if (!createin) { RAppend(r, "ShUiCreateIn missing\n"); return; }
        if (sscanf(line, "%*s %*s %u %u %d %f %f %f %f", &sid, &parent,
                   &cls, &x, &y, &w, &h) != 7) {
            RAppend(r, "usage: ui createin scene parent cls x y w h\n");
            return;
        }
        id = createin(sid, parent, cls, x, y, w, h);
        RAppend(r, "createin id %u err %d\n", id, lastErr ? lastErr() : -1);
        return;
    }
    if (strcmp(sub, "scenes") == 0) {
        int (*scenes)(char *, int);
        int (*active)(const char *);
        char list[1024] = {0}, name[64] = {0};
        *(FARPROC *)&scenes = GetProcAddress(m, "ShGameScenes");
        *(FARPROC *)&active = GetProcAddress(m, "ShGameSceneActive");
        if (!scenes || !active) { RAppend(r, "scene exports missing\n"); return; }
        if (sscanf(line, "%*s %*s %63s", name) == 1)
            RAppend(r, "%s active %d\n", name, active(name));
        RAppend(r, "%d drawn last frame: %s\n", scenes(list, sizeof(list)), list);
        return;
    }
    if (strcmp(sub, "reparent") == 0) {
        int (*rep)(uint32_t, uint32_t, int);
        unsigned parent = 0; int at = -1;
        *(FARPROC *)&rep = GetProcAddress(m, "ShUiReparent");
        if (!rep) { RAppend(r, "ShUiReparent missing\n"); return; }
        sscanf(line, "%*s %*s %u %u %d", &id, &parent, &at);
        RAppend(r, "reparent ok %d err %d\n", rep(id, parent, at),
                lastErr ? lastErr() : -1);
        return;
    }
    if (strcmp(sub, "children") == 0) {
        int (*count)(uint32_t);
        uint32_t (*at)(uint32_t, int);
        int i, n;
        *(FARPROC *)&count = GetProcAddress(m, "ShUiChildCount");
        *(FARPROC *)&at = GetProcAddress(m, "ShUiChildAt");
        if (!count || !at) { RAppend(r, "child exports missing\n"); return; }
        sscanf(line, "%*s %*s %u", &id);
        n = count(id);
        RAppend(r, "id %u has %d children:", id, n);
        for (i = 0; i < n; i++) RAppend(r, " %u", at(id, i));
        RAppend(r, "\n");
        return;
    }
    if (strcmp(sub, "focus") == 0) {
        int (*focus)(uint32_t, int);
        uint32_t (*focused)(void);
        unsigned sid = 0; int take = 1;
        *(FARPROC *)&focus = GetProcAddress(m, "ShUiFocus");
        *(FARPROC *)&focused = GetProcAddress(m, "ShUiFocused");
        if (!focus || !focused) { RAppend(r, "focus exports missing\n"); return; }
        sscanf(line, "%*s %*s %u %d", &sid, &take);
        RAppend(r, "focus ok %d now %u\n", focus(sid, take), focused());
        return;
    }
    if (strcmp(sub, "autosize") == 0) {
        int (*autosz)(uint32_t, int, int);
        int aw = 1, ah = 0;
        *(FARPROC *)&autosz = GetProcAddress(m, "ShUiSetAutoSize");
        if (!autosz) { RAppend(r, "ShUiSetAutoSize missing\n"); return; }
        sscanf(line, "%*s %*s %u %d %d", &id, &aw, &ah);
        RAppend(r, "autosize ok %d\n", autosz(id, aw, ah));
        return;
    }
    if (strcmp(sub, "batchtest") == 0) {
        /* begin, n position edits, commit, on this thread */
        int (*bbegin)(void); int (*bcommit)(void);
        int (*setv)(uint32_t, uint32_t, const float *, int);
        unsigned n = 20; int i, ok = 1; DWORD t0;
        float v[3] = {0, 20, 0};
        *(FARPROC *)&bbegin = GetProcAddress(m, "ShUiBegin");
        *(FARPROC *)&bcommit = GetProcAddress(m, "ShUiCommit");
        *(FARPROC *)&setv = GetProcAddress(m, "ShUiSetV");
        sscanf(line, "%*s %*s %u %u", &id, &n);
        if (!bbegin || !bcommit || !setv) { RAppend(r, "exports missing\n"); return; }
        t0 = GetTickCount();
        if (!bbegin()) { RAppend(r, "begin failed\n"); return; }
        for (i = 0; i < (int)n; i++) {
            v[0] = 20.0f + 3.0f * (float)i;
            if (!setv(id, 1, v, 3)) ok = 0;
        }
        RAppend(r, "batchtest id %u n %u edits ok %d commit %d in %lu ms\n",
                id, n, ok, bcommit(), (unsigned long)(GetTickCount() - t0));
        return;
    }
    if (strcmp(sub, "batch") == 0) {
        int (*bbegin)(void); int (*bcommit)(void); int (*babort)(void);
        char what[16] = {0};
        *(FARPROC *)&bbegin = GetProcAddress(m, "ShUiBegin");
        *(FARPROC *)&bcommit = GetProcAddress(m, "ShUiCommit");
        *(FARPROC *)&babort = GetProcAddress(m, "ShUiAbort");
        sscanf(line, "%*s %*s %15s", what);
        if (!bbegin || !bcommit || !babort) { RAppend(r, "batch exports missing\n"); return; }
        if (strcmp(what, "begin") == 0) RAppend(r, "begin ok %d\n", bbegin());
        else if (strcmp(what, "commit") == 0) RAppend(r, "commit ok %d\n", bcommit());
        else if (strcmp(what, "abort") == 0) RAppend(r, "abort ok %d\n", babort());
        else RAppend(r, "usage: ui batch begin|commit|abort\n");
        return;
    }
    if (strcmp(sub, "font") == 0 || strcmp(sub, "image") == 0) {
        int (*setdef)(const char *);
        char guid[64] = {0};
        *(FARPROC *)&setdef = GetProcAddress(m, strcmp(sub, "font") == 0
                                             ? "ShUiSetDefaultFont"
                                             : "ShUiSetDefaultImage");
        sscanf(line, "%*s %*s %63s", guid);
        if (!setdef) { RAppend(r, "export missing\n"); return; }
        ok = setdef(guid);
        RAppend(r, "default %s %s ok %d err %d\n", sub, guid, ok,
                lastErr ? lastErr() : -1);
        return;
    }
    if (strcmp(sub, "create") == 0) {
        /* ui create <parent> <cls> x y w h */
        uint32_t (*create)(uint32_t, int, float, float, float, float);
        unsigned parent = 0; int cls = 0; float cx = 0, cy = 0, cw = 0, ch = 0;
        *(FARPROC *)&create = GetProcAddress(m, "ShUiCreate");
        if (sscanf(line, "%*s %*s %u %d %f %f %f %f", &parent, &cls,
                   &cx, &cy, &cw, &ch) != 6 || !create) {
            RAppend(r, "usage: ui create <parent> <cls 1..4> x y w h\n");
            return;
        }
        id = create(parent, cls, cx, cy, cw, ch);
        RAppend(r, "create id %u err %d\n", id, lastErr ? lastErr() : -1);
        return;
    }
    if (strcmp(sub, "texraw") == 0) {
        /* ui texraw <addr> <w> <h>: RGBA8 already in memory */
        uint32_t (*texc)(int, int, const uint8_t *, int);
        char as[64] = {0}; int w = 0, h = 0; uint64_t addr;
        *(FARPROC *)&texc = GetProcAddress(m, "ShUiTextureCreate");
        if (sscanf(line, "%*s %*s %63s %d %d", as, &w, &h) != 3 || !texc) {
            RAppend(r, "usage: ui texraw <addr> <w> <h>\n"); return;
        }
        addr = ParseHex(as);
        if (!CanRead((void *)addr, (size_t)w * h * 4)) { RAppend(r, "pixels unreadable\n"); return; }
        id = texc(w, h, (const uint8_t *)addr, w * 4);
        RAppend(r, "texture id %u err %d\n", id, lastErr ? lastErr() : -1);
        return;
    }
    if (strcmp(sub, "imgset") == 0) {
        int (*imgset)(uint32_t, uint32_t);
        unsigned tid = 0;
        *(FARPROC *)&imgset = GetProcAddress(m, "ShUiImageSet");
        if (sscanf(line, "%*s %*s %u %u", &id, &tid) != 2 || !imgset) {
            RAppend(r, "usage: ui imgset <widget> <texture>\n"); return;
        }
        ok = imgset(id, tid);
        RAppend(r, "imgset ok %d err %d\n", ok, lastErr ? lastErr() : -1);
        return;
    }
    if (strcmp(sub, "props") == 0) {
        int (*count)(void);
        int (*at)(int, char *, int, uint32_t *, int *);
        int i, n;
        *(FARPROC *)&count = GetProcAddress(m, "ShUiPropCount");
        *(FARPROC *)&at = GetProcAddress(m, "ShUiPropAt");
        if (!count || !at) { RAppend(r, "prop exports missing\n"); return; }
        {
            int (*stats)(int *, uint64_t *);
            int sections = 0; uint64_t bytes = 0;
            *(FARPROC *)&stats = GetProcAddress(m, "ShUiPropStats");
            if (stats) {
                stats(&sections, &bytes);
                RAppend(r, "scanned %d sections, %llu bytes\n", sections,
                        (unsigned long long)bytes);
            }
        }
        n = count();
        RAppend(r, "%d property records\n", n);
        for (i = 0; i < n; i++) {
            char cls[48]; uint32_t pid; int type;
            if (at(i, cls, sizeof(cls), &pid, &type))
                RAppend(r, "  %-24s #%02x type %d\n", cls, pid, type);
        }
        return;
    }
    if (strcmp(sub, "ptype") == 0 || strcmp(sub, "getf") == 0 ||
        strcmp(sub, "getv") == 0 || strcmp(sub, "gets") == 0 ||
        strcmp(sub, "setf") == 0 || strcmp(sub, "setu") == 0 ||
        strcmp(sub, "setv") == 0 || strcmp(sub, "sets") == 0 ||
        strcmp(sub, "measure") == 0) {
        int (*ptype)(uint32_t, uint32_t);
        int (*setf)(uint32_t, uint32_t, float);
        int (*setu)(uint32_t, uint32_t, uint32_t);
        int (*setv)(uint32_t, uint32_t, const float *, int);
        int (*sets)(uint32_t, uint32_t, const char *);
        int (*getf)(uint32_t, uint32_t, float *);
        int (*getv)(uint32_t, uint32_t, float *, int);
        int (*gets)(uint32_t, uint32_t, char *, int);
        int (*measure)(uint32_t, float *, float *);
        unsigned pid = 0; float fv[3] = {0, 0, 0}; char sv[200] = {0};
        int n = 0;
        *(FARPROC *)&ptype = GetProcAddress(m, "ShUiPropType");
        *(FARPROC *)&setf = GetProcAddress(m, "ShUiSetF");
        *(FARPROC *)&setu = GetProcAddress(m, "ShUiSetU");
        *(FARPROC *)&setv = GetProcAddress(m, "ShUiSetV");
        *(FARPROC *)&sets = GetProcAddress(m, "ShUiSetS");
        *(FARPROC *)&getf = GetProcAddress(m, "ShUiGetF");
        *(FARPROC *)&getv = GetProcAddress(m, "ShUiGetV");
        *(FARPROC *)&gets = GetProcAddress(m, "ShUiGetS");
        *(FARPROC *)&measure = GetProcAddress(m, "ShUiMeasure");
        if (!ptype || !setf || !measure) { RAppend(r, "prop exports missing\n"); return; }
        if (strcmp(sub, "measure") == 0) {
            sscanf(line, "%*s %*s %u", &id);
            ok = measure(id, &fv[0], &fv[1]);
            RAppend(r, "measure id %u ok %d w %.1f h %.1f\n", id, ok, fv[0], fv[1]);
            return;
        }
        if (sscanf(line, "%*s %*s %u %x", &id, &pid) != 2) {
            RAppend(r, "usage: ui %s id prop [values]\n", sub);
            return;
        }
        if (strcmp(sub, "ptype") == 0) {
            RAppend(r, "id %u prop #%x type %d\n", id, pid, ptype(id, pid));
        } else if (strcmp(sub, "setf") == 0) {
            sscanf(line, "%*s %*s %*u %*x %f", &fv[0]);
            ok = setf(id, pid, fv[0]);
            RAppend(r, "setf ok %d\n", ok);
        } else if (strcmp(sub, "setu") == 0) {
            unsigned uv = 0;
            sscanf(line, "%*s %*s %*u %*x %u", &uv);
            ok = setu(id, pid, uv);
            RAppend(r, "setu ok %d\n", ok);
        } else if (strcmp(sub, "setv") == 0) {
            n = sscanf(line, "%*s %*s %*u %*x %f %f %f", &fv[0], &fv[1], &fv[2]);
            ok = setv(id, pid, fv, n);
            RAppend(r, "setv n %d ok %d\n", n, ok);
        } else if (strcmp(sub, "sets") == 0) {
            const char *t = strstr(line, " text ");
            ok = sets(id, pid, t ? t + 6 : "");
            RAppend(r, "sets ok %d\n", ok);
        } else if (strcmp(sub, "getf") == 0) {
            ok = getf(id, pid, &fv[0]);
            RAppend(r, "getf ok %d v %.3f\n", ok, fv[0]);
        } else if (strcmp(sub, "getv") == 0) {
            n = ptype(id, pid) == 5 ? 3 : 2;
            ok = getv(id, pid, fv, n);
            RAppend(r, "getv ok %d v %.3f %.3f %.3f\n", ok, fv[0], fv[1], fv[2]);
        } else if (strcmp(sub, "gets") == 0) {
            ok = gets(id, pid, sv, sizeof(sv));
            RAppend(r, "gets ok %d %s\n", ok, sv);
        }
        return;
    }
    if (strcmp(sub, "panel") == 0) {
        if (sscanf(line, "%*s %*s %f %f %f %f %x %f",
                   &x, &y, &w, &h, &rgb, &a) < 5) {
            RAppend(r, "usage: ui panel x y w h rgb [alpha]\n");
            return;
        }
        id = panel(x, y, w, h, rgb, a);
        RAppend(r, "panel id %u err %d\n", id, lastErr ? lastErr() : -1);
        return;
    }
    if (strcmp(sub, "label") == 0) {
        if (sscanf(line, "%*s %*s %u %f %f %f %f %x",
                   &parent, &x, &y, &w, &h, &rgb) != 6) {
            RAppend(r, "usage: ui label panel x y w h rgb text\n");
            return;
        }
        text = strstr(line, " text ");
        text = text ? text + 6 : "text";
        id = label(parent, x, y, w, h, text, rgb);
        RAppend(r, "label id %u err %d\n", id, lastErr ? lastErr() : -1);
        return;
    }
    if (strcmp(sub, "image") == 0) {
        if (sscanf(line, "%*s %*s %u %f %f %f %f %x %f",
                   &parent, &x, &y, &w, &h, &rgb, &a) < 6) {
            RAppend(r, "usage: ui image panel x y w h rgb [alpha]\n");
            return;
        }
        id = image(parent, x, y, w, h, rgb, a);
        RAppend(r, "image id %u err %d\n", id, lastErr ? lastErr() : -1);
        return;
    }
    if (strcmp(sub, "text") == 0) {
        char tmp[256] = {0};
        if (sscanf(line, "%*s %*s %u %255[^\n]", &id, tmp) != 2) {
            RAppend(r, "usage: ui text id words\n");
            return;
        }
        ok = setText(id, tmp);
    } else if (strcmp(sub, "pos") == 0) {
        if (sscanf(line, "%*s %*s %u %f %f", &id, &x, &y) != 3) {
            RAppend(r, "usage: ui pos id x y\n"); return;
        }
        ok = setPos(id, x, y);
    } else if (strcmp(sub, "size") == 0) {
        if (sscanf(line, "%*s %*s %u %f %f", &id, &w, &h) != 3) {
            RAppend(r, "usage: ui size id w h\n"); return;
        }
        ok = setSize(id, w, h);
    } else if (strcmp(sub, "colour") == 0) {
        if (sscanf(line, "%*s %*s %u %x", &id, &rgb) != 2) {
            RAppend(r, "usage: ui colour id rrggbb\n"); return;
        }
        ok = setColour(id, rgb);
    } else if (strcmp(sub, "alpha") == 0) {
        if (sscanf(line, "%*s %*s %u %f", &id, &a) != 2) {
            RAppend(r, "usage: ui alpha id a\n"); return;
        }
        ok = setAlpha(id, a);
    } else if (strcmp(sub, "show") == 0) {
        int v = 1;
        if (sscanf(line, "%*s %*s %u %d", &id, &v) != 2) {
            RAppend(r, "usage: ui show id 0|1\n"); return;
        }
        ok = show(id, v);
    } else if (strcmp(sub, "destroy") == 0) {
        if (sscanf(line, "%*s %*s %u", &id) != 1) {
            RAppend(r, "usage: ui destroy id\n"); return;
        }
        ok = destroy(id);
    } else {
        RAppend(r, "ui ready|panel|label|image|text|pos|size|"
                   "colour|alpha|show|destroy\n");
        return;
    }
    RAppend(r, "%s id %u ok %d err %d\n", sub, id, ok,
            lastErr ? lastErr() : -1);
}

static void CmdWriteBytes(Resp *r, const char *line) {
    char as[64] = {0}, hs[2048] = {0};
    uint8_t buf[1024], *addr;
    int n = 0;
    SIZE_T put = 0;

    if (sscanf(line, "%*s %63s %2047s", as, hs) != 2) {
        RAppend(r, "usage: writebytes <addr> <hexbytes>\n");
        return;
    }
    addr = (uint8_t *)ParseHex(as);
    for (const char *p = hs; p[0] && p[1] && n < 1024; p += 2) {
        unsigned v;
        if (sscanf(p, "%2x", &v) != 1) break;
        buf[n++] = (uint8_t)v;
    }
    if (!WriteProcessMemory(GetCurrentProcess(), addr, buf, n, &put)) {
        RAppend(r, "write failed at %p (err %lu)\n", addr,
                GetLastError());
        return;
    }
    RAppend(r, "wrote %d bytes at %p\n", (int)put, addr);
}

/* Scratch memory inside the process, never freed. */
static void CmdAlloc(Resp *r, const char *line) {
    char ss[64] = {0};
    size_t size;
    void *mem;

    sscanf(line, "%*s %63s", ss);
    size = ss[0] ? (size_t)ParseHex(ss) : 4096;
    mem = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE,
                       PAGE_READWRITE);
    if (!mem) {
        RAppend(r, "VirtualAlloc failed (err %lu)\n", GetLastError());
        return;
    }
    RAppend(r, "alloc %p size %llx\n", mem, (unsigned long long)size);
}

/* Scan once for objects with a given vtable, then follow an
 * offset chain in process and dump the bytes, one line each.
 */
static void CmdChase(Resp *r, const char *line) {
    char vs[64] = {0}, os[128] = {0}, ls[32] = {0}, ms[32] = {0};
    int64_t offs[8];
    int noffs = 0, len, max, found = 0;
    uint64_t want;
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = NULL;

    sscanf(line, "%*s %63s %127s %31s %31s", vs, os, ls, ms);
    if (!vs[0] || !os[0]) {
        RAppend(r, "usage: chase <vtable> <off,off,..> [len] [max]\n");
        RAppend(r, "  derefs at every offset but the last\n");
        return;
    }
    want = ParseHex(vs);
    len = ls[0] ? (int)ParseHex(ls) : 0x40;
    if (len > 512) len = 512;
    max = ms[0] ? (int)ParseHex(ms) : 4000;
    for (char *p = os; *p && noffs < 8; ) {
        char *comma = strchr(p, ',');
        if (comma) *comma = 0;
        offs[noffs++] = (int64_t)ParseHex(p);
        if (!comma) break;
        p = comma + 1;
    }
    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                            PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY)) &&
            !(mbi.Protect & PAGE_GUARD))
        {
            uint8_t *b = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize;
            for (size_t o = 0; o + 8 <= sz; o += 8) {
                uint64_t v, p;
                int i;
                memcpy(&v, b + o, 8);
                if (v != want) continue;
                p = (uint64_t)(b + o);
                RAppend(r, "obj=%p ", (void *)p);
                for (i = 0; i < noffs - 1 && p; i++)
                    p = SafeReadPtr((void *)(p + offs[i]));
                if (p) p += offs[noffs - 1];
                if (!p || !CanRead((void *)p, len)) {
                    RAppend(r, "at=%p -\n", (void *)p);
                } else {
                    RAppend(r, "at=%p ", (void *)p);
                    for (i = 0; i < len; i++)
                        RAppend(r, "%02x", ((uint8_t *)p)[i]);
                    RAppend(r, "\n");
                }
                if (++found >= max) goto done_chase;
            }
        }
        scan = next;
    }
done_chase:
    RAppend(r, "found %d\n", found);
}

/* ---- differential float scanner ---- */

typedef struct { uint8_t *addr; float val; } FCand;
static FCand  *g_cands = NULL;
static size_t  g_ncands = 0, g_capcands = 0;

static void CmdFsInit(Resp *r, const char *line) {
    float lo = -30000.0f, hi = 30000.0f;
    char als[64] = {0}, ahs[64] = {0};
    sscanf(line, "%*s %f %f %63s %63s", &lo, &hi, als, ahs);
    uint64_t alo = als[0] ? ParseHex(als) : 0;
    uint64_t ahi = ahs[0] ? ParseHex(ahs) : ~0ULL;
    if (g_cands) free(g_cands);
    g_capcands = 6u << 20;
    g_cands = (FCand *)malloc(g_capcands * sizeof(FCand));
    if (!g_cands) { RAppend(r, "alloc failed\n"); return; }
    g_ncands = 0;

    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = (uint8_t *)alo;
    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;
        if ((uint64_t)mbi.BaseAddress >= ahi) break;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD))
        {
            uint8_t *b = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize;
            for (size_t o = 0; o + 4 <= sz; o += 4) {
                if ((uint64_t)(b + o) < alo) continue;
                if ((uint64_t)(b + o) >= ahi) break;
                float v;
                memcpy(&v, b + o, 4);
                if (!(v > lo && v < hi)) continue;
                float a = v < 0 ? -v : v;
                if (a < 0.01f) continue;
                if (g_ncands >= g_capcands) goto full;
                g_cands[g_ncands].addr = b + o;
                g_cands[g_ncands].val = v;
                g_ncands++;
            }
        }
        scan = next;
    }
full:
    RAppend(r, "candidates: %llu\n", (unsigned long long)g_ncands);
}

static void CmdFsFilter(Resp *r, int wantChanged) {
    if (!g_cands) { RAppend(r, "run fsinit first\n"); return; }
    size_t w = 0;
    for (size_t i = 0; i < g_ncands; i++) {
        if (!CanRead(g_cands[i].addr, 4)) continue;
        float v;
        memcpy(&v, g_cands[i].addr, 4);
        int changed = (v != g_cands[i].val);
        if (changed == wantChanged) {
            g_cands[w].addr = g_cands[i].addr;
            g_cands[w].val = v;
            w++;
        }
    }
    g_ncands = w;
    RAppend(r, "candidates: %llu\n", (unsigned long long)g_ncands);
}

static void CmdFsList(Resp *r, const char *line) {
    int n = 20, skip = 0;
    sscanf(line, "%*s %d %d", &n, &skip);
    if (n > 400) n = 400;
    int shown = 0;
    for (size_t i = (size_t)skip; i < g_ncands && shown < n; i++, shown++)
        RAppend(r, "  %p = %.4f\n",
                g_cands[i].addr, g_cands[i].val);
    RAppend(r, "total %llu (skip %d)\n",
            (unsigned long long)g_ncands, skip);
}

/* keep candidates inside an address range */
static void CmdFsRange(Resp *r, const char *line) {
    char a[64] = {0}, b[64] = {0};
    sscanf(line, "%*s %63s %63s", a, b);
    if (!a[0] || !b[0]) { RAppend(r, "usage: fsrange <lo> <hi>\n"); return; }
    uint8_t *lo = (uint8_t *)ParseHex(a);
    uint8_t *hi = (uint8_t *)ParseHex(b);
    size_t w = 0;
    for (size_t i = 0; i < g_ncands; i++)
        if (g_cands[i].addr >= lo && g_cands[i].addr < hi)
            g_cands[w++] = g_cands[i];
    g_ncands = w;
    RAppend(r, "candidates: %llu\n", (unsigned long long)g_ncands);
}

/* print candidates inside a range without filtering */
static void CmdFsIn(Resp *r, const char *line) {
    char a[64] = {0}, b[64] = {0};
    sscanf(line, "%*s %63s %63s", a, b);
    if (!a[0] || !b[0]) { RAppend(r, "usage: fsin <lo> <hi>\n"); return; }
    uint8_t *lo = (uint8_t *)ParseHex(a);
    uint8_t *hi = (uint8_t *)ParseHex(b);
    int found = 0;
    for (size_t i = 0; i < g_ncands; i++) {
        if (g_cands[i].addr < lo || g_cands[i].addr >= hi) continue;
        if (found < 40)
            RAppend(r, "  %p = %.4f\n",
                    g_cands[i].addr, g_cands[i].val);
        found++;
    }
    RAppend(r, "in range: %d\n", found);
}

/* keep only candidates that start a plausible vec3 */
static void CmdFsVec(Resp *r) {
    size_t w = 0;
    for (size_t i = 0; i < g_ncands; i++) {
        uint8_t *a = g_cands[i].addr;
        if (!CanRead(a, 12)) continue;
        float v[3];
        memcpy(v, a, 12);
        int ok = 1;
        for (int k = 0; k < 3; k++) {
            float m = v[k] < 0 ? -v[k] : v[k];
            if (m < 0.5f || m > 30000.0f) ok = 0;
        }
        if (ok) g_cands[w++] = g_cands[i];
    }
    g_ncands = w;
    RAppend(r, "candidates: %llu\n", (unsigned long long)g_ncands);
}

static void CmdFsSave(Resp *r, const char *line) {
    char p[256] = {0};
    sscanf(line, "%*s %255s", p);
    if (!p[0]) { RAppend(r, "usage: fssave <path>\n"); return; }
    FILE *f = fopen(p, "wb");
    if (!f) { RAppend(r, "cannot open %s\n", p); return; }
    fwrite(&g_ncands, sizeof(g_ncands), 1, f);
    fwrite(g_cands, sizeof(FCand), g_ncands, f);
    fclose(f);
    RAppend(r, "saved %llu to %s\n", (unsigned long long)g_ncands, p);
}

static void CmdFsLoad(Resp *r, const char *line) {
    char p[256] = {0};
    sscanf(line, "%*s %255s", p);
    if (!p[0]) { RAppend(r, "usage: fsload <path>\n"); return; }
    FILE *f = fopen(p, "rb");
    if (!f) { RAppend(r, "cannot open %s\n", p); return; }
    size_t n = 0;
    if (fread(&n, sizeof(n), 1, f) != 1 || n > (6u << 20)) {
        fclose(f); RAppend(r, "bad file\n"); return;
    }
    if (g_cands) free(g_cands);
    g_capcands = n ? n : 1;
    g_cands = (FCand *)malloc(g_capcands * sizeof(FCand));
    if (!g_cands) { fclose(f); RAppend(r, "alloc failed\n"); return; }
    g_ncands = fread(g_cands, sizeof(FCand), n, f);
    fclose(f);
    RAppend(r, "loaded %llu from %s\n", (unsigned long long)g_ncands, p);
}

/* ---- hardware write breakpoint ---- */

static PVOID    g_veh = NULL;
static uint64_t g_bpAddr = 0;
static volatile uint64_t g_bpHits = 0;
static uint64_t g_bpRip[8];
static volatile int g_bpN = 0;

static volatile int g_ovOn = 0;
static float g_ovVec[3];

static uint64_t g_bpStack[4][160];
static uint64_t g_bpRsp[4];
static uint64_t g_bpRdx[4];
/* RCX is the this pointer under the MS x64 ABI. */
static uint64_t g_bpRcx[4];

/* A projectile keeps its hits at +0xA60, count at +0xA6A,
 * records of 0x80 with the object at +0x38, dist at +0x48.
 */
#define PROJ_LIST   0xA60
#define PROJ_COUNT  0xA6A
#define PROJ_STRIDE 0x80
/* +0xB0 is the instigator per FUN_154D3B8D0, the other
 * two are the runners up, all captured for comparison.
 */
static uint64_t g_bpOwnH[4][3];
static uint64_t g_bpOwn[4][3];
static int32_t  g_bpOwnF[4][3];
static uint16_t g_bpProjN[4];
static uint64_t g_bpProjObj[4][4];
static float    g_bpProjDist[4][4];
static float    g_bpProjPos[4][4][3];
static uint32_t g_bpProjId[4][4];
static volatile int g_bpS = 0;

static int g_bpRW = 0;

/* Lock taking engine calls deadlock from the socket
 * thread, so they queue and run on the game thread.
 */
typedef uint64_t (*gc_t)(uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t ImgAddr(uint64_t rva);
extern volatile int g_gcPending;
extern volatile int g_gcDone;
extern uint64_t g_gcFn;
extern uint64_t g_gcA[4];
extern uint64_t g_gcRet;
static volatile DWORD g_gcTid = 0;

/* Runs in the faulting thread before the process dies,
 * so record, then park it and let the REPL report.
 */
static volatile int      g_fltHit = 0;
static volatile int      g_fltPark = 1;
static uint64_t g_fltCode, g_fltAddr, g_fltRip, g_fltRsp;
static uint64_t g_fltAccess, g_fltRcx, g_fltRdx, g_fltR8;
static uint64_t g_fltStack[32];
static DWORD    g_fltTid = 0;

static int IsFault(DWORD c) {
    return c == EXCEPTION_ACCESS_VIOLATION
        || c == EXCEPTION_ILLEGAL_INSTRUCTION
        || c == EXCEPTION_PRIV_INSTRUCTION
        || c == EXCEPTION_INT_DIVIDE_BY_ZERO
        || c == EXCEPTION_STACK_OVERFLOW;
}

static LONG CALLBACK BpHandler(PEXCEPTION_POINTERS ei) {
    DWORD code = ei->ExceptionRecord->ExceptionCode;

    if (IsFault(code) && !g_fltHit) {
        int i;
        g_fltHit = 1;
        g_fltTid = GetCurrentThreadId();
        g_fltCode = code;
        g_fltAddr = (uint64_t)ei->ExceptionRecord->ExceptionAddress;
        g_fltRip = ei->ContextRecord->Rip;
        g_fltRsp = ei->ContextRecord->Rsp;
        g_fltRcx = ei->ContextRecord->Rcx;
        g_fltRdx = ei->ContextRecord->Rdx;
        g_fltR8  = ei->ContextRecord->R8;
        if (ei->ExceptionRecord->NumberParameters >= 2)
            g_fltAccess =
                ei->ExceptionRecord->ExceptionInformation[1];
        for (i = 0; i < 32; i++) {
            uint64_t *p = (uint64_t *)(g_fltRsp + i * 8);
            g_fltStack[i] = CanRead(p, 8) ? *p : 0;
        }
        /* Park rather than die, so the REPL can report. */
        while (g_fltPark) Sleep(50);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code == EXCEPTION_SINGLE_STEP) {
        g_bpHits++;
        /* ring buffers: keep the MOST RECENT samples */
        g_bpRip[g_bpN % 8] = ei->ContextRecord->Rip;
        g_bpN++;
        {
            int k = g_bpS++ % 4;
            uint64_t rsp = ei->ContextRecord->Rsp;
            g_bpRsp[k] = rsp;
            g_bpRdx[k] = ei->ContextRecord->Rdx;
            g_bpRcx[k] = ei->ContextRecord->Rcx;
            {
                uint64_t p = ei->ContextRecord->Rcx;
                uint16_t n = 0;
                int w;
                /* Instigator candidates, read AT the break
                 * because a projectile dies in a frame.
                 */
                static const int OFFS[3] = { 0xB0, 0x60, 0x58 };
                for (w = 0; w < 3; w++) {
                    uint64_t h = SafeReadPtr(
                        (void *)(p + OFFS[w]));
                    int32_t fl = 0;
                    g_bpOwnH[k][w] = h;
                    g_bpOwn[k][w] = 0;
                    if (!CanRead((void *)(h + 0x10), 4)) continue;
                    memcpy(&fl, (void *)(h + 0xC), 4);
                    g_bpOwnF[k][w] = fl;
                    if (fl < 0)
                        g_bpOwn[k][w] = SafeReadPtr((void *)h);
                }
                g_bpProjN[k] = 0;
                if (CanRead((void *)(p + PROJ_COUNT), 2)) {
                    memcpy(&n, (void *)(p + PROJ_COUNT), 2);
                    if (n && n < 64) {
                        uint64_t lst = SafeReadPtr(
                            (void *)(p + PROJ_LIST));
                        int j;
                        g_bpProjN[k] = n;
                        for (j = 0; j < 4 && j < n; j++) {
                            uint64_t rec = lst +
                                (uint64_t)j * PROJ_STRIDE;
                            if (!CanRead((void *)(rec + 0x50), 8))
                                break;
                            uint64_t h = SafeReadPtr(
                                (void *)(rec + 0x38));
                            int32_t fl = 0;
                            uint64_t ent = 0;
                            /* Masked handle, as FUN_154D37420
                             * resolves it before use. */
                            if (CanRead((void *)(h + 0x10), 4)) {
                                memcpy(&fl, (void *)(h + 0xC), 4);
                                if (fl < 0)
                                    ent = SafeReadPtr((void *)h);
                            }
                            g_bpProjObj[k][j] = ent;
                            memcpy(&g_bpProjDist[k][j],
                                   (void *)(rec + 0x48), 4);
                            memcpy(g_bpProjPos[k][j],
                                   (void *)rec, 12);
                            memcpy(&g_bpProjId[k][j],
                                   (void *)(rec + 0x40), 4);
                        }
                    }
                }
            }
            for (int i = 0; i < 160; i++) {
                uint64_t *p = (uint64_t *)(rsp + i * 8);
                g_bpStack[k][i] = CanRead(p, 8) ? *p : 0;
            }
        }
        if (g_ovOn && g_bpAddr)
            memcpy((void *)g_bpAddr, (const void *)g_ovVec, 12);

        /* Drain one queued call, on this thread. */
        if (g_gcPending && g_gcFn) {
            uint64_t f = g_gcFn;
            uint64_t a0 = g_gcA[0], a1 = g_gcA[1];
            uint64_t a2 = g_gcA[2], a3 = g_gcA[3];
            g_gcPending = 0;
            g_gcTid = GetCurrentThreadId();
            g_gcRet = ((gc_t)f)(a0, a1, a2, a3);
            g_gcDone++;
        }
        ei->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static int SetDrAllThreads(uint64_t addr, int enable) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    int n = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (te.th32ThreadID == me) continue;
            HANDLE h = OpenThread(THREAD_ALL_ACCESS, FALSE,
                                  te.th32ThreadID);
            if (!h) continue;
            SuspendThread(h);
            CONTEXT c;
            memset(&c, 0, sizeof(c));
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(h, &c)) {
                if (enable) {
                    /* RW0: 00 exec, 01 write, 11 read-write.
                     * Exec needs LEN0 = 00 as well.
                     */
                    uint64_t rw = g_bpRW == 2 ? 0ULL
                                : g_bpRW ? 3ULL : 1ULL;
                    uint64_t len = g_bpRW == 2 ? 0ULL : 3ULL;
                    c.Dr0 = addr;
                    c.Dr7 = (c.Dr7 & ~0xF0000ULL)
                          | (len << 18) | (rw << 16) | 1ULL;
                } else {
                    c.Dr0 = 0;
                    c.Dr7 &= ~1ULL;
                }
                if (SetThreadContext(h, &c)) n++;
            }
            ResumeThread(h);
            CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

static void CmdHwbp(Resp *r, const char *line) {
    char as[64] = {0}, ms[64] = {0};
    sscanf(line, "%*s %63s %63s", as, ms);
    if (!as[0]) {
        RAppend(r, "usage: hwbp <addr> [rw|x]  x = execute\n");
        return;
    }
    uint64_t a = ParseHex(as);
    g_bpRW = (ms[0] == 'x') ? 2
           : (ms[0] == 'r' || ms[0] == '3') ? 1 : 0;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, BpHandler);
    g_bpHits = 0; g_bpN = 0; g_bpS = 0; g_bpAddr = a;
    int n = SetDrAllThreads(a, 1);
    RAppend(r, "watching %s of %p on %d threads\n",
            g_bpRW == 2 ? "execution" :
            g_bpRW ? "reads+writes" : "writes", (void *)a, n);
}

static void CmdHwbpInfo(Resp *r) {
    RAppend(r, "watch %p  hits=%llu\n",
            (void *)g_bpAddr, (unsigned long long)g_bpHits);
    for (int i = 0; i < (g_bpN < 8 ? g_bpN : 8); i++)
        RAppend(r, "  writer rip = %p\n", (void *)g_bpRip[i]);
    for (int k = 0; k < (g_bpS < 4 ? g_bpS : 4); k++) {
        RAppend(r, "  --- sample %d  rsp=%p rdx=%p ---\n",
                k, (void *)g_bpRsp[k], (void *)g_bpRdx[k]);
        RAppend(r, "      rcx=%p\n", (void *)g_bpRcx[k]);
        {
            static const int OFFS[3] = { 0xB0, 0x60, 0x58 };
            int w;
            for (w = 0; w < 3; w++)
                RAppend(r, "        +%03x h=%p f=%08x ent=%p\n",
                        OFFS[w], (void *)g_bpOwnH[k][w],
                        (unsigned)g_bpOwnF[k][w],
                        (void *)g_bpOwn[k][w]);
        }
        if (g_bpProjN[k]) {
            int j;
            RAppend(r, "      HITS %u\n", g_bpProjN[k]);
            for (j = 0; j < 4 && j < g_bpProjN[k]; j++)
                RAppend(r, "        ent %p id %u  dist %.2f"
                           "  at %.1f %.1f %.1f\n",
                        (void *)g_bpProjObj[k][j],
                        g_bpProjId[k][j], g_bpProjDist[k][j],
                        g_bpProjPos[k][j][0], g_bpProjPos[k][j][1],
                        g_bpProjPos[k][j][2]);
        }
        uint64_t seen[64];
        int ns = 0;
        for (int i = 0; i < 160; i++) {
            uint64_t v = g_bpStack[k][i];
            if (!ShInImage(v)) continue;
            int dup = 0;
            for (int j = 0; j < ns; j++)
                if (seen[j] == v) { dup = 1; break; }
            if (dup) continue;
            if (ns < 64) seen[ns++] = v;
            RAppend(r, "    [rsp+%03x] = %p\n", i * 8, (void *)v);
        }
    }
}

/* overwrite watched position when the game writes it */
static void CmdTpOv(Resp *r, const char *line) {
    float x, y, z;
    if (sscanf(line, "%*s %f %f %f", &x, &y, &z) != 3) {
        RAppend(r, "usage: tpov <x> <y> <z>\n");
        return;
    }
    if (!g_bpAddr) { RAppend(r, "set hwbp <addr> first\n"); return; }
    g_ovVec[0] = x; g_ovVec[1] = y; g_ovVec[2] = z;
    g_ovOn = 1;
    RAppend(r, "override on: %p <- (%.2f, %.2f, %.2f)\n",
            (void *)g_bpAddr, x, y, z);
}

static void CmdTpOvOff(Resp *r) {
    g_ovOn = 0;
    RAppend(r, "override off\n");
}

static void CmdHwbpOff(Resp *r) {
    int n = SetDrAllThreads(0, 0);
    RAppend(r, "cleared on %d threads\n", n);
}

/* ---- call primitive ---- */

static float g_vec[4][4] __attribute__((aligned(16)));

static int IsImagePtr(uint64_t v) {
    return ShInImage(v);
}

static void CmdScratch(Resp *r) {
    for (int i = 0; i < 4; i++)
        RAppend(r, "  vec%d = %p  (%.2f, %.2f, %.2f, %.2f)\n",
                i, (void *)g_vec[i], g_vec[i][0], g_vec[i][1],
                g_vec[i][2], g_vec[i][3]);
}

static void CmdSetVec(Resp *r, const char *line) {
    int n; float x, y, z, w;
    if (sscanf(line, "%*s %d %f %f %f %f", &n, &x, &y, &z, &w) != 5) {
        RAppend(r, "usage: setvec <0-3> <x> <y> <z> <w>\n");
        return;
    }
    if (n < 0 || n > 3) { RAppend(r, "slot must be 0-3\n"); return; }
    g_vec[n][0] = x; g_vec[n][1] = y;
    g_vec[n][2] = z; g_vec[n][3] = w;
    RAppend(r, "vec%d = %p (%.2f, %.2f, %.2f, %.2f)\n",
            n, (void *)g_vec[n], x, y, z, w);
}

typedef uint64_t (*fn4_t)(uint64_t, uint64_t, uint64_t, uint64_t);

static void CmdCall(Resp *r, const char *line) {
    char fs[64] = {0}, a1[64] = {0}, a2[64] = {0};
    char a3[64] = {0}, a4[64] = {0};
    sscanf(line, "%*s %63s %63s %63s %63s %63s", fs, a1, a2, a3, a4);
    if (!fs[0]) { RAppend(r, "usage: call <fn> [rcx] [rdx] [r8]\n"); return; }

    uint64_t f  = ParseHex(fs);
    uint64_t p1 = a1[0] ? ParseHex(a1) : 0;
    uint64_t p2 = a2[0] ? ParseHex(a2) : 0;
    uint64_t p3 = a3[0] ? ParseHex(a3) : 0;
    uint64_t p4 = a4[0] ? ParseHex(a4) : 0;

    if (!CanRead((void *)f, 1)) {
        RAppend(r, "fn %p not readable\n", (void *)f);
        return;
    }
    RAppend(r, "calling %p(%p, %p, %p, %p)...\n",
            (void *)f, (void *)p1, (void *)p2, (void *)p3, (void *)p4);
    uint64_t ret = ((fn4_t)f)(p1, p2, p3, p4);
    RAppend(r, "returned %p\n", (void *)ret);
}

/* Ai::SpawningManagerUpdate, a per frame game thread
 * call, used as the trigger for queued work.
 */
#define RVA_FRAME_TRIGGER 0xC17FE70

/* gcall <fn> [rcx] [rdx] [r8] [r9] */
static void CmdGCall(Resp *r, const char *line) {
    char fs[64] = {0}, a[4][64] = {{0}};
    uint64_t f;
    int i, waited;

    if (sscanf(line, "%*s %63s %63s %63s %63s %63s", fs,
               a[0], a[1], a[2], a[3]) < 1 || !fs[0]) {
        RAppend(r, "usage: gcall <fn> [rcx] [rdx] [r8] [r9]\n");
        return;
    }
    f = ParseHex(fs);
    if (!CanRead((void *)f, 1)) {
        RAppend(r, "fn %p unreadable\n", (void *)f);
        return;
    }
    {
        HMODULE m = GetModuleHandleA("dinput8.dll");
        int (*qcall)(uint64_t, uint64_t, uint64_t,
                     uint64_t, uint64_t);
        int (*qres)(uint64_t *);
        uint64_t ret = 0;

        if (!m) { RAppend(r, "dinput8 not loaded\n"); return; }
        *(FARPROC *)&qcall = GetProcAddress(m, "ShQueueCall");
        *(FARPROC *)&qres  = GetProcAddress(m, "ShQueueResult");
        if (!qcall || !qres) {
            RAppend(r, "DLL queue missing, rebuild dinput8\n");
            return;
        }
        for (i = 0; i < 4; i++)
            g_gcA[i] = a[i][0] ? ParseHex(a[i]) : 0;

        /* Drained by the DLL physics hook, game thread.
         * Patching a new site trips integrity checks. */
        if (!qcall(f, g_gcA[0], g_gcA[1], g_gcA[2], g_gcA[3])) {
            RAppend(r, "queue busy or bad function\n");
            return;
        }
        for (waited = 0; waited < 400; waited++) {
            if (qres(&ret)) break;
            Sleep(10);
        }
        if (waited >= 400) {
            RAppend(r, "timed out, is physics ready\n");
            return;
        }
        RAppend(r, "returned %p after %dms\n",
                (void *)ret, waited * 10);
    }
}

/* gcall6 <fn> a0 a1 a2 a3 a4 a5: six integer arguments. */
static void CmdGCall6(Resp *r, const char *line) {
    char fs[64] = {0}, a[6][64] = {{0}};
    uint64_t f, args[6];
    int i, waited, n;
    HMODULE m = GetModuleHandleA("dinput8.dll");
    int (*q6)(uint64_t, const uint64_t *);
    int (*qres)(uint64_t *);
    uint64_t ret = 0;

    n = sscanf(line, "%*s %63s %63s %63s %63s %63s %63s %63s", fs,
               a[0], a[1], a[2], a[3], a[4], a[5]);
    if (n < 1 || !fs[0]) {
        RAppend(r, "usage: gcall6 <fn> a0 a1 a2 a3 a4 a5\n");
        return;
    }
    f = ParseHex(fs);
    if (!m) { RAppend(r, "dinput8 not loaded\n"); return; }
    *(FARPROC *)&q6 = GetProcAddress(m, "ShQueueCall6");
    *(FARPROC *)&qres = GetProcAddress(m, "ShQueueResult");
    if (!q6 || !qres) { RAppend(r, "ShQueueCall6 missing\n"); return; }
    for (i = 0; i < 6; i++) args[i] = a[i][0] ? ParseHex(a[i]) : 0;
    if (!q6(f, args)) { RAppend(r, "queue busy or bad function\n"); return; }
    for (waited = 0; waited < 400; waited++) {
        if (qres(&ret)) break;
        Sleep(10);
    }
    if (waited >= 400) { RAppend(r, "timed out, is physics ready\n"); return; }
    RAppend(r, "returned %p after %dms\n", (void *)ret, waited * 10);
}

/* gcallf <fn> <rcx> <rdx> <f2> <f3> <f4>, for calls whose
 * later args are floats. The int path cannot reach xmm.
 */
static void CmdGCallF(Resp *r, const char *line) {
    char fs[64] = {0}, a0[64] = {0}, a1[64] = {0};
    double d2 = 0, d3 = 0, d4 = 0;
    HMODULE m;
    int (*qcall)(uint64_t, uint64_t, uint64_t, float, float, float);
    int (*qres)(uint64_t *);
    uint64_t ret = 0;
    int waited;

    if (sscanf(line, "%*s %63s %63s %63s %lf %lf %lf",
               fs, a0, a1, &d2, &d3, &d4) < 3) {
        RAppend(r, "usage: gcallf <fn> <rcx> <rdx> <f2> <f3> <f4>\n");
        return;
    }
    m = GetModuleHandleA("dinput8.dll");
    if (!m) { RAppend(r, "dinput8 not loaded\n"); return; }
    *(FARPROC *)&qcall = GetProcAddress(m, "ShQueueCallF");
    *(FARPROC *)&qres  = GetProcAddress(m, "ShQueueResult");
    if (!qcall || !qres) {
        RAppend(r, "no ShQueueCallF, rebuild dinput8\n");
        return;
    }
    if (!qcall(ParseHex(fs), ParseHex(a0), ParseHex(a1),
               (float)d2, (float)d3, (float)d4)) {
        RAppend(r, "queue busy or bad function\n");
        return;
    }
    for (waited = 0; waited < 400; waited++) {
        if (qres(&ret)) break;
        Sleep(10);
    }
    if (waited >= 400) {
        RAppend(r, "timed out, is physics ready\n");
        return;
    }
    RAppend(r, "returned %p after %dms\n", (void *)ret, waited * 10);
}

/* placerot <entity> <x> <y> <z> <yaw> <pitch> <roll>
 * Calls the API itself, so no hand written matrix.
 */
static void CmdPlaceRot(Resp *r, const char *line) {
    char es[64] = {0};
    double x = 0, y = 0, z = 0, yaw = 0, pitch = 0, roll = 0;
    struct { float x, y, z; } pos;
    HMODULE m;
    int (*place)(uint64_t, const void *, float, float, float);
    int ok;

    if (sscanf(line, "%*s %63s %lf %lf %lf %lf %lf %lf",
               es, &x, &y, &z, &yaw, &pitch, &roll) < 4) {
        RAppend(r, "usage: placerot <ent> <x> <y> <z> "
                   "<yaw> <pitch> <roll>\n");
        return;
    }
    m = GetModuleHandleA("dinput8.dll");
    if (!m) { RAppend(r, "dinput8 not loaded\n"); return; }
    *(FARPROC *)&place = GetProcAddress(m, "ShPlaceEntityRot");
    if (!place) {
        RAppend(r, "no ShPlaceEntityRot, rebuild dinput8\n");
        return;
    }
    pos.x = (float)x;
    pos.y = (float)y;
    pos.z = (float)z;
    ok = place(ParseHex(es), &pos, (float)yaw, (float)pitch,
               (float)roll);
    RAppend(r, "placerot %s at %.2f %.2f %.2f ypr %.2f %.2f %.2f\n",
            ok ? "ok" : "FAILED", pos.x, pos.y, pos.z,
            yaw, pitch, roll);
}

/* chaos [name], to fire one effect without the wheel.
 * Bare lists them. Works with chaos switched off.
 */
static void CmdChaos(Resp *r, const char *line) {
    char want[64] = {0};
    HMODULE m = GetModuleHandleA("chaos.asi");
    int (*count)(void);
    const char *(*nameOf)(int);
    int (*fire)(const char *);
    int i, n;

    if (!m) { RAppend(r, "chaos.asi not loaded\n"); return; }
    *(FARPROC *)&count = GetProcAddress(m, "ChaosCount");
    *(FARPROC *)&nameOf = GetProcAddress(m, "ChaosName");
    *(FARPROC *)&fire = GetProcAddress(m, "ChaosFire");
    if (!count || !nameOf || !fire) {
        RAppend(r, "no chaos exports, rebuild chaos.asi\n");
        return;
    }

    if (sscanf(line, "%*s %63[^\n]", want) != 1 || !want[0]) {
        n = count();
        RAppend(r, "%d effects:\n", n);
        for (i = 0; i < n; i++)
            RAppend(r, "  %s\n", nameOf(i));
        return;
    }
    RAppend(r, fire(want) ? "fired %s\n" : "no effect matching %s\n",
            want);
}

/* havok             report the scan
 * havok <ent> <x> <y> <z>   add velocity to that entity
 */
static void CmdHavok(Resp *r, const char *line) {
    char es[64] = {0};
    double vx = 0, vy = 0, vz = 0;
    HMODULE m = GetModuleHandleA("dinput8.dll");
    uint64_t (*world)(void);
    int (*scan)(int *, int *, int *);
    unsigned (*bodyId)(uint64_t);
    int (*addVel)(uint64_t, const void *);
    int (*getVel)(uint64_t, void *);
    struct { float x, y, z; } v;
    int sc = 0, ow = 0, mp = 0;

    if (!m) { RAppend(r, "dinput8 not loaded\n"); return; }
    *(FARPROC *)&world = GetProcAddress(m, "ShHavokWorld");
    *(FARPROC *)&scan = GetProcAddress(m, "ShHavokScan");
    *(FARPROC *)&bodyId = GetProcAddress(m, "ShGetBodyId");
    *(FARPROC *)&addVel = GetProcAddress(m, "ShAddVelocity");
    *(FARPROC *)&getVel = GetProcAddress(m, "ShGetVelocity");
    if (!world || !scan || !bodyId || !addVel) {
        RAppend(r, "no havok exports, rebuild dinput8\n");
        return;
    }

    if (sscanf(line, "%*s %63s %lf %lf %lf", es, &vx, &vy, &vz) == 4) {
        uint64_t e = ParseHex(es);
        unsigned id = bodyId(e);

        v.x = (float)vx; v.y = (float)vy; v.z = (float)vz;
        RAppend(r, "entity %p body id %u\n", (void *)e, id);
        if (getVel(e, &v.x) == 0) RAppend(r, "  velocity unreadable\n");
        v.x = (float)vx; v.y = (float)vy; v.z = (float)vz;
        RAppend(r, "  add %.1f %.1f %.1f : %s\n", vx, vy, vz,
                addVel(e, &v) ? "ok" : "FAILED");
        return;
    }

    scan(&sc, &ow, &mp);
    RAppend(r, "hknpWorld %p\n", (void *)world());
    RAppend(r, "last lookup: fields %d, rigidbodies %d, "
               "cached offset %d\n", sc, ow, mp);
    RAppend(r, "usage: havok <ent> <x> <y> <z> to shove\n");
}

/* ---- arg capture hook on the teleport worker ---- */

#define HK_COUNT  0x200
#define HK_RET    0x208
#define HK_FILTER 0x290

static const struct { uint8_t a, b, c; int slot; } HK_REGS[16] = {
    {0x48,0x89,0x05, 0x210}, {0x48,0x89,0x1D, 0x218},
    {0x48,0x89,0x0D, 0x220}, {0x48,0x89,0x15, 0x228},
    {0x48,0x89,0x25, 0x230}, {0x48,0x89,0x2D, 0x238},
    {0x48,0x89,0x35, 0x240}, {0x48,0x89,0x3D, 0x248},
    {0x4C,0x89,0x05, 0x250}, {0x4C,0x89,0x0D, 0x258},
    {0x4C,0x89,0x15, 0x260}, {0x4C,0x89,0x1D, 0x268},
    {0x4C,0x89,0x25, 0x270}, {0x4C,0x89,0x2D, 0x278},
    {0x4C,0x89,0x35, 0x280}, {0x4C,0x89,0x3D, 0x288},
};
static const char *HK_NAMES[16] = {
    "rax","rbx","rcx","rdx","rsp","rbp","rsi","rdi",
    "r8 ","r9 ","r10","r11","r12","r13","r14","r15"
};

static uint8_t *g_stub = NULL;
static uint64_t g_hookAddr = 0;
static int      g_hookLen = 0;
static uint8_t  g_origBytes[32];

static uint8_t *AllocNear(uint64_t target) {
    for (uint64_t delta = 0x10000; delta < 0x40000000; delta += 0x10000) {
        uint64_t lo = target - delta;
        void *p = VirtualAlloc((void *)(lo & ~0xFFFFULL), 0x1000,
                               MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
        if (p) return (uint8_t *)p;
    }
    return NULL;
}

/* Thunk hook: rewrite the 16-byte int3-padded slot with
   jmp [rip+0] + absolute target. No stolen bytes. */
static void __attribute__((ms_abi)) TpCallback(uint64_t, uint64_t);

static uint64_t g_thAddr = 0;
static uint8_t  g_thOrig[16];
static uint8_t *g_thStub = NULL;
static uint64_t g_thReal = 0;

static void CmdThook(Resp *r, const char *line) {
    char as[64] = {0};
    sscanf(line, "%*s %63s", as);
    if (!as[0]) { RAppend(r, "usage: thook <thunk>\n"); return; }
    if (g_thAddr) { RAppend(r, "already installed\n"); return; }
    uint64_t t = ParseHex(as);
    if (!CanRead((void *)t, 16)) { RAppend(r, "unreadable\n"); return; }

    uint8_t *p = (uint8_t *)t;
    if (p[0] != 0xE9) {
        RAppend(r, "not an E9 thunk (%02X)\n", p[0]);
        return;
    }
    for (int i = 5; i < 16; i++) {
        if (p[i] != 0xCC) {
            RAppend(r, "slot not int3-padded at +%d (%02X)\n", i, p[i]);
            return;
        }
    }
    int32_t rel;
    memcpy(&rel, p + 1, 4);
    uint64_t real = t + 5 + (int64_t)rel;
    RAppend(r, "thunk %p -> real %p\n", (void *)t, (void *)real);

    uint8_t *s = AllocNear(t);
    if (!s) { RAppend(r, "alloc failed\n"); return; }
    memset(s, 0xCC, 0x1000);

    int o = 0;
    static const uint8_t PU[] = {
        0x50, 0x51, 0x52, 0x41,0x50, 0x41,0x51, 0x41,0x52, 0x41,0x53
    };
    memcpy(s + o, PU, sizeof(PU)); o += sizeof(PU);
    s[o++]=0x48; s[o++]=0x83; s[o++]=0xEC; s[o++]=0x20;
    s[o++]=0x48; s[o++]=0xB8;
    *(uint64_t *)(s+o) = (uint64_t)(uintptr_t)TpCallback; o += 8;
    s[o++]=0xFF; s[o++]=0xD0;
    s[o++]=0x48; s[o++]=0x83; s[o++]=0xC4; s[o++]=0x20;
    static const uint8_t PO[] = {
        0x41,0x5B, 0x41,0x5A, 0x41,0x59, 0x41,0x58, 0x5A, 0x59, 0x58
    };
    memcpy(s + o, PO, sizeof(PO)); o += sizeof(PO);
    /* tail-jump to the real function */
    s[o++]=0xFF; s[o++]=0x25;
    *(int32_t *)(s+o)=0; o+=4;
    *(uint64_t *)(s+o) = real;

    DWORD old;
    if (!VirtualProtect((void *)t, 16, PAGE_EXECUTE_READWRITE, &old)) {
        RAppend(r, "protect failed\n"); return;
    }
    memcpy(g_thOrig, (void *)t, 16);
    uint8_t patch[14];
    patch[0] = 0xFF; patch[1] = 0x25;
    *(int32_t *)(patch + 2) = 0;
    *(uint64_t *)(patch + 6) = (uint64_t)(uintptr_t)s;
    memcpy((void *)t, patch, 14);
    VirtualProtect((void *)t, 16, old, &old);

    g_thAddr = t; g_thStub = s; g_thReal = real;
    RAppend(r, "thunk hooked, stub %p\n", s);
}

static void CmdUnthook(Resp *r) {
    if (!g_thAddr) { RAppend(r, "none\n"); return; }
    DWORD old;
    VirtualProtect((void *)g_thAddr, 16, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void *)g_thAddr, g_thOrig, 16);
    VirtualProtect((void *)g_thAddr, 16, old, &old);
    g_thAddr = 0;
    RAppend(r, "thunk restored\n");
}

/* force a position into [rsi+off] at a code point */
static volatile int g_fpOn = 0;
static volatile int g_fpHits = 0;
static uint32_t     g_fpOff = 0xF0;
static float        g_fpDest[3];

static void __attribute__((ms_abi)) FpCallback(uint64_t rsi) {
    g_fpHits++;
    if (!g_fpOn || !rsi) return;
    memcpy((void *)(rsi + g_fpOff), (const void *)g_fpDest, 12);
}

static uint8_t *g_fhStub = NULL;
static uint64_t g_fhAddr = 0;
static int      g_fhLen = 0;
static uint8_t  g_fhOrig[32];

static void CmdFHook(Resp *r, const char *line) {
    char as[64] = {0}, ns[64] = {0};
    sscanf(line, "%*s %63s %63s", as, ns);
    if (!as[0]) { RAppend(r, "usage: fhook <addr> <stolen>\n"); return; }
    if (g_fhStub) { RAppend(r, "already installed\n"); return; }
    uint64_t fn = ParseHex(as);
    int n = ns[0] ? (int)strtol(ns, NULL, 10) : 7;
    if (n < 5 || n > 24) { RAppend(r, "stolen 5..24\n"); return; }
    if (!CanRead((void *)fn, n)) { RAppend(r, "unreadable\n"); return; }

    uint8_t *s = AllocNear(fn);
    if (!s) { RAppend(r, "alloc failed\n"); return; }
    memset(s, 0xCC, 0x1000);

    int o = 0;
    static const uint8_t PU[] = {
        0x50, 0x51, 0x52, 0x41,0x50, 0x41,0x51, 0x41,0x52, 0x41,0x53
    };
    memcpy(s + o, PU, sizeof(PU)); o += sizeof(PU);
    s[o++]=0x48; s[o++]=0x89; s[o++]=0xF1;
    s[o++]=0x48; s[o++]=0x83; s[o++]=0xEC; s[o++]=0x20;
    s[o++]=0x48; s[o++]=0xB8;
    *(uint64_t *)(s+o) = (uint64_t)(uintptr_t)FpCallback; o += 8;
    s[o++]=0xFF; s[o++]=0xD0;
    s[o++]=0x48; s[o++]=0x83; s[o++]=0xC4; s[o++]=0x20;
    static const uint8_t PO[] = {
        0x41,0x5B, 0x41,0x5A, 0x41,0x59, 0x41,0x58, 0x5A, 0x59, 0x58
    };
    memcpy(s + o, PO, sizeof(PO)); o += sizeof(PO);

    memcpy(s + o, (void *)fn, n); o += n;
    s[o++]=0xFF; s[o++]=0x25;
    *(int32_t *)(s+o)=0; o+=4;
    *(uint64_t *)(s+o) = fn + n;

    int64_t rel = (int64_t)s - (int64_t)(fn + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) {
        RAppend(r, "stub too far\n"); return;
    }
    DWORD old;
    if (!VirtualProtect((void *)fn, n, PAGE_EXECUTE_READWRITE, &old)) {
        RAppend(r, "protect failed\n"); return;
    }
    memcpy(g_fhOrig, (void *)fn, n);
    uint8_t patch[32];
    memset(patch, 0x90, n);
    patch[0] = 0xE9;
    *(int32_t *)(patch + 1) = (int32_t)rel;
    memcpy((void *)fn, patch, n);
    VirtualProtect((void *)fn, n, old, &old);

    g_fhStub = s; g_fhAddr = fn; g_fhLen = n;
    RAppend(r, "force hook at %p, writes [rsi+%#x]\n",
            (void *)fn, g_fpOff);
}

static void CmdFHookOff(Resp *r) {
    if (!g_fhStub) { RAppend(r, "none\n"); return; }
    DWORD old;
    VirtualProtect((void *)g_fhAddr, g_fhLen,
                   PAGE_EXECUTE_READWRITE, &old);
    memcpy((void *)g_fhAddr, g_fhOrig, g_fhLen);
    VirtualProtect((void *)g_fhAddr, g_fhLen, old, &old);
    g_fhStub = NULL; g_fpOn = 0;
    RAppend(r, "force hook removed\n");
}

static void CmdFPos(Resp *r, const char *line) {
    float x, y, z;
    char os[64] = {0};
    int n = sscanf(line, "%*s %f %f %f %63s", &x, &y, &z, os);
    if (n < 3) { RAppend(r, "usage: fpos <x> <y> <z> [off]\n"); return; }
    g_fpDest[0] = x; g_fpDest[1] = y; g_fpDest[2] = z;
    if (os[0]) g_fpOff = (uint32_t)ParseHex(os);
    g_fpOn = 1;
    RAppend(r, "forcing (%.2f, %.2f, %.2f) into +%#x\n",
            x, y, z, g_fpOff);
}

static void CmdFOff(Resp *r) {
    g_fpOn = 0;
    RAppend(r, "force off, hits=%d\n", g_fpHits);
}

/* hook that calls TpCallback on the game thread */
static void __attribute__((ms_abi)) TpCallback(uint64_t, uint64_t);

static uint8_t *g_cbStub = NULL;
static uint64_t g_cbAddr = 0;
static int      g_cbLen = 0;
static uint8_t  g_cbOrig[32];

static uint8_t  g_raySnapA[0x200];
static uint8_t  g_raySnapB[0x200];
static uint64_t g_rayCtx, g_rayRdx, g_rayR8;
static volatile int g_raySnapped;
static uint8_t *g_rayStub = NULL;
static uint64_t g_rayAddr = 0;
static uint8_t  g_rayOrig[24];
static int      g_rayOrigLen = 0;

typedef void (__attribute__((ms_abi)) *RayImpl_t)(uint64_t, void *,
                                                  uint64_t, void *);
typedef uint8_t (__attribute__((ms_abi)) *GroundFn_t)(void *,
                                                      const void *,
                                                      void *, int);

static volatile int g_gndReq = 0;
static volatile int g_gndDone = 0;
static uint64_t g_gndThis = 0;
/* Resolved lazily, since the base is only known at run. */
static uint64_t g_gndFn = 0;
#define GND_FN_RVA 0x1997270
static float    g_gndIn[4] __attribute__((aligned(16)));
static float    g_gndOut = -1000.0f;
static int      g_gndRet = -1;
static int      g_gndHere = 0;
static uint64_t g_gndCtx = 0;
static float    g_gndOrigin[4];

typedef uint8_t (__attribute__((ms_abi)) *CastRay_t)(void *, void *,
                                                    void *, char, char,
                                                    uint8_t, char);

static uint8_t  g_castBuf[0x880] __attribute__((aligned(16)));
static uint8_t  g_castRec[16 * 0x80] __attribute__((aligned(16)));
static volatile int g_castReq = 0;
static volatile int g_castDone = 0;
static uint64_t g_castB = 0;
static uint64_t g_castMask = 0x4000;
static uint64_t g_castFn = 0;
#define CAST_FN_RVA 0xFBE88D0
static int      g_castRet = -1;
static int      g_castCount = -1;
static float    g_castHit[4];
static int      g_castOwnDesc = 0;
static float    g_castOrigin[4];
static uint8_t  g_castDesc[0x80] __attribute__((aligned(16)));
static float    g_castRayOrg[4] __attribute__((aligned(16)));
static float    g_castRayDir[4] __attribute__((aligned(16)));
static int      g_castTp = 0;
static float    g_castTpZOff = 0.5f;
static int      g_castTpDone = 0;

typedef int (*ShTeleport_t)(const void *, const void *);
static ShTeleport_t g_shTeleport = NULL;

static void ResolveTeleport(void) {
    HMODULE m;
    if (g_shTeleport) return;
    m = GetModuleHandleA("dinput8.dll");
    if (!m) return;
    g_shTeleport = (ShTeleport_t)(void *)
        GetProcAddress(m, "ShTeleportPlayer");
}

static volatile int g_rayReq = 0;
static volatile int g_rayDone = 0;
static float    g_rayOrigin[4];
static float    g_rayDir[4];
static float    g_rayOut[4];
static uint64_t g_rayImpl = 0;
static int      g_rayBusy = 0;
static uint64_t g_rayPin = 0;
static uint64_t g_rayUsedCtx = 0;

/* Runtime descriptor overrides, applied after build */
#define RAY_OVR_MAX 12
static struct { int off, size; uint64_t val; } g_rayOvr[RAY_OVR_MAX];
static int g_rayOvrN = 0;
static int g_rayCollOvrOff[RAY_OVR_MAX];
static uint64_t g_rayCollOvrVal[RAY_OVR_MAX];
static int g_rayCollOvrSize[RAY_OVR_MAX];
static int g_rayCollOvrN = 0;

/* Runs inside a live call, so the caller's stack
 * structures are still valid to reuse.
 */
static void __attribute__((ms_abi))
RayCallback(uint64_t rcx, uint64_t rdx, uint64_t r8) {
    /* Queued engine calls run here: game thread, and
     * outside the spawn subsystem's own locks.
     */
    if (g_gcPending && g_gcFn) {
        uint64_t f = g_gcFn;
        uint64_t a0 = g_gcA[0], a1 = g_gcA[1];
        uint64_t a2 = g_gcA[2], a3 = g_gcA[3];
        g_gcPending = 0;
        g_gcTid = GetCurrentThreadId();
        g_gcRet = ((gc_t)f)(a0, a1, a2, a3);
        g_gcDone++;
    }
    if (!g_raySnapped) {
        g_rayCtx = rcx; g_rayRdx = rdx; g_rayR8 = r8;
        if (CanRead((void *)rdx, 0x200))
            memcpy(g_raySnapA, (void *)rdx, 0x200);
        if (CanRead((void *)r8, 0x200))
            memcpy(g_raySnapB, (void *)r8, 0x200);
        g_raySnapped = 1;
    }
    /* Direct cast with the engine's own live descriptor,
     * our own hit array. No coordinates of ours.
     */
    if (!g_castFn) g_castFn = SH_IMG(CAST_FN_RVA);
    if (!g_gndFn) g_gndFn = SH_IMG(GND_FN_RVA);
    if (g_castReq && !g_rayBusy && g_castB && g_castFn) {
        g_rayBusy = 1;
        g_castReq = 0;
        memset(g_castBuf, 0, sizeof(g_castBuf));
        memset(g_castRec, 0, sizeof(g_castRec));
        *(void **)(g_castBuf + 0x10) = g_castRec;
        *(uint32_t *)(g_castBuf + 0x18) = 0x00008010u;
        *(uint64_t *)(g_castBuf + 0x860) = g_castMask;
        /* Copy the descriptor: the callee writes +0x00
         * and +0x08, so never hand it the live one.
         */
        memset(g_castDesc, 0, sizeof(g_castDesc));
        if (CanRead((void *)rdx, 0x68))
            memcpy(g_castDesc, (void *)rdx, 0x68);

        if (g_castOwnDesc) {
            uint16_t all = 0xFFFF;
            uint32_t two = 2, mask = 0, i;
            const uint32_t BIG = 0x7F7FFFEEu;
            float inv[4];
            memset(g_castDesc, 0, sizeof(g_castDesc));
            memcpy(g_castDesc + 0x10, &all, 2);
            memcpy(g_castDesc + 0x20, &two, 4);
            memcpy(g_castDesc + 0x30, g_castRayOrg, 16);
            memcpy(g_castDesc + 0x40, g_castRayDir, 16);
            for (i = 0; i < 4; i++) {
                float d = g_castRayDir[i];
                if (d == 0.0f) memcpy(&inv[i], &BIG, 4);
                else inv[i] = 1.0f / d;
                if (d >= 0.0f) mask |= (1u << i);
            }
            memcpy(g_castDesc + 0x50, inv, 16);
            mask = (mask & 7u) | 0x3F000000u;
            memcpy(g_castDesc + 0x5C, &mask, 4);
        }
        memcpy(g_castOrigin, g_castDesc + 0x30, 16);
        g_castRet = (int)((CastRay_t)g_castFn)(g_castBuf,
                                               (void *)g_castB,
                                               g_castDesc,
                                               0, 0, 0, 0);
        g_castCount = *(uint16_t *)(g_castBuf + 0x1a);
        memcpy(g_castHit, g_castRec, 16);
        g_castDone = 1;
        g_rayBusy = 0;
    }

    /* Ground query runs here so it inherits the game
     * thread and the physics lock state.
     */
    if (g_gndReq && !g_rayBusy && g_gndThis && g_gndFn) {
        g_rayBusy = 1;
        g_gndReq = 0;
        g_gndOut = -1000.0f;
        g_gndCtx = rcx;
        /* Live descriptor origin is already physics space */
        if (g_gndHere && CanRead((void *)(rdx + 0x30), 16)) {
            float o[4];
            memcpy(o, (void *)(rdx + 0x30), 16);
            memcpy(g_gndOrigin, o, 16);
            g_gndIn[0] = -o[0];
            g_gndIn[1] =  o[2];
            g_gndIn[2] =  o[1];
            g_gndIn[3] =  0.0f;
        }
        g_gndRet = (int)((GroundFn_t)g_gndFn)((void *)g_gndThis,
                                              g_gndIn, &g_gndOut, 1);
        g_gndDone = 1;
        g_rayBusy = 0;
    }

    if (!g_rayReq || g_rayBusy || !g_rayImpl) return;
    /* A pin substitutes the world to query. */
    if (g_rayPin) rcx = g_rayPin;

    /* Only hijack calls whose query object and
     * collector are the types we decompiled.
     */
    if (!CanRead((void *)(rcx + 0x478), 8)) return;
    {
        uint64_t qobj = 0, qvt = 0, cvt = 0;
        memcpy(&qobj, (void *)(rcx + 0x478), 8);
        if (!qobj || !CanRead((void *)qobj, 8)) return;
        memcpy(&qvt, (void *)qobj, 8);
        if (qvt != SH_IMG(0x3D82AD0)) return;
        if (!CanRead((void *)r8, 8)) return;
        memcpy(&cvt, (void *)r8, 8);
        if (cvt != SH_IMG(0x3ADB6F0)) return;
    }
    g_rayUsedCtx = rcx;
    if (!CanRead((void *)rdx, 0x80) || !CanRead((void *)r8, 0x100))
        return;
    if (!CanRead((void *)(rcx + 0x480), 8)) return;

    g_rayBusy = 1;
    g_rayReq = 0;
    {
        uint8_t desc[0x80];
        uint8_t coll[0x100];
        uint64_t obj, a3;
        float ones[4] = {1.0f, 1.0f, 1.0f, 1.0f};

        memcpy(&obj, (void *)(rcx + 0x478), 8);
        memcpy(&a3, (void *)(rcx + 0x28), 8);
        memcpy(desc, (void *)rdx, sizeof(desc));
        memcpy(coll, (void *)r8, sizeof(coll));

        /* Slab test uses reciprocals at +0x50 and a
         * sign mask at +0x5c, rebuild both.
         */
        {
            float inv[4];
            uint32_t mask = 0, i;
            const uint32_t BIG = 0x7F7FFFEEu;
            memcpy(desc + 0x30, g_rayOrigin, 16);
            memcpy(desc + 0x40, g_rayDir, 16);
            for (i = 0; i < 4; i++) {
                float d = g_rayDir[i];
                if (d == 0.0f) memcpy(&inv[i], &BIG, 4);
                else inv[i] = 1.0f / d;
                if (d >= 0.0f) mask |= (1u << i);
            }
            memcpy(desc + 0x50, inv, 16);
            mask = (mask & 7u) | 0x3F000000u;
            memcpy(desc + 0x5C, &mask, 4);
            if (CanRead((void *)(rcx + 0x470), 8)) {
                uint64_t f = 0;
                memcpy(&f, (void *)(rcx + 0x470), 8);
                memcpy(desc + 0x08, &f, 8);
            }
            if (CanRead((void *)(rcx + 0x9d8), 8)) {
                uint64_t w = 0;
                memcpy(&w, (void *)(rcx + 0x9d8), 8);
                memcpy(desc + 0x00, &w, 8);
            }
            {
                uint16_t all = 0xFFFF;
                uint64_t zero = 0;
                uint32_t two = 2;
                memcpy(desc + 0x10, &all, 2);
                memcpy(desc + 0x18, &zero, 8);
                memcpy(desc + 0x20, &two, 4);
            }
        }

        coll[0x10] = 0;
        memcpy(coll + 0x20, ones, 16);

        {
            int k;
            for (k = 0; k < g_rayOvrN; k++) {
                int off = g_rayOvr[k].off;
                if (off < 0 || off + g_rayOvr[k].size > 0x80) continue;
                memcpy(desc + off, &g_rayOvr[k].val,
                       (size_t)g_rayOvr[k].size);
            }
            for (k = 0; k < g_rayCollOvrN; k++) {
                int off = g_rayCollOvrOff[k];
                if (off < 0 || off + g_rayCollOvrSize[k] > 0x100)
                    continue;
                memcpy(coll + off, &g_rayCollOvrVal[k],
                       (size_t)g_rayCollOvrSize[k]);
            }
        }

        if (obj) {
            ((RayImpl_t)g_rayImpl)(obj, desc, a3, coll);
            memcpy(g_rayOut, coll + 0x20, 16);
            g_rayDone = 1;
        }
    }
    g_rayBusy = 0;
}

static void CmdRayCast(Resp *r, const char *line) {
    char is[64] = {0};
    float x = 0, y = 0, z = 0, dz = -900.0f;
    sscanf(line, "%*s %f %f %f %f %63s", &x, &y, &z, &dz, is);
    if (is[0]) g_rayImpl = ParseHex(is);
    if (!g_rayImpl) g_rayImpl = SH_IMG(0x17E5E780);
    if (!g_rayStub) { RAppend(r, "arm raysnap first\n"); return; }

    g_rayOrigin[0] = x; g_rayOrigin[1] = y;
    g_rayOrigin[2] = z; g_rayOrigin[3] = 0.0f;
    g_rayDir[0] = 0.0f; g_rayDir[1] = 0.0f;
    g_rayDir[2] = dz;   g_rayDir[3] = 1.0f;
    g_rayOut[0] = g_rayOut[1] = g_rayOut[2] = g_rayOut[3] = -1.0f;
    g_rayDone = 0;
    g_rayReq = 1;
    RAppend(r, "cast queued from (%.2f, %.2f, %.2f) dz %.1f\n",
            x, y, z, dz);
}

static void CmdGround(Resp *r, const char *line) {
    float x = 0, y = 0, z = 0;
    char ts[64] = {0};
    sscanf(line, "%*s %f %f %f %63s", &x, &y, &z, ts);
    if (ts[0]) g_gndThis = ParseHex(ts);
    if (!g_gndThis) { RAppend(r, "usage: ground x y z <this>\n"); return; }
    if (!g_rayStub) { RAppend(r, "arm raysnap first\n"); return; }

    /* Probe axis is lane 1, and lane 0 is negated. */
    g_gndIn[0] = -x; g_gndIn[1] = z; g_gndIn[2] = y; g_gndIn[3] = 0.0f;
    g_gndHere = 0;
    g_gndOut = -777.0f;
    g_gndRet = -1;
    g_gndDone = 0;
    g_gndReq = 1;
    RAppend(r, "ground queued (%.2f, %.2f, %.2f) this %p\n",
            x, y, z, (void *)g_gndThis);
}

static void CmdCastLive(Resp *r, const char *line) {
    char bs[64] = {0}, ms[64] = {0};
    sscanf(line, "%*s %63s %63s", bs, ms);
    if (bs[0]) g_castB = ParseHex(bs);
    if (ms[0]) g_castMask = ParseHex(ms);
    if (!g_castB) { RAppend(r, "usage: castlive <B> [mask]\n"); return; }
    if (!g_rayStub) { RAppend(r, "arm raysnap first\n"); return; }
    g_castOwnDesc = 0;
    g_castRet = -1;
    g_castCount = -1;
    g_castDone = 0;
    g_castReq = 1;
    RAppend(r, "queued B=%p mask=%#llx\n", (void *)g_castB,
            (unsigned long long)g_castMask);
}

/* Protected int: 4 byte pointers plus an xor key.
 * value = interleave(*p0,*p1,*p2,*p3) ^ key
 */
typedef struct {
    uint8_t *p[4];
    uint32_t key;
} ProtInt;

static int ProtRead(uint64_t addr, uint32_t *out) {
    ProtInt s;
    uint32_t v = 0;
    int i, bit = 0;
    uint8_t b[4];

    if (!CanRead((void *)addr, sizeof(s))) return 0;
    memcpy(&s, (void *)addr, sizeof(s));
    for (i = 0; i < 4; i++) {
        if (!CanRead(s.p[i], 1)) return 0;
        b[i] = *s.p[i];
    }
    while (bit < 32) {
        for (i = 0; i < 4; i++) {
            v |= (uint32_t)(b[i] & 1) << bit;
            b[i] >>= 1;
            bit++;
        }
    }
    *out = v ^ s.key;
    return 1;
}

static int ProtWrite(uint64_t addr, uint32_t val) {
    ProtInt s;
    uint32_t enc;
    uint8_t b[4] = {0, 0, 0, 0};
    int i, bit = 0, round = 0;

    if (!CanRead((void *)addr, sizeof(s))) return 0;
    memcpy(&s, (void *)addr, sizeof(s));
    enc = val ^ s.key;
    while (bit < 32) {
        for (i = 0; i < 4; i++) {
            b[i] |= (uint8_t)((enc >> bit) & 1) << round;
            bit++;
        }
        round++;
    }
    for (i = 0; i < 4; i++) {
        if (!CanRead(s.p[i], 1)) return 0;
        *s.p[i] = b[i];
    }
    return 1;
}

static void CmdHpRead(Resp *r, const char *line) {
    char as[64] = {0};
    uint64_t obj;
    uint32_t cur = 0, mx = 0;
    sscanf(line, "%*s %63s", as);
    if (!as[0]) { RAppend(r, "usage: hpread <component>\n"); return; }
    obj = ParseHex(as);
    if (CanRead((void *)(obj + 0xF0), 4))
        memcpy(&mx, (void *)(obj + 0xF0), 4);
    if (!ProtRead(obj + 0xF8, &cur)) {
        RAppend(r, "protected read failed\n");
        return;
    }
    RAppend(r, "max=%u cur=%u\n", mx, cur);
}

static void CmdHpWrite(Resp *r, const char *line) {
    char as[64] = {0};
    uint32_t val = 0, back = 0;
    uint64_t obj;
    sscanf(line, "%*s %63s %u", as, &val);
    if (!as[0]) { RAppend(r, "usage: hpwrite <component> <v>\n"); return; }
    obj = ParseHex(as);
    if (!ProtWrite(obj + 0xF8, val)) {
        RAppend(r, "protected write failed\n");
        return;
    }
    ProtRead(obj + 0xF8, &back);
    RAppend(r, "wrote %u, reads back %u\n", val, back);
}

/* Find health components by their storage signature. */
static void CmdHpScan(Resp *r, const char *line) {
    MEMORY_BASIC_INFORMATION mbi;
    char ms[32] = {0}, ss[64] = {0}, ls[32] = {0};
    uint32_t minMax = 20;
    uint64_t start = 0x10000000ULL;
    int limit = 40, found = 0;
    uint8_t *scan;

    sscanf(line, "%*s %31s %63s %31s", ms, ss, ls);
    if (ms[0]) minMax = (uint32_t)strtoul(ms, NULL, 10);
    if (ss[0]) start = ParseHex(ss);
    if (ls[0]) limit = (int)strtol(ls, NULL, 10);
    if (limit > 200) limit = 200;
    scan = (uint8_t *)start;

    while (VirtualQuery(scan, &mbi, sizeof(mbi)) && found < limit) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;
        if ((uint64_t)(uintptr_t)mbi.BaseAddress >= 0x800000000000ULL) break;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & PAGE_READWRITE) &&
            !(mbi.Protect & PAGE_GUARD))
        {
            uint8_t *b = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize, o;
            for (o = 0; o + 0x3A0 <= sz; o += 8) {
                uint64_t obj = (uint64_t)(uintptr_t)(b + o);
                uint32_t mx = 0, cur = 0;
                uint64_t pv[4];
                int k, bad = 0;
                memcpy(&mx, b + o + 0xF0, 4);
                if (mx < minMax || mx > 100000) continue;
                memcpy(pv, b + o + 0xF8, 32);
                for (k = 0; k < 4; k++) {
                    if (pv[k] < 0x1000000ULL ||
                        pv[k] >= 0x800000000000ULL) bad = 1;
                    if (k && pv[k] == pv[0]) bad = 1;
                }
                if (bad) continue;
                if (!ProtRead(obj + 0xF8, &cur)) continue;
                if (cur == 0 || cur > mx) continue;
                RAppend(r, "  %p max=%u cur=%u flags %02x %02x %02x"
                           " %02x %02x %02x\n",
                        (void *)obj, mx, cur,
                        b[o + 0x200], b[o + 0x201], b[o + 0x202],
                        b[o + 0x203], b[o + 0x204], b[o + 0x205]);
                found++;
                if (found >= limit) break;
            }
        }
        scan = next;
    }
    RAppend(r, "candidates: %d\n", found);
}

/* ---- ScriptHook API, resolved from dinput8 ---- */

typedef struct { float x, y, z; } ApiVec3;
typedef struct { uint64_t entity, node, root; } ApiPlayer;

/* Mirrors ShEntity in scripthook.h. */
typedef struct {
    uint64_t entity;
    ApiVec3  pos;
    float    distance;
    int      kind;
    uint32_t maxHealth;
    char     name[32];
} ApiEntity;

#define API_FIND_UNNAMED 0x1

static struct {
    int   ready;
    int   (*LastError)(void);
    const char *(*ErrorString)(int);
    int   (*GetGameState)(void);
    int   (*GetGameStateName)(char *, int);
    int   (*IsInGame)(void);
    int   (*GetPlayer)(ApiPlayer *);
    int   (*GetPlayerPosition)(ApiVec3 *);
    int   (*TeleportPlayer)(const ApiVec3 *, const ApiVec3 *);
    int   (*TeleportPlayerHops)(const ApiVec3 *, const ApiVec3 *,
                                float, int);
    int   (*IsInVehicle)(void);
    int   (*QueueCall)(uint64_t, uint64_t, uint64_t,
                       uint64_t, uint64_t);
    int   (*QueueResult)(uint64_t *);
    int   (*FindEntities)(int, float, uint32_t, void *, int);
    int   (*GetEntityKind)(uint64_t);
    const char *(*KindName)(int);
    int   (*PlaceEntity)(uint64_t, const ApiVec3 *,
                         const ApiVec3 *);
    int   (*TeleportPlayerToGround)(float, float, float);
    int   (*GroundHeight)(float, float, float *);
    int   (*GetHealthPlayer)(uint32_t *, uint32_t *);
    int   (*SetHealthPlayer)(uint32_t);
    int   (*SetGodModePlayer)(int);
    int   (*SetCannotDiePlayer)(int);
    int   (*PhysicsReady)(void);
    void  (*Invalidate)(void);
} g_api;

static int ApiInit(Resp *r) {
    HMODULE m;
    if (g_api.ready) return 1;
    m = GetModuleHandleA("dinput8.dll");
    if (!m) {
        if (r) RAppend(r, "dinput8.dll not loaded\n");
        return 0;
    }
#define GETFN(f, n) \
    *(FARPROC *)&g_api.f = GetProcAddress(m, n)
    GETFN(LastError, "ShLastError");
    GETFN(ErrorString, "ShErrorString");
    GETFN(GetGameState, "ShGetGameState");
    GETFN(GetGameStateName, "ShGetGameStateName");
    GETFN(IsInGame, "ShIsInGame");
    GETFN(GetPlayer, "ShGetPlayer");
    GETFN(GetPlayerPosition, "ShGetPlayerPosition");
    GETFN(TeleportPlayer, "ShTeleportPlayer");
    GETFN(TeleportPlayerHops, "ShTeleportPlayerHops");
    GETFN(IsInVehicle, "ShIsInVehicle");
    GETFN(QueueCall, "ShQueueCall");
    GETFN(QueueResult, "ShQueueResult");
    GETFN(FindEntities, "ShFindEntities");
    GETFN(GetEntityKind, "ShGetEntityKind");
    GETFN(KindName, "ShKindName");
    GETFN(PlaceEntity, "ShPlaceEntity");
    GETFN(TeleportPlayerToGround, "ShTeleportPlayerToGround");
    GETFN(GroundHeight, "ShGroundHeight");
    GETFN(GetHealthPlayer, "ShGetHealthPlayer");
    GETFN(SetHealthPlayer, "ShSetHealthPlayer");
    GETFN(SetGodModePlayer, "ShSetGodModePlayer");
    GETFN(SetCannotDiePlayer, "ShSetCannotDiePlayer");
    GETFN(PhysicsReady, "ShPhysicsReady");
    GETFN(Invalidate, "ShInvalidate");
#undef GETFN
    g_api.ready = 1;
    return 1;
}

static void ApiWhy(Resp *r, const char *what, int ok) {
    const char *e = "";
    if (!ok && g_api.LastError && g_api.ErrorString)
        e = g_api.ErrorString(g_api.LastError());
    RAppend(r, "%s %s %s\n", what, ok ? "ok" : "FAIL", e);
}

static const char *FaultName(uint64_t c) {
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION:    return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal insn";
    case EXCEPTION_PRIV_INSTRUCTION:    return "priv insn";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:  return "divide by zero";
    case EXCEPTION_STACK_OVERFLOW:      return "stack overflow";
    }
    return "other";
}

/* `fault` reports, `fault go` releases the thread. */
static void CmdFault(Resp *r, const char *line) {
    char what[32] = {0};
    int i;

    sscanf(line, "%*s %31s", what);
    if (strcmp(what, "go") == 0) {
        g_fltPark = 0;
        RAppend(r, "released, the thread will now die\n");
        return;
    }
    if (!g_fltHit) {
        RAppend(r, "no fault caught. If the game is frozen it "
                   "is a hang, not a crash\n");
        return;
    }
    RAppend(r, "FAULT %s (%#llx) on thread %lu\n",
            FaultName(g_fltCode),
            (unsigned long long)g_fltCode, (unsigned long)g_fltTid);
    RAppend(r, "  at   %p\n", (void *)g_fltAddr);
    RAppend(r, "  rip  %p  rsp %p\n",
            (void *)g_fltRip, (void *)g_fltRsp);
    RAppend(r, "  tried to touch %p\n", (void *)g_fltAccess);
    RAppend(r, "  rcx %p rdx %p r8 %p\n",
            (void *)g_fltRcx, (void *)g_fltRdx, (void *)g_fltR8);
    for (i = 0; i < 32; i++) {
        uint64_t v = g_fltStack[i];
        if (ShInImage(v))
            RAppend(r, "  [rsp+%03x] = %p\n", i * 8, (void *)v);
    }
}

static void CmdApi(Resp *r) {
    ApiPlayer p;
    ApiVec3 v;
    uint32_t cur = 0, mx = 0;
    char name[96] = "?";

    if (!ApiInit(r)) return;
    if (g_api.GetGameStateName) g_api.GetGameStateName(name, sizeof(name));
    RAppend(r, "state    %s (%d)  ingame=%d\n", name,
            g_api.GetGameState ? g_api.GetGameState() : -1,
            g_api.IsInGame ? g_api.IsInGame() : -1);
    RAppend(r, "physics  ready=%d\n",
            g_api.PhysicsReady ? g_api.PhysicsReady() : -1);
    if (g_api.GetPlayerPosition && g_api.GetPlayerPosition(&v))
        RAppend(r, "pos      %.2f %.2f %.2f\n", v.x, v.y, v.z);
    if (g_api.GetPlayer && g_api.GetPlayer(&p))
        RAppend(r, "player   entity %p node %p root %p\n",
                (void *)p.entity, (void *)p.node, (void *)p.root);
    if (g_api.GetHealthPlayer && g_api.GetHealthPlayer(&cur, &mx))
        RAppend(r, "health   %u/%u\n", cur, mx);
}

static uint64_t ImgAddr(uint64_t rva);
static uint64_t EntWorld(void);
static int EntList(uint64_t *outList, uint32_t *outCount);
static int EntIsEntity(uint64_t e);

#define OFF_ENT_COMPS    0x78
#define OFF_ENT_NCOMPS   0x82

/* Entity creation, the recipe from FUN_14C90ACD0. */
#define RVA_MAKE_ENTITY  0x93A8050
#define RVA_SET_POS      0x17DEDE0
#define RVA_ACTIVATE     0xC6C13F0

typedef uint64_t (__attribute__((ms_abi)) *MakeEntity_t)(void);
typedef void (__attribute__((ms_abi)) *SetPos_t)(uint64_t,
                                                 const float *);
typedef void (__attribute__((ms_abi)) *Activate_t)(uint64_t, int);

/* spawn <x> <y> <z> */
static void CmdSpawn(Resp *r, const char *line) {
    double x = 0, y = 0, z = 0;
    uint64_t e, world = 0, lst = 0;
    uint32_t before = 0, after = 0, cnt = 0;
    float pos[4];

    if (sscanf(line, "%*s %lf %lf %lf", &x, &y, &z) != 3) {
        RAppend(r, "usage: spawn <x> <y> <z>\n");
        return;
    }
    world = EntWorld();
    if (!world) {
        RAppend(r, "no world, are you in game\n");
        return;
    }
    if (EntList(&lst, &cnt)) before = cnt;

    e = ((MakeEntity_t)ImgAddr(RVA_MAKE_ENTITY))();
    if (!e) {
        RAppend(r, "creation returned null\n");
        return;
    }
    pos[0] = (float)x; pos[1] = (float)y;
    pos[2] = (float)z; pos[3] = 1.0f;
    ((SetPos_t)ImgAddr(RVA_SET_POS))(e, pos);

    /* Bind to the world before activating, the null world
     * pointer is why activation did nothing before.
     */
    if (CanRead((void *)(e + 0x60), 8))
        memcpy((void *)(e + 0x60), &world, 8);
    ((Activate_t)ImgAddr(RVA_ACTIVATE))(e, 1);

    if (EntList(&lst, &cnt)) after = cnt;
    RAppend(r, "entity %p at %.1f %.1f %.1f, registry %u -> %u\n",
            (void *)e, x, y, z, before, after);
    if (CanRead((void *)(e + 0x50), 12)) {
        float p[3];
        memcpy(p, (void *)(e + 0x50), 12);
        RAppend(r, "  readback pos %.1f %.1f %.1f\n",
                p[0], p[1], p[2]);
    }
}

/* The engine's generic instantiate, from a descriptor.
 * size at desc+0x28, ctor at desc+0x48.
 */
#define RVA_CREATE_INST  0xE0E0C70

typedef uint64_t (__attribute__((ms_abi)) *CreateInst_t)(uint64_t,
                                                         uint64_t);

/* createcomp <entity> <descriptor> [attach] */
static void CmdCreateComp(Resp *r, const char *line) {
    char es[64] = {0}, ds[64] = {0}, opt[16] = {0};
    uint64_t ent, desc, comp, arr;
    uint32_t size = 0;
    uint16_t n = 0;

    if (sscanf(line, "%*s %63s %63s %15s", es, ds, opt) < 2) {
        RAppend(r, "usage: createcomp <entity> <desc> [attach]\n");
        return;
    }
    ent = ParseHex(es);
    desc = ParseHex(ds);
    if (!CanRead((void *)(desc + 0x50), 8)) {
        RAppend(r, "descriptor %p unreadable\n", (void *)desc);
        return;
    }
    memcpy(&size, (void *)(desc + 0x28), 4);

    comp = ((CreateInst_t)ImgAddr(RVA_CREATE_INST))(desc, 0);
    if (!comp) {
        RAppend(r, "creation returned null\n");
        return;
    }
    RAppend(r, "component %p, class size %#x, vtable %p\n",
            (void *)comp, size,
            (void *)SafeReadPtr((void *)comp));

    /* Owner back pointer, the shape health uses. */
    if (ent && CanRead((void *)(comp + 0x28), 8))
        memcpy((void *)(comp + 0x28), &ent, 8);

    if (strcmp(opt, "attach") != 0) return;
    if (!ent || !EntIsEntity(ent)) {
        RAppend(r, "  attach skipped, %p is not an entity\n",
                (void *)ent);
        return;
    }
    arr = SafeReadPtr((void *)(ent + OFF_ENT_COMPS));
    if (!arr || !CanRead((void *)(ent + OFF_ENT_NCOMPS), 2)) {
        RAppend(r, "  attach skipped, no component array\n");
        return;
    }
    memcpy(&n, (void *)(ent + OFF_ENT_NCOMPS), 2);
    if (n >= 512) {
        RAppend(r, "  array full at %u\n", n);
        return;
    }
    if (!CanRead((void *)(arr + (uint64_t)n * 8), 8)) {
        RAppend(r, "  slot %u unwritable\n", n);
        return;
    }
    memcpy((void *)(arr + (uint64_t)n * 8), &comp, 8);
    n++;
    memcpy((void *)(ent + OFF_ENT_NCOMPS), &n, 2);
    RAppend(r, "  attached to %p, now %u components\n",
            (void *)ent, n);
}

/* Must match ShHit in scripthook.h field for field. */
typedef struct {
    uint64_t entity;
    uint64_t root;
    int      kind;
    uint32_t id;
    ApiVec3  pos;
    ApiVec3  normal;
    float    distance;
    uint64_t shooter;
    int      byPlayer;
    uint64_t projectile;
    int      index;
} ApiHit;

/* Must match ShShot in scripthook.h field for field. */
typedef struct {
    uint64_t shooter;
    int      kind;
    int      byPlayer;
    ApiVec3  origin;
    ApiVec3  dir;
    float    yaw;
    float    pitch;
    float    range;
    uint64_t projectile;
} ApiShot;

/* onfire [count], who fired and along what angle. */
static void CmdOnFire(Resp *r, const char *line) {
    HMODULE m = GetModuleHandleA("dinput8.dll");
    int (*install)(void);
    uint32_t (*total)(void);
    int (*get)(ApiShot *, int);
    const char *(*kindName)(int);
    ApiShot buf[32];
    char what[32] = {0};
    int n, i, want;

    sscanf(line, "%*s %31s", what);
    if (!m) { RAppend(r, "dinput8 missing\n"); return; }
    *(FARPROC *)&install = GetProcAddress(m, "ShHitHookInstall");
    *(FARPROC *)&total = GetProcAddress(m, "ShShotCount");
    *(FARPROC *)&get = GetProcAddress(m, "ShGetShots");
    *(FARPROC *)&kindName = GetProcAddress(m, "ShKindName");
    if (!install || !total || !get) {
        RAppend(r, "fire API missing, rebuild dinput8\n");
        return;
    }
    if (strcmp(what, "on") == 0) {
        if (!install()) { ApiWhy(r, "HitHookInstall", 0); return; }
        RAppend(r, "hook installed, shots and hits both live\n");
        return;
    }
    want = what[0] ? atoi(what) : 10;
    if (want > 32) want = 32;
    if (want < 1) want = 10;
    n = get(buf, want);
    RAppend(r, "%u shots total, newest %d:\n", total(), n);
    for (i = 0; i < n; i++)
        RAppend(r, "  %p %-8s %s yaw %7.1f pitch %6.1f  "
                   "from %.1f %.1f %.1f  range %.0f\n",
                (void *)buf[i].shooter,
                kindName ? kindName(buf[i].kind) : "?",
                buf[i].byPlayer ? "ME " : "   ",
                buf[i].yaw, buf[i].pitch, buf[i].origin.x,
                buf[i].origin.y, buf[i].origin.z, buf[i].range);
}

typedef struct {
    float px, py, pz;
    float rx, ry, rz;
    float fx, fy, fz;
    float ux, uy, uz;
    float fov;
    int   mode;
    uint64_t camera;
} ApiCamera;

/* cam, cam orbit <back> <up>, cam set x y z, cam off */
static void CmdCam(Resp *r, const char *line) {
    HMODULE m = GetModuleHandleA("dinput8.dll");
    int (*install)(void);
    int (*getCam)(ApiCamera *);
    int (*orbit)(float, float);
    int (*setCam)(const float *);
    void (*release)(void);
    uint64_t (*calls)(void);
    uint64_t (*writes)(void);
    ApiCamera c;
    char what[32] = {0};
    float a = 0.0f, b = 0.0f, cz = 0.0f;

    sscanf(line, "%*s %31s %f %f %f", what, &a, &b, &cz);
    if (!m) { RAppend(r, "dinput8 missing\n"); return; }
    *(FARPROC *)&install = GetProcAddress(m, "ShCameraHookInstall");
    *(FARPROC *)&getCam = GetProcAddress(m, "ShGetCamera");
    *(FARPROC *)&orbit = GetProcAddress(m, "ShCameraOrbit");
    *(FARPROC *)&setCam = GetProcAddress(m, "ShSetCamera");
    *(FARPROC *)&release = GetProcAddress(m, "ShCameraRelease");
    *(FARPROC *)&calls = GetProcAddress(m, "ShCameraCalls");
    *(FARPROC *)&writes = GetProcAddress(m, "ShCameraWrites");
    if (!install || !getCam || !orbit || !setCam || !release) {
        RAppend(r, "camera API missing, rebuild dinput8\n");
        return;
    }

    if (strcmp(what, "on") == 0) {
        if (!install()) { ApiWhy(r, "CameraHookInstall", 0); return; }
        RAppend(r, "camera hook installed\n");
        return;
    }
    if (strcmp(what, "orbit") == 0) {
        if (!orbit(a, b)) { ApiWhy(r, "CameraOrbit", 0); return; }
        RAppend(r, "orbit back %.1f up %.1f\n", a, b);
        return;
    }
    if (strcmp(what, "set") == 0) {
        float p[3];
        p[0] = a; p[1] = b; p[2] = cz;
        if (!setCam(p)) { ApiWhy(r, "SetCamera", 0); return; }
        RAppend(r, "camera pinned to %.1f %.1f %.1f\n", a, b, cz);
        return;
    }
    if (strcmp(what, "off") == 0) {
        release();
        RAppend(r, "camera released\n");
        return;
    }
    if (strcmp(what, "fov") == 0) {
        struct {
            uint32_t apply;
            float px, py, pz;
            float yaw, pitch, roll;
            float fov;
            float skewX, skewY;
            int   mode;
        } o;
        int (*apply)(const void *);

        *(FARPROC *)&apply = GetProcAddress(m, "ShCameraApply");
        if (!apply) {
            RAppend(r, "ShCameraApply missing, rebuild dinput8\n");
            return;
        }
        memset(&o, 0, sizeof(o));
        o.apply = 0x04;          /* SH_CAM_FOV */
        o.fov = (a > 0.0f) ? a : 0.815f;
        if (!apply(&o)) { ApiWhy(r, "CameraApply", 0); return; }
        RAppend(r, "fov held at %.4f rad (%.1f deg)\n",
                o.fov, o.fov * 57.2957795f);
        return;
    }
    if (strcmp(what, "free") == 0) {
        int (*freeCam)(const float *, float, float);
        int (*angles)(float *, float *);
        float yaw = 0.0f, pitch = 0.0f, p[3];
        float dx = a, dy = b, dz = cz;

        *(FARPROC *)&freeCam = GetProcAddress(m, "ShCameraFree");
        *(FARPROC *)&angles = GetProcAddress(m, "ShCameraAngles");
        if (!freeCam || !angles) {
            RAppend(r, "free camera missing, rebuild dinput8\n");
            return;
        }
        if (!install()) { ApiWhy(r, "CameraHookInstall", 0); return; }
        if (!getCam(&c)) { ApiWhy(r, "GetCamera", 0); return; }
        angles(&yaw, &pitch);
        p[0] = c.px + dx;
        p[1] = c.py + dy;
        p[2] = c.pz + dz;
        if (!freeCam(p, yaw, pitch)) { ApiWhy(r, "CameraFree", 0); return; }
        RAppend(r, "free at %.1f %.1f %.1f  yaw %.3f pitch %.3f\n",
                p[0], p[1], p[2], yaw, pitch);
        return;
    }

    if (!install()) { ApiWhy(r, "CameraHookInstall", 0); return; }
    if (!getCam(&c)) { ApiWhy(r, "GetCamera", 0); return; }
    RAppend(r, "camera %016llX  mode %d  fov %.4f\n",
            (unsigned long long)c.camera, c.mode, c.fov);
    RAppend(r, "  pos      %.2f %.2f %.2f\n", c.px, c.py, c.pz);
    RAppend(r, "  right    %.3f %.3f %.3f\n", c.rx, c.ry, c.rz);
    RAppend(r, "  forward  %.3f %.3f %.3f\n", c.fx, c.fy, c.fz);
    RAppend(r, "  up       %.3f %.3f %.3f\n", c.ux, c.uy, c.uz);
    if (calls && writes)
        RAppend(r, "  calls %llu  writes %llu\n",
                (unsigned long long)calls(),
                (unsigned long long)writes());
}

/* vis <who> <0|1> [persist] [head|all|<nodeHex>] */
static void CmdVis(Resp *r, const char *line) {
    HMODULE m = GetModuleHandleA("dinput8.dll");
    int (*setVis)(uint64_t, uint64_t, int, int);
    int (*nodeCount)(uint64_t);
    int (*headNodes)(uint64_t, uint64_t *, int);
    uint64_t parts[64];
    char who[32] = {0}, what[32] = {0};
    uint64_t ent = 0;
    int visible = 1, persist = 0, i, n = 0;

    sscanf(line, "%*s %31s %d %d %31s", who, &visible, &persist, what);
    if (!m) { RAppend(r, "dinput8 missing\n"); return; }
    *(FARPROC *)&setVis = GetProcAddress(m, "ShSetVisible");
    *(FARPROC *)&nodeCount = GetProcAddress(m, "ShEntityNodeCount");
    *(FARPROC *)&headNodes = GetProcAddress(m, "ShGetHeadNodes");
    if (!setVis || !nodeCount || !headNodes) {
        RAppend(r, "visibility API missing, rebuild dinput8\n");
        return;
    }

    if (!who[0] || strcmp(who, "player") == 0) {
        int (*getPlayer)(ApiPlayer *);
        ApiPlayer p;

        /* Resolved here rather than from g_api, which is
         * only filled in once another command has run.
         */
        *(FARPROC *)&getPlayer = GetProcAddress(m, "ShGetPlayer");
        memset(&p, 0, sizeof(p));
        if (!getPlayer || !getPlayer(&p)) {
            RAppend(r, "no player\n");
            return;
        }
        /* Nodes hang off the root, which is what the
         * teleport work proved is the real object.
         */
        ent = p.root ? p.root : p.entity;
    } else {
        ent = strtoull(who, NULL, 16);
    }

    RAppend(r, "entity %016llX  render nodes %d\n",
            (unsigned long long)ent, nodeCount(ent));

    if (strcmp(what, "head") == 0) {
        n = headNodes(ent, parts, 64);
        if (n <= 0) { ApiWhy(r, "GetHeadNodes", 0); return; }
        for (i = 0; i < n; i++)
            setVis(ent, parts[i], visible, persist);
        RAppend(r, "head group, %d parts %s%s\n", n,
                visible ? "visible" : "hidden",
                (persist && !visible) ? ", held" : "");
        return;
    }

    if (what[0] && strcmp(what, "all") != 0) {
        uint64_t node = strtoull(what, NULL, 16);
        if (!setVis(ent, node, visible, persist)) {
            ApiWhy(r, "SetVisible", 0);
            return;
        }
        RAppend(r, "node %016llX %s\n", (unsigned long long)node,
                visible ? "visible" : "hidden");
        return;
    }

    if (!setVis(ent, 0, visible, persist)) {
        ApiWhy(r, "SetVisible", 0);
        return;
    }
    RAppend(r, "whole entity %s%s\n", visible ? "visible" : "hidden",
            (persist && !visible) ? ", held every frame" : "");
}

/* onhit on|off|<count>, the engine's own hit records. */
static void CmdOnHit(Resp *r, const char *line) {
    HMODULE m = GetModuleHandleA("dinput8.dll");
    int (*install)(void);
    int (*ready)(void);
    uint32_t (*total)(void);
    int (*get)(ApiHit *, int);
    void (*selfFilter)(int);
    ApiHit buf[32];
    char what[32] = {0};
    int n, i, want;

    sscanf(line, "%*s %31s", what);
    if (!m) { RAppend(r, "dinput8 missing\n"); return; }
    *(FARPROC *)&install = GetProcAddress(m, "ShHitHookInstall");
    *(FARPROC *)&ready = GetProcAddress(m, "ShHitHookReady");
    *(FARPROC *)&total = GetProcAddress(m, "ShHitCount");
    *(FARPROC *)&get = GetProcAddress(m, "ShGetHits");
    selfFilter = NULL;
    if (!install || !ready || !total || !get) {
        RAppend(r, "hit API missing, rebuild dinput8\n");
        return;
    }
    if (strcmp(what, "on") == 0) {
        if (!install()) { ApiWhy(r, "HitHookInstall", 0); return; }
        RAppend(r, "hit hook installed\n");
        return;
    }
    (void)selfFilter;
    if (strcmp(what, "noself") == 0) {
        if (selfFilter) selfFilter(1);
        RAppend(r, "self hits dropped\n");
        return;
    }
    if (!ready()) {
        RAppend(r, "hook not installed, run: onhit on\n");
        return;
    }
    want = what[0] ? atoi(what) : 10;
    if (want > 32) want = 32;
    if (want < 1) want = 10;
    n = get(buf, want);
    RAppend(r, "%u hits total, newest %d:\n", total(), n);
    for (i = 0; i < n; i++)
        RAppend(r, "  %p id %u  %.2fm  at %.1f %.1f %.1f\n"
                   "     shooter %p  byPlayer %d  kind %d\n",
                (void *)buf[i].entity, buf[i].id, buf[i].distance,
                buf[i].pos.x, buf[i].pos.y, buf[i].pos.z,
                (void *)buf[i].shooter, buf[i].byPlayer,
                buf[i].kind);
}

typedef struct {
    ApiVec3  origin;
    ApiVec3  dir;
    ApiVec3  hitPos;
    uint32_t hits;
    uint64_t descriptor;
    uint64_t collector;
    uint8_t  raw[32];
    uint64_t projectile;
    uint32_t projHits;
    uint64_t hitObject;
    float    hitDist;
} ApiRay;

typedef struct {
    float   minLength;
    float   maxLength;
    int     hitsOnly;
    ApiVec3 from;
    float   fromRadius;
    ApiVec3 through;
    float   throughRadius;
} ApiRayQuery;

/* raylog on|all|off, or a query:
 * raylog [n] [min L] [max L] [hits] [me R] [at x y z R]
 */
static void CmdRayLog(Resp *r, const char *line) {
    HMODULE m = GetModuleHandleA("dinput8.dll");
    void (*setMode)(int);
    uint32_t (*total)(void);
    int (*query)(const ApiRayQuery *, ApiRay *, int);
    ApiRayQuery q;
    ApiRay buf[64];
    char what[32] = {0};
    const char *p;
    int n, i, want = 12;

    sscanf(line, "%*s %31s", what);
    if (!m) { RAppend(r, "dinput8 missing\n"); return; }
    *(FARPROC *)&setMode = GetProcAddress(m, "ShRayLog");
    *(FARPROC *)&total = GetProcAddress(m, "ShRayCount");
    *(FARPROC *)&query = GetProcAddress(m, "ShQueryRays");
    if (!setMode || !query || !total) {
        RAppend(r, "ray log missing, rebuild dinput8\n");
        return;
    }
    if (strcmp(what, "on") == 0) {
        setMode(2);
        RAppend(r, "ray log on, directed traces only\n");
        return;
    }
    if (strcmp(what, "all") == 0) {
        setMode(1);
        RAppend(r, "ray log on, everything including probes\n");
        return;
    }
    if (strcmp(what, "off") == 0) {
        setMode(0);
        RAppend(r, "ray log off, %u recorded\n", total());
        return;
    }
    if (strcmp(what, "shots") == 0) {
        void (*filt)(float, float);
        float rad = 5.0f, minl = 20.0f;
        sscanf(line, "%*s %*s %f %f", &rad, &minl);
        *(FARPROC *)&filt = GetProcAddress(m, "ShRayFilterPlayer");
        if (!filt) {
            RAppend(r, "filter missing, rebuild dinput8\n");
            return;
        }
        filt(rad, minl);
        setMode(2);
        RAppend(r, "recording traces from within %.0fm of you,"
                   " at least %.0fm long\n", rad, minl);
        return;
    }
    if (strcmp(what, "nofilter") == 0) {
        void (*filt)(float, float);
        *(FARPROC *)&filt = GetProcAddress(m, "ShRayFilterPlayer");
        if (filt) filt(0.0f, 0.0f);
        RAppend(r, "filter cleared\n");
        return;
    }

    memset(&q, 0, sizeof(q));
    if (what[0] >= '0' && what[0] <= '9') want = atoi(what);
    if (want > 64) want = 64;
    if (want < 1) want = 1;

    if ((p = strstr(line, "min ")) != NULL)
        q.minLength = (float)atof(p + 4);
    if ((p = strstr(line, "max ")) != NULL)
        q.maxLength = (float)atof(p + 4);
    if (strstr(line, "hits")) q.hitsOnly = 1;
    if ((p = strstr(line, "me ")) != NULL) {
        if (ApiInit(r) && g_api.GetPlayerPosition &&
            g_api.GetPlayerPosition(&q.from))
            q.fromRadius = (float)atof(p + 3);
    }
    if ((p = strstr(line, "at ")) != NULL)
        sscanf(p + 3, "%f %f %f %f", &q.through.x, &q.through.y,
               &q.through.z, &q.throughRadius);

    n = query(&q, buf, want);
    RAppend(r, "%u recorded, %d match\n", total(), n);
    for (i = 0; i < n; i++) {
        float dx = buf[i].dir.x, dy = buf[i].dir.y;
        float dz = buf[i].dir.z;
        RAppend(r, "  len %8.1f  hits %u  from %.1f %.1f %.1f\n",
                sqrtf(dx * dx + dy * dy + dz * dz), buf[i].hits,
                buf[i].origin.x, buf[i].origin.y, buf[i].origin.z);
        if (buf[i].projectile)
            RAppend(r, "     BULLET proj %p  hits %u  obj %p"
                       "  dist %.2f\n",
                    (void *)buf[i].projectile, buf[i].projHits,
                    (void *)buf[i].hitObject, buf[i].hitDist);
        RAppend(r, "     dir %8.1f %8.1f %8.1f", dx, dy, dz);
        if (buf[i].hits)
            RAppend(r, "  hit %.1f %.1f %.1f", buf[i].hitPos.x,
                    buf[i].hitPos.y, buf[i].hitPos.z);
        RAppend(r, "\n");
        if (strstr(line, "raw")) {
            int b;
            RAppend(r, "     raw");
            for (b = 0; b < 32; b++)
                RAppend(r, " %02x", buf[i].raw[b]);
            RAppend(r, "\n");
        }
    }
}

typedef struct {
    uint64_t component;
    uint64_t dataBlock;
    uint32_t classHash;
    char     name[32];
} ApiComponent;

/* comps <entity> [hash], the ABI component list. */
static void CmdComps(Resp *r, const char *line) {
    HMODULE m = GetModuleHandleA("dinput8.dll");
    int (*get)(uint64_t, ApiComponent *, int);
    ApiComponent buf[128];
    char es[64] = {0}, hs[64] = {0};
    uint64_t ent;
    uint32_t want = 0;
    int n, i;

    if (sscanf(line, "%*s %63s %63s", es, hs) < 1) {
        RAppend(r, "usage: comps <entity> [classhash]\n");
        return;
    }
    if (!m) { RAppend(r, "dinput8 missing\n"); return; }
    *(FARPROC *)&get = GetProcAddress(m, "ShGetComponents");
    if (!get) {
        RAppend(r, "component API missing, rebuild dinput8\n");
        return;
    }
    ent = ParseHex(es);
    if (hs[0]) want = (uint32_t)ParseHex(hs);

    n = get(ent, buf, 128);
    if (!n) { ApiWhy(r, "GetComponents", 0); return; }
    RAppend(r, "%d components on %p\n", n, (void *)ent);
    for (i = 0; i < n; i++) {
        if (want && buf[i].classHash != want) continue;
        RAppend(r, "  [%02d] %p  blk %p  %#010x  %s\n",
                i, (void *)buf[i].component,
                (void *)buf[i].dataBlock, buf[i].classHash,
                buf[i].name[0] ? buf[i].name : "-");
    }
}

typedef struct { uint32_t id; const char *name; } ApiVehicle;

/* vehlist [filter], the ABI catalogue. */
static void CmdVehList(Resp *r, const char *line) {
    HMODULE m = GetModuleHandleA("dinput8.dll");
    int (*count)(void);
    const ApiVehicle *(*at)(int);
    char filt[64] = {0};
    int i, n, shown = 0;

    sscanf(line, "%*s %63s", filt);
    if (!m) { RAppend(r, "dinput8 missing\n"); return; }
    *(FARPROC *)&count = GetProcAddress(m, "ShVehicleCount");
    *(FARPROC *)&at = GetProcAddress(m, "ShVehicleAt");
    if (!count || !at) {
        RAppend(r, "catalogue missing, rebuild dinput8\n");
        return;
    }
    n = count();
    for (i = 0; i < n; i++) {
        const ApiVehicle *v = at(i);
        if (!v) continue;
        if (filt[0] && !strstr(v->name, filt)) continue;
        RAppend(r, "  %2d  %#010x  %s\n", i, v->id, v->name);
        shown++;
    }
    RAppend(r, "%d of %d vehicles\n", shown, n);
}

/* spawnveh <index|0xid> [x y z] */
static void CmdSpawnVeh(Resp *r, const char *line) {
    HMODULE m = GetModuleHandleA("dinput8.dll");
    const ApiVehicle *(*at)(int);
    const char *(*nameOf)(uint32_t);
    uint64_t (*spawn)(uint32_t, const ApiVec3 *);
    char who[64] = {0};
    ApiVec3 pos;
    uint32_t id;
    uint64_t ent;
    int got;

    got = sscanf(line, "%*s %63s %f %f %f", who, &pos.x, &pos.y,
                 &pos.z);
    if (got < 1) {
        RAppend(r, "usage: spawnveh <index|0xid> [x y z]\n");
        return;
    }
    if (!m) { RAppend(r, "dinput8 missing\n"); return; }
    *(FARPROC *)&at = GetProcAddress(m, "ShVehicleAt");
    *(FARPROC *)&nameOf = GetProcAddress(m, "ShVehicleName");
    *(FARPROC *)&spawn = GetProcAddress(m, "ShSpawnVehicle");
    if (!spawn || !at || !nameOf) {
        RAppend(r, "spawn API missing, rebuild dinput8\n");
        return;
    }

    if (who[0] == '0' && (who[1] == 'x' || who[1] == 'X')) {
        id = (uint32_t)ParseHex(who);
    } else {
        const ApiVehicle *v = at(atoi(who));
        if (!v) { RAppend(r, "index out of range\n"); return; }
        id = v->id;
    }

    if (got < 4) {
        if (!ApiInit(r) || !g_api.GetPlayerPosition) return;
        if (!g_api.GetPlayerPosition(&pos)) {
            ApiWhy(r, "GetPlayerPosition", 0);
            return;
        }
        pos.x += 6.0f;
        pos.z += 1.0f;
    }

    ent = spawn(id, &pos);
    if (!ent) {
        ApiWhy(r, "SpawnVehicle", 0);
        return;
    }
    RAppend(r, "%#010x %s\n  entity %p at %.0f %.0f %.0f\n",
            id, nameOf(id) ? nameOf(id) : "?", (void *)ent,
            pos.x, pos.y, pos.z);
}

/* apifind [radius] [kind] [all], for ShFindEntities. */
static void CmdApiFind(Resp *r, const char *line) {
    static const char *KINDS[] = {
        "any", "player", "npc", "vehicle", "drone", "teammate",
        "turret", "mine", "door", "lootchest", "other"
    };
    ApiEntity ents[64];
    char kindArg[32] = {0}, allArg[16] = {0};
    double rad = 60.0;
    uint32_t flags = 0;
    int kind = 0, n, i;
    size_t k;

    sscanf(line, "%*s %lf %31s %15s", &rad, kindArg, allArg);
    if (kindArg[0]) {
        if (strcmp(kindArg, "all") == 0) {
            flags |= API_FIND_UNNAMED;
        } else {
            for (k = 0; k < sizeof(KINDS) / sizeof(KINDS[0]); k++)
                if (strcmp(kindArg, KINDS[k]) == 0) kind = (int)k;
        }
    }
    if (strcmp(allArg, "all") == 0) flags |= API_FIND_UNNAMED;

    if (!ApiInit(r) || !g_api.FindEntities) return;
    n = g_api.FindEntities(kind, (float)rad, flags, ents, 64);
    if (!n) {
        ApiWhy(r, "FindEntities", 0);
        return;
    }
    RAppend(r, "%d entities, kind %s, radius %.0f%s\n", n,
            KINDS[kind], rad,
            (flags & API_FIND_UNNAMED) ? ", unnamed shown" : "");
    for (i = 0; i < n; i++) {
        RAppend(r, "  %7.1fm  %p  %-16s hp %-6u %.0f %.0f %.0f\n",
                ents[i].distance, (void *)ents[i].entity,
                ents[i].name[0] ? ents[i].name : "-",
                ents[i].maxHealth, ents[i].pos.x, ents[i].pos.y,
                ents[i].pos.z);
    }
}

/* apikind <entity>, for ShGetEntityKind. */
static void CmdApiKind(Resp *r, const char *line) {
    char es[64] = {0};
    uint64_t e;
    int k;

    if (sscanf(line, "%*s %63s", es) != 1) {
        RAppend(r, "usage: apikind <entity>\n");
        return;
    }
    if (!ApiInit(r) || !g_api.GetEntityKind) return;
    e = ParseHex(es);
    k = g_api.GetEntityKind(e);
    RAppend(r, "%p kind %d %s\n", (void *)e, k,
            g_api.KindName ? g_api.KindName(k) : "?");
}

static void CmdApiTp(Resp *r, const char *line) {
    ApiVec3 v;
    if (sscanf(line, "%*s %f %f %f", &v.x, &v.y, &v.z) != 3) {
        RAppend(r, "usage: apitp <x> <y> <z>\n");
        return;
    }
    if (!ApiInit(r) || !g_api.TeleportPlayer) return;
    ApiWhy(r, "TeleportPlayer", g_api.TeleportPlayer(&v, NULL));
}

/* pos is required, the rest are optional and default. */
static void CmdApiHop(Resp *r, const char *line) {
    ApiVec3 v, o;
    float ox = 0, oy = 0, oz = 0, hop = 0;
    int delay = 0, got;

    got = sscanf(line, "%*s %f %f %f %f %d %f %f %f",
                 &v.x, &v.y, &v.z, &hop, &delay, &ox, &oy, &oz);
    if (got < 3) {
        RAppend(r, "usage: apihop <x> <y> <z> [hop] [delayMs]"
                   " [ox] [oy] [oz]\n");
        return;
    }
    if (!ApiInit(r) || !g_api.TeleportPlayerHops) return;
    o.x = ox; o.y = oy; o.z = oz;
    RAppend(r, "hop %.0fm delay %dms invehicle %d\n",
            hop > 0 ? hop : 300.0f, delay,
            g_api.IsInVehicle ? g_api.IsInVehicle() : -1);
    ApiWhy(r, "TeleportPlayerHops",
           g_api.TeleportPlayerHops(&v, got >= 8 ? &o : NULL,
                                    hop, delay));
}

static void CmdApiTpGround(Resp *r, const char *line) {
    float x, y, c = 1.0f;
    if (sscanf(line, "%*s %f %f %f", &x, &y, &c) < 2) {
        RAppend(r, "usage: apiground <x> <y> [clearance]\n");
        return;
    }
    if (!ApiInit(r) || !g_api.TeleportPlayerToGround) return;
    ApiWhy(r, "TeleportPlayerToGround",
           g_api.TeleportPlayerToGround(x, y, c));
}

static void CmdApiHeight(Resp *r, const char *line) {
    float x, y, z = 0.0f;
    int ok;
    if (sscanf(line, "%*s %f %f", &x, &y) != 2) {
        RAppend(r, "usage: apiheight <x> <y>\n");
        return;
    }
    if (!ApiInit(r) || !g_api.GroundHeight) return;
    ok = g_api.GroundHeight(x, y, &z);
    ApiWhy(r, "GroundHeight", ok);
    if (ok) RAppend(r, "  z = %.3f\n", z);
}

static void CmdApiPlace(Resp *r, const char *line) {
    char es[64] = {0};
    ApiVec3 v;
    if (sscanf(line, "%*s %63s %f %f %f", es, &v.x, &v.y, &v.z) != 4) {
        RAppend(r, "usage: apiplace <entity> <x> <y> <z>\n");
        return;
    }
    if (!ApiInit(r) || !g_api.PlaceEntity) return;
    ApiWhy(r, "PlaceEntity",
           g_api.PlaceEntity(ParseHex(es), &v, NULL));
}

static void CmdApiHp(Resp *r, const char *line) {
    char what[32] = {0};
    unsigned v = 0;
    uint32_t cur = 0, mx = 0;

    sscanf(line, "%*s %31s %u", what, &v);
    if (!ApiInit(r)) return;
    if (strcmp(what, "set") == 0 && g_api.SetHealthPlayer)
        ApiWhy(r, "SetHealthPlayer", g_api.SetHealthPlayer(v));
    else if (strcmp(what, "god") == 0 && g_api.SetGodModePlayer)
        ApiWhy(r, "SetGodModePlayer", g_api.SetGodModePlayer((int)v));
    else if (strcmp(what, "nodie") == 0 && g_api.SetCannotDiePlayer)
        ApiWhy(r, "SetCannotDiePlayer",
               g_api.SetCannotDiePlayer((int)v));
    else if (g_api.GetHealthPlayer) {
        ApiWhy(r, "GetHealthPlayer", g_api.GetHealthPlayer(&cur, &mx));
        RAppend(r, "  %u/%u\n", cur, mx);
    }
}

static void CmdCastRay(Resp *r, const char *line) {
    float x = 0, y = 0, z = 0, dx = 0, dy = 0, dz = -80.0f;
    char bs[64] = {0}, ms[64] = {0};
    int n = sscanf(line, "%*s %f %f %f %f %f %f %63s %63s",
                   &x, &y, &z, &dx, &dy, &dz, bs, ms);
    if (n < 6) {
        RAppend(r, "usage: castray x y z dx dy dz [B] [mask]\n");
        return;
    }
    if (bs[0]) g_castB = ParseHex(bs);
    if (ms[0]) g_castMask = ParseHex(ms);
    if (!g_castB) { RAppend(r, "need B\n"); return; }
    if (!g_rayStub) { RAppend(r, "arm raysnap first\n"); return; }

    g_castRayOrg[0] = x;  g_castRayOrg[1] = y;
    g_castRayOrg[2] = z;  g_castRayOrg[3] = 0.0f;
    g_castRayDir[0] = dx; g_castRayDir[1] = dy;
    g_castRayDir[2] = dz; g_castRayDir[3] = 1.0f;
    g_castOwnDesc = 1;
    g_castRet = -1;
    g_castCount = -1;
    g_castDone = 0;
    g_castReq = 1;
    RAppend(r, "own ray (%.2f, %.2f, %.2f) dir (%.2f, %.2f, %.2f)"
               " mask %#llx\n", x, y, z, dx, dy, dz,
            (unsigned long long)g_castMask);
}

static void CmdCastRes(Resp *r) {
    RAppend(r, "done=%d ret=%d count=%d\n", g_castDone, g_castRet,
            g_castCount);
    RAppend(r, "hit    %.3f %.3f %.3f\n", g_castHit[0], g_castHit[1],
            g_castHit[2]);
    RAppend(r, "origin %.3f %.3f %.3f\n", g_castOrigin[0],
            g_castOrigin[1], g_castOrigin[2]);
}

static void CmdGroundHere(Resp *r, const char *line) {
    char ts[64] = {0};
    sscanf(line, "%*s %63s", ts);
    if (ts[0]) g_gndThis = ParseHex(ts);
    if (!g_gndThis) { RAppend(r, "usage: groundhere <this>\n"); return; }
    if (!g_rayStub) { RAppend(r, "arm raysnap first\n"); return; }
    g_gndHere = 1;
    g_gndOut = -777.0f;
    g_gndRet = -1;
    g_gndDone = 0;
    g_gndReq = 1;
    RAppend(r, "queued using the live ray origin\n");
}

static void CmdGroundRes(Resp *r) {
    RAppend(r, "done=%d ret=%d height=%.3f\n", g_gndDone, g_gndRet,
            g_gndOut);
    RAppend(r, "in  %.3f %.3f %.3f  ctx %p\n", g_gndIn[0], g_gndIn[1],
            g_gndIn[2], (void *)g_gndCtx);
    RAppend(r, "origin %.3f %.3f %.3f\n", g_gndOrigin[0],
            g_gndOrigin[1], g_gndOrigin[2]);
}

static void CmdGroundFn(Resp *r, const char *line) {
    char as[64] = {0};
    sscanf(line, "%*s %63s", as);
    if (!g_gndFn) g_gndFn = SH_IMG(GND_FN_RVA);
    if (as[0]) g_gndFn = ParseHex(as);
    RAppend(r, "ground fn = %p\n", (void *)g_gndFn);
}

static void CmdRaySet(Resp *r, const char *line) {
    char ws[16] = {0}, os[32] = {0}, vs[64] = {0}, ss[16] = {0};
    sscanf(line, "%*s %15s %31s %63s %15s", ws, os, vs, ss);
    if (!ws[0]) {
        RAppend(r, "usage: rayset d|c <off> <hexval> [size]\n");
        RAppend(r, "       rayset clear\n");
        return;
    }
    if (strcmp(ws, "clear") == 0) {
        g_rayOvrN = 0;
        g_rayCollOvrN = 0;
        RAppend(r, "overrides cleared\n");
        return;
    }
    {
        int off = (int)ParseHex(os);
        uint64_t val = ParseHex(vs);
        int size = ss[0] ? (int)strtol(ss, NULL, 10) : 8;
        if (size != 1 && size != 2 && size != 4 && size != 8) {
            RAppend(r, "size must be 1,2,4,8\n");
            return;
        }
        if (ws[0] == 'c') {
            if (g_rayCollOvrN >= RAY_OVR_MAX) {
                RAppend(r, "full\n"); return;
            }
            g_rayCollOvrOff[g_rayCollOvrN] = off;
            g_rayCollOvrVal[g_rayCollOvrN] = val;
            g_rayCollOvrSize[g_rayCollOvrN] = size;
            g_rayCollOvrN++;
            RAppend(r, "collector +%#x = %#llx (%d)\n", off,
                    (unsigned long long)val, size);
        } else {
            if (g_rayOvrN >= RAY_OVR_MAX) {
                RAppend(r, "full\n"); return;
            }
            g_rayOvr[g_rayOvrN].off = off;
            g_rayOvr[g_rayOvrN].val = val;
            g_rayOvr[g_rayOvrN].size = size;
            g_rayOvrN++;
            RAppend(r, "desc +%#x = %#llx (%d)\n", off,
                    (unsigned long long)val, size);
        }
    }
}

static void CmdRayPin(Resp *r, const char *line) {
    char as[64] = {0};
    sscanf(line, "%*s %63s", as);
    g_rayPin = as[0] ? ParseHex(as) : 0;
    RAppend(r, "ctx pin = %p\n", (void *)g_rayPin);
}

static void CmdRayRes(Resp *r) {
    RAppend(r, "ctx used %p\n", (void *)g_rayUsedCtx);
    RAppend(r, "done=%d out %.4f %.4f %.4f %.4f\n", g_rayDone,
            g_rayOut[0], g_rayOut[1], g_rayOut[2], g_rayOut[3]);
    if (g_rayDone && g_rayOut[0] >= 0.0f && g_rayOut[0] < 1.0f)
        RAppend(r, "hit Z = %.3f\n",
                g_rayOrigin[2] + g_rayDir[2] * g_rayOut[0]);
}

static void CmdRaySnap(Resp *r, const char *line) {
    char as[64] = {0}, ns[64] = {0};
    sscanf(line, "%*s %63s %63s", as, ns);
    if (!as[0]) { RAppend(r, "usage: raysnap <addr> [stolen]\n"); return; }
    if (g_rayStub) { RAppend(r, "already installed\n"); return; }

    uint64_t fn = ParseHex(as);
    for (int g = 0; g < 8; g++) {
        if (!CanRead((void *)fn, 5)) break;
        if (*(uint8_t *)fn != 0xE9) break;
        int32_t rel;
        memcpy(&rel, (void *)(fn + 1), 4);
        fn = fn + 5 + (int64_t)rel;
    }
    int n = ns[0] ? (int)strtol(ns, NULL, 10) : 5;
    if (n < 5 || n > 24) { RAppend(r, "stolen 5..24\n"); return; }
    if (!CanRead((void *)fn, n)) { RAppend(r, "unreadable\n"); return; }

    uint8_t *s = AllocNear(fn);
    if (!s) { RAppend(r, "alloc failed\n"); return; }
    memset(s, 0xCC, 0x1000);

    int o = 0;
    static const uint8_t PU[] = {
        0x50, 0x51, 0x52, 0x41,0x50, 0x41,0x51, 0x41,0x52, 0x41,0x53
    };
    memcpy(s + o, PU, sizeof(PU)); o += sizeof(PU);
    s[o++]=0x48; s[o++]=0x83; s[o++]=0xEC; s[o++]=0x20;
    s[o++]=0x48; s[o++]=0xB8;
    *(uint64_t *)(s+o) = (uint64_t)(uintptr_t)RayCallback; o += 8;
    s[o++]=0xFF; s[o++]=0xD0;
    s[o++]=0x48; s[o++]=0x83; s[o++]=0xC4; s[o++]=0x20;
    static const uint8_t PO[] = {
        0x41,0x5B, 0x41,0x5A, 0x41,0x59, 0x41,0x58, 0x5A, 0x59, 0x58
    };
    memcpy(s + o, PO, sizeof(PO)); o += sizeof(PO);

    memcpy(s + o, (void *)fn, n); o += n;
    s[o++]=0xFF; s[o++]=0x25;
    *(int32_t *)(s+o)=0; o+=4;
    *(uint64_t *)(s+o) = fn + n;

    int64_t rel = (int64_t)s - (int64_t)(fn + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) {
        RAppend(r, "stub too far\n"); return;
    }
    DWORD old;
    if (!VirtualProtect((void *)fn, n, PAGE_EXECUTE_READWRITE, &old)) {
        RAppend(r, "protect failed\n"); return;
    }
    memcpy(g_rayOrig, (void *)fn, n);
    g_rayOrigLen = n;
    uint8_t patch[24];
    memset(patch, 0x90, n);
    patch[0] = 0xE9;
    *(int32_t *)(patch + 1) = (int32_t)rel;
    memcpy((void *)fn, patch, n);
    VirtualProtect((void *)fn, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)fn, n);

    g_rayStub = s;
    g_rayAddr = fn;
    g_raySnapped = 0;
    RAppend(r, "raysnap armed at %p, stub %p\n",
            (void *)fn, (void *)s);
}

static void CmdRayInfo(Resp *r) {
    if (!g_raySnapped) { RAppend(r, "no snapshot yet\n"); return; }
    RAppend(r, "ctx=%p rdx=%p r8=%p\n", (void *)g_rayCtx,
            (void *)g_rayRdx, (void *)g_rayR8);
    RAppend(r, "bufA=%p bufB=%p\n", (void *)g_raySnapA,
            (void *)g_raySnapB);
}

static void CmdRayOff(Resp *r) {
    if (!g_rayStub) { RAppend(r, "not installed\n"); return; }
    DWORD old;
    if (VirtualProtect((void *)g_rayAddr, g_rayOrigLen,
                       PAGE_EXECUTE_READWRITE, &old)) {
        memcpy((void *)g_rayAddr, g_rayOrig, g_rayOrigLen);
        VirtualProtect((void *)g_rayAddr, g_rayOrigLen, old, &old);
        FlushInstructionCache(GetCurrentProcess(),
                              (void *)g_rayAddr, g_rayOrigLen);
    }
    g_rayStub = NULL;
    g_rayAddr = 0;
    RAppend(r, "raysnap removed\n");
}

static void CmdHookCb(Resp *r, const char *line) {
    char as[64] = {0}, ns[64] = {0};
    sscanf(line, "%*s %63s %63s", as, ns);
    if (!as[0]) { RAppend(r, "usage: hookcb <addr> <stolen>\n"); return; }
    if (g_cbStub) { RAppend(r, "already installed\n"); return; }

    uint64_t fn = ParseHex(as);
    for (int g = 0; g < 8; g++) {
        if (!CanRead((void *)fn, 5)) break;
        if (*(uint8_t *)fn != 0xE9) break;
        int32_t rel;
        memcpy(&rel, (void *)(fn + 1), 4);
        fn = fn + 5 + (int64_t)rel;
    }
    int n = ns[0] ? (int)strtol(ns, NULL, 10) : 5;
    if (n < 5 || n > 24) { RAppend(r, "stolen 5..24\n"); return; }
    if (!CanRead((void *)fn, n)) { RAppend(r, "unreadable\n"); return; }

    uint8_t *s = AllocNear(fn);
    if (!s) { RAppend(r, "alloc failed\n"); return; }
    memset(s, 0xCC, 0x1000);

    int o = 0;
    /* push volatiles, align, call, restore */
    static const uint8_t PUSHES[] = {
        0x50, 0x51, 0x52, 0x41,0x50, 0x41,0x51, 0x41,0x52, 0x41,0x53
    };
    /* 7 pushes leave rsp 16-aligned; 0x20 keeps it aligned */
    memcpy(s + o, PUSHES, sizeof(PUSHES)); o += sizeof(PUSHES);
    s[o++]=0x48; s[o++]=0x83; s[o++]=0xEC; s[o++]=0x20;
    s[o++]=0x48; s[o++]=0xB8;
    *(uint64_t *)(s+o) = (uint64_t)(uintptr_t)TpCallback; o += 8;
    s[o++]=0xFF; s[o++]=0xD0;
    s[o++]=0x48; s[o++]=0x83; s[o++]=0xC4; s[o++]=0x20;
    static const uint8_t POPS[] = {
        0x41,0x5B, 0x41,0x5A, 0x41,0x59, 0x41,0x58, 0x5A, 0x59, 0x58
    };
    memcpy(s + o, POPS, sizeof(POPS)); o += sizeof(POPS);

    memcpy(s + o, (void *)fn, n); o += n;
    s[o++]=0xFF; s[o++]=0x25;
    *(int32_t *)(s+o)=0; o+=4;
    *(uint64_t *)(s+o) = fn + n;

    int64_t rel = (int64_t)s - (int64_t)(fn + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) {
        RAppend(r, "stub too far\n"); return;
    }
    DWORD old;
    if (!VirtualProtect((void *)fn, n, PAGE_EXECUTE_READWRITE, &old)) {
        RAppend(r, "protect failed\n"); return;
    }
    memcpy(g_cbOrig, (void *)fn, n);
    uint8_t patch[32];
    memset(patch, 0x90, n);
    patch[0] = 0xE9;
    *(int32_t *)(patch + 1) = (int32_t)rel;
    memcpy((void *)fn, patch, n);
    VirtualProtect((void *)fn, n, old, &old);

    g_cbStub = s; g_cbAddr = fn; g_cbLen = n;
    RAppend(r, "callback hook at %p -> stub %p\n", (void *)fn, s);
}

static void CmdUnhookCb(Resp *r) {
    if (!g_cbStub) { RAppend(r, "none\n"); return; }
    DWORD old;
    VirtualProtect((void *)g_cbAddr, g_cbLen,
                   PAGE_EXECUTE_READWRITE, &old);
    memcpy((void *)g_cbAddr, g_cbOrig, g_cbLen);
    VirtualProtect((void *)g_cbAddr, g_cbLen, old, &old);
    g_cbStub = NULL;
    RAppend(r, "callback hook removed\n");
}

static void CmdHook(Resp *r, const char *line) {
    char as[64] = {0}, ns[64] = {0}, fs[64] = {0};
    sscanf(line, "%*s %63s %63s %63s", as, ns, fs);
    if (!as[0]) {
        RAppend(r, "usage: hook <addr> <stolen> [rdx_filter]\n");
        return;
    }
    uint64_t filt = fs[0] ? ParseHex(fs) : 0;
    if (g_stub) { RAppend(r, "already hooked %p\n", (void *)g_hookAddr); return; }

    uint64_t fn = ParseHex(as);

    /* follow jmp rel32 trampolines to the real function */
    for (int guard = 0; guard < 8; guard++) {
        if (!CanRead((void *)fn, 5)) break;
        if (*(uint8_t *)fn != 0xE9) break;
        int32_t rel32;
        memcpy(&rel32, (void *)(fn + 1), 4);
        uint64_t next = fn + 5 + (int64_t)rel32;
        RAppend(r, "following trampoline %p -> %p\n",
                (void *)fn, (void *)next);
        fn = next;
    }

    int n = ns[0] ? (int)strtol(ns, NULL, 10) : 5;
    if (n < 5 || n > 0x50) { RAppend(r, "stolen must be 5..80\n"); return; }
    if (!CanRead((void *)fn, n)) { RAppend(r, "cannot read fn\n"); return; }

    uint8_t *s = AllocNear(fn);
    if (!s) { RAppend(r, "alloc near failed\n"); return; }
    memset(s, 0xCC, 0x1000);
    memset(s + 0x200, 0, 0x100);

    int o = 0;
    int jneAt = -1;

    if (filt) {
        *(uint64_t *)(s + HK_FILTER) = filt;
        s[o++]=0x48; s[o++]=0x3B; s[o++]=0x15;
        { int32_t d = (int32_t)(HK_FILTER - (o + 4));
          memcpy(s+o,&d,4); o+=4; }
        s[o++]=0x75; jneAt = o; s[o++]=0x00;
    }

    for (int i = 0; i < 16; i++) {
        s[o++] = HK_REGS[i].a;
        s[o++] = HK_REGS[i].b;
        s[o++] = HK_REGS[i].c;
        int32_t d = (int32_t)(HK_REGS[i].slot - (o + 4));
        memcpy(s + o, &d, 4); o += 4;
    }

    /* rax already saved; reuse it for the return address */
    s[o++]=0x48; s[o++]=0x8B; s[o++]=0x04; s[o++]=0x24;
    s[o++]=0x48; s[o++]=0x89; s[o++]=0x05;
    { int32_t d = (int32_t)(HK_RET - (o + 4)); memcpy(s+o,&d,4); o+=4; }
    s[o++]=0x48; s[o++]=0xFF; s[o++]=0x05;
    { int32_t d = (int32_t)(HK_COUNT - (o + 4)); memcpy(s+o,&d,4); o+=4; }
    /* restore rax so the stolen bytes see the original */
    s[o++]=0x48; s[o++]=0x8B; s[o++]=0x05;
    { int32_t d = (int32_t)(HK_REGS[0].slot - (o + 4));
      memcpy(s+o,&d,4); o+=4; }

    if (jneAt >= 0) s[jneAt] = (uint8_t)(o - (jneAt + 1));

    /* copy stolen bytes, fixing rip-relative lea disps */
    memcpy(s + o, (void *)fn, n);
    for (int i = 0; i + 7 <= n; i++) {
        uint8_t b0 = s[o + i];
        if (b0 != 0x48 && b0 != 0x4C) continue;
        if (s[o + i + 1] != 0x8D) continue;
        if ((s[o + i + 2] & 0xC7) != 0x05) continue;
        int32_t d;
        memcpy(&d, s + o + i + 3, 4);
        uint64_t tgt = fn + i + 7 + (int64_t)d;
        int64_t nd = (int64_t)tgt - (int64_t)(s + o + i + 7);
        if (nd > 0x7FFFFFFFLL || nd < -0x7FFFFFFFLL) {
            RAppend(r, "lea fixup out of range\n");
            return;
        }
        int32_t nd32 = (int32_t)nd;
        memcpy(s + o + i + 3, &nd32, 4);
        RAppend(r, "fixed rip-lea at +%d -> %p\n", i, (void *)tgt);
    }
    o += n;

    s[o++]=0xFF; s[o++]=0x25;
    *(int32_t *)(s+o)=0; o+=4;
    *(uint64_t *)(s+o) = fn + n;

    int64_t rel = (int64_t)s - (int64_t)(fn + 5);
    if (rel > 0x7FFFFFFFLL || rel < -0x7FFFFFFFLL) {
        RAppend(r, "stub too far\n"); return;
    }

    DWORD old;
    if (!VirtualProtect((void *)fn, n, PAGE_EXECUTE_READWRITE, &old)) {
        RAppend(r, "VirtualProtect failed %lu\n", GetLastError());
        return;
    }
    memcpy(g_origBytes, (void *)fn, n);
    uint8_t patch[0x50];
    memset(patch, 0x90, n);
    patch[0] = 0xE9;
    *(int32_t *)(patch + 1) = (int32_t)rel;
    memcpy((void *)fn, patch, n);
    VirtualProtect((void *)fn, n, old, &old);

    g_stub = s; g_hookAddr = fn; g_hookLen = n;
    RAppend(r, "hooked %p -> stub %p, stole %d, ret %p\n",
            (void *)fn, s, n, (void *)(fn + n));
}

static void CmdHookInfo(Resp *r) {
    if (!g_stub) { RAppend(r, "not hooked\n"); return; }
    uint64_t cnt = *(uint64_t *)(g_stub + HK_COUNT);
    uint64_t ra  = *(uint64_t *)(g_stub + HK_RET);
    RAppend(r, "hook %p  calls=%llu\n",
            (void *)g_hookAddr, (unsigned long long)cnt);
    RAppend(r, "  caller = %p\n", (void *)ra);
    for (int i = 0; i < 16; i++) {
        uint64_t v = *(uint64_t *)(g_stub + HK_REGS[i].slot);
        RAppend(r, "  %s = %018p", HK_NAMES[i], (void *)v);
        if (ShInImage(v))
            RAppend(r, "  [image]");
        else if (CanRead((void *)v, 8)) {
            uint64_t p0 = SafeReadPtr((void *)v);
            if (ShInImage(p0))
                RAppend(r, "  [obj vt=%p]", (void *)p0);
            uint64_t p8 = SafeReadPtr((void *)(v + 8));
            if (ShInImage(p8))
                RAppend(r, "  [+8 vt=%p]", (void *)p8);
        }
        RAppend(r, "\n");
    }
}

static void CmdUnhook(Resp *r) {
    if (!g_stub) { RAppend(r, "not hooked\n"); return; }
    DWORD old;
    VirtualProtect((void *)g_hookAddr, g_hookLen,
                   PAGE_EXECUTE_READWRITE, &old);
    memcpy((void *)g_hookAddr, g_origBytes, g_hookLen);
    VirtualProtect((void *)g_hookAddr, g_hookLen, old, &old);
    g_stub = NULL; g_hookAddr = 0;
    RAppend(r, "unhooked\n");
}

/* find call/jmp rel32 sites targeting an address */
static void CmdXCall(Resp *r, const char *addrStr) {
    uint8_t *target = (uint8_t *)ParseHex(addrStr);
    RAppend(r, "scanning for call/jmp rel32 to %p...\n", target);
    int found = 0;
    for (int s = 0; s < g_nsections; s++) {
        uint32_t ch = g_sections[s].chars;
        if (!(ch & IMAGE_SCN_MEM_EXECUTE) &&
            !(ch & IMAGE_SCN_CNT_CODE)) continue;
        uint8_t *base = g_sections[s].base;
        size_t sz = g_sections[s].size;
        if (sz < 5) continue;
        for (size_t i = 0; i <= sz - 5; i++) {
            uint8_t op = base[i];
            if (op != 0xE8 && op != 0xE9) continue;
            int32_t rel;
            memcpy(&rel, base + i + 1, 4);
            if (base + i + 5 + rel != target) continue;
            RAppend(r, "  %s at %p (%.8s+0x%zx)\n",
                    op == 0xE8 ? "call" : "jmp ",
                    base + i, g_sections[s].name, i);
            found++;
            if (found >= 40) goto done_xc;
        }
    }
done_xc:
    RAppend(r, "found %d\n", found);
}

/* ---- deferred teleport on the game thread ---- */

typedef void (*pub_t)(uint64_t, uint64_t, uint64_t, uint64_t);

static volatile int g_tpPending = 0;
static uint64_t     g_tpEntity = 0;
static uint64_t     g_tpPublish = 0;
static float        g_tpDest[3];
static volatile int g_tpDone = 0;
static volatile int g_tpTicks = 0;

volatile int g_gcPending = 0;
volatile int g_gcDone = 0;
uint64_t     g_gcFn = 0;
uint64_t     g_gcA[4];
uint64_t     g_gcRet = 0;

static uint64_t     g_tpArgRcx = 0;
static uint64_t     g_tpArgRdx = 0;
static uint32_t     g_tpMtxOff = 0x30;
static volatile int g_tpHits = 0;
static volatile int g_tpActive = 0;
static uint64_t     g_tpNode = 0;
static float        g_tpLast[3];

/* Substitute the matrix the engine is committing.
   rcx = TransformNode, rdx = float4x4, translation
   is the fourth row at +0x30. */
static void __attribute__((ms_abi))
TpCallback(uint64_t node, uint64_t mtx) {
    g_tpTicks++;
    g_tpArgRcx = node;
    g_tpArgRdx = mtx;
    /* queued engine call runs at any hook point */
    if (g_gcPending) {
        g_gcPending = 0;
        g_gcRet = ((gc_t)g_gcFn)(g_gcA[0], g_gcA[1], g_gcA[2], g_gcA[3]);
        g_gcDone++;
    }
    if (!g_tpNode || node != g_tpNode) return;
    if (!mtx) return;
    g_tpHits++;
    memcpy((void *)g_tpLast, (const void *)(mtx + g_tpMtxOff), 12);
    if (!g_tpActive) return;
    memcpy((void *)(mtx + g_tpMtxOff), (const void *)g_tpDest, 12);
    g_tpDone++;
}

static void CmdTpNode(Resp *r, const char *line) {
    char es[64] = {0}, os[64] = {0};
    sscanf(line, "%*s %63s %63s", es, os);
    if (!es[0]) {
        RAppend(r, "usage: tpnode <gate> [xlate-offset]\n");
        return;
    }
    g_tpNode = ParseHex(es);
    if (os[0]) g_tpMtxOff = (uint32_t)ParseHex(os);
    g_tpHits = 0;
    RAppend(r, "gate %p, translation at +%#x\n",
            (void *)g_tpNode, g_tpMtxOff);
}

static void CmdTpSet(Resp *r, const char *line) {
    float x, y, z;
    if (sscanf(line, "%*s %f %f %f", &x, &y, &z) != 3) {
        RAppend(r, "usage: tpset <x> <y> <z>\n");
        return;
    }
    g_tpDest[0] = x; g_tpDest[1] = y; g_tpDest[2] = z;
    g_tpActive = 1;
    RAppend(r, "substituting -> (%.2f, %.2f, %.2f)\n", x, y, z);
}

static void CmdTpOff(Resp *r) {
    g_tpActive = 0;
    RAppend(r, "substitution off\n");
}

/* main-thread execution via the game's window proc */
static WNDPROC g_oldProc = NULL;
static HWND    g_hwnd = NULL;
static volatile int g_wndTicks = 0;

static LRESULT CALLBACK HookProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    g_wndTicks++;
    if (g_gcPending) {
        g_gcPending = 0;
        g_gcRet = ((gc_t)g_gcFn)(g_gcA[0], g_gcA[1], g_gcA[2], g_gcA[3]);
        g_gcDone++;
    }
    return CallWindowProcA(g_oldProc, h, m, w, l);
}

/* first visible window of this process, and a listing */
static HWND g_bestWnd = NULL;
static long g_bestArea = 0;
static Resp *g_wndResp = NULL;

static BOOL CALLBACK EnumProc(HWND h, LPARAM p) {
    (void)p;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    RECT rc;
    GetClientRect(h, &rc);
    long area = (long)(rc.right - rc.left) * (rc.bottom - rc.top);
    if (g_wndResp)
        RAppend(g_wndResp, "  hwnd %p vis=%d area=%ld\n",
                (void *)h, IsWindowVisible(h) ? 1 : 0, area);
    if (!g_bestWnd && IsWindowVisible(h)) {
        g_bestWnd = h; g_bestArea = area;
    }
    return TRUE;
}

static void CmdWndHook(Resp *r, const char *line) {
    if (g_oldProc) { RAppend(r, "already hooked\n"); return; }
    char hs[64] = {0};
    sscanf(line, "%*s %63s", hs);
    g_bestWnd = NULL; g_bestArea = 0;
    g_wndResp = r;
    EnumWindows(EnumProc, 0);
    g_wndResp = NULL;
    HWND h = hs[0] ? (HWND)(uintptr_t)ParseHex(hs) : g_bestWnd;
    if (!h) { RAppend(r, "no window found\n"); return; }
    RAppend(r, "using window %p\n", (void *)h);
    g_hwnd = h;
    g_oldProc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC,
                                           (LONG_PTR)HookProc);
    if (!g_oldProc) { RAppend(r, "subclass failed\n"); return; }
    RAppend(r, "window %p subclassed, old proc %p\n",
            (void *)h, (void *)g_oldProc);
}

static void CmdWndOff(Resp *r) {
    if (!g_oldProc) { RAppend(r, "none\n"); return; }
    SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_oldProc);
    g_oldProc = NULL;
    RAppend(r, "window proc restored\n");
}

static void CmdWndStat(Resp *r) {
    RAppend(r, "wndticks=%d hooked=%d\n", g_wndTicks, g_oldProc != NULL);
}

/* Older variant, drained by the window proc hook. */
static void CmdGCallWnd(Resp *r, const char *line) {
    char f[64] = {0}, a[4][64] = {{0}};
    int n = sscanf(line, "%*s %63s %63s %63s %63s %63s",
                   f, a[0], a[1], a[2], a[3]);
    if (n < 1 || !f[0]) {
        RAppend(r, "usage: gcallwnd <fn> [a] [b] [c] [d]\n");
        return;
    }
    g_gcFn = ParseHex(f);
    for (int i = 0; i < 4; i++)
        g_gcA[i] = a[i][0] ? ParseHex(a[i]) : 0;
    g_gcRet = 0;
    g_gcPending = 1;
    /* wake the window proc so it runs without user input */
    if (g_hwnd) PostMessageA(g_hwnd, WM_NULL, 0, 0);
    RAppend(r, "queued %p(%p, %p, %p, %p) on game thread\n",
            (void *)g_gcFn, (void *)g_gcA[0], (void *)g_gcA[1],
            (void *)g_gcA[2], (void *)g_gcA[3]);
}

/* A ring, so a buffer handed to the engine is never
 * overwritten later. One static buffer froze the game
 * twice, which looks like a retained pointer. */
#define MTX_RING 64
static uint8_t g_mtxRing[MTX_RING][64] __attribute__((aligned(16)));
static int g_mtxNext = 0;

static void CmdMkMtx(Resp *r, const char *line) {
    char ns[64] = {0};
    float x, y, z;
    uint8_t *slot;
    int used;

    if (sscanf(line, "%*s %63s %f %f %f", ns, &x, &y, &z) != 4) {
        RAppend(r, "usage: mkmtx <node> <x> <y> <z>\n");
        return;
    }
    uint64_t n = ParseHex(ns);
    if (!CanRead((void *)(n + 0x20), 64)) {
        RAppend(r, "cannot read matrix\n");
        return;
    }
    used = g_mtxNext;
    slot = g_mtxRing[used];
    g_mtxNext = (g_mtxNext + 1) % MTX_RING;

    memcpy(slot, (void *)(n + 0x20), 64);
    float t[4] = {x, y, z, 1.0f};
    memcpy(slot + 0x30, t, 16);
    RAppend(r, "matrix at %p -> (%.2f, %.2f, %.2f)  slot %d\n",
            (void *)slot, x, y, z, used);
}

static void CmdGStat(Resp *r) {
    RAppend(r, "gcall pending=%d done=%d ret=%p\n",
            g_gcPending, g_gcDone, (void *)g_gcRet);
}

static void CmdTpStat(Resp *r) {
    RAppend(r, "ticks=%d hits=%d active=%d done=%d\n",
            g_tpTicks, g_tpHits, g_tpActive, g_tpDone);
    RAppend(r, "gate=%p last seen (%.2f, %.2f, %.2f)\n",
            (void *)g_tpNode, g_tpLast[0], g_tpLast[1], g_tpLast[2]);
    RAppend(r, "last args rcx=%p rdx=%p\n",
            (void *)g_tpArgRcx, (void *)g_tpArgRdx);
}

/* enumerate transform objects and print the scene tree */
typedef struct { uint64_t obj, node, parent; float pos[3]; } TNode;

static void CmdTree(Resp *r, const char *line) {
    char vs[64] = {0}, ms[64] = {0};
    sscanf(line, "%*s %63s %63s", vs, ms);
    uint64_t vt = vs[0] ? ParseHex(vs) : SH_IMG(0x39C6FC8);
    int maxn = ms[0] ? (int)strtol(ms, NULL, 10) : 4000;
    if (maxn > 20000) maxn = 20000;

    TNode *t = (TNode *)malloc(sizeof(TNode) * maxn);
    if (!t) { RAppend(r, "alloc failed\n"); return; }
    int n = 0;

    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = (uint8_t *)0x10000000;
    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;
        if ((uint64_t)mbi.BaseAddress >= 0x800000000000ULL) break;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & PAGE_READWRITE) &&
            !(mbi.Protect & PAGE_GUARD))
        {
            uint8_t *b = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize;
            for (size_t o = 0; o + 0x60 <= sz; o += 8) {
                uint64_t v;
                memcpy(&v, b + o, 8);
                if (v != vt) continue;
                if (n >= maxn) goto done_tree;
                uint64_t base = (uint64_t)(b + o);
                t[n].obj = base;
                t[n].node = SafeReadPtr((void *)(base + 0x18));
                t[n].parent = 0;
                memcpy(t[n].pos, (void *)(base + 0x50), 12);
                n++;
            }
        }
        scan = next;
    }
done_tree:
    /* link: parent is whoever holds this object at +0x40 */
    for (int i = 0; i < n; i++) {
        uint64_t child = SafeReadPtr((void *)(t[i].obj + 0x40));
        for (int j = 0; j < n; j++)
            if (t[j].obj == child) { t[j].parent = t[i].obj; break; }
    }

    int roots = 0;
    for (int i = 0; i < n; i++) if (!t[i].parent) roots++;
    RAppend(r, "objects=%d roots=%d vt=%p\n", n, roots, (void *)vt);
    int shown = 0;
    for (int i = 0; i < n && shown < 200; i++) {
        RAppend(r, "  %p node=%p parent=%p (%.1f, %.1f, %.1f)\n",
                (void *)t[i].obj, (void *)t[i].node,
                (void *)t[i].parent,
                t[i].pos[0], t[i].pos[1], t[i].pos[2]);
        shown++;
    }
    free(t);
}

/* skeletons: pos mirrored at +0x120 and +0x250 */
static void CmdFindSkel(Resp *r) {
    uint8_t *pp = PlayerPosPtr();
    if (!pp) { RAppend(r, "player pos unknown\n"); return; }
    float t[3];
    memcpy(t, pp, 12);
    RAppend(r, "player (%.2f, %.2f, %.2f)\n", t[0], t[1], t[2]);
    const float TOL = 3.0f;

    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = (uint8_t *)0x1000000;
    int found = 0;
    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & PAGE_READWRITE) &&
            !(mbi.Protect & PAGE_GUARD))
        {
            uint8_t *b = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize;
            if (sz < 0x300) { scan = next; continue; }
            for (size_t o = 0; o + 0x300 <= sz; o += 8) {
                float a[3], c[3];
                memcpy(a, b + o + 0x120, 12);
                int ok = 1;
                for (int k = 0; k < 3; k++) {
                    float d = a[k] - t[k]; if (d < 0) d = -d;
                    if (!(d <= TOL)) ok = 0;
                }
                if (!ok) continue;
                memcpy(c, b + o + 0x250, 12);
                for (int k = 0; k < 3; k++) {
                    float d = c[k] - t[k]; if (d < 0) d = -d;
                    if (!(d <= TOL)) ok = 0;
                }
                if (!ok) continue;
                /* validate the whole chain before reporting */
                uint64_t owner = SafeReadPtr(b + o + 0x10);
                if (!IsHeapPtr(owner)) continue;
                uint64_t node = SafeReadPtr((void *)(owner + 0x18));
                if (!IsHeapPtr(node)) continue;
                float np[3];
                if (!SafeReadFloat3((void *)(node + 0x30), np)) continue;
                int nok = 1;
                for (int k = 0; k < 3; k++) {
                    float d = np[k] - t[k]; if (d < 0) d = -d;
                    if (!(d <= TOL)) nok = 0;
                }
                if (!nok) continue;

                RAppend(r, "  skel=%p owner=%p node=%p\n",
                        b + o, (void *)owner, (void *)node);
                RAppend(r, "     node+0x30 = %p (%.2f, %.2f, %.2f)\n",
                        (void *)(node + 0x30), np[0], np[1], np[2]);
                if (++found >= 12) goto done_fs;
            }
        }
        scan = next;
    }
done_fs:
    RAppend(r, "found %d\n", found);
}

/* exact 4-byte value search over committed RW memory */
static void CmdFind32(Resp *r, const char *line) {
    char vs[64] = {0};
    sscanf(line, "%*s %63s", vs);
    if (!vs[0]) { RAppend(r, "usage: find32 <hex32>\n"); return; }
    uint32_t want = (uint32_t)ParseHex(vs);

    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = (uint8_t *)0x1000000;
    int found = 0;
    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & PAGE_READWRITE) &&
            !(mbi.Protect & PAGE_GUARD))
        {
            uint8_t *b = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize;
            for (size_t o = 0; o + 4 <= sz; o += 4) {
                uint32_t v;
                memcpy(&v, b + o, 4);
                if (v != want) continue;
                RAppend(r, "  %p\n", b + o);
                if (++found >= 4000) goto done_f32;
            }
        }
        scan = next;
    }
done_f32:
    RAppend(r, "found %d\n", found);
}

/* ---- entity registry ---- */

/* RVAs, so these survive a relocated image. */
#define RVA_PLAYER_MGR   0x4BB6438
#define RVA_VT_ENTITY    0x39C6FC8
#define OFF_MGR_WORLD    0x98
#define OFF_WORLD_LIST   0xBA0
#define OFF_WORLD_COUNT  0xBA8
#define OFF_ENT_POS      0x50
#define OFF_SUB_OWNER    0x28
#define OFF_SUB_MAX      0xF0
#define SUB_WINDOW       0x200

static uint64_t ImgAddr(uint64_t rva) {
    return (uint64_t)(uintptr_t)g_imageBase + rva;
}

static uint64_t EntWorld(void) {
    uint64_t mgr = SafeReadPtr((void *)ImgAddr(RVA_PLAYER_MGR));
    if (!mgr) return 0;
    return SafeReadPtr((void *)(mgr + OFF_MGR_WORLD));
}

static int EntList(uint64_t *outList, uint32_t *outCount) {
    uint64_t w = EntWorld(), lst;
    uint16_t n = 0;

    if (!w) return 0;
    lst = SafeReadPtr((void *)(w + OFF_WORLD_LIST));
    if (!lst) return 0;
    if (!CanRead((void *)(w + OFF_WORLD_COUNT), 2)) return 0;
    memcpy(&n, (void *)(w + OFF_WORLD_COUNT), 2);
    if (!n || n > 4096) return 0;
    *outList = lst;
    *outCount = n;
    return 1;
}

/* Health is a sub component: it names the entity as its
 * owner and carries a plausible max. Class independent.
 */
static uint64_t EntHealth(uint64_t ent, uint32_t *outMax) {
    uint64_t arr;
    uint16_t n = 0, i;

    arr = SafeReadPtr((void *)(ent + OFF_ENT_COMPS));
    if (!arr || !CanRead((void *)(ent + OFF_ENT_NCOMPS), 2)) return 0;
    memcpy(&n, (void *)(ent + OFF_ENT_NCOMPS), 2);
    if (!n || n > 512) return 0;

    for (i = 0; i < n; i++) {
        uint64_t c = SafeReadPtr((void *)(arr + (uint64_t)i * 8));
        uint64_t o;
        if (!c || !CanRead((void *)c, SUB_WINDOW)) continue;
        for (o = 0; o + 8 <= SUB_WINDOW; o += 8) {
            uint64_t sub = SafeReadPtr((void *)(c + o));
            uint32_t mx;
            if (!sub || !CanRead((void *)sub, OFF_SUB_MAX + 4))
                continue;
            if (SafeReadPtr((void *)(sub + OFF_SUB_OWNER)) != ent)
                continue;
            memcpy(&mx, (void *)(sub + OFF_SUB_MAX), 4);
            if (mx < 1 || mx > 100000) continue;
            if (outMax) *outMax = mx;
            return sub;
        }
    }
    return 0;
}

static int EntPos(uint64_t ent, float *xyz) {
    if (!CanRead((void *)(ent + OFF_ENT_POS), 12)) return 0;
    memcpy(xyz, (void *)(ent + OFF_ENT_POS), 12);
    return 1;
}

/* Each entity carries one component whose CLASS is its
 * type, the net identity. Hashes, so no addresses.
 */
#define VT_GETDESC       0x30
#define OFF_DESC_HASH    0x24

/* All 51, generated by nettypes.py from the image. */
static const struct { uint32_t hash; const char *name; } g_kinds[] = {
    { 0xA69E4386u, "AlarmNet"              },
    { 0x7EDAAB74u, "BearTrap"              },
    { 0x5644C87Fu, "Codes"                 },
    { 0xD839E268u, "Destructible"          },
    { 0xD571F90Au, "DominationPoint"       },
    { 0x059648D5u, "Door"                  },
    { 0x5D621856u, "Drone"                 },
    { 0xEB9D9BC9u, "DroneJammer"           },
    { 0xFC1DFB0Du, "Dummy"                 },
    { 0x84C5EE35u, "DynamicEvent"          },
    { 0xB22CC0B5u, "EMPBlast"              },
    { 0xCAE95025u, "ElectricDevice"        },
    { 0xB6E07575u, "ExtractionPoint"       },
    { 0x7B8D1BCDu, "Flare"                 },
    { 0xD19EB2D4u, "FlareGun"              },
    { 0x19362348u, "HackedServer"          },
    { 0x1F4B1328u, "InteractiveElement"    },
    { 0x4029E6AEu, "LightPanel"            },
    { 0xCC586D4Au, "LootChest"             },
    { 0xA23FCD3Du, "MagneticSensorGrenade" },
    { 0x7C1D8BE9u, "MercenaryAmmo"         },
    { 0x86EB450Au, "MercenaryAttachment"   },
    { 0x8D7F33FCu, "MercenaryCountable"    },
    { 0x8C89D0DDu, "MercenaryItem"         },
    { 0x35A02C34u, "MercenaryLoot"         },
    { 0x4433B66Au, "MercenaryWeapon"       },
    { 0xE6DBCF6Bu, "Mine"                  },
    { 0x2F0FA0B5u, "MineAdv"               },
    { 0x868D075Fu, "Mortar"                },
    { 0xFEE4759Au, "NPC"                   },
    { 0xDA86CE85u, "OrdersBeacon"          },
    { 0x04877609u, "PVPHostage"            },
    { 0xCA10AECDu, "PVPPrisonCell"         },
    { 0x507DC2ECu, "Player"                },
    { 0xF9789C28u, "PlayerLoot"            },
    { 0xE162A6A3u, "RCED"                  },
    { 0xB631BC8Cu, "RCEDAdv"               },
    { 0xD3D3F022u, "RemoteJammer"          },
    { 0x8FE59CC6u, "ResourceDrop"          },
    { 0x3D328332u, "ResupplyDrop"          },
    { 0x9DE70FD6u, "SAMLauncher"           },
    { 0x8211F572u, "SatComTracker"         },
    { 0x84299CADu, "SearchLight"           },
    { 0xEF953B76u, "Teammate"              },
    { 0x3591E249u, "Throwable"             },
    { 0xAF802229u, "Tool"                  },
    { 0x5E2209ADu, "Tripwire"              },
    { 0x0186C7ACu, "Turret"                },
    { 0x8F2CBBBAu, "Vehicle"               },
    { 0xF033F1E9u, "WeaponDrop"            },
    { 0xB6BC2554u, "WorldEvent"            },
};

/* vtable +0x30 is a thunk to a getter returning the
 * class descriptor, whose +0x24 is the class hash.
 */
static uint32_t ClassHashOf(uint64_t obj) {
    uint64_t vt, thunk, tgt, desc;
    uint8_t b[16];
    int32_t rel, disp;
    uint32_t hash = 0;
    int i;

    if (!CanRead((void *)obj, 8)) return 0;
    vt = SafeReadPtr((void *)obj);
    if (!CanRead((void *)(vt + VT_GETDESC), 8)) return 0;
    thunk = SafeReadPtr((void *)(vt + VT_GETDESC));
    if (!CanRead((void *)thunk, 8)) return 0;
    memcpy(b, (void *)thunk, 8);
    if (b[0] != 0xE9) return 0;
    memcpy(&rel, b + 1, 4);
    tgt = thunk + 5 + rel;
    if (!CanRead((void *)tgt, 16)) return 0;
    memcpy(b, (void *)tgt, 16);
    /* mov rax, [rip+disp]; ret */
    for (i = 0; i + 7 <= 16; i++) {
        if (b[i] == 0x48 && b[i + 1] == 0x8B && b[i + 2] == 0x05) {
            memcpy(&disp, b + i + 3, 4);
            desc = SafeReadPtr((void *)(tgt + i + 7 + disp));
            if (CanRead((void *)(desc + OFF_DESC_HASH), 4))
                memcpy(&hash, (void *)(desc + OFF_DESC_HASH), 4);
            return hash;
        }
    }
    return 0;
}

static const char *EntKind(uint64_t ent) {
    uint64_t arr;
    uint16_t n = 0, i;
    size_t k;

    arr = SafeReadPtr((void *)(ent + OFF_ENT_COMPS));
    if (!arr || !CanRead((void *)(ent + OFF_ENT_NCOMPS), 2)) return NULL;
    memcpy(&n, (void *)(ent + OFF_ENT_NCOMPS), 2);
    if (!n || n > 512) return NULL;

    for (i = 0; i < n; i++) {
        uint64_t c = SafeReadPtr((void *)(arr + (uint64_t)i * 8));
        uint32_t h;
        if (!c) continue;
        h = ClassHashOf(c);
        if (!h) continue;
        for (k = 0; k < sizeof(g_kinds) / sizeof(g_kinds[0]); k++)
            if (g_kinds[k].hash == h) return g_kinds[k].name;
    }
    return NULL;
}

static int EntIsEntity(uint64_t e) {
    if (e < 0x1000000ULL || e > 0x800000000000ULL) return 0;
    if (!CanRead((void *)e, 8)) return 0;
    return SafeReadPtr((void *)e) == ImgAddr(RVA_VT_ENTITY);
}

/* ents [radius] [all] */
static void CmdEnts(Resp *r, const char *line) {
    char opt[32] = {0};
    double radius = 100.0;
    uint64_t lst = 0, me = 0;
    uint32_t cnt = 0, i, shown = 0, withhp = 0;
    float mp[3] = {0, 0, 0};
    int haveMe = 0, all;

    sscanf(line, "%*s %lf %31s", &radius, opt);
    all = (strcmp(opt, "all") == 0);

    if (!EntList(&lst, &cnt)) {
        RAppend(r, "no entity list, are you in game\n");
        return;
    }
    (void)me;
    {
        uint8_t *pp = PlayerPosPtr();
        if (pp && CanRead(pp, 12)) {
            memcpy(mp, pp, 12);
            haveMe = 1;
        }
    }

    RAppend(r, "world list %p count %u, radius %.0f%s\n",
            (void *)lst, cnt, radius, all ? " (all)" : "");

    for (i = 0; i < cnt; i++) {
        uint64_t e = SafeReadPtr((void *)(lst + (uint64_t)i * 8));
        uint64_t hp;
        uint32_t mx = 0;
        float p[3];
        double d = 0.0;

        if (!EntIsEntity(e)) continue;
        if (!EntPos(e, p)) continue;
        if (haveMe) {
            double dx = p[0] - mp[0], dy = p[1] - mp[1];
            double dz = p[2] - mp[2];
            d = sqrt(dx * dx + dy * dy + dz * dz);
            if (d > radius) continue;
        }
        {
            const char *kind = EntKind(e);
            hp = EntHealth(e, &mx);
            if (hp) withhp++;
            if (!all && !kind) continue;

            RAppend(r, "  %7.1fm [%3u] %p  %-16s hp %-6u"
                       " pos %.0f %.0f %.0f\n",
                    d, i, (void *)e, kind ? kind : "-",
                    mx, p[0], p[1], p[2]);
            shown++;
        }
    }
    RAppend(r, "shown %u, with health %u\n", shown, withhp);
}

/* enthp <entity>, enttp <entity> <x> <y> <z> */
static void CmdEntHp(Resp *r, const char *line) {
    char es[64] = {0};
    uint64_t e, hp;
    uint32_t mx = 0;

    if (sscanf(line, "%*s %63s", es) != 1) {
        RAppend(r, "usage: enthp <entity>\n");
        return;
    }
    e = ParseHex(es);
    if (!EntIsEntity(e)) {
        RAppend(r, "%p is not an entity\n", (void *)e);
        return;
    }
    {
        const char *kind = EntKind(e);
        RAppend(r, "%p kind %s\n", (void *)e, kind ? kind : "unknown");
    }
    hp = EntHealth(e, &mx);
    if (!hp) {
        RAppend(r, "  no health sub component\n");
        return;
    }
    RAppend(r, "  health sub %p max %u\n", (void *)hp, mx);
}

/* exact 8-byte value search over committed RW memory */
static void CmdFindPtr(Resp *r, const char *line) {
    char vs[64] = {0}, os[64] = {0};
    sscanf(line, "%*s %63s %63s", vs, os);
    if (!vs[0]) {
        RAppend(r, "usage: findptr <hex64> [obj_offset]\n");
        return;
    }
    uint64_t want = ParseHex(vs);
    int64_t adj = os[0] ? (int64_t)ParseHex(os) : 0;

    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = NULL;
    int found = 0;
    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;
        /* Any readable page. Image data is WRITECOPY
         * under Wine, so an RW only filter skips it.
         */
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                            PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY)) &&
            !(mbi.Protect & PAGE_GUARD))
        {
            uint8_t *b = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize;
            for (size_t o = 0; o + 8 <= sz; o += 8) {
                uint64_t v;
                memcpy(&v, b + o, 8);
                if (v != want) continue;
                uint64_t owner = (uint64_t)(b + o - adj);
                uint64_t sub = SafeReadPtr((void *)(owner + 0x240));
                uint64_t svt = sub ? SafeReadPtr((void *)sub) : 0;
                if (svt >= SH_IMG(0x3B7D000) && svt < SH_IMG(0x3B7E000))
                    RAppend(r, "  at=%p obj=%p  LOCO=%p vt=%p\n",
                            b + o, (void *)owner,
                            (void *)sub, (void *)svt);
                else
                    RAppend(r, "  at=%p obj=%p\n", b + o, (void *)owner);
                found++;
                if (found >= 4000) goto done_fp2;
            }
        }
        scan = next;
    }
done_fp2:
    RAppend(r, "found %d\n", found);
}

/* [p+8]=vtable, [vt+190]=code, [p+50]=heap ptr */
static void CmdFindLoco(Resp *r) {
    RAppend(r, "scanning for objects with vtable+0x190...\n");
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = NULL;
    int found = 0;

    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;

        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD))
        {
            uint8_t *base = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize;
            for (size_t off = 0; off + 0x58 <= sz; off += 8) {
                uint64_t vt = SafeReadPtr(base + off + 8);
                if (!IsImagePtr(vt)) continue;
                uint64_t fn = SafeReadPtr((void *)(vt + 0x190));
                if (!IsImagePtr(fn)) continue;
                uint64_t p50 = SafeReadPtr(base + off + 0x50);
                if (!IsHeapPtr(p50)) continue;
                RAppend(r, "  obj=%p vt=%p [vt+190]=%p [+50]=%p\n",
                        base + off, (void *)vt, (void *)fn, (void *)p50);
                found++;
                if (found >= 40) goto done_loco;
            }
        }
        scan = next;
    }
done_loco:
    RAppend(r, "found %d candidates\n", found);
}

/* ---- entity chain search ---- */

static void CmdFindPlayer(Resp *r) {
    RAppend(r, "searching for entity list using CT chain pattern...\n");
    RAppend(r, "chain: global -> [ptr+50] -> [i*8] -> +10 -> "
            "+E0 -> +10C0 -> +50 = XYZ\n\n");

    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = NULL;
    int found = 0;

    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;

        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD) &&
            mbi.RegionSize >= 0x100)
        {
            uint8_t *base = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize;

            for (size_t off = 0; off + 8 <= sz; off += 8) {
                uint64_t val = SafeReadPtr(base + off);
                if (val < 0x1000000 || val > 0x7FFFFFFFFFFF)
                    continue;

                uint64_t arr = SafeReadPtr((void *)(val + 0x50));
                if (arr < 0x1000000 || arr > 0x7FFFFFFFFFFF)
                    continue;

                for (int i = 0; i < 8; i++) {
                    uint64_t elem = SafeReadPtr((void *)(arr + i * 8));
                    if (!elem) continue;

                    uint64_t p1 = SafeReadPtr((void *)(elem + 0x10));
                    if (!p1) continue;

                    uint64_t p2 = SafeReadPtr((void *)(p1 + 0xE0));
                    if (!p2) continue;

                    uint64_t p3 = SafeReadPtr((void *)(p2 + 0x10C0));
                    if (!p3) continue;

                    float pos[3];
                    if (!SafeReadFloat3((void *)(p3 + 0x50), pos))
                        continue;

                    if (pos[0] > 100 && pos[0] < 40000 &&
                        pos[1] > 100 && pos[1] < 40000 &&
                        pos[2] > -5000 && pos[2] < 40000)
                    {
                        RAppend(r, "HIT at %p: list=%p\n",
                                base + off, (void *)val);
                        RAppend(r, "  [+50]=%p -> [%d]=%p\n",
                                (void *)arr, i, (void *)elem);
                        RAppend(r, "  +10=%p +E0=%p +10C0=%p\n",
                                (void *)p1, (void *)p2, (void *)p3);
                        RAppend(r, "  +50 pos = (%.2f, %.2f, %.2f)\n",
                                pos[0], pos[1], pos[2]);
                        found++;
                        if (found >= 10) goto done_fp;
                    }
                }
            }
        }
        scan = next;
    }
done_fp:
    RAppend(r, "found %d matches\n", found);
}

/* ---- player symbol resolution ---- */

#define PLAYER_TF_VTABLE SH_IMG(0x39EA9F8)
#define PLAYER_POS_OFF   0x90

static uint8_t *g_playerTf = NULL;

/* a real player tf has a sane world position and W == 1 */
static int PosSane(uint8_t *p) {
    if (!CanRead(p + PLAYER_POS_OFF, 16)) return 0;
    float v[4];
    memcpy(v, p + PLAYER_POS_OFF, 16);
    if (v[3] < 0.99f || v[3] > 1.01f) return 0;
    for (int i = 0; i < 3; i++) {
        if (v[i] != v[i]) return 0;
        float m = v[i] < 0 ? -v[i] : v[i];
        if (m > 100000.0f) return 0;
    }
    return 1;
}

static int TfValid(uint8_t *p) {
    return p && CanRead(p, 8) &&
           SafeReadPtr(p) == PLAYER_TF_VTABLE &&
           PosSane(p);
}

static uint8_t *ResolvePlayer(int force) {
    if (!force && TfValid(g_playerTf)) return g_playerTf;
    g_playerTf = NULL;

    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = (uint8_t *)0x1000000;
    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;
        /* Any readable page. Image data is WRITECOPY
         * under Wine, so an RW only filter skips it.
         */
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                            PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY)) &&
            !(mbi.Protect & PAGE_GUARD))
        {
            uint8_t *b = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize;
            for (size_t o = 0; o + 8 <= sz; o += 8) {
                uint64_t v;
                memcpy(&v, b + o, 8);
                if (v != PLAYER_TF_VTABLE) continue;
                if (!PosSane(b + o)) continue;
                g_playerTf = b + o;
                return g_playerTf;
            }
        }
        scan = next;
    }
    return NULL;
}

static uint8_t *PlayerPosPtr(void) {
    uint8_t *tf = ResolvePlayer(0);
    return tf ? tf + PLAYER_POS_OFF : NULL;
}

static void CmdPos(Resp *r) {
    uint8_t *p = PlayerPosPtr();
    if (!p) { RAppend(r, "player not found\n"); return; }
    float v[4];
    memcpy(v, p, 16);
    RAppend(r, "player tf  = %p\n", g_playerTf);
    RAppend(r, "player pos = %p\n", p);
    RAppend(r, "  X %.3f  Y %.3f  Z %.3f  W %.1f\n",
            v[0], v[1], v[2], v[3]);
}

static void CmdTp(Resp *r, const char *line) {
    float x, y, z;
    if (sscanf(line, "%*s %f %f %f", &x, &y, &z) != 3) {
        RAppend(r, "usage: tp <x> <y> <z>\n");
        return;
    }
    uint8_t *p = PlayerPosPtr();
    if (!p) { RAppend(r, "player not found\n"); return; }
    float old[3];
    memcpy(old, p, 12);
    float nv[3] = {x, y, z};
    memcpy(p, nv, 12);
    RAppend(r, "pos %p: (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f)\n",
            p, old[0], old[1], old[2], x, y, z);
}

/* ---- on-screen coordinate overlay ---- */

static HWND      g_ovlWnd = NULL;
static uint8_t  *g_ovlAddr = NULL;
static HANDLE    g_ovlThread = NULL;

/* Entity mode: label what you are looking at, sorted by
 * how close it is to the middle of the view.
 */
static volatile int   g_ovlEnts = 0;
static volatile float g_ovlRadius = 60.0f;
static int g_ovlW = 1920, g_ovlH = 1080;

/* Camera basis rows and projection scales, tunable at
 * runtime so the mapping can be fixed without a rebuild.
 */
#define RVA_CAM_GLOBAL   0x4BC3358
#define OFF_CAM_BASIS    0x90
#define OFF_CAM_POS      0xC0
#define OFF_CAM_PROJ     0xD0

static int CamView(float *pos, float *fwd) {
    uint64_t cam = SafeReadPtr((void *)ImgAddr(RVA_CAM_GLOBAL));
    if (!cam) return 0;
    if (!CanRead((void *)(cam + OFF_CAM_BASIS), 0x50)) return 0;
    memcpy(fwd, (void *)(cam + OFF_CAM_BASIS + 0x10), 12);
    memcpy(pos, (void *)(cam + OFF_CAM_POS), 12);
    return 1;
}

static float Dot3(const float *a, const float *b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* Readable list, nearest the middle of the view first. */
static void PaintEnts(HDC dc) {
    struct { float ang, d; uint64_t e; } best[16];
    uint64_t lst = 0;
    uint32_t cnt = 0, i;
    float cp[3], cf[3];
    char buf[220];
    int n = 0, k, y = 10;

    if (!CamView(cp, cf) || !EntList(&lst, &cnt)) {
        SetTextColor(dc, RGB(255, 60, 60));
        TextOutA(dc, 10, 10, "no camera or entity list", 24);
        return;
    }
    for (i = 0; i < cnt; i++) {
        uint64_t e = SafeReadPtr((void *)(lst + (uint64_t)i * 8));
        float p[3], rel[3], d, dot, ang;
        int j;

        if (!EntIsEntity(e) || !EntPos(e, p)) continue;
        rel[0] = p[0] - cp[0];
        rel[1] = p[1] - cp[1];
        rel[2] = p[2] - cp[2];
        d = sqrtf(Dot3(rel, rel));
        if (d < 0.4f || d > g_ovlRadius) continue;

        dot = Dot3(rel, cf) / d;
        if (dot > 1.0f) dot = 1.0f;
        if (dot < -1.0f) dot = -1.0f;
        ang = acosf(dot) * 57.2957795f;
        if (ang > 45.0f) continue;

        for (j = n; j > 0 && best[j - 1].ang > ang; j--) {
            if (j < 16) best[j] = best[j - 1];
        }
        if (j < 16) {
            best[j].ang = ang;
            best[j].d = d;
            best[j].e = e;
            if (n < 16) n++;
        }
    }

    snprintf(buf, sizeof(buf), "looking at, %d within %.0fm",
             n, g_ovlRadius);
    SetTextColor(dc, RGB(0, 190, 0));
    TextOutA(dc, 10, y, buf, (int)strlen(buf));
    y += 24;

    for (k = 0; k < n; k++) {
        const char *kind = EntKind(best[k].e);
        uint32_t mx = 0;
        EntHealth(best[k].e, &mx);
        snprintf(buf, sizeof(buf),
                 "%2.0fdeg %6.1fm  %p  %-16s hp %u",
                 best[k].ang, best[k].d, (void *)best[k].e,
                 kind ? kind : "-", mx);
        SetTextColor(dc, k == 0 ? RGB(255, 255, 0)
                                : RGB(0, 210, 0));
        TextOutA(dc, 10, y, buf, (int)strlen(buf));
        y += 20;
    }
}

static LRESULT CALLBACK OvlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);

        if (g_ovlEnts) {
            PaintEnts(dc);
            EndPaint(h, &ps);
            return 0;
        }

        char buf[160];
        float v[3] = {0, 0, 0};
        if (g_ovlAddr && CanRead(g_ovlAddr, 12))
            memcpy(v, g_ovlAddr, 12);
        snprintf(buf, sizeof(buf),
                 "X %.2f   Y %.2f   Z %.2f", v[0], v[1], v[2]);

        SetTextColor(dc, RGB(0, 255, 0));
        TextOutA(dc, 8, 6, buf, (int)strlen(buf));

        snprintf(buf, sizeof(buf), "GRW ScriptHook  %p", g_ovlAddr);
        SetTextColor(dc, RGB(0, 160, 0));
        TextOutA(dc, 8, 26, buf, (int)strlen(buf));

        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_TIMER) {
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    if (m == WM_DESTROY) {
        g_ovlWnd = NULL;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI OvlThread(LPVOID p) {
    (void)p;
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = OvlProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "GRWScriptHookOverlay";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    /* Same flags as the version that worked. Only the size
     * grows, so labels can sit where the objects are.
     */
    g_ovlW = GetSystemMetrics(SM_CXSCREEN);
    g_ovlH = GetSystemMetrics(SM_CYSCREEN);
    if (g_ovlW < 640) g_ovlW = 1920;
    if (g_ovlH < 480) g_ovlH = 1080;

    g_ovlWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        "GRWScriptHookOverlay", "GRW", WS_POPUP,
        12, 12, 620, 400,
        NULL, NULL, wc.hInstance, NULL);
    if (!g_ovlWnd) return 1;

    ShowWindow(g_ovlWnd, SW_SHOWNOACTIVATE);
    SetTimer(g_ovlWnd, 1, 100, NULL);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

static void CmdOverlay(Resp *r, const char *line) {
    char as[64] = {0};
    sscanf(line, "%*s %63s", as);
    if (as[0]) {
        g_ovlAddr = (uint8_t *)ParseHex(as);
    } else {
        g_ovlAddr = PlayerPosPtr();
        if (!g_ovlAddr) { RAppend(r, "player not found\n"); return; }
    }
    if (!g_ovlThread)
        g_ovlThread = CreateThread(NULL, 0, OvlThread, NULL, 0, NULL);
    RAppend(r, "overlay showing %p\n", g_ovlAddr);
}

/* entmark [radius], lists what you are looking at. */
static void CmdEntMark(Resp *r, const char *line) {
    double rad = 40.0;
    char off[16] = {0};

    if (sscanf(line, "%*s %15s", off) == 1
        && strcmp(off, "off") == 0) {
        g_ovlEnts = 0;
        RAppend(r, "entity overlay off\n");
        return;
    }
    /* entmark axes <right> <up> <fwd> <sr> <su> <sf> to fix
     * the basis mapping live, no rebuild needed.
     */
    sscanf(line, "%*s %lf", &rad);
    if (rad > 1.0) g_ovlRadius = (float)rad;
    g_ovlEnts = 1;
    if (!g_ovlThread)
        g_ovlThread = CreateThread(NULL, 0, OvlThread, NULL, 0, NULL);
    RAppend(r, "entity overlay on, radius %.0f, 35 degree cone\n",
            g_ovlRadius);
}

static void CmdOverlayOff(Resp *r) {
    if (g_ovlWnd) {
        PostMessageA(g_ovlWnd, WM_CLOSE, 0, 0);
        RAppend(r, "overlay closed\n");
    } else {
        RAppend(r, "no overlay\n");
    }
    g_ovlThread = NULL;
}

/* ---- dispatch ---- */

/* Bulk read, raw hex. One command instead of 128 reads. */
#define READHEX_MAX (512 * 1024)

static void CmdReadHex(Resp *r, const char *line) {
    char as[64] = {0}, ls[64] = {0};
    static const char hx[] = "0123456789abcdef";
    uint8_t *buf;
    uint64_t addr;
    size_t len, i;
    SIZE_T got = 0;

    if (sscanf(line, "%*s %63s %63s", as, ls) != 2) {
        RAppend(r, "usage: readhex <hex> <len>\n");
        return;
    }
    addr = ParseHex(as);
    len = (size_t)ParseHex(ls);
    if (len > READHEX_MAX) len = READHEX_MAX;
    buf = (uint8_t *)VirtualAlloc(NULL, len, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!buf) { RAppend(r, "no memory\n"); return; }
    if (!CanRead((void *)(uintptr_t)addr, len) ||
        !ReadProcessMemory(GetCurrentProcess(),
                           (void *)(uintptr_t)addr, buf, len, &got)) {
        got = 0;
    }
    RAppend(r, "hex %p %u\n", (void *)(uintptr_t)addr, (unsigned)got);
    if (got && r->len + got * 2 + 2 < (size_t)r->cap) {
        char *out = r->buf + r->len;
        for (i = 0; i < got; i++) {
            out[i * 2] = hx[buf[i] >> 4];
            out[i * 2 + 1] = hx[buf[i] & 15];
        }
        out[got * 2] = '\n';
        r->len += (int)(got * 2 + 1);
    }
    VirtualFree(buf, 0, MEM_RELEASE);
}

/* Masked qword search over a range, in process. */
static void CmdFindQ(Resp *r, const char *line) {
    char ss[64] = {0}, ls[64] = {0}, vs[64] = {0}, ms[64] = {0};
    char st[64] = {0}, mx[64] = {0};
    uint64_t start, len, val, mask, stride = 8, maxhits = 2000;
    uint64_t a, end, found = 0;
    uint8_t page[4096];
    SIZE_T got;

    if (sscanf(line, "%*s %63s %63s %63s %63s %63s %63s",
               ss, ls, vs, ms, st, mx) < 4) {
        RAppend(r, "usage: findq <start> <len> <val> <mask> "
                   "[stride] [max]\n");
        return;
    }
    start = ParseHex(ss); len = ParseHex(ls);
    val = ParseHex(vs); mask = ParseHex(ms);
    if (st[0]) stride = ParseHex(st);
    if (mx[0]) maxhits = ParseHex(mx);
    if (!stride || stride > 4096) stride = 8;
    end = start + len;
    for (a = start; a + 8 <= end && found < maxhits; a += sizeof(page)) {
        size_t n = sizeof(page), o;
        if (a + n > end) n = (size_t)(end - a);
        if (!ReadProcessMemory(GetCurrentProcess(),
                               (void *)(uintptr_t)a, page, n, &got) ||
            got < 8)
            continue;
        for (o = 0; o + 8 <= got && found < maxhits; o += stride) {
            uint64_t q;
            memcpy(&q, page + o, 8);
            if ((q & mask) == val) {
                RAppend(r, "  %p %016llx\n", (void *)(uintptr_t)(a + o),
                        (unsigned long long)q);
                found++;
            }
        }
    }
    RAppend(r, "found %llu\n", (unsigned long long)found);
}

static void Dispatch(const char *line, Resp *r) {
    RefreshRanges();
    while (*line == ' ') line++;
    char cmd[64] = {0};
    char arg1[512] = {0};
    char arg2[64] = {0};
    sscanf(line, "%63s %511s %63s", cmd, arg1, arg2);

    if (strcmp(cmd, "help") == 0)           CmdHelp(r);
    else if (strcmp(cmd, "crash") == 0)     CmdCrash(r, line);
    else if (strcmp(cmd, "base") == 0)      CmdBase(r);
    else if (strcmp(cmd, "sections") == 0)  CmdSections(r);
    else if (strcmp(cmd, "scan") == 0) {
        const char *rest = line + 4;
        while (*rest == ' ') rest++;
        CmdScan(r, rest);
    }
    else if (strcmp(cmd, "scanhex") == 0)   CmdScanHex(r, arg1);
    else if (strcmp(cmd, "patch") == 0)     CmdPatch(r, line);
    else if (strcmp(cmd, "unpatch") == 0)   CmdUnpatch(r);
    else if (strcmp(cmd, "xref") == 0)      CmdXref(r, arg1);
    else if (strcmp(cmd, "read") == 0) {
        int len = arg2[0] ? (int)ParseHex(arg2) : 128;
        CmdRead(r, (uint8_t *)ParseHex(arg1), len);
    }
    else if (strcmp(cmd, "readhex") == 0)   CmdReadHex(r, line);
    else if (strcmp(cmd, "findq") == 0)     CmdFindQ(r, line);
    else if (strcmp(cmd, "readstr") == 0)   CmdReadStr(r, (uint8_t *)ParseHex(arg1));
    else if (strcmp(cmd, "dis") == 0) {
        int len = arg2[0] ? (int)ParseHex(arg2) : 64;
        CmdDis(r, (uint8_t *)ParseHex(arg1), len);
    }
    else if (strcmp(cmd, "strings") == 0)   CmdStrings(r, (uint8_t *)ParseHex(arg1), (size_t)ParseHex(arg2));
    else if (strcmp(cmd, "silexlist") == 0)  CmdSilexList(r);
    else if (strcmp(cmd, "scanfloat") == 0) CmdScanFloat(r, arg1, arg2);
    else if (strcmp(cmd, "findent") == 0)   CmdFindEnt(r);
    else if (strcmp(cmd, "tpshift") == 0)   CmdTpShift(r, line);
    else if (strcmp(cmd, "scanvec3d") == 0) CmdScanVec3d(r, line);
    else if (strcmp(cmd, "writedouble") == 0) CmdWriteDouble(r, line);
    else if (strcmp(cmd, "scanvec3") == 0) {
        char a3[64] = {0}, lo[64] = {0}, hi[64] = {0};
        sscanf(line, "%*s %63s %63s %63s %63s %63s",
               arg1, arg2, a3, lo, hi);
        CmdScanFloat3(r, arg1, arg2, a3,
                      lo[0] ? ParseHex(lo) : 0,
                      hi[0] ? ParseHex(hi) : 0);
    }
    else if (strcmp(cmd, "writefloat") == 0) CmdWriteFloat(r, arg1, arg2);
    else if (strcmp(cmd, "findplayer") == 0) CmdFindPlayer(r);
    else if (strcmp(cmd, "findloco") == 0)  CmdFindLoco(r);
    else if (strcmp(cmd, "findptr") == 0)   CmdFindPtr(r, line);
    else if (strcmp(cmd, "find32") == 0)    CmdFind32(r, line);
    else if (strcmp(cmd, "findskel") == 0)  CmdFindSkel(r);
    else if (strcmp(cmd, "tree") == 0)      CmdTree(r, line);
    else if (strcmp(cmd, "fhook") == 0)     CmdFHook(r, line);
    else if (strcmp(cmd, "fhookoff") == 0)  CmdFHookOff(r);
    else if (strcmp(cmd, "fpos") == 0)      CmdFPos(r, line);
    else if (strcmp(cmd, "foff") == 0)      CmdFOff(r);
    else if (strcmp(cmd, "thook") == 0)     CmdThook(r, line);
    else if (strcmp(cmd, "unthook") == 0)   CmdUnthook(r);
    else if (strcmp(cmd, "hookcb") == 0)    CmdHookCb(r, line);
    else if (strcmp(cmd, "raysnap") == 0)   CmdRaySnap(r, line);
    else if (strcmp(cmd, "rayinfo") == 0)   CmdRayInfo(r);
    else if (strcmp(cmd, "rayoff") == 0)    CmdRayOff(r);
    else if (strcmp(cmd, "raycast") == 0)   CmdRayCast(r, line);
    else if (strcmp(cmd, "rayres") == 0)    CmdRayRes(r);
    else if (strcmp(cmd, "raypin") == 0)    CmdRayPin(r, line);
    else if (strcmp(cmd, "rayset") == 0)    CmdRaySet(r, line);
    else if (strcmp(cmd, "ground") == 0)    CmdGround(r, line);
    else if (strcmp(cmd, "groundres") == 0) CmdGroundRes(r);
    else if (strcmp(cmd, "groundhere") == 0) CmdGroundHere(r, line);
    else if (strcmp(cmd, "castlive") == 0)  CmdCastLive(r, line);
    else if (strcmp(cmd, "castray") == 0)   CmdCastRay(r, line);
    else if (strcmp(cmd, "spawn") == 0)     CmdSpawn(r, line);
    else if (strcmp(cmd, "spawnveh") == 0)  CmdSpawnVeh(r, line);
    else if (strcmp(cmd, "vehlist") == 0)   CmdVehList(r, line);
    else if (strcmp(cmd, "comps") == 0)     CmdComps(r, line);
    else if (strcmp(cmd, "raylog") == 0)    CmdRayLog(r, line);
    else if (strcmp(cmd, "onhit") == 0)     CmdOnHit(r, line);
    else if (strcmp(cmd, "cam") == 0)       CmdCam(r, line);
    else if (strcmp(cmd, "vis") == 0)       CmdVis(r, line);
    else if (strcmp(cmd, "onfire") == 0)    CmdOnFire(r, line);
    else if (strcmp(cmd, "createcomp") == 0) CmdCreateComp(r, line);
    else if (strcmp(cmd, "writeq") == 0)    CmdWriteQ(r, line);
    else if (strcmp(cmd, "writebytes") == 0) CmdWriteBytes(r, line);
    else if (strcmp(cmd, "alloc") == 0)     CmdAlloc(r, line);
    else if (strcmp(cmd, "chase") == 0)     CmdChase(r, line);
    else if (strcmp(cmd, "ui") == 0)        CmdUi(r, line);
    else if (strcmp(cmd, "ents") == 0)      CmdEnts(r, line);
    else if (strcmp(cmd, "entmark") == 0)   CmdEntMark(r, line);
    else if (strcmp(cmd, "enthp") == 0)     CmdEntHp(r, line);
    else if (strcmp(cmd, "fault") == 0)     CmdFault(r, line);
    else if (strcmp(cmd, "api") == 0)       CmdApi(r);
    else if (strcmp(cmd, "apifind") == 0)   CmdApiFind(r, line);
    else if (strcmp(cmd, "apikind") == 0)   CmdApiKind(r, line);
    else if (strcmp(cmd, "apitp") == 0)     CmdApiTp(r, line);
    else if (strcmp(cmd, "apihop") == 0)    CmdApiHop(r, line);
    else if (strcmp(cmd, "apiground") == 0) CmdApiTpGround(r, line);
    else if (strcmp(cmd, "apiheight") == 0) CmdApiHeight(r, line);
    else if (strcmp(cmd, "apiplace") == 0)  CmdApiPlace(r, line);
    else if (strcmp(cmd, "apihp") == 0)     CmdApiHp(r, line);
    else if (strcmp(cmd, "hpscan") == 0)    CmdHpScan(r, line);
    else if (strcmp(cmd, "hpread") == 0)    CmdHpRead(r, line);
    else if (strcmp(cmd, "hpwrite") == 0)   CmdHpWrite(r, line);
    else if (strcmp(cmd, "castres") == 0)   CmdCastRes(r);
    else if (strcmp(cmd, "groundfn") == 0)  CmdGroundFn(r, line);
    else if (strcmp(cmd, "unhookcb") == 0)  CmdUnhookCb(r);
    else if (strcmp(cmd, "tpnode") == 0)    CmdTpNode(r, line);
    else if (strcmp(cmd, "tpset") == 0)     CmdTpSet(r, line);
    else if (strcmp(cmd, "tpoff") == 0)     CmdTpOff(r);
    else if (strcmp(cmd, "gcall") == 0)     CmdGCall(r, line);
    else if (strcmp(cmd, "gcallf") == 0)    CmdGCallF(r, line);
    else if (strcmp(cmd, "gcall6") == 0)    CmdGCall6(r, line);
    else if (strcmp(cmd, "placerot") == 0)  CmdPlaceRot(r, line);
    else if (strcmp(cmd, "chaos") == 0)     CmdChaos(r, line);
    else if (strcmp(cmd, "havok") == 0)     CmdHavok(r, line);
    else if (strcmp(cmd, "gstat") == 0)     CmdGStat(r);
    else if (strcmp(cmd, "wndhook") == 0)   CmdWndHook(r, line);
    else if (strcmp(cmd, "wndoff") == 0)    CmdWndOff(r);
    else if (strcmp(cmd, "wndstat") == 0)   CmdWndStat(r);
    else if (strcmp(cmd, "mkmtx") == 0)     CmdMkMtx(r, line);
    else if (strcmp(cmd, "tpstat") == 0)    CmdTpStat(r);
    else if (strcmp(cmd, "xcall") == 0)     CmdXCall(r, arg1);
    else if (strcmp(cmd, "fsinit") == 0)    CmdFsInit(r, line);
    else if (strcmp(cmd, "fschanged") == 0) CmdFsFilter(r, 1);
    else if (strcmp(cmd, "fssame") == 0)    CmdFsFilter(r, 0);
    else if (strcmp(cmd, "fslist") == 0)    CmdFsList(r, line);
    else if (strcmp(cmd, "fsrange") == 0)   CmdFsRange(r, line);
    else if (strcmp(cmd, "fsin") == 0)      CmdFsIn(r, line);
    else if (strcmp(cmd, "fsvec") == 0)     CmdFsVec(r);
    else if (strcmp(cmd, "fssave") == 0)    CmdFsSave(r, line);
    else if (strcmp(cmd, "fsload") == 0)    CmdFsLoad(r, line);
    else if (strcmp(cmd, "hwbp") == 0)      CmdHwbp(r, line);
    else if (strcmp(cmd, "hwbpinfo") == 0)  CmdHwbpInfo(r);
    else if (strcmp(cmd, "hwbpoff") == 0)   CmdHwbpOff(r);
    else if (strcmp(cmd, "pos") == 0)       CmdPos(r);
    else if (strcmp(cmd, "tp") == 0)        CmdTp(r, line);
    else if (strcmp(cmd, "overlay") == 0)   CmdOverlay(r, line);
    else if (strcmp(cmd, "overlayoff") == 0) CmdOverlayOff(r);
    else if (strcmp(cmd, "tpov") == 0)      CmdTpOv(r, line);
    else if (strcmp(cmd, "tpovoff") == 0)   CmdTpOvOff(r);
    else if (strcmp(cmd, "hook") == 0)      CmdHook(r, line);
    else if (strcmp(cmd, "hookinfo") == 0)  CmdHookInfo(r);
    else if (strcmp(cmd, "unhook") == 0)    CmdUnhook(r);
    else if (strcmp(cmd, "scratch") == 0)   CmdScratch(r);
    else if (strcmp(cmd, "setvec") == 0)    CmdSetVec(r, line);
    else if (strcmp(cmd, "call") == 0)      CmdCall(r, line);
    else if (strcmp(cmd, "gcallwnd") == 0)  CmdGCallWnd(r, line);
    else RAppend(r, "unknown command: %s (try 'help')\n", cmd);
}

/* Commands run on a disposable worker with a deadline, so
 * one that blocks on a frozen thread's locks costs its
 * worker, never the REPL itself. */
typedef struct {
    char line[1024];
    Resp r;
} Job;

static DWORD WINAPI JobThread(LPVOID p) {
    Job *j = (Job *)p;

    Dispatch(j->line, &j->r);
    return 0;
}

/* VirtualAlloc, deliberately: a corpse can hold the CRT
 * heap lock, and a timed out job's block is leaked so a
 * late finisher writes into memory nothing else owns. */
static void RunWithTimeout(SOCKET client, const char *line) {
    char msg[128];
    HANDLE t;
    Job *j = (Job *)VirtualAlloc(NULL, sizeof(Job) + RESP_MAX,
                                 MEM_COMMIT | MEM_RESERVE,
                                 PAGE_READWRITE);

    if (!j) {
        const char *e = "out of memory for command\n";
        send(client, e, (int)strlen(e), 0);
        return;
    }
    strncpy(j->line, line, sizeof(j->line) - 1);
    j->line[sizeof(j->line) - 1] = 0;
    j->r.buf = (char *)(j + 1);
    j->r.len = 0;
    j->r.cap = RESP_MAX;

    t = CreateThread(NULL, 0, JobThread, j, 0, NULL);
    if (!t) {
        Dispatch(j->line, &j->r);
        if (j->r.len > 0) send(client, j->r.buf, j->r.len, 0);
        VirtualFree(j, 0, MEM_RELEASE);
        return;
    }
    if (WaitForSingleObject(t, CMD_TIMEOUT_MS) == WAIT_OBJECT_0) {
        CloseHandle(t);
        if (j->r.len > 0) send(client, j->r.buf, j->r.len, 0);
        VirtualFree(j, 0, MEM_RELEASE);
        return;
    }
    CloseHandle(t);
    snprintf(msg, sizeof(msg),
             "timed out after %ds. worker abandoned, its result"
             " is discarded.\n", CMD_TIMEOUT_MS / 1000);
    send(client, msg, (int)strlen(msg), 0);
}

/* ---- TCP server ---- */

static DWORD WINAPI ServerThread(LPVOID param) {
    (void)param;
    PLog("waiting 10s for game init...");
    Sleep(10000);

    g_imageBase = (uint8_t *)GetModuleHandleA(NULL);
    MapSections();
    PLog("mapped %d sections, base=%p", g_nsections, g_imageBase);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) {
        PLog("socket() failed: %d", WSAGetLastError());
        return 1;
    }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(REPL_PORT);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        PLog("bind() failed: %d", WSAGetLastError());
        closesocket(srv);
        return 1;
    }
    listen(srv, 1);
    PLog("REPL listening on 127.0.0.1:%d", REPL_PORT);

    for (;;) {
        SOCKET client = accept(srv, NULL, NULL);
        if (client == INVALID_SOCKET) continue;
        PLog("client connected");

        const char *banner =
            "GRW ScriptHook REPL v0.1\n"
            "type 'help' for commands\n> ";
        send(client, banner, (int)strlen(banner), 0);

        char linebuf[1024];
        int  linepos = 0;

        for (;;) {
            char tmp[256];
            int n = recv(client, tmp, sizeof(tmp), 0);
            if (n <= 0) break;

            for (int i = 0; i < n; i++) {
                if (tmp[i] == '\n' || tmp[i] == '\r') {
                    if (linepos == 0) continue;
                    linebuf[linepos] = 0;
                    linepos = 0;

                    if (strcmp(linebuf, "quit") == 0 ||
                        strcmp(linebuf, "exit") == 0) {
                        const char *bye = "bye\n";
                        send(client, bye, 4, 0);
                        goto disc;
                    }

                    PLog("cmd: %s", linebuf);
                    RunWithTimeout(client, linebuf);

                    const char *prompt = "> ";
                    send(client, prompt, 2, 0);
                } else if (linepos < 1023) {
                    linebuf[linepos++] = tmp[i];
                }
            }
        }
disc:
        closesocket(client);
        PLog("client disconnected");
    }

    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_log = fopen("scripthook_repl.log", "w");
        PLog("REPL plugin loaded");
        CreateThread(NULL, 0, ServerThread, NULL, 0, NULL);
    }
    return TRUE;
}
