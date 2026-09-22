/* In-game microphone fix, and a microphone picker for a game that has none.
 *
 * ---- the defect -------------------------------------------------------
 *
 * GRW.exe reads every recording device's name and cannot cope with a name
 * that is not ASCII. On a Chinese Windows every device is called something
 * like "麦克风 (Realtek(R) Audio)", so the game finds no microphone at all
 * and its sound settings offer nothing to pick. The community fix is manual
 * and system wide: open the legacy Sound control panel, show the disabled
 * devices, and rename every one of them to an English name. Renaming works
 * because the game then reads a name it can understand.
 *
 * This plugin does the same thing, for this process only: it hands the game
 * an ASCII alias - "Mic1(Realtek High Definition Audio)" and so on, one per
 * device - for every recording device that has a non-ASCII name in either of
 * the two doors the game asks (see below). Nothing in the system is touched -
 * no registry write, no device rename, no other program affected - and the
 * alias disappears with the process.
 *
 * It also does what the game does not: it lists the recording devices in the
 * F4 menu and can force one of them, so the game is given that device and no
 * other. See "switches" below.
 *
 * ---- where the game reads the name (evidence) -------------------------
 *
 * Checked 2026-09-19 against the installed GRW.exe (405,703,672 bytes):
 *
 *   - the import table names ole32!CoCreateInstance and friends, and winmm
 *     for timeGetTime alone: no waveIn*, no dsound, no MMDevAPI.
 *   - there is no "IMMDevice", "IAudioClient", "EnumAudioEndpoints",
 *     "GetDefaultAudioEndpoint" or "BCDE0395" (CLSID_MMDeviceEnumerator)
 *     anywhere in the binary, ASCII or UTF-16.
 *   - the UTF-16 string block at file offset ~63,340,000 holds, in one run:
 *     "Audio Capture", "Tee Connection", ".Sample Grabber", "Null Renderer",
 *     "FriendlyName".
 *
 * That is a DirectShow capture graph, so the chain the game walks is:
 *
 *   CoCreateInstance(CLSID_SystemDeviceEnum)
 *     -> ICreateDevEnum::CreateClassEnumerator(CLSID_AudioInputDeviceCategory)
 *       -> IEnumMoniker::Next
 *         -> IMoniker::BindToStorage(IID_IPropertyBag)
 *           -> IPropertyBag::Read(L"FriendlyName")            <-- the name
 *
 * and the fix is to answer those Read calls with the alias. What the probe
 * log (see below) is for: the runtime shape is the one assumption this
 * plugin makes, and a session that shows the chain going somewhere else
 * says so in logs\micfix.log. If Read never carries the name, the next
 * place to look is the moniker itself - but read the warning on
 * IMoniker::GetDisplayName further down before freeing anything it hands
 * back.
 *
 * ---- the second door (evidence) ---------------------------------------
 *
 * The DirectShow chain above is not the only place the name is read. The
 * probe log of 2026-09-19 shows the game walking Core Audio as well: it
 * creates the MMDevice enumerator, asks for the default capture endpoint,
 * opens that endpoint's property store and reads PKEY_Device_FriendlyName -
 * the same device, under a name of its own. The two doors read different
 * registry values (the moniker's name comes from the legacy waveIn path and
 * only follows a rename when the device is re-enumerated), so after a rename
 * they can and do disagree. See AliasFor for what the game needs from them.
 *
 * ---- what the field runs cost -----------------------------------------
 *
 * Two lessons, both paid for in game launches.
 *
 * 1. The first two runs died within a second of the plugin loading, with
 *    0xC0000374 (heap corruption) in ntdll and no access violation
 *    anywhere - the signature of a bad free, not a bad read. It was one:
 *    the scan read the first device's display name and freed it with
 *    SysFreeString, while the string comes from devenum.dll, which
 *    allocates it with CoTaskMemAlloc. Hence the rule above BagReadStr:
 *    ask the bag, never the moniker, and never free anything the moniker
 *    handed over.
 *
 * 2. The next two ran fine and did nothing, because this file's
 *    IID_IPropertyBag had 42DB where the SDK says 42CB. The plugin's own
 *    BindToStorage answered E_NOINTERFACE (so the menu listed nameless
 *    devices), and the game's calls - with the correct IID, visible in the
 *    probe log - were not recognised as a property bag at all, so the
 *    alias never reached the game and the microphone stayed unusable.
 *    A GUID is copied from the header, never from memory.
 *
 * 3. The one that took longest, because it looked like a success. With a
 *    Chinese device name the microphone worked - and then stopped working
 *    whenever the plugin renamed one door and not the other. Four runs with
 *    the names the two doors answered, and the outcome:
 *
 *      store "Mic1(...)"  bag "Mic1(...)"   microphone works
 *      store real name    bag real name     microphone works
 *      store "Mic1(...)"  bag real name     no capture opened at all
 *      store real name    bag "Mic1(...)"   no capture opened at all
 *
 *    The game needs the same answer from both doors, not an ASCII answer
 *    from one of them. Hence AliasFor renames a device on both doors or on
 *    neither, and the two doors are paired by the controller in brackets
 *    when the rename of the second one happens before the scan has published
 *    the device. A reboot does not save you here: 2026-09-19 16:35 was a
 *    rebooted machine whose two doors still disagreed.
 *
 * ---- how the interception is done ------------------------------------
 *
 * MinHook owns exactly one target, ole32!CoCreateInstance: everything else
 * on the chain is a COM object reached at run time, so the vtable slots are
 * patched instead (VirtualProtect + the original kept to call through).
 * Slot patches are idempotent per (vtable, slot) and are undone in
 * DLL_PROCESS_DETACH.
 *
 * The bag slot is the one that must not misfire: an IPropertyBag vtable
 * can be shared with bags that have nothing to do with audio, so Read is
 * only rewritten for bag pointers this plugin registered itself, at the
 * BindToStorage it saw. Everything else - another property, another bag,
 * a failed read, a value that is not a BSTR - returns untouched.
 *
 * The enumerator's vtable is patched while this plugin scans the devices
 * itself at start up, which is also why the patch does not depend on the
 * CoCreateInstance hook seeing the game's own creation: the vtable belongs
 * to the class, so the game's later enumerators come through here too. The
 * hook stays as the guaranteed path, and the two are idempotent together.
 *
 * ---- switches ---------------------------------------------------------
 *
 * Both are live, and both are written back to this plugin's own ini
 * plugins\micfix\micfix.ini:
 *
 *   fix=1     hand the game ASCII aliases for names it cannot read.
 *             On by default: enabling the plugin in scripthook.ini is
 *             already the opt-in, and a fix plugin that does nothing until
 *             a second switch is flipped is a plugin nobody can tell apart
 *             from a broken one.
 *   force=0   offer the game the picked device and no other. Picking a
 *             device in the menu turns this on, because that is what
 *             picking one means.
 *   probe=0   log every device, every property read and every rename. Off
 *             by default - it writes a great deal, and the milestones are
 *             logged either way. Turn it on when a session has to be
 *             explained.
 *
 * The hooks go in whether or not a switch is on - the switches are meant to
 * be usable without a restart, and the cost of that is one flag read in the
 * Read detour while they are off. No name is ever rewritten while `fix` is
 * off, so "loaded but doing nothing" still holds in the only sense that
 * matters: the game sees exactly what it would see without this plugin.
 *
 * ---- why a plugin and not the framework (a convergence item) ----------
 *
 * docs/plugins.md says a plugin does not install a hook: anything that
 * patches code belongs to the framework, in front of an API, so two plugins
 * cannot tread on the same byte. This one carries MinHook of its own, like
 * skipintro, ModeExitProbe, ModeCallProbe and AllLanguages before it,
 * and it is the fifth entry on that list - not a licence to add a sixth.
 * What makes it defensible here: the targets are ole32 and system COM
 * objects, nothing in this process writes engine memory through them, and
 * there is exactly one consumer, so a framework call in front of it would
 * be an API with one caller. If a second plugin ever wants the device
 * enumeration, the layer belongs in the framework and this file becomes its
 * first customer.
 */
#include <windows.h>
#include <objbase.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "scripthook.h"

/* What this plugin needs of the framework: nothing newer than the first
 * version of the plugin API, so any ScriptHook that carries the API at all can
 * load this (see SH_REQUIRES_API). Name the last thing you use, not the header
 * you happened to build against. */
SH_REQUIRES_API(1);
#include "log.h"
#include "third_party/minhook/include/MinHook.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ---- sizes ------------------------------------------------------------ */

#define MIC_DEV_MAX   64     /* the menu list holds 64, so this is 64    */
#define MIC_BAG_MAX   96     /* bags seen in one session                 */
#define MIC_SLOT_MAX  48     /* patched vtable slots                     */
#define MIC_PASS_MAX  16     /* enumerators that fell back to pass through */
#define MIC_MON_MAX   32     /* monikers noted for the probe log         */
#define MIC_NAMEALIAS_MAX 16 /* names seen outside the scan, aliased     */
#define MIC_CLSID_MAX 40     /* distinct CLSIDs logged per session       */
#define MIC_KEY_MAX   192    /* a device's identity string               */
#define MIC_NAME_MAX  256    /* a device's own (UTF-16) name             */
#define MIC_ALIAS_MAX 64     /* what the GAME is told - ASCII only       */
#define MIC_LABEL_MAX 96     /* what the MENU shows - UTF-8, any script  */

/* A menu row's value column is 48 bytes (ShMenuRow.value), so a label past
 * that is shown cut. Both limits stay under it with room for the frame. */
#define MIC_LABEL_ROOM 44

/* vtable slots, counted from the interface's own methods. IUnknown is
 * QueryInterface(0) / AddRef(1) / Release(2) in every one of them. */
#define VT_RELEASE                 2
#define VT_CREATE_CLASS_ENUMERATOR 3   /* ICreateDevEnum                    */
#define VT_ENUM_NEXT               3   /* IEnumMoniker                      */
#define VT_ENUM_RESET              5   /* IEnumMoniker                      */
#define VT_BIND_TO_OBJECT          8   /* IMoniker (after IPersistStream)   */
#define VT_BIND_TO_STORAGE         9   /* IMoniker                          */
#define VT_GET_DISPLAY_NAME       20   /* IMoniker - probe only, see below  */
#define VT_BAG_READ                3   /* IPropertyBag                      */

/* ---- the interfaces this plugin has to name -------------------------
 * Declared here rather than pulled in from dshow.h / strmiids.lib: three
 * GUIDs, two slot offsets and one property name are the whole contract,
 * and the SDK headers would drag in a filter graph this plugin never
 * builds. The GUIDs are the published ones. */

static const GUID kCLSID_SystemDeviceEnum =
    { 0x62BE5D10, 0x60EB, 0x11D0, { 0xBD, 0x3B, 0x00, 0xA0, 0xC9, 0x11, 0xCE, 0x86 } };
static const GUID kCLSID_AudioInputDeviceCategory =
    { 0x33D9A762, 0x90C8, 0x11D0, { 0xBD, 0x43, 0x00, 0xA0, 0xC9, 0x11, 0xCE, 0x86 } };
static const GUID kIID_ICreateDevEnum =
    { 0x29840822, 0x5B84, 0x11D0, { 0xBD, 0x3B, 0x00, 0xA0, 0xC9, 0x11, 0xCE, 0x86 } };
/* {55272A00-42CB-11CE-8135-00AA004BB851}, copied from oaidl.h. The
 * Data2 word is the one to get right: 42DB is IPersistPropertyBag's
 * neighbour in the same family and is what this file said for two rounds
 * of field testing, with every symptom of a wrong constant - the plugin's
 * own binds answered E_NOINTERFACE, the game's correct calls were not
 * recognised as a property bag at all, and so the alias never reached the
 * game while the device list stayed nameless. */
static const GUID kIID_IPropertyBag =
    { 0x55272A00, 0x42CB, 0x11CE, { 0x81, 0x35, 0x00, 0xAA, 0x00, 0x4B, 0xB8, 0x51 } };
/* {E30629D2-27E5-11CE-875D-00608CB78066}, CLSID_AudioCapture: the filter a
 * recording device's moniker carries, and noted in the device's own bag as
 * "CLSID" (seen in the probe log, 2026-09-19). It is what tells a bag that is
 * a recording device's from one that merely shares its class. Compared as the
 * text the bag answers with, in the canonical form StringFromGUID2 writes. */
static const WCHAR kAudioCaptureClsidText[] =
    L"{E30629D2-27E5-11CE-875D-00608CB78066}";

/* ---- the calls this plugin makes through the slots -------------------- */

typedef HRESULT (WINAPI *MicCoCreateInstance_t)(const GUID *, void *, DWORD,
                                                const GUID *, void **);
typedef HRESULT (STDMETHODCALLTYPE *MicCreateClassEnum_t)(void *, const GUID *,
                                                          void **, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *MicNext_t)(void *, ULONG, void **, ULONG *);
typedef HRESULT (STDMETHODCALLTYPE *MicReset_t)(void *);
typedef HRESULT (STDMETHODCALLTYPE *MicBindToStorage_t)(void *, void *, void *,
                                                        const GUID *, void **);
typedef HRESULT (STDMETHODCALLTYPE *MicBindToObject_t)(void *, void *, void *,
                                                       const GUID *, void **);
typedef HRESULT (STDMETHODCALLTYPE *MicGetDisplayName_t)(void *, void *, void *,
                                                         WCHAR **);
typedef HRESULT (STDMETHODCALLTYPE *MicRead_t)(void *, const WCHAR *, VARIANT *,
                                               void *);
typedef ULONG   (STDMETHODCALLTYPE *MicRelease_t)(void *);

/* The one MinHook target, kept from the moment it is hooked: the detour
 * calls it through, and the plugin's own scan wants the unhooked one. */
static MicCoCreateInstance_t g_realCoCreateInstance;

/* ---- settings --------------------------------------------------------- */

static volatile LONG g_cfgFix   = 1;
static volatile LONG g_cfgForce = 0;
static volatile LONG g_cfgProbe = 1;
static volatile LONG g_cfgDevice;         /* row value: a device's position */
static char g_targetKey[MIC_KEY_MAX];     /* what force matches on          */
static volatile LONG g_targetOk;          /* 1 when that key was in the scan */

static HINSTANCE g_inst;
static char      g_iniPath[MAX_PATH];

static volatile LONG g_stop;
static volatile LONG g_rescanWanted;
static volatile LONG g_aliasSeq;

static uint32_t g_menu;

static CRITICAL_SECTION g_lock;
static volatile LONG    g_lockReady;

/* Set on the plugin's scan thread while the scan walks its own enumerator.
 * The detours are there to answer the game, and must not answer the plugin:
 * a scan read that comes back aliased records the alias where a device name
 * belongs - "name='Mic1(Realtek High Definition Audio)'" in the log of
 * 2026-09-19 - and then no device is ever matched by name, and none is ever
 * recorded as having a non-ASCII one. Thread local, so a game thread reading
 * a device name at that moment is still answered. */
static __declspec(thread) int t_inScan;

static void Lock(void)   { if (g_lockReady) EnterCriticalSection(&g_lock); }
static void Unlock(void) { if (g_lockReady) LeaveCriticalSection(&g_lock); }

static int WantFix(void)   { return InterlockedCompareExchange(&g_cfgFix, 0, 0) ? 1 : 0; }
static int WantForce(void) { return InterlockedCompareExchange(&g_cfgForce, 0, 0) ? 1 : 0; }
static int WantProbe(void) { return InterlockedCompareExchange(&g_cfgProbe, 0, 0) ? 1 : 0; }

