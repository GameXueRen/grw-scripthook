/* Widget properties by id, resolved from the engine's own
 * property tables and RTTI at runtime. No offsets, no raw
 * field writes: every set goes through the class setter. */
#include <windows.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"

#define MAX_PROPS   512
#define MAX_CLASSES 32
#define NAME_LEN    48

extern int ShReadableAddr(uint64_t addr, size_t len);

typedef struct {
    uint32_t id;
    int      type;        /* SH_PT_* */
    int      byRef;       /* Reference accessor: getter returns ptr */
    uint64_t set, get;
    int      cls;         /* index into g_cls */
} Prop;

static Prop g_props[MAX_PROPS];
static int  g_nProps;
static char g_cls[MAX_CLASSES][NAME_LEN];
static int  g_nCls;
static int  g_resolved;
static int  g_sections;
static uint64_t g_bytes;

static uint64_t RQ(uint64_t a) {
    uint64_t v = 0;
    if (ShReadableAddr(a, 8)) memcpy(&v, (void *)(uintptr_t)a, 8);
    return v;
}

static int InImage(uint64_t a) {
    return a >= SH_IMG(0) && a < SH_IMG(0x18B09000);
}

/* RTTI: vtable, then object locator, then type descriptor,
 * then the mangled name. NULL when any step fails. */
static const char *RttiName(uint64_t vt) {
    uint64_t col, td;
    uint32_t rva;

    if (!InImage(vt)) return NULL;
    col = RQ(vt - 8);
    if (!InImage(col) || !ShReadableAddr(col + 0x10, 4)) return NULL;
    memcpy(&rva, (void *)(uintptr_t)(col + 0xC), 4);
    td = SH_IMG(rva);
    if (!InImage(td) || !ShReadableAddr(td + 0x10, NAME_LEN)) return NULL;
    return (const char *)(uintptr_t)(td + 0x10);
}

/* The mangled RTTI name behind any object's vtable, for
 * callers that want to check what they found. */
int ShPropRttiOf(uint64_t obj, char *out, int n) {
    const char *nm = RttiName(RQ(obj));
    if (!nm) return 0;
    strncpy(out, nm, n - 1);
    out[n - 1] = 0;
    return 1;
}

/* ".?AVLabelWidget@phoenix@@" to "LabelWidget" */
int ShPropClassOf(uint64_t handle, char *out, int n) {
    const char *nm = RttiName(RQ(handle));
    const char *e;
    if (!nm || strncmp(nm, ".?AV", 4) != 0) return 0;
    nm += 4;
    e = strchr(nm, '@');
    if (!e || e - nm >= n) return 0;
    memcpy(out, nm, e - nm);
    out[e - nm] = 0;
    return 1;
}

static int ClassIndex(const char *name) {
    int i;
    for (i = 0; i < g_nCls; i++)
        if (strcmp(g_cls[i], name) == 0) return i;
    if (g_nCls >= MAX_CLASSES) return -1;
    strncpy(g_cls[g_nCls], name, NAME_LEN - 1);
    g_cls[g_nCls][NAME_LEN - 1] = 0;
    return g_nCls++;
}

/* .?AV?$ValuePropertyAccessor@VWidget@phoenix@@M@phoenix@@
 * class sits between "Accessor@V" and "@", the value type
 * follows the class's "@phoenix@@". */
static int ParseAccessor(const char *nm, char *cls, int *type, int *byRef) {
    const char *p, *e, *t;
    int len;

    if (!nm || strstr(nm, "PropertyAccessor@V") == NULL) return 0;
    *byRef = strstr(nm, "ReferencePropertyAccessor") != NULL;
    p = strstr(nm, "Accessor@V") + 10;
    e = strchr(p, '@');
    if (!e) return 0;
    len = (int)(e - p);
    if (len <= 0 || len >= NAME_LEN) return 0;
    memcpy(cls, p, len);
    cls[len] = 0;
    t = strstr(e, "@@");
    if (!t) return 0;
    t += 2;
    if (strncmp(t, "_N@", 3) == 0)            *type = SH_PT_BOOL;
    else if (strncmp(t, "M@", 2) == 0)        *type = SH_PT_FLOAT;
    else if (strncmp(t, "K@", 2) == 0)        *type = SH_PT_UINT;
    else if (strncmp(t, "VVector2f", 9) == 0) *type = SH_PT_VEC2;
    else if (strncmp(t, "VVectorSIMD3f", 13) == 0) *type = SH_PT_VEC3;
    else if (strstr(t, "GearBasicString"))    *type = SH_PT_STRING;
    else return 0;
    return 1;
}