static int IsAsciiOnly(const WCHAR *s) {
    for (; s && *s; s++) if (*s > 0x7F) return 0;
    return 1;
}

/* ---- narrow text ------------------------------------------------------
 * Two jobs, two different conversions:
 *
 *   - what the GAME is handed must be pure ASCII. It cannot read anything
 *     else, which is the whole reason this plugin exists, so the alias is
 *     built by dropping every character outside printable ASCII.
 *   - what the MENU shows may be Chinese and has to be UTF-8, because that
 *     is what the framework's text pipeline draws. A GBK byte string in a
 *     row comes out as mojibake.
 *
 * The menu row shows a list option in a 48 byte value column
 * (ShMenuRow.value), so labels are capped below that.
 */

static void WideToUtf8(const WCHAR *w, char *out, int cap) {
    if (cap <= 0) return;
    out[0] = 0;
    if (w && w[0]) WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
}

/* Drop the bytes of a UTF-8 sequence the cap cut in half: a valid string
 * never ends in a continuation or a lead byte, so this only touches what
 * truncation produced. */
static void Utf8Trim(char *s) {
    size_t n = strlen(s);

    while (n && ((unsigned char)s[n - 1] & 0xC0) == 0x80) s[--n] = 0;
    if (n && (unsigned char)s[n - 1] >= 0xC0) s[n - 1] = 0;
}

/* "麦克风 (Realtek(R) Audio)" -> "Realtek(R) Audio"; "荣耀亲选耳机X8i" is
 * kept as it is. The part in brackets is what a player recognises on the
 * Windows sound page, and on this machine it is the controller or the
 * headset - exactly what tells two microphones apart. */
static void StripDecor(const WCHAR *in, WCHAR *out, int cap) {
    const WCHAR *open = NULL, *close = NULL, *p;

    out[0] = 0;
    if (!in || !in[0] || cap <= 0) return;
    for (p = in; *p; p++) {
        if (*p == L'(' && !open) open = p;
        if (*p == L')') close = p;
    }
    if (open && close > open + 1) {
        size_t n = (size_t)(close - open - 1);

        if (n >= (size_t)cap) n = (size_t)cap - 1;
        memcpy(out, open + 1, n * sizeof(WCHAR));
        out[n] = 0;
        return;
    }
    lstrcpynW(out, in, cap);
}

/* Printable ASCII kept, everything else dropped, runs of blanks collapsed
 * to one space. Empty when nothing readable is left. */
static void AsciiOnly(const WCHAR *in, char *out, int cap) {
    const WCHAR *p;
    int n = 0, pending = 0;

    if (cap <= 0) return;
    out[0] = 0;
    if (!in || cap <= 1) return;
    for (p = in; *p; p++) {
        WCHAR c = *p;

        if (c == L' ' || c == L'\t') { if (n) pending = 1; continue; }
        if (c < 0x20 || c > 0x7E) continue;
        if (n + (pending ? 1 : 0) >= cap - 1) break;
        if (pending) { out[n++] = ' '; pending = 0; }
        out[n++] = (char)c;
    }
    out[n] = 0;
}

/* ---- probe bookkeeping ------------------------------------------------
 * Three small tables that exist for logs\micfix.log and nothing else: the
 * distinct CLSIDs the process asked for, and the alias of each moniker
 * this plugin handed out. The second one is what turns "the game bound a
 * moniker" into "the game bound Mic 2", which is the question a machine
 * whose echo test stays silent has to answer.
 */

static GUID g_clsid[MIC_CLSID_MAX];
static int  g_nclsid;

static void GuidText(const GUID *g, char *out, int cap) {
    WCHAR w[64];

    out[0] = 0;
    if (g && StringFromGUID2(g, w, (int)ARRAY_LEN(w)) > 0)
        WideCharToMultiByte(CP_ACP, 0, w, -1, out, cap, NULL, NULL);
    else
        snprintf(out, cap, "(null)");
}

static void NoteClsid(const GUID *g) {
    char t[64];
    int  i;

    if (!g) return;
    Lock();
    for (i = 0; i < g_nclsid; i++)
        if (IsEqualGUID(&g_clsid[i], g)) { Unlock(); return; }
    if (g_nclsid < MIC_CLSID_MAX) g_clsid[g_nclsid++] = *g;
    Unlock();

    GuidText(g, t, sizeof(t));
    Log("CoCreateInstance: %s", t);
}

typedef struct MicMon {
    void *moniker;
    char  alias[MIC_ALIAS_MAX];
} MicMon;

static MicMon g_mon[MIC_MON_MAX];
static int    g_nmon;

static void MonoNote(void *moniker, const char *alias) {
    int i;

    if (!moniker || !alias || !alias[0]) return;
    Lock();
    for (i = 0; i < g_nmon; i++)
        if (g_mon[i].moniker == moniker) {
            snprintf(g_mon[i].alias, sizeof(g_mon[i].alias), "%s", alias);
            Unlock();
            return;
        }
    if (g_nmon < MIC_MON_MAX) {
        g_mon[g_nmon].moniker = moniker;
        snprintf(g_mon[g_nmon].alias, sizeof(g_mon[g_nmon].alias), "%s", alias);
        g_nmon++;
    }
    Unlock();
}

static void MonAlias(void *moniker, char *out, int cap) {
    int i;

    snprintf(out, cap, "(unknown)");
    Lock();
    for (i = 0; i < g_nmon; i++)
        if (g_mon[i].moniker == moniker) {
            snprintf(out, cap, "%s", g_mon[i].alias);
            break;
        }
    Unlock();
}

/* ---- vtable slots ----------------------------------------------------- */

typedef struct MicSlot {
    void **vtbl;
    int    index;
    void  *original;
} MicSlot;

static MicSlot g_slot[MIC_SLOT_MAX];
static int     g_nslot;

/* The slot's real function: what we patched over, or the slot's own value
 * when it was never patched. Every call this plugin makes into a chain
 * object goes through here, so the plugin's own scan sees the whole list
 * even while `force` is filtering what the game sees. */
static void *SlotOriginal(void **vtbl, int index) {
    int i, n;
    void *fn;

    if (!vtbl) return NULL;
    Lock();
    n = g_nslot;
    for (i = 0; i < n; i++)
        if (g_slot[i].vtbl == vtbl && g_slot[i].index == index) {
            fn = g_slot[i].original;
            Unlock();
            return fn;
        }
    Unlock();
    return vtbl[index];
}

/* One 8 byte write into a vtable, pages protected across the whole range
 * so a slot that straddles a page boundary is still written safely. */
static int WriteSlot(void **vtbl, int index, void *value) {
    unsigned char *p = (unsigned char *)&vtbl[index];
    unsigned char *lo, *hi;
    DWORD old = 0;

    lo = (unsigned char *)((uintptr_t)p & ~(uintptr_t)0xFFF);
    hi = (unsigned char *)(((uintptr_t)(p + sizeof(void *)) + 0xFFF) & ~(uintptr_t)0xFFF);
    if (!VirtualProtect(lo, (SIZE_T)(hi - lo), PAGE_READWRITE, &old)) return 0;
    memcpy(p, &value, sizeof(void *));
    VirtualProtect(lo, (SIZE_T)(hi - lo), old, &old);
    return 1;
}

/* Install one detour, once. Returns the function to call through. */
static void *SlotInstall(void **vtbl, int index, void *detour, const char *what) {
    void *original;
    int   i, full = 0;

    if (!vtbl) return NULL;

    Lock();
    for (i = 0; i < g_nslot; i++)
        if (g_slot[i].vtbl == vtbl && g_slot[i].index == index) {
            original = g_slot[i].original;
            Unlock();
            return original;   /* already ours: the first original stands */
        }

    original = vtbl[index];
    if (!original || original == detour) { Unlock(); return original; }
    if (g_nslot >= MIC_SLOT_MAX) full = 1;
    else if (!WriteSlot(vtbl, index, detour)) {
        Unlock();
        Log("VirtualProtect refused: %s (vtbl %p slot %d) is left alone",
            what, (void *)vtbl, index);
        return original;
    } else {
        g_slot[g_nslot].vtbl     = vtbl;
        g_slot[g_nslot].index    = index;
        g_slot[g_nslot].original = original;
        g_nslot++;
    }
    Unlock();

    if (full) {
        Log("slot table is full (%d): %s is left alone", MIC_SLOT_MAX, what);
        return original;
    }
    Log("slot patched: %s  vtbl=%p slot=%d original=%p", what, (void *)vtbl,
        index, original);
    return original;
}

static void RestoreSlots(void) {
    int i, n;

    Lock();
    n = g_nslot;
    for (i = 0; i < n; i++) {
        if (g_slot[i].vtbl) WriteSlot(g_slot[i].vtbl, g_slot[i].index,
                                      g_slot[i].original);
    }
    g_nslot = 0;
    Unlock();
}

/* ---- chain helpers ---------------------------------------------------- */

static void *OrigFor(void *obj, int index) {
    return SlotOriginal(*(void ***)obj, index);
}

static ULONG ObjRelease(void *obj) {
    MicRelease_t fn;

    if (!obj) return 0;
    fn = (MicRelease_t)OrigFor(obj, VT_RELEASE);
    return fn ? fn(obj) : 0;
}

/* ---- reading the property bag -----------------------------------------
 * Everything this plugin wants to know about a device comes out of its
 * property bag, as a BSTR inside a VARIANT the caller owns: free it with
 * VariantClear and there is no question about which allocator made it.
 *
 * IMoniker::GetDisplayName is deliberately NOT used, and this is the one
 * hard-won line in this file. It hands back a string from the moniker's
 * own allocator - devenum.dll builds it with CoTaskMemAlloc, checked in
 * that module's import table - and freeing it the obvious way with
 * SysFreeString makes OLEAUT32 free one DWORD *before* the block. The heap
 * reports that as corruption: GRW.exe died twice on 2026-09-19 with
 * 0xC0000374 in ntdll, no access violation anywhere, each time a moment
 * after this plugin's scan had read the first device. The device path says
 * everything the display name did - ASCII, unique, stable across sessions
 * - and it comes out of the bag.
 */

static int BagReadStr(void *bag, const WCHAR *prop, WCHAR *out, int cap) {
    MicRead_t rd;
    VARIANT   v;
    HRESULT   hr = E_FAIL;
    unsigned  vt = 0;

    out[0] = 0;
    if (!bag || cap <= 0) return 0;
    rd = (MicRead_t)OrigFor(bag, VT_BAG_READ);
    if (!rd) return 0;

    VariantInit(&v);
    hr = rd(bag, prop, &v, NULL);
    vt = (unsigned)v.vt;
    /* The interface contract is a BSTR, and a bag that answers with a
     * plain wide string instead is tolerated rather than thrown away:
     * VariantClear releases either one correctly, and a VARIANT keeps both
     * in the same union slot. */
    if (SUCCEEDED(hr) && (v.vt == VT_BSTR || v.vt == VT_LPWSTR) && v.bstrVal)
        lstrcpynW(out, v.bstrVal, cap);
    if (WantProbe()) {
        char got[MIC_NAME_MAX];

        WideToUtf8(out, got, sizeof(got));
        Log("read(%p, '%ls') hr=0x%08lX vt=%u -> '%s'", bag, prop,
            (unsigned long)hr, vt, got);
    }
    VariantClear(&v);
    return out[0] != 0;
}

/* One of the bag's numeric properties, "WaveInID" being the one that
 * matters: it is how the same device is named to winmm. */
static int BagReadLong(void *bag, const WCHAR *prop, int *out) {
    MicRead_t rd;
    VARIANT   v;
    HRESULT   hr = E_FAIL;
    unsigned  vt = 0;
    int       ok = 0;

    if (out) *out = -1;
    if (!bag || !out) return 0;
    rd = (MicRead_t)OrigFor(bag, VT_BAG_READ);
    if (!rd) return 0;

    VariantInit(&v);
    hr = rd(bag, prop, &v, NULL);
    vt = (unsigned)v.vt;
    if (SUCCEEDED(hr) && v.vt == VT_I4) { *out = (int)v.lVal; ok = 1; }
    if (WantProbe())
        Log("read(%p, '%ls') hr=0x%08lX vt=%u -> %d", bag, prop,
            (unsigned long)hr, vt, *out);
    VariantClear(&v);
    return ok;
}

/* The moniker's bag, with one reference the caller releases. */
static void *MonikerBindBag(void *moniker) {
    MicBindToStorage_t bts =
        (MicBindToStorage_t)OrigFor(moniker, VT_BIND_TO_STORAGE);
    void *bag = NULL;
    HRESULT hr = E_FAIL;

    if (!bts) return NULL;
    hr = bts(moniker, NULL, NULL, &kIID_IPropertyBag, &bag);
    if (WantProbe())
        Log("bind bag(%p) hr=0x%08lX bag=%p", moniker, (unsigned long)hr, bag);
    return FAILED(hr) ? NULL : bag;
}

/* The device's identity: its device path when the bag carries one, its own
 * name as a key when it does not. Narrow, because it is written to the ini
 * and compared with strcmp. */
static int BagKey(void *bag, char *out, int cap) {
    WCHAR w[MIC_NAME_MAX];

    out[0] = 0;
    if (BagReadStr(bag, L"DevicePath", w, (int)ARRAY_LEN(w))) {
        WideToUtf8(w, out, cap);
        return out[0] != 0;
    }
    if (BagReadStr(bag, L"FriendlyName", w, (int)ARRAY_LEN(w))) {
        char n[MIC_NAME_MAX];

        WideToUtf8(w, n, sizeof(n));
        snprintf(out, cap, "name:%s", n);
        return 1;
    }
    return 0;
}

/* The name a player recognises: "Description" first - that is the
 * controller or the headset as Windows names it - and the device's own
 * friendly name when there is no description. */
static int BagDesc(void *bag, WCHAR *out, int cap) {
    out[0] = 0;
    if (BagReadStr(bag, L"Description", out, cap) && out[0]) return 1;
    if (BagReadStr(bag, L"FriendlyName", out, cap) && out[0]) return 1;
    return 0;
}

/* The two names of one device, both built from what the bag could say:
 *
 *   alias  "Mic1(Realtek(R) Audio)"  - pure ASCII, what the GAME is told,
 *          because it cannot read anything else.
 *   label  "Mic1(Realtek(R) Audio)"  - UTF-8, what the MENU shows, so a
 *          Chinese device name stays readable and the player can tell the
 *          devices apart.
 */
static void MakeNames(int index, const WCHAR *desc, char *alias, int acap,
                      char *label, int lcap) {
    WCHAR inner[MIC_NAME_MAX];
    char  ascii[MIC_ALIAS_MAX];
    char  utf8[MIC_LABEL_MAX];

    StripDecor(desc, inner, (int)ARRAY_LEN(inner));

    AsciiOnly(inner, ascii, (int)ARRAY_LEN(ascii));
    if (strlen(ascii) >= 2) snprintf(alias, acap, "Mic%d(%s)", index, ascii);
    else                    snprintf(alias, acap, "Mic%d", index);

    WideToUtf8(inner, utf8, (int)ARRAY_LEN(utf8));
    if (utf8[0]) snprintf(label, lcap, "Mic%d(%s)", index, utf8);
    else         snprintf(label, lcap, "Mic%d", index);
    /* Cut to the room a menu row has, and never past the buffer this call was
     * given: the two are the same number today, which is not a reason to
     * trust it. */
    {
        int cut = MIC_LABEL_ROOM;

        if (cut > lcap - 1) cut = lcap - 1;
        if (cut > 0 && (int)strlen(label) > cut) {
            label[cut] = 0;
            Utf8Trim(label);
        }
    }
}

/* ---- the device table -------------------------------------------------
 * One row per recording device, in scan order. The alias is what the game
 * is told; the key is the device path, which is ASCII, unique and the same
 * across sessions, so it is what the picked device is stored as.
 */

typedef struct MicDev {
    char alias[MIC_ALIAS_MAX];   /* "Mic1(Realtek(R) Audio)" - to the game  */
    char label[MIC_LABEL_MAX];   /* the same, UTF-8 - to the menu           */
    char key[MIC_KEY_MAX];       /* device path, the device's identity      */
    char name[MIC_NAME_MAX];     /* the device's own name, for the log      */
    char core[MIC_ALIAS_MAX];    /* what stands inside its brackets, ASCII  */
    int  waveInId;               /* the bag's WaveInID, -1 when it has none */
    int  nonAscii;               /* this name has a character above 0x7F    */
    int  aliasNeeded;            /* one of its names had to be replaced     */
} MicDev;

static MicDev         g_dev[MIC_DEV_MAX];
static volatile LONG  g_ndev;

/* Every reader of the table takes the lock, and every reader takes the count
 * inside it. A rescan rewrites the whole table - not "only grows", which is
 * what this comment used to say while the scan was doing a memcpy over it -
 * so a walk that is not holding the lock can read half of one scan and half
 * of the next. The lock is re-entrant, which is what lets AliasFor hold it
 * across a lookup and the row that lookup named. */
static const MicDev *DevByKey(const char *key) {
    const MicDev *hit = NULL;
    int i, n;

    if (!key || !key[0]) return NULL;
    Lock();
    n = (int)InterlockedCompareExchange(&g_ndev, 0, 0);
    for (i = 0; i < n; i++)
        if (strcmp(g_dev[i].key, key) == 0) { hit = &g_dev[i]; break; }
    Unlock();
    return hit;
}

/* What stands inside the brackets of a device name, ASCII only - see below. */
static void NameCore(const WCHAR *orig, char *out, int cap);

/* Which recording device of the scan calls itself this name? The name is the
 * one thing that is the same about a device no matter which door the game
 * came in through - the DirectShow property bag, winmm, or the Core Audio
 * property store all read the same registry value. Keying on the name instead
 * of on an address is not a detail: the run of 2026-09-19 crossed two
 * devices' aliases because a freed bag's address was reused by the next
 * device in the same walk. */
static int DevIndexByName(const char *u8) {
    int i, n, hit = -1;

    if (!u8 || !u8[0]) return -1;
    Lock();
    n = (int)InterlockedCompareExchange(&g_ndev, 0, 0);
    for (i = 0; i < n; i++)
        if (g_dev[i].name[0] && strcmp(g_dev[i].name, u8) == 0) {
            hit = i;
            break;
        }
    Unlock();
    return hit;
}

/* The same device under the other of its two names. The property store calls
 * the default microphone "麦克风 (Realtek High Definition Audio)" while the
 * moniker still calls it "Microphone (Realtek High Definition Audio)" until
 * the device is re-enumerated, and the two doors agree on nothing but the
 * part in the brackets.
 *
 * A controller is shared - the speakers of one codec sit behind the same
 * "(Realtek High Definition Audio)" - so a controller that picks out more
 * than one recording device is not an identity, and no answer is given. */
static int DevIndexByCore(const WCHAR *orig) {
    char core[MIC_ALIAS_MAX];
    int  i, n, hit = -1, seen = 0;

    NameCore(orig, core, sizeof(core));
    if (!core[0]) return -1;

    Lock();
    n = (int)InterlockedCompareExchange(&g_ndev, 0, 0);
    for (i = 0; i < n; i++) {
        if (!g_dev[i].core[0] || _stricmp(g_dev[i].core, core) != 0) continue;
        if (++seen > 1) { hit = -1; break; }
        hit = i;
    }
    Unlock();
    return hit;
}

/* Already one of ours? Then it is passed through untouched. A device bag
 * hands out the same string every time it is asked, so an alias that went
 * out once can come back on the next read - and aliasing an alias produced
 * "Mic1" then "Mic2" then "Mic4" for the same device in the run of
 * 2026-09-19, which is a name no game can match against anything. */
static int IsOurAlias(const WCHAR *w) {
    char u8[MIC_ALIAS_MAX];
    int  i, n;

    if (!w || !w[0]) return 0;
    WideToUtf8(w, u8, sizeof(u8));
    if (!u8[0]) return 0;

    Lock();
    n = (int)InterlockedCompareExchange(&g_ndev, 0, 0);
    for (i = 0; i < n; i++)
        if (strcmp(g_dev[i].alias, u8) == 0) { Unlock(); return 1; }
    Unlock();

    /* And the bare form a device the scan never saw would have been given. */
    if (u8[0] == 'M' && u8[1] == 'i' && u8[2] == 'c' &&
        u8[3] >= '0' && u8[3] <= '9') {
        i = 4;
        while (u8[i] >= '0' && u8[i] <= '9') i++;
        if (u8[i] == 0 || u8[i] == '(') return 1;
    }
    return 0;
}

/* The same device, reached by the ID winmm knows it by. That ID is in the
 * device's property bag ("WaveInID"), and it is the only thing that ties a
 * DirectShow moniker to a waveIn device, so the two doors can be answered
 * with the same alias.
 *
 * A device whose names are all ASCII is not answered at all: see AliasFor. */
static int DevByWaveInId(int id, char *alias, int cap) {
    int i, n;

    if (id < 0) return 0;
    Lock();
    n = (int)InterlockedCompareExchange(&g_ndev, 0, 0);
    for (i = 0; i < n; i++)
        if (g_dev[i].waveInId == id &&
            (g_dev[i].nonAscii || g_dev[i].aliasNeeded)) {
            snprintf(alias, cap, "%s", g_dev[i].alias);
            Unlock();
            return 1;
        }
    Unlock();
    return 0;
}

/* The device this name belongs to, together with a copy of its row, taken in
 * one critical section. Two calls would not do: a rescan on the menu thread
 * rewrites the whole table between them, and the row it would then read back
 * by index is another device's row - handed to the game as a name, that is
 * the mismatch this plugin must never create. Returns -1, and leaves *row
 * untouched, when no recording device answers to the name. */
static int DevLookup(const char *u8, const WCHAR *orig, MicDev *row) {
    int idx;

    Lock();
    idx = DevIndexByName(u8);
    if (idx < 0) idx = DevIndexByCore(orig);
    if (idx >= 0 && row) *row = g_dev[idx];
    Unlock();
    return idx;
}

/* Remember that this device has had to be renamed, so its other names are
 * renamed too. By index into the current table, because the row that carried
 * the decision may have been swapped out since. */
static void DevMarkAliasNeeded(int idx) {
    Lock();
    if (idx >= 0 && idx < (int)InterlockedCompareExchange(&g_ndev, 0, 0))
        g_dev[idx].aliasNeeded = 1;
    Unlock();
}

/* ---- aliases for names the scan never saw -----------------------------
 * The scan knows the devices one layer of Windows reports. The same
 * physical device appears under other names in other layers - the Core
 * Audio endpoint of the earbuds still calls itself "erji (荣耀亲选耳机X8i)"
 * while the kernel-streaming device underneath it has been renamed - and a
 * name that is not in the table used to be passed to the game untouched,
 * Chinese and all, which is the one thing this plugin exists to prevent.
 *
 * So: a name with no row gets an alias of its own, remembered for the
 * session. And when its bracketed part matches one that is already in the
 * table ("X8i" is "X8i"), that device's alias is reused, so the game sees
 * one name for one device no matter which layer it asked.
 */

typedef struct MicNameAlias {
    char name[MIC_NAME_MAX];
    char alias[MIC_ALIAS_MAX];
    char core[MIC_ALIAS_MAX];    /* what its brackets say, ASCII */
} MicNameAlias;

static MicNameAlias g_nameAlias[MIC_NAMEALIAS_MAX];
static int          g_nnameAlias;

/* The part inside the brackets, ASCII only: "erji (荣耀亲选耳机X8i)" and
 * "Mic1(X8i)" both come out as "X8i". */
static void NameCore(const WCHAR *orig, char *out, int cap) {
    WCHAR inner[MIC_NAME_MAX];

    StripDecor(orig, inner, (int)ARRAY_LEN(inner));
    AsciiOnly(inner, out, cap);
}

static int AliasFor(const WCHAR *orig, char *out, int cap) {
    char   u8[MIC_NAME_MAX], core[MIC_ALIAS_MAX];
    MicDev d;
    int    i, idx, seq, need, hit, seen;

    if (!orig || !orig[0]) return 0;
    WideToUtf8(orig, u8, sizeof(u8));
    if (!u8[0]) return 0;
    NameCore(orig, core, sizeof(core));

    /* An alias that went out once goes out again: a device bag hands the same
     * string back on every read, and aliasing an alias produced "Mic1" then
     * "Mic2" then "Mic4" for one device in the run of 2026-09-19. */
    Lock();
    for (i = 0; i < g_nnameAlias; i++)
        if (strcmp(g_nameAlias[i].name, u8) == 0) {
            snprintf(out, cap, "%s", g_nameAlias[i].alias);
            Unlock();
            return 1;
        }
    Unlock();

    /* Which recording device of the scan is called this - by name, or by the
     * controller it sits behind - and its row with it: see DevLookup. */
    idx = DevLookup(u8, orig, &d);

    if (idx >= 0) {
        /* One device, one name, on every door. A device is renamed because
         * one of its names has to be - this one, or the one the other door
         * read - and then every name of it is, so that whatever the game asks
         * about it, it hears the same thing twice. The evidence is in the
         * runs of 2026-09-19: 15:52, 16:15 and 16:3x, two doors agreeing,
         * microphone works; 16:23 and 16:35, one door renamed and the other
         * not, microphone opens nowhere.
         *
         * `force` does not enter into it: it decides which devices are
         * offered, and the device it picks is matched by its own name, so
         * that name has nothing to gain from being replaced. */
        need = !IsAsciiOnly(orig) || d.nonAscii || d.aliasNeeded;
        if (need) {
            DevMarkAliasNeeded(idx);
            snprintf(out, cap, "%s", d.alias);
            return 1;
        }
        /* Every name of this device is ASCII so far. Fall through all the
         * same: a name of it may have been renamed before the scan could
         * publish this device, and then only the other table has it. */
    }

    /* The other of the device's two names. The property store calls the
     * default microphone "麦克风 (Realtek High Definition Audio)" while the
     * DirectShow moniker still calls it "Microphone (Realtek High Definition
     * Audio)" until something re-enumerates the device, and the two agree on
     * nothing but the part in the brackets - so the brackets are what pairs
     * the name in front of us with the alias that went out before this device
     * was known. Uniqueness is what keeps the microphone and the speakers of
     * one codec, which share that controller, apart.
     *
     * This is the one place a name that is readable as it stands is still
     * replaced, and only because its device is already being renamed: leaving
     * it alone would put one name on one door and another on the next, which
     * is the run of 2026-09-19 16:35. */
    hit  = -1;
    seen = 0;
    if (core[0]) {
        Lock();
        for (i = 0; i < g_nnameAlias; i++) {
            if (!g_nameAlias[i].core[0] ||
                _stricmp(g_nameAlias[i].core, core) != 0) continue;
            if (++seen > 1) { hit = -1; break; }
            hit = i;
        }
        if (hit >= 0) {
            /* The scan's alias stands when the device is known, and the entry
             * that recorded the other name is brought into line with it. */
            snprintf(out, cap, "%s",
                     idx >= 0 ? d.alias : g_nameAlias[hit].alias);
            if (idx >= 0)
                snprintf(g_nameAlias[hit].alias, MIC_ALIAS_MAX, "%s", d.alias);
        }
        Unlock();
    }
    if (hit >= 0) {
        if (idx >= 0) DevMarkAliasNeeded(idx);
        if (WantProbe())
            Log("alias: '%s' is the other name of a device already renamed "
                "(controller '%s') - answered as '%s'", u8, core, out);
        return 1;
    }

    /* A name no recording device of the scan claims. Only a name that cannot
     * be read as it stands is replaced; anything else is left alone. */
    if (IsAsciiOnly(orig)) {
        if (WantProbe())
            Log(idx >= 0
                ? "alias: '%s' is a recording device whose names are all "
                  "ASCII - left as it is"
                : "alias: '%s' is already ASCII and belongs to no recording "
                  "device of the scan - left as it is", u8);
        return 0;
    }

    seq = (int)InterlockedIncrement(&g_aliasSeq);

    {
        char label[MIC_LABEL_MAX];

        MakeNames(seq, orig, out, cap, label, sizeof(label));
    }

    /* Room is checked where the row is written, not before it is built. Two
     * detours are in this function at once whenever the game asks its two
     * doors from two threads, and a check that runs outside the lock is one
     * both of them can pass together - the second then writes one row past
     * the end of the table, which is what this file did until 2026-09-19.
     * All three strings go in under the one lock, so a reader never finds a
     * row with a name and no alias. */
    Lock();
    if (g_nnameAlias >= MIC_NAMEALIAS_MAX) { Unlock(); return 0; }
    snprintf(g_nameAlias[g_nnameAlias].name, MIC_NAME_MAX, "%s", u8);
    snprintf(g_nameAlias[g_nnameAlias].alias, MIC_ALIAS_MAX, "%s", out);
    snprintf(g_nameAlias[g_nnameAlias].core, MIC_ALIAS_MAX, "%s", core);
    g_nnameAlias++;
    Unlock();

    Log("alias for a name the scan never saw: '%s' -> '%s'", u8, out);
    return 1;
}

/* ---- the bags this plugin has seen ------------------------------------
 * The table below is only a fallback now that the alias comes from the name
 * in front of us; it is what answers for a device that never got a name
 * back from its own bag.
 */

typedef struct MicBag {
    void *bag;
    char  key[MIC_KEY_MAX];
    char  alias[MIC_ALIAS_MAX];
} MicBag;

static MicBag g_bag[MIC_BAG_MAX];
static int    g_nbag;

/* Was this bag registered by this plugin? The Read slot is patched on a whole
 * class, and one devenum bag class serves every device category, so a bag
 * that has nothing to do with recording devices - a video capture device's,
 * say - arrives at HookBagRead too. What tells them apart is this table: it
 * is filled at the BindToStorage this plugin saw, and that only ever happens
 * for monikers of the audio capture category. Asked about the bag pointer,
 * never about the class. */
static int BagKnown(void *bag) {
    int i, found = 0;

    if (!bag) return 0;
    Lock();
    for (i = 0; i < g_nbag; i++)
        if (g_bag[i].bag == bag) { found = 1; break; }
    Unlock();
    return found;
}

/* Is this bag a recording device's? The Read slot is patched on a whole class
 * and one devenum bag class serves every category, so a bag can arrive at
 * HookBindToStorage from anywhere. Two properties answer it, and neither
 * exists on a camera or a renderer: the capture filter's own CLSID, and the
 * waveIn device number - the number the sound control panel's legacy list
 * uses. A bag that answers neither is not registered, and an unregistered bag
 * is one no door of this plugin ever rewrites. */
static int BagIsAudioCapture(void *bag) {
    WCHAR cls[80];
    int   id = -1;

    if (BagReadStr(bag, L"CLSID", cls, (int)ARRAY_LEN(cls)) &&
        lstrcmpiW(cls, kAudioCaptureClsidText) == 0)
        return 1;
    if (BagReadLong(bag, L"WaveInID", &id) && id >= 0) return 1;
    return 0;
}