static void Consider(uint64_t at) {
    uint64_t vt = RQ(at), set = RQ(at + 0x10), get = RQ(at + 0x18);
    uint64_t idq = RQ(at + 8);
    char cls[NAME_LEN];
    int type, byRef, ci;
    Prop *p;

    if (!InImage(vt) || !InImage(get)) return;
    if (set && !InImage(set)) return;
    if ((idq >> 32) != 0 || !(idq & 0x80000000u) || (idq & 0x7FFF0000u)) return;
    if (!ParseAccessor(RttiName(vt), cls, &type, &byRef)) return;
    ci = ClassIndex(cls);
    if (ci < 0 || g_nProps >= MAX_PROPS) return;
    p = &g_props[g_nProps++];
    p->id = (uint32_t)(idq & 0xFFFF);
    p->type = type; p->byRef = byRef;
    p->set = set; p->get = get; p->cls = ci;
}

/* One walk over the executable's data sections for the
 * 0x20 byte {accessorVtable, id | 0x80000000, set, get}
 * records the engine keeps per widget class. */
int ShPropResolve(void) {
    uint8_t *base = (uint8_t *)(uintptr_t)SH_IMG(0);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    static uint8_t chunk[0x10000];
    int i;

    if (g_resolved) return g_nProps > 0;
    g_resolved = 1;
    if (!ShReadableAddr(SH_IMG(0), 0x1000)) return 0;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint64_t start = SH_IMG(sec[i].VirtualAddress);
        uint64_t size = sec[i].Misc.VirtualSize, done = 0;
        /* The section flags are scrambled by the protector
         * (.rsrc is marked code and holds the tables), so
         * every section is scanned, flags ignored. */
        g_sections++;
        while (done + 0x20 <= size) {
            size_t want = size - done, got = 0, o;
            if (want > sizeof(chunk)) want = sizeof(chunk);
            /* a page the process cannot read is skipped,
             * not a reason to give the section up */
            if (!ReadProcessMemory(GetCurrentProcess(),
                                   (void *)(uintptr_t)(start + done),
                                   chunk, want, &got) || got < 0x20) {
                done += 0x1000;
                continue;
            }
            g_bytes += got;
            for (o = 0; o + 0x20 <= got; o += 8) {
                uint32_t idlo, idhi;
                memcpy(&idlo, chunk + o + 8, 4);
                memcpy(&idhi, chunk + o + 12, 4);
                if (idhi != 0 || !(idlo & 0x80000000u) ||
                    (idlo & 0x7FFF0000u)) continue;
                Consider(start + done + o);
            }
            /* overlap, so a record on the chunk edge is seen */
            done += (got > 0x18) ? got - 0x18 : got;
        }
    }
    return g_nProps > 0;
}

/* The widget's own class first, then Widget. */
static const Prop *Find(uint64_t handle, uint32_t id) {
    char cls[NAME_LEN];
    int ci, wi, i;

    if (!ShPropResolve()) return NULL;
    if (!ShPropClassOf(handle, cls, sizeof(cls))) return NULL;
    ci = ClassIndex(cls);
    wi = ClassIndex("Widget");
    for (i = 0; i < g_nProps; i++)
        if (g_props[i].id == id && g_props[i].cls == ci) return &g_props[i];
    for (i = 0; i < g_nProps; i++)
        if (g_props[i].id == id && g_props[i].cls == wi) return &g_props[i];
    return NULL;
}

int ShPropType(uint64_t handle, uint32_t id) {
    const Prop *p = Find(handle, id);
    return p ? p->type : 0;
}

int ShPropCount(void) {
    ShPropResolve();
    return g_nProps;
}

/* Scan diagnostics: sections seen, bytes read, records. */
void ShPropStats(int *sections, uint64_t *bytes, int *records) {
    *sections = g_sections; *bytes = g_bytes; *records = g_nProps;
}

int ShPropAt(int i, char *cls, int n, uint32_t *id, int *type,
             uint64_t *set, uint64_t *get) {
    if (!ShPropResolve() || i < 0 || i >= g_nProps) return 0;
    strncpy(cls, g_cls[g_props[i].cls], n - 1);
    cls[n - 1] = 0;
    *id = g_props[i].id; *type = g_props[i].type;
    *set = g_props[i].set; *get = g_props[i].get;
    return 1;
}

/* ---- calls: game thread, under the scene lock ---- */

typedef void (__attribute__((ms_abi)) *SetF_t)(uint64_t, float);
typedef void (__attribute__((ms_abi)) *SetU_t)(uint64_t, uint32_t);
typedef void (__attribute__((ms_abi)) *SetP_t)(uint64_t, const void *);
typedef float (__attribute__((ms_abi)) *GetF_t)(uint64_t);
typedef uint64_t (__attribute__((ms_abi)) *GetU_t)(uint64_t);
typedef void *(__attribute__((ms_abi)) *GetP_t)(uint64_t);
typedef void *(__attribute__((ms_abi)) *GetOut_t)(uint64_t, void *);