/* The recording device a registered bag belongs to, by the key the scan filed
 * it under. NULL for a bag the scan does not know. */
static const MicDev *BagDevice(void *bag) {
    char key[MIC_KEY_MAX];
    int  i;

    key[0] = 0;
    Lock();
    for (i = 0; i < g_nbag; i++)
        if (g_bag[i].bag == bag) {
            snprintf(key, sizeof(key), "%s", g_bag[i].key);
            break;
        }
    Unlock();
    return key[0] ? DevByKey(key) : NULL;
}

static void BagRegister(void *moniker, void *bag) {
    char key[MIC_KEY_MAX];
    char alias[MIC_ALIAS_MAX];
    char label[MIC_LABEL_MAX];
    const MicDev *d;
    int  seq;

    if (!BagKey(bag, key, sizeof(key)))
        snprintf(key, sizeof(key), "bag:%p", bag);

    /* A device seen before keeps its alias, so two enumerations in one
     * session - the settings list, then the capture - agree. */
    alias[0] = 0;
    label[0] = 0;

    Lock();
    {
        int i;
        for (i = 0; i < g_nbag; i++)
            if (strcmp(g_bag[i].key, key) == 0) {
                snprintf(alias, sizeof(alias), "%s", g_bag[i].alias);
                break;
            }
    }
    Unlock();

    if (!alias[0] && (d = DevByKey(key)) != NULL) {
        snprintf(alias, sizeof(alias), "%s", d->alias);
        snprintf(label, sizeof(label), "%s", d->label);
    }
    if (!alias[0]) {
        /* A device the scan never saw - plugged in after it ran. It still
         * gets a name of its own, read from the bag in front of us. */
        WCHAR desc[MIC_NAME_MAX];

        seq = (int)InterlockedIncrement(&g_aliasSeq);
        if (!BagDesc(bag, desc, (int)ARRAY_LEN(desc))) desc[0] = 0;
        MakeNames(seq, desc, alias, sizeof(alias), label, sizeof(label));
    }

    Lock();
    if (g_nbag < MIC_BAG_MAX) {
        g_bag[g_nbag].bag = bag;
        snprintf(g_bag[g_nbag].key, sizeof(g_bag[g_nbag].key), "%s", key);
        snprintf(g_bag[g_nbag].alias, sizeof(g_bag[g_nbag].alias), "%s", alias);
        g_nbag++;
    } else {
        Log("the bag table is full (%d); '%s' keeps the name it has",
            MIC_BAG_MAX, key);
    }
    Unlock();

    MonoNote(moniker, alias);
    Log("bag %p -> '%s'  key='%s'", bag, alias, key);
}

/* ---- the detours ------------------------------------------------------ */

/* The alias as the BSTR a VARIANT carries. The alias is ASCII, so the
 * conversion has nothing to translate. */
static int AliasBstr(const char *alias, VARIANT *var) {
    WCHAR w[MIC_ALIAS_MAX];
    BSTR  nb;

    /* The buffer starts empty and the conversion is checked: an alias that
     * did not convert would otherwise be SysAllocString over an uninitialised
     * stack buffer, which reads past the end of it. */
    w[0] = 0;
    if (!alias || MultiByteToWideChar(CP_UTF8, 0, alias, -1, w,
                                      (int)ARRAY_LEN(w)) <= 0)
        return 0;
    nb = SysAllocString(w);
    if (!nb) return 0;
    var->vt      = VT_BSTR;
    var->bstrVal = nb;
    return 1;
}

/* The last one on the chain, and the only one that changes an answer. */
static HRESULT STDMETHODCALLTYPE HookBagRead(void *self, const WCHAR *name,
                                             VARIANT *var, void *errlog) {
    MicRead_t orig = (MicRead_t)OrigFor(self, VT_BAG_READ);
    char    alias[MIC_ALIAS_MAX];
    int     have;
    HRESULT hr;

    if (!orig) return E_FAIL;
    hr = orig(self, name, var, errlog);

    /* The scan asks this same bag for the same name, and it has to get the
     * device's real name back: this detour is what makes the game's reads,
     * and it must not make the plugin's own. */
    if (t_inScan) return hr;

    /* One of the bags this plugin registered, not merely one of the class it
     * patched - see BagKnown. */
    if (!BagKnown(self)) return hr;
    if (!WantFix()) return hr;
    if (!name || lstrcmpiW(name, L"FriendlyName") != 0) return hr;

    if (hr == S_OK && var && (var->vt == VT_BSTR || var->vt == VT_LPWSTR) &&
        var->bstrVal) {
        if (IsOurAlias(var->bstrVal)) {
            if (WantProbe())
                Log("read: an alias came back round; left as it is");
            return hr;
        }

        /* The alias of the device that calls itself what we just read: the
         * name is the only identity that survives the walk the game makes,
         * where bags are released and their addresses reused. Whether that
         * device is renamed at all is AliasFor's decision, and it is the same
         * one for every door - a name this plugin has no business in is not
         * given a new one here. */
        have = AliasFor(var->bstrVal, alias, sizeof(alias));
        if (!have) {
            if (WantProbe()) {
                char was[MIC_NAME_MAX];

                WideToUtf8(var->bstrVal, was, sizeof(was));
                Log("read: '%s' is left as it is", was);
            }
            return hr;
        }

        if (WantProbe()) {
            char was[MIC_NAME_MAX];

            WideToUtf8(var->bstrVal, was, sizeof(was));
            Log("read: '%s' -> '%s'", was, alias);
        }

        /* The pointer is replaced, and what came back is NOT released. A
         * device bag answers with the same string every time it is asked -
         * it keeps its own copy - so freeing it, which the VARIANT contract
         * permits, leaves the bag holding freed memory: the next read of
         * that device then returns whatever the allocator put there, which
         * in the run of 2026-09-19 was this plugin's own alias. One small
         * string per read is the price of never touching memory the bag
         * still owns. */
        if (!AliasBstr(alias, var)) return hr;
        return hr;
    }

    /* No name came back at all: an unnamed device is one the game cannot
     * offer, which is the failure this plugin exists for, so the alias is
     * the answer rather than the failure. Only a VARIANT the failed read
     * left EMPTY is touched - an empty VARIANT owns nothing, so there is
     * nothing here that could be released twice. */
    if (hr != S_OK && var && var->vt == VT_EMPTY) {
        const MicDev *d = BagDevice(self);

        /* The device's own alias, and only when that device is being renamed
         * at all. An alias handed to a device whose other door reads its name
         * as it stands would be two names for one device - the disagreement
         * that cost this plugin its microphone in the runs of 2026-09-19
         * 16:23 and 16:35. */
        if (d && (d->nonAscii || d->aliasNeeded) && AliasBstr(d->alias, var)) {
            if (WantProbe())
                Log("read: no name from the device; answering '%s'", d->alias);
            return S_OK;
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookBindToStorage(void *self, void *pbc,
                                                   void *left,
                                                   const GUID *riid,
                                                   void **ppv) {
    MicBindToStorage_t orig = (MicBindToStorage_t)OrigFor(self, VT_BIND_TO_STORAGE);
    HRESULT hr;

    if (!orig) return E_FAIL;
    hr = orig(self, pbc, left, riid, ppv);

    if (WantProbe()) {
        char what[64], alias[MIC_ALIAS_MAX];

        GuidText(riid, what, sizeof(what));
        MonAlias(self, alias, sizeof(alias));
        Log("BindToStorage(%p = %s, %s) hr=0x%08lX obj=%p", self, alias,
            what, (unsigned long)hr, ppv ? *ppv : NULL);
    }

    if (SUCCEEDED(hr) && ppv && *ppv && riid &&
        IsEqualGUID(riid, &kIID_IPropertyBag) && BagIsAudioCapture(*ppv)) {
        BagRegister(self, *ppv);
        SlotInstall(*(void ***)*ppv, VT_BAG_READ, (void *)HookBagRead,
                    "IPropertyBag::Read");
    }
    return hr;
}

/* Probe only: report what the game asks a moniker for and pass it through
 * untouched. The string GetDisplayName hands back is NOT ours to free - the
 * caller owns it, and the caller here is the game (see the block above
 * BagReadStr for the crash that taught this file the difference). */
static HRESULT STDMETHODCALLTYPE HookGetDisplayName(void *self, void *pbc,
                                                    void *left, WCHAR **out) {
    MicGetDisplayName_t orig =
        (MicGetDisplayName_t)OrigFor(self, VT_GET_DISPLAY_NAME);
    HRESULT hr;

    if (!orig) return E_FAIL;
    hr = orig(self, pbc, left, out);
    if (WantProbe()) {
        char alias[MIC_ALIAS_MAX], got[MIC_KEY_MAX];

        MonAlias(self, alias, sizeof(alias));
        got[0] = 0;
        if (SUCCEEDED(hr) && out && *out) WideToUtf8(*out, got, sizeof(got));
        Log("GetDisplayName(%p = %s) hr=0x%08lX -> '%s'", self, alias,
            (unsigned long)hr, got);
    }
    return hr;
}

/* Probe only: the game binding a moniker to the filter it will capture
 * with. This is the line that says WHICH device the game picked. */
static HRESULT STDMETHODCALLTYPE HookBindToObject(void *self, void *pbc,
                                                  void *left, const GUID *riid,
                                                  void **ppv) {
    MicBindToObject_t orig = (MicBindToObject_t)OrigFor(self, VT_BIND_TO_OBJECT);
    HRESULT hr;

    if (!orig) return E_FAIL;
    hr = orig(self, pbc, left, riid, ppv);
    if (WantProbe()) {
        char alias[MIC_ALIAS_MAX], what[64];

        MonAlias(self, alias, sizeof(alias));
        GuidText(riid, what, sizeof(what));
        Log("BindToObject(%p = %s, %s) hr=0x%08lX obj=%p", self, alias, what,
            (unsigned long)hr, ppv ? *ppv : NULL);
    }
    return hr;
}

static void PatchMoniker(void *moniker) {
    if (!moniker) return;
    SlotInstall(*(void ***)moniker, VT_BIND_TO_STORAGE,
                (void *)HookBindToStorage, "IMoniker::BindToStorage");

    /* The next two only exist to answer "what does the game do with the
     * monikers?" in the log, so they go in with the probe and not without
     * it: an installation that is not being diagnosed is not paying for
     * two more detours. */
    if (!WantProbe()) return;
    SlotInstall(*(void ***)moniker, VT_BIND_TO_OBJECT,
                (void *)HookBindToObject, "IMoniker::BindToObject");
    SlotInstall(*(void ***)moniker, VT_GET_DISPLAY_NAME,
                (void *)HookGetDisplayName, "IMoniker::GetDisplayName");

    /* Name it in the log before the game gets hold of it, so the two
     * logging detours above can say "Mic 2" instead of a bare pointer. */
    if (WantProbe()) {
        void *bag = MonikerBindBag(moniker);

        if (bag) {
            char key[MIC_KEY_MAX], alias[MIC_ALIAS_MAX];
            const MicDev *d = NULL;

            if (BagKey(bag, key, sizeof(key)) && (d = DevByKey(key)) != NULL)
                snprintf(alias, sizeof(alias), "%s", d->alias);
            else
                snprintf(alias, sizeof(alias), "(unnamed %p)", bag);
            MonoNote(moniker, alias);
            ObjRelease(bag);
        }
    }
}

/* ---- the enumerators that are the audio capture category --------------
 * IEnumMoniker is patched on its class, so this detour sees every moniker
 * enumeration in the process - the video categories, anything another mod
 * asks for. Only the enumerators that came out of
 * CreateClassEnumerator(CLSID_AudioInputDeviceCategory), plus the one this
 * plugin made for its own scan, are this plugin's business. Without that
 * gate, `force` would filter a list of cameras down to one microphone, and
 * every bag bound from such a list would be registered as a recording
 * device's.
 */
#define MIC_CAPENUM_MAX 8
static void *g_capEnum[MIC_CAPENUM_MAX];

static void NoteCaptureEnumerator(void *e) {
    int i;

    if (!e) return;
    Lock();
    for (i = 0; i < MIC_CAPENUM_MAX; i++) {
        if (g_capEnum[i] == e) { Unlock(); return; }
        if (!g_capEnum[i]) { g_capEnum[i] = e; Unlock(); return; }
    }
    Unlock();
}

static int IsCaptureEnumerator(void *e) {
    int i, found = 0;

    if (!e) return 0;
    Lock();
    for (i = 0; i < MIC_CAPENUM_MAX; i++)
        if (g_capEnum[i] == e) { found = 1; break; }
    Unlock();
    return found;
}

/* Enumerators that were handed the whole list because the picked device was
 * not in them. Once marked, the rest of that enumerator's calls go straight
 * through, which is what keeps a game loop from being handed the same first
 * device forever. */
static void *g_pass[MIC_PASS_MAX];

static int PassThrough(void *enumerator) {
    int i, found = 0;

    Lock();
    for (i = 0; i < MIC_PASS_MAX; i++)
        if (g_pass[i] == enumerator) { found = 1; break; }
    Unlock();
    return found;
}

static void MarkPassThrough(void *enumerator) {
    int i;

    Lock();
    for (i = 0; i < MIC_PASS_MAX; i++) {
        if (g_pass[i] == enumerator) { Unlock(); return; }
        if (!g_pass[i]) { g_pass[i] = enumerator; Unlock(); return; }
    }
    Unlock();
}

/* The picked device's key, copied out under the lock: the menu writes it on
 * one thread while the game's enumeration reads it on another, and a
 * half-written key is a device that stops matching. */
static void TargetKeyCopy(char *out, int cap) {
    Lock();
    snprintf(out, cap, "%s", g_targetKey);
    Unlock();
}

static int ForceActive(void) {
    char key[MIC_KEY_MAX];

    if (!WantForce()) return 0;
    TargetKeyCopy(key, sizeof(key));
    if (!key[0]) return 0;
    /* A key that is only a position or a pointer is not an identity: the
     * machine that produced it could not be asked for a real one, so there
     * is nothing to match and `force` stays out of the way. */
    if (!_strnicmp(key, "index:", 6) || !_strnicmp(key, "bag:", 4))
        return 0;
    return InterlockedCompareExchange(&g_targetOk, 0, 0) ? 1 : 0;
}

static int MonikerIsTarget(void *moniker) {
    void *bag = MonikerBindBag(moniker);
    char  key[MIC_KEY_MAX], want[MIC_KEY_MAX];
    int   hit = 0;

    if (!bag) return 0;
    TargetKeyCopy(want, sizeof(want));
    if (BagKey(bag, key, sizeof(key)))
        hit = _stricmp(key, want) == 0;
    ObjRelease(bag);
    return hit;
}

/* The list the game gets. Without `force`, the original one, with every
 * moniker patched on the way out. With it, only the picked device - and if
 * that device is not in this enumerator at all (unplugged since the scan),
 * the whole list, once, rather than an empty one. */
static HRESULT STDMETHODCALLTYPE HookEnumNext(void *self, ULONG celt,
                                              void **rgelt, ULONG *fetched) {
    MicNext_t orig = (MicNext_t)OrigFor(self, VT_ENUM_NEXT);
    ULONG i, got = 0, seen = 0;
    int   done = 0;

    if (!orig) return E_FAIL;

    /* Not an enumerator of the capture category: see g_capEnum. Handed
     * straight back, because this class is shared with every other moniker
     * enumeration in the process. Logged, because an enumerator nobody
     * recorded is also what a game that asked for its devices before this
     * plugin loaded would look like - and then the fix would be passing
     * through where it was supposed to answer. */
    if (!IsCaptureEnumerator(self)) {
        if (WantProbe())
            Log("enum: an enumerator of another category (or one made before "
                "this plugin loaded); left alone");
        return orig(self, celt, rgelt, fetched);
    }

    if (!ForceActive() || PassThrough(self)) {
        HRESULT hr = orig(self, celt, rgelt, fetched);

        if (SUCCEEDED(hr) && rgelt) {
            /* Without a count out, a partial answer is unreadable: S_OK
             * means the whole request was filled and S_FALSE means the
             * caller was told nothing about how much of it. */
            ULONG n = fetched ? *fetched : (hr == S_OK ? celt : 0);

            for (i = 0; i < n; i++) PatchMoniker(rgelt[i]);
        }
        return hr;
    }

    if (!celt) { if (fetched) *fetched = 0; return S_OK; }
    if (!rgelt) return E_POINTER;

    for (i = 0; i < celt && !done; ) {
        void   *mon = NULL;
        HRESULT hr = orig(self, 1, &mon, NULL);

        if (hr != S_OK || !mon) { done = 1; break; }
        seen++;
        if (MonikerIsTarget(mon)) {
            PatchMoniker(mon);
            rgelt[got++] = mon;
            i++;
        } else {
            /* Not the picked one: taken out of the list and released, and
             * the walk carries on. Stopping at the first one instead was
             * the bug that made `force` a no-op whenever the picked device
             * was not first in the enumeration. */
            ObjRelease(mon);
        }
    }

    if (got == 0 && seen > 0) {
        MicReset_t rst = (MicReset_t)OrigFor(self, VT_ENUM_RESET);
        HRESULT   hr;

        MarkPassThrough(self);
        Log("enum: the picked device is not in this enumerator; the full "
            "list is offered instead");
        if (rst) rst(self);
        hr = orig(self, celt, rgelt, fetched);
        if (SUCCEEDED(hr) && rgelt) {
            ULONG n = fetched ? *fetched : (hr == S_OK ? celt : 0);

            for (i = 0; i < n; i++) PatchMoniker(rgelt[i]);
        }
        return hr;
    }

    if (fetched) *fetched = got;
    return (got == celt) ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE HookCreateClassEnumerator(void *self,
                                                           const GUID *clsid,
                                                           void **ppEnum,
                                                           DWORD flags) {
    MicCreateClassEnum_t orig =
        (MicCreateClassEnum_t)OrigFor(self, VT_CREATE_CLASS_ENUMERATOR);
    HRESULT hr;

    if (!orig) return E_FAIL;
    hr = orig(self, clsid, ppEnum, flags);

    /* Only the capture devices: the same enumerator serves the video
     * categories, and those are none of this plugin's business. */
    if (hr == S_OK && ppEnum && *ppEnum && clsid &&
        IsEqualGUID(clsid, &kCLSID_AudioInputDeviceCategory)) {
        NoteCaptureEnumerator(*ppEnum);
        SlotInstall(*(void ***)*ppEnum, VT_ENUM_NEXT, (void *)HookEnumNext,
                    "IEnumMoniker::Next");
        if (WantProbe())
            Log("audio capture category enumerated by the game (enumerator "
                "%p)", *ppEnum);
    }
    return hr;
}

/* ---- the Core Audio door (IMMDevice / IPropertyStore) -----------------
 * The third place a recording device's name is read: the endpoint API the
 * Sound control panel itself uses. PKEY_Device_FriendlyName and
 * IPropertyBag sit within a few dozen bytes of each other in GRW.exe, and
 * a PROPERTYKEY is only needed by code that reads a name through
 * IMMDevice::OpenPropertyStore - the DirectShow route reads "FriendlyName"
 * as a plain string and never mentions a PROPERTYKEY at all. The chain is
 * patched the same way as the DirectShow one: the object CoCreateInstance
 * returns, then the object each call after it returns.
 */

static const GUID kCLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID kPKEY_Device_fmtid =
    { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } };
static const GUID kPKEY_DeviceInterface_fmtid =
    { 0x026E516E, 0xB814, 0x414B, { 0x83, 0xCD, 0x85, 0x6D, 0x6F, 0xEF, 0x48, 0x22 } };

#define MIC_PID_DEVICE_DEVICEDESC    2
#define MIC_PID_DEVICE_FRIENDLYNAME 14

#define VT_MM_ENUM_ENDPOINTS 3   /* IMMDeviceEnumerator::EnumAudioEndpoints */
#define VT_MM_GET_DEFAULT    4   /* IMMDeviceEnumerator::GetDefaultAudioEndpoint */
#define VT_MM_GET_DEVICE     5   /* IMMDeviceEnumerator::GetDevice          */
#define VT_MM_COLL_ITEM      4   /* IMMDeviceCollection::Item               */
#define VT_MM_ACTIVATE       3   /* IMMDevice::Activate                     */
#define VT_MM_OPEN_STORE     4   /* IMMDevice::OpenPropertyStore            */
#define VT_MM_GET_ID         5   /* IMMDevice::GetId                        */
#define VT_PS_GET_VALUE      5   /* IPropertyStore::GetValue                */
#define VT_PS_SET_VALUE      6   /* IPropertyStore::SetValue                */
#define VT_PS_COMMIT         7   /* IPropertyStore::Commit                  */
#define VT_AC_INITIALIZE     3   /* IAudioClient::Initialize                */
#define VT_AC_START         10   /* IAudioClient::Start                     */
#define VT_AC_GET_SERVICE   14   /* IAudioClient::GetService                */

typedef HRESULT (STDMETHODCALLTYPE *MicEnumEndpoints_t)(void *, int, DWORD, void **);
typedef HRESULT (STDMETHODCALLTYPE *MicCollItem_t)(void *, UINT, void **);
typedef HRESULT (STDMETHODCALLTYPE *MicGetDefault_t)(void *, int, int, void **);
typedef HRESULT (STDMETHODCALLTYPE *MicGetDevice_t)(void *, const WCHAR *, void **);
typedef HRESULT (STDMETHODCALLTYPE *MicActivate_t)(void *, const GUID *, DWORD,
                                                   void *, void **);
typedef HRESULT (STDMETHODCALLTYPE *MicOpenStore_t)(void *, DWORD, void **);
typedef HRESULT (STDMETHODCALLTYPE *MicGetId_t)(void *, WCHAR **);
typedef HRESULT (STDMETHODCALLTYPE *MicGetValue_t)(void *, const void *, void *);
typedef HRESULT (STDMETHODCALLTYPE *MicSetValue_t)(void *, const void *, const void *);
typedef HRESULT (STDMETHODCALLTYPE *MicCommit_t)(void *);
typedef HRESULT (STDMETHODCALLTYPE *MicAcInit_t)(void *, int, DWORD, INT64, INT64,
                                                 const void *, const GUID *);
typedef HRESULT (STDMETHODCALLTYPE *MicAcStart_t)(void *);
typedef HRESULT (STDMETHODCALLTYPE *MicAcGetService_t)(void *, const GUID *, void **);

/* The audio client family, as Activate is called with it. */
static const GUID kIID_IAudioClient  =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID kIID_IAudioClient2 =
    { 0x726778CD, 0xF60A, 0x4EDA, { 0x82, 0xDE, 0xE4, 0x76, 0x10, 0xCD, 0x78, 0xAA } };
static const GUID kIID_IAudioClient3 =
    { 0x7ED4EE07, 0x8E67, 0x4CD4, { 0x8C, 0x1A, 0x2B, 0x7A, 0x59, 0x87, 0xAD, 0x42 } };

/* A PROPERTYKEY, and the 24 bytes a VARIANT and a PROPVARIANT both live in,
 * so this file can read and replace a property store value without pulling
 * in propsys.h. On x64 both are vt at 0 and the value at 8. */
typedef struct MicPropertyKey { GUID fmtid; DWORD pid; } MicPropertyKey;
typedef struct MicPropVariant {
    WORD vt;
    WORD r1, r2, r3;
    union { BSTR bstrVal; WCHAR *pwszVal; void *p; int i; DWORD d; } u;
} MicPropVariant;

/* Put the alias where the old value was. The old one is not released, for
 * the same reason as in HookBagRead: a property store that answers with the
 * same string twice is left pointing at its own copy, not at freed memory.
 * The caller releases the alias this returns, on the contract that a wide
 * string is task-allocated and a BSTR is a BSTR. */
static int ReplacePropValue(MicPropVariant *v, const char *alias) {
    WCHAR w[MIC_ALIAS_MAX];
    int   n;

    /* Empty first and checked, for the same reason as in AliasBstr. */
    w[0] = 0;
    if (!alias || MultiByteToWideChar(CP_UTF8, 0, alias, -1, w,
                                      (int)ARRAY_LEN(w)) <= 0)
        return 0;
    n = lstrlenW(w);

    if (v->vt == VT_LPWSTR) {
        WCHAR *nb = (WCHAR *)CoTaskMemAlloc(((size_t)n + 1) * sizeof(WCHAR));

        if (!nb) return 0;
        memcpy(nb, w, ((size_t)n + 1) * sizeof(WCHAR));
        v->u.pwszVal = nb;
        return 1;
    }
    if (v->vt == VT_BSTR) {
        BSTR nb = SysAllocString(w);

        if (!nb) return 0;
        v->u.bstrVal = nb;
        return 1;
    }
    return 0;
}

static void PropValueText(const MicPropVariant *v, char *out, int cap) {
    out[0] = 0;
    if (!v) return;
    if (v->vt == VT_BSTR)        WideToUtf8(v->u.bstrVal, out, cap);
    else if (v->vt == VT_LPWSTR) WideToUtf8(v->u.pwszVal, out, cap);
    else if (v->vt == VT_I4 || v->vt == VT_UI4 || v->vt == VT_INT ||
             v->vt == VT_UINT)
        snprintf(out, cap, "%d", v->u.i);
    else if (v->vt == VT_UI2 || v->vt == VT_I2 || v->vt == VT_BOOL)
        snprintf(out, cap, "%u", (unsigned)(v->u.d & 0xFFFFu));  /* low half */
}

/* ---- which side of the audio stack a property store belongs to --------
 * IMMDevice::OpenPropertyStore is the one place where a store and its device
 * meet, and the device's own id says which side that device is on:
 * "{0.0.1.00000000}.{...}" is a recording endpoint and "{0.0.0.00000000}.
 * {...}" is a playback one.
 *
 * The run of 2026-09-19 15:59 is why this exists: without it the plugin
 * renamed the speakers - a playback device - and gave them the microphone's
 * own alias, "Mic1(Realtek High Definition Audio)", because one codec's
 * microphone and speakers share a controller name. A game that matches
 * devices by name then has two devices with one name, and it lost the
 * microphone.
 *
 * A store this plugin cannot place is never rewritten: a name left alone is
 * a name the game can still match. */
#define MIC_STORE_MAX 32

static struct {
    void *store;
    int   capture;
} g_storeOwner[MIC_STORE_MAX];

static void RememberStore(void *store, int capture) {
    int i;

    if (!store) return;
    Lock();
    for (i = 0; i < MIC_STORE_MAX; i++)
        if (g_storeOwner[i].store == store) {
            g_storeOwner[i].capture = capture;
            Unlock();
            return;
        }
    for (i = 0; i < MIC_STORE_MAX; i++)
        if (!g_storeOwner[i].store) {
            g_storeOwner[i].store   = store;
            g_storeOwner[i].capture = capture;
            break;
        }
    Unlock();
}

static int StoreIsCapture(void *store) {
    int i, capture = 0;

    if (!store) return 0;
    Lock();
    for (i = 0; i < MIC_STORE_MAX; i++)
        if (g_storeOwner[i].store == store) {
            capture = g_storeOwner[i].capture;
            break;
        }
    Unlock();
    return capture;
}

/* The answer is 0 - "not a recording device" - whenever the id is not there
 * or does not read the way a recording endpoint's does. */
static int DeviceIsCapture(void *dev) {
    MicGetId_t getId = (MicGetId_t)OrigFor(dev, VT_MM_GET_ID);
    WCHAR     *id    = NULL;
    int        capture = 0;

    if (getId && SUCCEEDED(getId(dev, &id)) && id) {
        /* Long enough to hold the prefix, before any of it is read: the id
         * comes from another module and is not this plugin's to trust. */
        capture = (lstrlenW(id) > 6 &&
                   id[0] == L'{' && id[1] == L'0' && id[2] == L'.' &&
                   id[3] == L'0' && id[4] == L'.' && id[5] == L'1' &&
                   id[6] == L'.');
        CoTaskMemFree(id);
    }
    return capture;
}

/* Does anything write a device's properties? Worth knowing twice over: a
 * property store that is written and committed renames the device in the
 * system, and the run of 2026-09-19 ended with a recording device whose own
 * name was this plugin's alias - a value this plugin only ever puts into a
 * caller's copy of it. Whoever wrote it, the log says so here. */
static HRESULT STDMETHODCALLTYPE HookStoreSetValue(void *self, const void *key,
                                                   const void *value) {
    MicSetValue_t         orig = (MicSetValue_t)OrigFor(self, VT_PS_SET_VALUE);
    const MicPropertyKey *pk = (const MicPropertyKey *)key;
    char                  t[64], txt[MIC_NAME_MAX];
    HRESULT               hr;

    if (!orig) return E_FAIL;
    hr = orig(self, key, value);
    GuidText(pk ? &pk->fmtid : NULL, t, sizeof(t));
    PropValueText((const MicPropVariant *)value, txt, sizeof(txt));
    Log("IPropertyStore::SetValue(%s pid %lu, vt=%u '%s') -> 0x%08lX", t,
        (unsigned long)(pk ? pk->pid : 0),
        (unsigned)(value ? ((const MicPropVariant *)value)->vt : 0), txt,
        (unsigned long)hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookStoreCommit(void *self) {
    MicCommit_t orig = (MicCommit_t)OrigFor(self, VT_PS_COMMIT);
    HRESULT     hr   = orig ? orig(self) : E_FAIL;

    Log("IPropertyStore::Commit() -> 0x%08lX", (unsigned long)hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookStoreGetValue(void *self, const void *key,
                                                   void *value) {
    MicGetValue_t         orig = (MicGetValue_t)OrigFor(self, VT_PS_GET_VALUE);
    const MicPropertyKey *pk = (const MicPropertyKey *)key;
    MicPropVariant       *v  = (MicPropVariant *)value;
    char                  alias[MIC_ALIAS_MAX];
    HRESULT               hr;
    int                   nameish = 0;

    if (!orig) return E_FAIL;
    hr = orig(self, key, value);

    if (pk) {
        if (IsEqualGUID(&pk->fmtid, &kPKEY_Device_fmtid) &&
            (pk->pid == MIC_PID_DEVICE_FRIENDLYNAME ||
             pk->pid == MIC_PID_DEVICE_DEVICEDESC))
            nameish = 1;
        else if (IsEqualGUID(&pk->fmtid, &kPKEY_DeviceInterface_fmtid))
            nameish = 1;
    }

    /* Every property a device is asked for, not only the names: what the
     * game looks at while deciding whether the microphone is usable is the
     * thing this plugin has never been able to see. */
    if (WantProbe() && pk) {
        char t[64], txt[MIC_NAME_MAX];

        GuidText(&pk->fmtid, t, sizeof(t));
        PropValueText((const MicPropVariant *)value, txt, sizeof(txt));
        Log("property store %s pid %lu -> hr=0x%08lX vt=%u '%s'%s", t,
            (unsigned long)pk->pid, (unsigned long)hr,
            (unsigned)(v ? v->vt : 0), txt, nameish ? "   (a name)" : "");
    }

    if (!nameish || !WantFix()) return hr;

    /* Recording devices only. A playback endpoint is renamed by no door of
     * this plugin - see g_storeOwner. */
    if (!StoreIsCapture(self)) {
        if (WantProbe())
            Log("property store: not a recording device's store - left as it "
                "is");
        return hr;
    }

    if (SUCCEEDED(hr) && v &&
        ((v->vt == VT_BSTR   && IsOurAlias(v->u.bstrVal)) ||
         (v->vt == VT_LPWSTR && IsOurAlias(v->u.pwszVal)))) {
        if (WantProbe())
            Log("property store: an alias came back round; left as it is");
        return hr;
    }

    if (SUCCEEDED(hr) && v &&
        ((v->vt == VT_BSTR   && AliasFor(v->u.bstrVal, alias, sizeof(alias))) ||
         (v->vt == VT_LPWSTR && AliasFor(v->u.pwszVal, alias, sizeof(alias))))) {
        if (WantProbe()) {
            char was[MIC_NAME_MAX];

            was[0] = 0;
            if (v->vt == VT_BSTR) WideToUtf8(v->u.bstrVal, was, sizeof(was));
            else                  WideToUtf8(v->u.pwszVal, was, sizeof(was));
            Log("property store (pid %lu): '%s' -> '%s'",
                (unsigned long)(pk ? pk->pid : 0), was, alias);
        }
        ReplacePropValue(v, alias);
        return hr;
    }

    return hr;   /* every call was already logged on the way in */
}

/* Opening the endpoint. This is where a capture client would be created, so
 * the log can say whether the game got as far as activating the device - the
 * one step the first probe logs could never see. */
static void NoteAudioClient(void *client);

static HRESULT STDMETHODCALLTYPE HookActivate(void *self, const GUID *iid,
                                              DWORD flags, void *params,
                                              void **out) {
    MicActivate_t orig = (MicActivate_t)OrigFor(self, VT_MM_ACTIVATE);
    HRESULT       hr;
    char          t[64];

    if (!orig) return E_FAIL;
    hr = orig(self, iid, flags, params, out);
    GuidText(iid, t, sizeof(t));
    Log("IMMDevice::Activate(%s, flags=0x%lX) -> 0x%08lX obj=%p", t,
        (unsigned long)flags, (unsigned long)hr, (out ? *out : NULL));

    /* An audio client of any version: follow it, so the log can say whether
     * a stream is ever created on it and what service it asks for. */
    if (SUCCEEDED(hr) && out && *out && iid &&
        (IsEqualGUID(iid, &kIID_IAudioClient) ||
         IsEqualGUID(iid, &kIID_IAudioClient2) ||
         IsEqualGUID(iid, &kIID_IAudioClient3)))
        NoteAudioClient(*out);
    return hr;
}

/* The audio client itself, once Activate has handed one back: whether a
 * client is ever opened on a capture endpoint is the question every probe
 * so far has ended without an answer for. GetService is the decisive line -
 * it is where a capture stream asks for IAudioCaptureClient. */
static HRESULT STDMETHODCALLTYPE HookAcInitialize(void *self, int share,
                                                  DWORD flags, INT64 dur,
                                                  INT64 period, const void *fmt,
                                                  const GUID *session) {
    MicAcInit_t orig = (MicAcInit_t)OrigFor(self, VT_AC_INITIALIZE);
    HRESULT     hr;

    if (!orig) return E_FAIL;
    hr = orig(self, share, flags, dur, period, fmt, session);
    Log("IAudioClient::Initialize(share=%d flags=0x%lX) -> 0x%08lX", share,
        (unsigned long)flags, (unsigned long)hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookAcGetService(void *self, const GUID *iid,
                                                  void **out) {
    MicAcGetService_t orig = (MicAcGetService_t)OrigFor(self, VT_AC_GET_SERVICE);
    HRESULT           hr;
    char              t[64];

    if (!orig) return E_FAIL;
    hr = orig(self, iid, out);
    GuidText(iid, t, sizeof(t));
    Log("IAudioClient::GetService(%s) -> 0x%08lX obj=%p", t, (unsigned long)hr,
        (out ? *out : NULL));
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookAcStart(void *self) {
    MicAcStart_t orig = (MicAcStart_t)OrigFor(self, VT_AC_START);
    HRESULT      hr   = orig ? orig(self) : E_FAIL;

    Log("IAudioClient::Start() -> 0x%08lX", (unsigned long)hr);
    return hr;
}

static void NoteAudioClient(void *client) {
    if (!client) return;
    SlotInstall(*(void ***)client, VT_AC_INITIALIZE,
                (void *)HookAcInitialize, "IAudioClient::Initialize");
    SlotInstall(*(void ***)client, VT_AC_GET_SERVICE,
                (void *)HookAcGetService, "IAudioClient::GetService");
    SlotInstall(*(void ***)client, VT_AC_START,
                (void *)HookAcStart, "IAudioClient::Start");
}

static HRESULT STDMETHODCALLTYPE HookOpenPropertyStore(void *self, DWORD access,
                                                       void **store) {
    MicOpenStore_t orig = (MicOpenStore_t)OrigFor(self, VT_MM_OPEN_STORE);
    HRESULT        hr;
    int            capture;

    if (!orig) return E_FAIL;

    /* Asked before the call, while there is no store yet to be confused
     * with: this is the device the store will belong to. */
    capture = DeviceIsCapture(self);

    hr = orig(self, access, store);
    if (SUCCEEDED(hr) && store && *store) {
        SlotInstall(*(void ***)*store, VT_PS_GET_VALUE,
                    (void *)HookStoreGetValue, "IPropertyStore::GetValue");
        SlotInstall(*(void ***)*store, VT_PS_SET_VALUE,
                    (void *)HookStoreSetValue, "IPropertyStore::SetValue");
        SlotInstall(*(void ***)*store, VT_PS_COMMIT,
                    (void *)HookStoreCommit, "IPropertyStore::Commit");
        RememberStore(*store, capture);
        if (!capture && WantProbe())
            Log("property store of a playback device (%p) - its names are "
                "never rewritten", *store);
    }
    return hr;
}

static void NoteEndpoint(void *dev, const char *why) {
    if (!dev) return;
    SlotInstall(*(void ***)dev, VT_MM_OPEN_STORE,
                (void *)HookOpenPropertyStore, why);
    SlotInstall(*(void ***)dev, VT_MM_ACTIVATE,
                (void *)HookActivate, "IMMDevice::Activate");
}

static HRESULT STDMETHODCALLTYPE HookCollectionItem(void *self, UINT index,
                                                    void **dev) {
    MicCollItem_t orig = (MicCollItem_t)OrigFor(self, VT_MM_COLL_ITEM);
    HRESULT       hr;

    if (!orig) return E_FAIL;
    hr = orig(self, index, dev);
    if (SUCCEEDED(hr) && dev && *dev)
        NoteEndpoint(*dev, "IMMDevice::OpenPropertyStore (collection item)");
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookEnumAudioEndpoints(void *self, int flow,
                                                        DWORD mask,
                                                        void **coll) {
    MicEnumEndpoints_t orig =
        (MicEnumEndpoints_t)OrigFor(self, VT_MM_ENUM_ENDPOINTS);
    HRESULT hr;

    if (!orig) return E_FAIL;
    hr = orig(self, flow, mask, coll);
    if (WantProbe())
        Log("EnumAudioEndpoints(flow=%d mask=0x%lX) -> 0x%08lX", flow,
            (unsigned long)mask, (unsigned long)hr);
    if (SUCCEEDED(hr) && coll && *coll)
        SlotInstall(*(void ***)*coll, VT_MM_COLL_ITEM,
                    (void *)HookCollectionItem, "IMMDeviceCollection::Item");
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookGetDefaultAudioEndpoint(void *self,
                                                             int flow, int role,
                                                             void **dev) {
    MicGetDefault_t orig = (MicGetDefault_t)OrigFor(self, VT_MM_GET_DEFAULT);
    HRESULT         hr;

    if (!orig) return E_FAIL;
    hr = orig(self, flow, role, dev);
    if (WantProbe())
        Log("GetDefaultAudioEndpoint(flow=%d role=%d) -> 0x%08lX obj=%p", flow,
            role, (unsigned long)hr, (dev ? *dev : NULL));
    if (SUCCEEDED(hr) && dev && *dev)
        NoteEndpoint(*dev, "IMMDevice::OpenPropertyStore (default endpoint)");
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookGetDevice(void *self, const WCHAR *id,
                                               void **dev) {
    MicGetDevice_t orig = (MicGetDevice_t)OrigFor(self, VT_MM_GET_DEVICE);
    HRESULT        hr;

    if (!orig) return E_FAIL;
    hr = orig(self, id, dev);
    if (WantProbe())
        Log("GetDevice('%ls') -> 0x%08lX obj=%p", id ? id : L"",
            (unsigned long)hr, (dev ? *dev : NULL));
    if (SUCCEEDED(hr) && dev && *dev)
        NoteEndpoint(*dev, "IMMDevice::OpenPropertyStore (GetDevice)");
    return hr;
}

/* The one MinHook target. A fast path, because every caller in the process
 * comes through here: one GUID compare, and the original call out. */
static HRESULT WINAPI HookCoCreateInstance(const GUID *rclsid, void *outer,
                                           DWORD ctx, const GUID *riid,
                                           void **ppv) {
    MicCoCreateInstance_t orig = g_realCoCreateInstance;
    HRESULT hr;

    if (!orig) return E_FAIL;
    if (WantProbe()) NoteClsid(rclsid);
    hr = orig(rclsid, outer, ctx, riid, ppv);
    if (FAILED(hr) || !ppv || !*ppv || !rclsid) return hr;

    if (IsEqualGUID(rclsid, &kCLSID_SystemDeviceEnum)) {
        SlotInstall(*(void ***)*ppv, VT_CREATE_CLASS_ENUMERATOR,
                    (void *)HookCreateClassEnumerator,
                    "ICreateDevEnum::CreateClassEnumerator (CoCreateInstance)");
    } else if (IsEqualGUID(rclsid, &kCLSID_MMDeviceEnumerator)) {
        SlotInstall(*(void ***)*ppv, VT_MM_ENUM_ENDPOINTS,
                    (void *)HookEnumAudioEndpoints,
                    "IMMDeviceEnumerator::EnumAudioEndpoints");
        SlotInstall(*(void ***)*ppv, VT_MM_GET_DEFAULT,
                    (void *)HookGetDefaultAudioEndpoint,
                    "IMMDeviceEnumerator::GetDefaultAudioEndpoint");
        SlotInstall(*(void ***)*ppv, VT_MM_GET_DEVICE,
                    (void *)HookGetDevice, "IMMDeviceEnumerator::GetDevice");
    }
    return hr;
}

/* ---- the legacy waveIn door ------------------------------------------
 * The capture device has two names in this process. The DirectShow moniker
 * above is one; winmm's waveIn API is the other, and it names the same
 * device out of the same registry value. A game that finds its microphone
 * by matching one against the other sees a mismatch the moment only one of
 * the two is aliased - and that is what the run of 2026-09-19 showed: the
 * game enumerated the devices, read the aliases, and never opened one.
 * Both doors answer with the same alias here, matched through the WaveInID
 * the property bag carries.
 *
 * These entry points are reached by GetProcAddress rather than by import
 * (no module in the game folder imports winmm's waveIn functions), which is
 * exactly why the first probe log could say the game read the names and
 * nothing more: whatever it opens the device with is not an import.
 */

#define MIC_PNAME_LEN 32   /* MAXPNAMELEN */

typedef struct MicWaveInCapsW {
    WORD  wMid;
    WORD  wPid;
    UINT  vDriverVersion;
    WCHAR szPname[MIC_PNAME_LEN];
    DWORD dwFormats;
    WORD  wChannels;
    WORD  wReserved1;
} MicWaveInCapsW;

/* The A variant is a different struct, not the same one with a different
 * string type: its name field is half the size, so dwFormats sits at a
 * different offset. */
typedef struct MicWaveInCapsA {
    WORD  wMid;
    WORD  wPid;
    UINT  vDriverVersion;
    char  szPname[MIC_PNAME_LEN];
    DWORD dwFormats;
    WORD  wChannels;
    WORD  wReserved1;
} MicWaveInCapsA;

typedef UINT (WINAPI *MicWaveInNum_t)(void);
typedef UINT (WINAPI *MicWaveInCapsW_t)(UINT_PTR, MicWaveInCapsW *, UINT);
typedef UINT (WINAPI *MicWaveInCapsA_t)(UINT_PTR, MicWaveInCapsA *, UINT);
typedef UINT (WINAPI *MicWaveInOpen_t)(void **, UINT, const void *, DWORD_PTR,
                                       DWORD_PTR, DWORD);
typedef UINT (WINAPI *MicWaveInHandle_t)(void *);

static MicWaveInNum_t    g_waveInNum;
static MicWaveInCapsW_t  g_waveInCapsW;
static MicWaveInCapsA_t  g_waveInCapsA;
static MicWaveInOpen_t   g_waveInOpen;
static MicWaveInHandle_t g_waveInStart;
static MicWaveInHandle_t g_waveInStop;
static MicWaveInHandle_t g_waveInClose;

/* The alias into a waveIn name field, W or A. Both are capped at
 * MIC_PNAME_LEN by the API's own contract, so the copy is bounded by the
 * destination, never by the alias. */
static void WaveInAliasW(char *alias, WCHAR *out) {
    WCHAR w[MIC_ALIAS_MAX];

    /* Empty first and checked, or a failed conversion leaves lstrcpynW
     * reading an uninitialised buffer past its end. */
    w[0] = 0;
    if (alias && MultiByteToWideChar(CP_UTF8, 0, alias, -1, w,
                                     (int)ARRAY_LEN(w)) <= 0)
        w[0] = 0;
    lstrcpynW(out, w, MIC_PNAME_LEN);
}

static UINT WINAPI HookWaveInGetNumDevs(void) {
    UINT n = g_waveInNum ? g_waveInNum() : 0;

    if (WantProbe()) Log("waveInGetNumDevs() -> %u", n);
    return n;
}

/* The alias for the name this door is about to answer with, found the same
 * way the other two doors find theirs - and through the same name-to-alias
 * memory, so all three hand the game one string per device.
 *
 * Matching by WaveInID alone was not enough, and that is the defect of
 * 2026-09-21: the ID is only in the scan's table, and the table was written
 * from reads this plugin had already aliased (see HookBagRead), so a device
 * could sit there with its ID recorded and "has a non-ASCII name" never set -
 * which is exactly what DevByWaveInId requires. Every device of that kind,
 * and every device one of the other doors cannot cover at all - a disabled
 * one among them - kept its non-ASCII name on this door, so the game saw two
 * names for one device and offered no microphone. */
static int WaveInAliasFor(const WCHAR *name, UINT_PTR id, char *out, int cap) {
    if (name && name[0] && AliasFor(name, out, cap)) return 1;
    return DevByWaveInId((int)id, out, cap);
}

/* The A door's name is in the ANSI code page - GBK on a Chinese Windows -
 * so it is widened before the same matching runs. 0 when there is nothing to
 * match on, and the caller falls back to the ID. */
static int WaveInNameWide(const char *a, WCHAR *out, int cap) {
    out[0] = 0;
    if (a && a[0] && MultiByteToWideChar(CP_ACP, 0, a, -1, out, cap) <= 0)
        out[0] = 0;
    return out[0] != 0;
}

static UINT WINAPI HookWaveInGetDevCapsW(UINT_PTR id, MicWaveInCapsW *caps,
                                         UINT cb) {
    UINT r = g_waveInCapsW ? g_waveInCapsW(id, caps, cb) : 1;
    char alias[MIC_ALIAS_MAX];
    int  known = 0;

    if (r == 0 && caps && cb >= sizeof(MicWaveInCapsW))
        known = WaveInAliasFor(caps->szPname, id, alias, sizeof(alias));

    /* The name is logged before it is replaced, so the probe log carries what
     * the system said as well as what the game is given. */
    if (WantProbe())
        Log("waveInGetDevCapsW(id=%d) -> %u name='%ls'%s", (int)id, r,
            (r == 0 && caps) ? caps->szPname : L"",
            known ? "  (aliased)" : "");
    if (known && WantFix()) {
        WaveInAliasW(alias, caps->szPname);
        if (WantProbe())
            Log("waveInGetDevCapsW(id=%d): the name is answered as '%s'",
                (int)id, alias);
    }
    return r;
}

static UINT WINAPI HookWaveInGetDevCapsA(UINT_PTR id, MicWaveInCapsA *caps,
                                         UINT cb) {
    UINT r = g_waveInCapsA ? g_waveInCapsA(id, caps, cb) : 1;
    char alias[MIC_ALIAS_MAX];
    int  known = 0;

    if (r == 0 && caps && cb >= sizeof(MicWaveInCapsA)) {
        WCHAR w[MIC_NAME_MAX];

        /* The A variant's name is one byte per character in the ANSI code
         * page, so it is widened before it goes into the same matching the W
         * door uses. An empty or unconvertible name falls back to the ID. */
        known = WaveInNameWide(caps->szPname, w, (int)ARRAY_LEN(w))
                    ? WaveInAliasFor(w, id, alias, sizeof(alias))
                    : DevByWaveInId((int)id, alias, sizeof(alias));
    }

    if (WantProbe())
        Log("waveInGetDevCapsA(id=%d) -> %u name='%s'%s", (int)id, r,
            (r == 0 && caps) ? caps->szPname : "",
            known ? "  (aliased)" : "");
    if (known && WantFix()) {
        snprintf(caps->szPname, MIC_PNAME_LEN, "%s", alias);
        if (WantProbe())
            Log("waveInGetDevCapsA(id=%d): the name is answered as '%s'",
                (int)id, alias);
    }
    return r;
}

/* Whether the game opens a device at all, and which one, is the question
 * the first probe log could not answer, so these three say it without
 * waiting to be asked. */
static UINT WINAPI HookWaveInOpen(void **hwi, UINT id, const void *fmt,
                                  DWORD_PTR callback, DWORD_PTR inst,
                                  DWORD flags) {
    char alias[MIC_ALIAS_MAX];
    int  known = DevByWaveInId((int)id, alias, sizeof(alias));
    UINT r = g_waveInOpen ? g_waveInOpen(hwi, id, fmt, callback, inst, flags) : 1;

    Log("waveInOpen(id=%d%s%s, flags=0x%lX) -> %u hwi=%p", (int)id,
        known ? " = " : "", known ? alias : "", (unsigned long)flags, r,
        (hwi ? *hwi : NULL));
    return r;
}

static UINT WINAPI HookWaveInStart(void *hwi) {
    UINT r = g_waveInStart ? g_waveInStart(hwi) : 1;

    Log("waveInStart(%p) -> %u", hwi, r);
    return r;
}

static UINT WINAPI HookWaveInStop(void *hwi) {
    UINT r = g_waveInStop ? g_waveInStop(hwi) : 1;

    Log("waveInStop(%p) -> %u", hwi, r);
    return r;
}

static UINT WINAPI HookWaveInClose(void *hwi) {
    UINT r = g_waveInClose ? g_waveInClose(hwi) : 1;

    Log("waveInClose(%p) -> %u", hwi, r);
    return r;
}

static void InstallWaveInHooks(void) {
    static const struct {
        const char *name;
        void       *detour;
        void      **original;
    } table[] = {
        { "waveInGetNumDevs", (void *)HookWaveInGetNumDevs, (void **)&g_waveInNum    },
        { "waveInGetDevCapsW", (void *)HookWaveInGetDevCapsW, (void **)&g_waveInCapsW },
        { "waveInGetDevCapsA", (void *)HookWaveInGetDevCapsA, (void **)&g_waveInCapsA },
        { "waveInOpen", (void *)HookWaveInOpen, (void **)&g_waveInOpen },
        { "waveInStart", (void *)HookWaveInStart, (void **)&g_waveInStart },
        { "waveInStop", (void *)HookWaveInStop, (void **)&g_waveInStop },
        { "waveInClose", (void *)HookWaveInClose, (void **)&g_waveInClose }
    };
    HMODULE winmm = GetModuleHandleA("winmm.dll");
    int     i;

    if (!winmm) winmm = LoadLibraryA("winmm.dll");
    if (!winmm) {
        Log("winmm.dll is not loaded: the waveIn names stay as they are");
        return;
    }

    for (i = 0; i < (int)ARRAY_LEN(table); i++) {
        void     *fn = (void *)GetProcAddress(winmm, table[i].name);
        MH_STATUS st;

        if (!fn) continue;
        st = MH_CreateHook(fn, table[i].detour, table[i].original);
        if (st != MH_OK) {
            Log("MH_CreateHook(winmm!%s): %s", table[i].name,
                MH_StatusToString(st));
            continue;
        }
        st = MH_EnableHook(fn);
        if (st != MH_OK) {
            /* MinHook tears the hook down without touching the global, and
             * the trampoline it handed back goes with it: a later call
             * through that pointer would be a jump into freed memory. */
            Log("MH_EnableHook(winmm!%s): %s", table[i].name,
                MH_StatusToString(st));
            MH_RemoveHook(fn);
            *(table[i].original) = NULL;
            continue;
        }
        Log("hook installed: winmm!%s at %p", table[i].name, fn);
    }
}

/* ---- the game's own diagnostics ---------------------------------------
 * Every probe has ended at the same place: the game reads the microphone's
 * name and then never opens it. No audio API call says why. The game itself
 * might: the voice chat is Ubisoft's Echo module (storm::echo::
 * ChatterManager, with an "Unable to start Voice Chat. Error:" path in the
 * binary), and an engine logger reaches the debug channel, which a process
 * can listen to from inside. What it says goes to logs\micfix_dbg.log so
 * the probe log does not drown in it. Bounded, and only while probing.
 *
 * ntdll's DbgPrint is left alone on purpose: it is a varargs function, and
 * a detour that cannot forward its arguments would cost the game its debug
 * output for no gain.
 */

#define MIC_DBG_LINES 4000

static void (WINAPI *g_realOdsA)(const char *);
static void (WINAPI *g_realOdsW)(const WCHAR *);
static FILE  *g_dbgFile;
static LONG   g_dbgLines;

static void DbgWrite(const char *text) {
    char path[MAX_PATH];

    if (!text || !text[0] || !WantProbe()) return;
    if (!g_dbgFile) {
        if (!ShLogPath("micfix_dbg.log", path, (int)ARRAY_LEN(path))) return;
        g_dbgFile = fopen(path, "w");
        if (!g_dbgFile) return;
    }
    if (InterlockedIncrement(&g_dbgLines) > MIC_DBG_LINES) return;

    Lock();
    fprintf(g_dbgFile, "%s\n", text);
    fflush(g_dbgFile);
    Unlock();
}

static void WINAPI HookOutputDebugStringA(const char *text) {
    DbgWrite(text);
    if (g_realOdsA) g_realOdsA(text);
}

static void WINAPI HookOutputDebugStringW(const WCHAR *text) {
    char u8[1024];

    WideToUtf8(text, u8, sizeof(u8));
    DbgWrite(u8);
    if (g_realOdsW) g_realOdsW(text);
}

static void InstallDebugHooks(void) {
    static const struct {
        const char *name;
        void       *detour;
        void      **original;
    } table[] = {
        { "OutputDebugStringA", (void *)HookOutputDebugStringA, (void **)&g_realOdsA },
        { "OutputDebugStringW", (void *)HookOutputDebugStringW, (void **)&g_realOdsW }
    };
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    int     i;

    if (!k32) return;
    for (i = 0; i < (int)ARRAY_LEN(table); i++) {
        void     *fn = (void *)GetProcAddress(k32, table[i].name);
        MH_STATUS st;

        if (!fn) continue;
        st = MH_CreateHook(fn, table[i].detour, table[i].original);
        if (st != MH_OK) continue;
        if (MH_EnableHook(fn) != MH_OK) {
            MH_RemoveHook(fn);
            *(table[i].original) = NULL;
            continue;
        }
        Log("hook installed: kernel32!%s (the game's own log goes to micfix_dbg.log)",
            table[i].name);
    }
}

/* ---- install / uninstall --------------------------------------------- */

static void *g_cciTarget;

static void InstallHooks(void) {
    MH_STATUS st;
    HMODULE   ole32 = GetModuleHandleA("ole32.dll");

    if (!ole32) ole32 = LoadLibraryA("ole32.dll");
    if (!ole32) {
        Log("ole32.dll is not loaded: the fix is off this session");
        return;
    }
    g_cciTarget = (void *)GetProcAddress(ole32, "CoCreateInstance");
    if (!g_cciTarget) {
        Log("ole32!CoCreateInstance was not found: the fix is off");
        return;
    }

    st = MH_Initialize();
    if (st != MH_OK) {
        Log("MH_Initialize: %s", MH_StatusToString(st));
        g_cciTarget = NULL;
        return;
    }

    st = MH_CreateHook(g_cciTarget, (void *)HookCoCreateInstance,
                       (void **)&g_realCoCreateInstance);
    if (st == MH_OK) {
        MH_STATUS en = MH_EnableHook(g_cciTarget);

        if (en != MH_OK) {
            Log("MH_EnableHook(CoCreateInstance): %s", MH_StatusToString(en));
            MH_RemoveHook(g_cciTarget);
            g_realCoCreateInstance = NULL;
        } else {
            Log("hook installed: ole32!CoCreateInstance at %p", g_cciTarget);
        }
    } else {
        Log("MH_CreateHook(CoCreateInstance): %s", MH_StatusToString(st));
        g_realCoCreateInstance = NULL;
    }

    InstallWaveInHooks();
    InstallDebugHooks();
}

static void UninstallHooks(void) {
    /* Every hook, not only the one this file keeps a target for. The waveIn
     * and debug detours are torn down by MH_Uninitialize in any case, but that
     * leaves the pointers this file calls through aimed at freed trampolines;
     * removing them here and clearing the pointers means nothing this plugin
     * owns can call into a trampoline that no longer exists. */
    static const char *const others[] = {
        "waveInGetNumDevs", "waveInGetDevCapsW", "waveInGetDevCapsA",
        "waveInOpen", "waveInStart", "waveInStop", "waveInClose",
        "OutputDebugStringA", "OutputDebugStringW"
    };
    HMODULE mm  = GetModuleHandleA("winmm.dll");
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    int     i;

    if (g_cciTarget) {
        MH_DisableHook(g_cciTarget);
        MH_RemoveHook(g_cciTarget);
        g_cciTarget = NULL;
        g_realCoCreateInstance = NULL;
    }

    for (i = 0; i < (int)ARRAY_LEN(others); i++) {
        void *fn = NULL;

        if (mm)  fn = (void *)GetProcAddress(mm, others[i]);
        if (!fn && k32) fn = (void *)GetProcAddress(k32, others[i]);
        if (!fn) continue;
        MH_DisableHook(fn);
        MH_RemoveHook(fn);
    }

    g_waveInNum   = NULL;
    g_waveInCapsW = NULL;
    g_waveInCapsA = NULL;
    g_waveInOpen  = NULL;
    g_waveInStart = NULL;
    g_waveInStop  = NULL;
    g_waveInClose = NULL;
    g_realOdsA    = NULL;
    g_realOdsW    = NULL;

    MH_Uninitialize();
    RestoreSlots();
}

/* ---- the scan ---------------------------------------------------------
 * The plugin's own walk of the chain, through the saved originals so that
 * `force` filtering does not hide the other devices from the menu. It also
 * patches the enumerator's vtable, which is how the game's own enumerators
 * end up intercepted even if the CoCreateInstance hook never saw them.
 */
static int ScanDevicesRaw(void) {
    static MicDev tmp[MIC_DEV_MAX];
    MicCoCreateInstance_t cci = g_realCoCreateInstance;
    MicCreateClassEnum_t  cce;
    MicNext_t             next;
    void *pEnum = NULL, *pMonEnum = NULL;
    HRESULT hr;
    int n = 0, i;

    if (!cci)
        cci = (MicCoCreateInstance_t)(void *)GetProcAddress(
                  GetModuleHandleA("ole32.dll"), "CoCreateInstance");
    if (!cci) { Log("scan: CoCreateInstance is not reachable"); return 0; }

    hr = cci(&kCLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER,
             &kIID_ICreateDevEnum, &pEnum);
    if (FAILED(hr) || !pEnum) {
        Log("scan: the system device enumerator failed (0x%08lX)",
            (unsigned long)hr);
        return 0;
    }

    SlotInstall(*(void ***)pEnum, VT_CREATE_CLASS_ENUMERATOR,
                (void *)HookCreateClassEnumerator,
                "ICreateDevEnum::CreateClassEnumerator (scan)");

    cce = (MicCreateClassEnum_t)OrigFor(pEnum, VT_CREATE_CLASS_ENUMERATOR);
    hr = cce ? cce(pEnum, &kCLSID_AudioInputDeviceCategory, &pMonEnum, 0)
             : E_FAIL;
    if (hr != S_OK || !pMonEnum) {
        Log("scan: no recording devices (create class enumerator 0x%08lX)",
            (unsigned long)hr);
        ObjRelease(pEnum);
        Lock();
        InterlockedExchange(&g_ndev, 0);
        InterlockedExchange(&g_targetOk, 0);
        Unlock();
        return 0;
    }

    SlotInstall(*(void ***)pMonEnum, VT_ENUM_NEXT, (void *)HookEnumNext,
                "IEnumMoniker::Next (scan)");
    NoteCaptureEnumerator(pMonEnum);   /* the scan asked the capture category */

    next = (MicNext_t)OrigFor(pMonEnum, VT_ENUM_NEXT);
    for (;;) {
        void  *mon = NULL;
        void  *bag;
        WCHAR  friendly[MIC_NAME_MAX];
        WCHAR  desc[MIC_NAME_MAX];

        if (!next || next(pMonEnum, 1, &mon, NULL) != S_OK || !mon) break;

        friendly[0] = 0;
        desc[0] = 0;
        /* Every field of the row, not most of them: tmp is static and a
         * rescan reuses it, so anything left over from the previous scan is
         * one device carrying another device's data. `core` was the field
         * that got away - a device whose FriendlyName read failed this time
         * kept the core of whatever sat in this slot before, and
         * DevIndexByCore then paired it with the wrong name. */
        tmp[n].nonAscii   = 0;
        tmp[n].aliasNeeded = 0;
        tmp[n].key[0]     = 0;
        tmp[n].name[0]    = 0;
        tmp[n].core[0]    = 0;
        tmp[n].waveInId   = -1;

        bag = MonikerBindBag(mon);
        if (bag) {
            if (!BagKey(bag, tmp[n].key, sizeof(tmp[n].key)))
                snprintf(tmp[n].key, sizeof(tmp[n].key), "index:%d", n + 1);
            if (!BagDesc(bag, desc, (int)ARRAY_LEN(desc))) desc[0] = 0;
            BagReadLong(bag, L"WaveInID", &tmp[n].waveInId);
            if (BagReadStr(bag, L"FriendlyName", friendly,
                           (int)ARRAY_LEN(friendly))) {
                WideToUtf8(friendly, tmp[n].name, sizeof(tmp[n].name));
                tmp[n].nonAscii = !IsAsciiOnly(friendly);
                NameCore(friendly, tmp[n].core, sizeof(tmp[n].core));
            }
            if (WantProbe()) {
                /* Every key a DirectShow device moniker is known to carry,
                 * so the log says whether this bag is empty or whether it
                 * is a bag of a different shape. BagReadStr logs each one. */
                static const WCHAR *const props[] = {
                    L"FriendlyName", L"Description", L"DevicePath",
                    L"WaveInID", L"CLSID"
                };
                WCHAR scratch[MIC_NAME_MAX];
                int   k;

                for (k = 0; k < (int)ARRAY_LEN(props); k++)
                    BagReadStr(bag, props[k], scratch,
                               (int)ARRAY_LEN(scratch));
            }
            ObjRelease(bag);
        } else {
            snprintf(tmp[n].key, sizeof(tmp[n].key), "index:%d", n + 1);
        }

        MakeNames(n + 1, desc, tmp[n].alias, sizeof(tmp[n].alias),
                  tmp[n].label, sizeof(tmp[n].label));

        Log("scan: %d. alias='%s'  label='%s'  name='%s'%s  key='%s'",
            n + 1, tmp[n].alias, tmp[n].label, tmp[n].name,
            tmp[n].nonAscii ? "  [non-ASCII]" : "", tmp[n].key);

        ObjRelease(mon);
        n++;
        if (n >= MIC_DEV_MAX) {
            Log("scan: more than %d devices; the rest are ignored",
                MIC_DEV_MAX);
            break;
        }
    }
    ObjRelease(pMonEnum);
    ObjRelease(pEnum);

    Lock();
    memcpy(g_dev, tmp, sizeof(MicDev) * (size_t)n);
    InterlockedExchange(&g_ndev, n);
    if (g_targetKey[0]) {
        const MicDev *d = NULL;

        for (i = 0; i < n; i++)
            if (strcmp(g_dev[i].key, g_targetKey) == 0) { d = &g_dev[i]; break; }
        InterlockedExchange(&g_targetOk, d ? 1 : 0);
        if (!d)
            Log("scan: the picked device ('%s') is not here - `force` stays "
                "off until it is", g_targetKey);
    } else {
        InterlockedExchange(&g_targetOk, 0);
    }
    Unlock();

    Log("scan: %d recording device(s)", n);
    return n;
}

/* The scan runs on a thread of this plugin's own, and COM has to be
 * initialized on the thread that uses it: nothing else does that for a
 * plugin thread. The detours, by contrast, run on the game's own threads,
 * which the game initialized long before. */
static int ScanDevices(void) {
    HRESULT coinit = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int     n;

    t_inScan = 1;
    n = ScanDevicesRaw();
    t_inScan = 0;

    if (SUCCEEDED(coinit)) CoUninitialize();
    return n;
}

/* ---- this plugin's ini ------------------------------------------------- */

static void ResolveIniPath(void) {
    char mod[MAX_PATH];
    const char *dot;
    size_t n;

    g_iniPath[0] = 0;
    if (!g_inst || !GetModuleFileNameA(g_inst, mod, sizeof(mod))) return;
    dot = strrchr(mod, '.');
    n = dot ? (size_t)(dot - mod) : strlen(mod);
    if (n >= sizeof(g_iniPath)) n = sizeof(g_iniPath) - 1;
    memcpy(g_iniPath, mod, n);
    g_iniPath[n] = 0;
    strncat(g_iniPath, ".ini", sizeof(g_iniPath) - n - 1);
}

static int IniInt(const char *key, int fallback) {
    char buf[64];

    if (g_iniPath[0] &&
        GetPrivateProfileStringA("Settings", key, "", buf, sizeof(buf),
                                 g_iniPath) > 0 && buf[0])
        return atoi(buf);
    return fallback;
}

static void IniStr(const char *key, char *out, int cap, const char *fallback) {
    out[0] = 0;
    if (g_iniPath[0] &&
        GetPrivateProfileStringA("Settings", key, "", out, cap, g_iniPath) > 0)
        return;
    snprintf(out, cap, "%s", fallback ? fallback : "");
}

static void SaveIni(void) {
    char buf[16];
    char key[MIC_KEY_MAX];
    int  ok = 1;

    if (!g_iniPath[0]) return;
    snprintf(buf, sizeof(buf), "%d", WantFix());
    ok = WritePrivateProfileStringA("Settings", "fix", buf, g_iniPath) != 0;
    snprintf(buf, sizeof(buf), "%d", WantForce());
    ok = WritePrivateProfileStringA("Settings", "force", buf, g_iniPath) != 0 && ok;
    snprintf(buf, sizeof(buf), "%d", WantProbe());
    ok = WritePrivateProfileStringA("Settings", "probe", buf, g_iniPath) != 0 && ok;
    snprintf(buf, sizeof(buf), "%d",
             (int)InterlockedCompareExchange(&g_cfgDevice, 0, 0));
    ok = WritePrivateProfileStringA("Settings", "device", buf, g_iniPath) != 0 && ok;
    TargetKeyCopy(key, sizeof(key));
    ok = WritePrivateProfileStringA("Settings", "device_key", key,
                                    g_iniPath) != 0 && ok;
    if (!ok)
        Log("the settings could not be written to %s", g_iniPath);
}

static void LoadConfig(void) {
    InterlockedExchange(&g_cfgFix,   IniInt("fix", 1) ? 1 : 0);
    InterlockedExchange(&g_cfgForce, IniInt("force", 0) ? 1 : 0);
    InterlockedExchange(&g_cfgProbe, IniInt("probe", 0) ? 1 : 0);
    InterlockedExchange(&g_cfgDevice, IniInt("device", 0));
    IniStr("device_key", g_targetKey, sizeof(g_targetKey), "");
    Log("config: fix=%d force=%d probe=%d device=%d key='%s'", WantFix(),
        WantForce(), WantProbe(),
        (int)InterlockedCompareExchange(&g_cfgDevice, 0, 0), g_targetKey);
}

/* ---- text -------------------------------------------------------------
 * The keys are stable IDs, so a row can be reworded without breaking a
 * lang.ini. Both languages are compiled in; the file only overrides.
 *
 * The Chinese is the reference for a row's width and the English is kept no
 * longer than it, counting a CJK character as two columns. Menus that fit
 * one language should not grow a scrollbar in the other.
 */
static const ShText kEn[] = {
    { "@mic.page",       "Game mic fix" },
    { "@mic.fix",        "Give ASCII mic names (restart game)" },
    { "@mic.force",      "Force picked microphone (restart game)" },
    { "@mic.device",     "Microphone" },
    { "@mic.rescan",     "Rescan microphones" },
    { "@mic.status",     "Mic: %d   Force: %s   %s" },
    { "@mic.force.on",   "On" },
    { "@mic.force.off",  "Off" },
    { "@mic.note.off",   "Fix off: names are untouched" },
    { "@mic.note.fix",   "Non-ASCII names get replaced" },
    { "@mic.note.force", "Picked microphone only" },
    { "@mic.none",       "No microphone" },
    { "@mic.hint",       "Fixes the mic not working in some setups" }
};

static const ShText kZh[] = {
    { "@mic.page",       "游戏麦克风修复" },
    { "@mic.fix",        "交给游戏纯英文麦克风名(开关后重启生效)" },
    { "@mic.force",      "强制游戏使用选择的麦克风(开关后重启生效)" },
    { "@mic.device",     "选择麦克风" },
    { "@mic.rescan",     "重新扫描麦克风设备" },
    { "@mic.status",     "麦克风: %d   强制使用: %s   %s" },
    { "@mic.force.on",   "开" },
    { "@mic.force.off",  "关" },
    { "@mic.note.off",   "修复关闭: 麦克风名原样交给游戏" },
    { "@mic.note.fix",   "含中文的麦克风名将被替换为别名" },
    { "@mic.note.force", "游戏只能看到选择的麦克风" },
    { "@mic.none",       "没有找到麦克风设备" },
    { "@mic.hint",       "修复部分环境下，游戏麦无法正常使用的故障" }
};

static void MicText(void) {
    static int done;

    if (done) return;
    done = 1;
    ShLangDeclare("micfix", "en-US", kEn, (int)ARRAY_LEN(kEn));
    ShLangDeclare("micfix", "zh-CN", kZh, (int)ARRAY_LEN(kZh));
}

/* ---- the menu --------------------------------------------------------- */

static const char  *g_optPtr[MIC_DEV_MAX];
static char         g_optBuf[MIC_DEV_MAX][MIC_LABEL_MAX];
static char         g_noDev[MIC_NAME_MAX];
static int          g_nopt;

static void UpdateStatus(void) {
    if (!g_menu) return;
    ShMenuStatusF(g_menu, "@mic.status",
                  (int)InterlockedCompareExchange(&g_ndev, 0, 0),
                  ShLangText("micfix", WantForce() ? "@mic.force.on"
                                                   : "@mic.force.off"),
                  ShLangText("micfix", !WantFix()   ? "@mic.note.off"
                                       : WantForce() ? "@mic.note.force"
                                                     : "@mic.note.fix"));
}

static void BuildDeviceOptions(void) {
    int i, n = (int)InterlockedCompareExchange(&g_ndev, 0, 0);

    Lock();
    for (i = 0; i < n; i++) {
        /* The label, not the alias: the menu is read by a player, the
         * alias is what the game is handed. */
        snprintf(g_optBuf[i], MIC_LABEL_MAX, "%s", g_dev[i].label);
        g_optPtr[i] = g_optBuf[i];
    }
    Unlock();

    if (n == 0) {
        snprintf(g_noDev, sizeof(g_noDev), "%s",
                 ShLangText("micfix", "@mic.none"));
        g_optPtr[0] = g_noDev;
        n = 1;
    }
    g_nopt = n;
}

static void MenuFix(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    InterlockedExchange(&g_cfgFix, v ? 1 : 0);
    SaveIni();
    UpdateStatus();
    Log("menu: fix=%d", WantFix());
}

static void MenuForce(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    InterlockedExchange(&g_cfgForce, v ? 1 : 0);
    SaveIni();
    UpdateStatus();
    Log("menu: force=%d", WantForce());
}

static void MenuDevice(uint32_t m, uint32_t it, int v, void *u) {
    int n = (int)InterlockedCompareExchange(&g_ndev, 0, 0);

    (void)m; (void)it; (void)u;
    if (v < 0 || v >= n) return;

    InterlockedExchange(&g_cfgDevice, v);
    Lock();
    snprintf(g_targetKey, sizeof(g_targetKey), "%s", g_dev[v].key);
    InterlockedExchange(&g_targetOk, 1);
    Unlock();

    /* Picking a device is what "use this one" means: the force row follows
     * the pick, and is synced without firing its own callback. */
    InterlockedExchange(&g_cfgForce, 1);
    if (g_menu) ShMenuSetValue(g_menu, "@mic.force", 1);

    SaveIni();
    UpdateStatus();
    Log("menu: device %d ('%s') picked; force is on", v, g_targetKey);
}

/* Deferred on purpose: rebuilding the page from inside one of its own
 * callbacks would drop the item the framework is still walking. */
static void MenuRescan(uint32_t m, uint32_t it, int v, void *u) {
    (void)m; (void)it; (void)u;
    InterlockedExchange(&g_rescanWanted, 1);
    Log("menu: rescan asked for");
}

static void BuildMenuItems(void) {
    ShMenuToggle(g_menu, "@mic.fix", WantFix(), MenuFix, NULL);
    ShMenuToggle(g_menu, "@mic.force", WantForce(), MenuForce, NULL);
    BuildDeviceOptions();
    ShMenuList(g_menu, "@mic.device", g_optPtr, g_nopt,
               (int)InterlockedCompareExchange(&g_cfgDevice, 0, 0) < g_nopt
                   ? (int)InterlockedCompareExchange(&g_cfgDevice, 0, 0) : 0,
               MenuDevice, NULL);
    ShMenuAction(g_menu, "@mic.rescan", MenuRescan, NULL);
    ShMenuHint(g_menu, "@mic.hint");
}

static void BuildMenu(void) {
    MicText();
    g_menu = ShMenuCreate("@mic.page");
    if (!g_menu) {
        Log("ShMenuCreate failed: no menu this session");
        return;
    }
    BuildMenuItems();
    UpdateStatus();
    Log("menu created (%u)", (unsigned)g_menu);
}

static DWORD WINAPI PollThread(LPVOID p) {
    (void)p;
    for (;;) {
        Sleep(700);
        if (InterlockedCompareExchange(&g_stop, 0, 0)) return 0;
        if (InterlockedExchange(&g_rescanWanted, 0)) {
            if (InterlockedCompareExchange(&g_stop, 0, 0)) return 0;
            Log("rescan: asked for from the menu");
            ScanDevices();
            if (g_menu) {
                ShMenuClear(g_menu);
                BuildMenuItems();
            }
            UpdateStatus();
        }
        if (g_menu && ShMenuIsShowing(g_menu)) UpdateStatus();
    }
    return 0;
}

/* ---- startup ---------------------------------------------------------- */

static DWORD WINAPI InitThread(LPVOID p) {
    (void)p;

    /* log.h: this translation unit gets its own file and its own Log. */
    LogInitAlways("micfix.log");
    Log("--- in-game microphone fix ---");

    InitializeCriticalSection(&g_lock);
    InterlockedExchange(&g_lockReady, 1);

    ResolveIniPath();
    LoadConfig();

    InstallHooks();
    ScanDevices();

    /* Voice chat matters most where the game is played with others, so this
     * plugin is never the one a mode switch takes away. */
    if (!ShPluginBlacklist(SH_MODE_BLACKLIST_NONE))
        Log("the blacklist declaration was refused");

    BuildMenu();

    {
        HANDLE h = CreateThread(NULL, 0, PollThread, NULL, 0, NULL);

        if (h) CloseHandle(h);   /* never waited on */
    }

    Log("ready: fix=%d force=%d probe=%d devices=%d target='%s'", WantFix(),
        WantForce(), WantProbe(),
        (int)InterlockedCompareExchange(&g_ndev, 0, 0), g_targetKey);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = inst;
        DisableThreadLibraryCalls(inst);
        {
            HANDLE h = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);

            if (h) CloseHandle(h);   /* never waited on */
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        /* The detours live in this module: an unload that left them behind
         * would be a jump into unmapped memory the next time the game opens
         * its sound settings. There is no thread of ours left to do it by
         * then, so it is done here.
         *
         * The poll thread is told to stop and the menu handle is dropped
         * before the hooks come out, so a thread that was asleep in its loop
         * wakes up with nothing to do instead of calling the framework from a
         * module on its way out. It is deliberately NOT joined: this runs
         * under the loader lock, and that thread reaches COM through a rescan
         * (CoCreateInstance, which loads modules), which is the textbook way
         * to deadlock a DllMain. Nothing is lost by not waiting - nothing in
         * this project ever frees a plugin, so the only detach is the one the
         * process does as it dies, and by then there is nothing left to
         * protect. */
        InterlockedExchange(&g_stop, 1);
        g_menu = 0;
        UninstallHooks();
    }
    return TRUE;
}