int ShPropSetF(uint64_t handle, uint32_t id, float v) {
    const Prop *p = Find(handle, id);
    if (!p || !p->set || p->type != SH_PT_FLOAT) return 0;
    ((SetF_t)p->set)(handle, v);
    return 1;
}

int ShPropSetU(uint64_t handle, uint32_t id, uint32_t v) {
    const Prop *p = Find(handle, id);
    if (!p || !p->set) return 0;
    if (p->type != SH_PT_BOOL && p->type != SH_PT_UINT) return 0;
    ((SetU_t)p->set)(handle, p->type == SH_PT_BOOL ? (v != 0) : v);
    return 1;
}

int ShPropSetV(uint64_t handle, uint32_t id, const float *v, int n) {
    const Prop *p = Find(handle, id);
    float buf[4] = {0, 0, 0, 0};
    if (!p || !p->set) return 0;
    if (p->type == SH_PT_VEC2 && n == 2) { buf[0] = v[0]; buf[1] = v[1]; }
    else if (p->type == SH_PT_VEC3 && n == 3) {
        buf[0] = v[0]; buf[1] = v[1]; buf[2] = v[2];
    } else return 0;
    ((SetP_t)p->set)(handle, buf);
    return 1;
}

/* Shared string: header word from the current value, block
 * at refcount 0 so the widget's copy is the owner. */
int ShPropSetS(uint64_t handle, uint32_t id, uint64_t block) {
    const Prop *p = Find(handle, id);
    uint64_t str[3];
    uint64_t cur;
    if (!p || !p->set || p->type != SH_PT_STRING || !block) return 0;
    cur = (uint64_t)(uintptr_t)((GetP_t)p->get)(handle);
    str[0] = cur ? RQ(cur) : 0;
    str[1] = block;
    str[2] = 5;
    ((SetP_t)p->set)(handle, str);
    return 1;
}

int ShPropGetF(uint64_t handle, uint32_t id, float *out) {
    const Prop *p = Find(handle, id);
    if (!p || !p->get || p->type != SH_PT_FLOAT) return 0;
    if (p->byRef) {
        float *f = (float *)((GetP_t)p->get)(handle);
        if (!f) return 0;
        *out = *f;
    } else {
        *out = ((GetF_t)p->get)(handle);
    }
    return 1;
}

int ShPropGetU(uint64_t handle, uint32_t id, uint32_t *out) {
    const Prop *p = Find(handle, id);
    if (!p || !p->get) return 0;
    if (p->type != SH_PT_BOOL && p->type != SH_PT_UINT) return 0;
    if (p->byRef) {
        uint32_t *u = (uint32_t *)((GetP_t)p->get)(handle);
        if (!u) return 0;
        *out = p->type == SH_PT_BOOL ? (*(uint8_t *)u) : *u;
    } else {
        uint64_t r = ((GetU_t)p->get)(handle);
        *out = p->type == SH_PT_BOOL ? (uint32_t)(r & 0xFF) : (uint32_t)r;
    }
    return 1;
}

int ShPropGetV(uint64_t handle, uint32_t id, float *out, int n) {
    const Prop *p = Find(handle, id);
    float buf[4];
    const float *src;
    if (!p || !p->get) return 0;
    if ((p->type == SH_PT_VEC2 && n != 2) || (p->type == SH_PT_VEC3 && n != 3))
        return 0;
    if (p->type != SH_PT_VEC2 && p->type != SH_PT_VEC3) return 0;
    if (p->byRef) src = (const float *)((GetP_t)p->get)(handle);
    else src = (const float *)((GetOut_t)p->get)(handle, buf);
    if (!src) return 0;
    memcpy(out, src, sizeof(float) * n);
    return 1;
}

/* Returns the block pointer of a string property, or 0. */
uint64_t ShPropGetS(uint64_t handle, uint32_t id) {
    const Prop *p = Find(handle, id);
    uint64_t str;
    if (!p || !p->get || p->type != SH_PT_STRING) return 0;
    str = (uint64_t)(uintptr_t)((GetP_t)p->get)(handle);
    return str ? RQ(str + 8) : 0;
}

/* Label text bounds: the text engine at priv+0x218 keeps
 * the laid out rectangle at [[+0x218]+8]+0xe0 as min x,
 * min y, max x, max y, valid after the label's update. */
int ShPropMeasure(uint64_t handle, float *w, float *h) {
    uint64_t priv = RQ(handle + 0x20), te, inner;
    float r[4];
    te = RQ(priv + 0x218);
    inner = te ? RQ(te + 8) : 0;
    if (!inner || !ShReadableAddr(inner + 0xE0, 16)) return 0;
    memcpy(r, (void *)(uintptr_t)(inner + 0xE0), 16);
    *w = r[2] - r[0];
    *h = r[3] - r[1];
    return 1;
}
