/* GRW ScriptHook API implementation.
 * Build pinned: GRW Definitive, base 0x140000000.
 */
#include <windows.h>
#include <string.h>
#include <math.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"
#include "log.h"

/* Static anchors, verified in FINDINGS.md */
#define SH_PLAYER_GLOBAL   SH_IMG(0x4BC3358)
#define SH_VT_ENTITY       SH_IMG(0x39C6FC8)
#define SH_VT_SKELETON     SH_IMG(0x3ACBBD8)

/* Object layout */
#define OFF_ENT_NODE       0x18
#define OFF_ENT_POS        0x50
#define OFF_ENT_MATRIX     0x20
#define OFF_NODE_LINK      0x68
#define OFF_SKEL_OWNER     0x10

/* Parent link, a tagged union at node+0x68 */
#define LINK_TYPE          0x00
#define LINK_HANDLE_T1     0x08
#define LINK_HANDLE_T2     0x10
#define HANDLE_VALUE       0x00
#define HANDLE_FLAGS       0x0C

/* Global chain to the mirrored player position */
#define OFF_GLOBAL_TF      0x30
#define OFF_TF_POS         0x90

/* Wine maps engine objects far above the game heap, so
 * the window has to cover the whole user address space.
 */
#define SH_HEAP_LO         0x1000000ULL
#define SH_HEAP_HI         0x800000000000ULL

static ShPlayer g_player;
static int      g_resolved;
static int      g_logReady;
static int      g_lastError;

static int ShFail(int err) {
    g_lastError = err;
    return 0;
}

/* Single error channel, shared with the physics side. */
void ShSetError(int err) {
    g_lastError = err;
}

SH_API int ShLastError(void) {
    return g_lastError;
}

SH_API const char *ShErrorString(int err) {
    switch (err) {
    case SH_OK:            return "ok";
    case SH_ERR_BAD_ARG:   return "bad argument";
    case SH_ERR_NO_GLOBAL: return "player global is null";
    case SH_ERR_NO_POSITION: return "player position unreadable";
    case SH_ERR_NO_CANDIDATE: return "no entity at player position";
    case SH_ERR_NO_ROOT:   return "parent chain has no root";
    case SH_ERR_UNWRITABLE: return "root position not writable";
    case SH_ERR_NOT_ENTITY: return "address is not an entity";
    case SH_ERR_HOOK_FAILED:
        return "could not install the physics hook";
    case SH_ERR_NO_PHYSICS:
        return "physics world not resolved, are you in a level";
    case SH_ERR_NO_GROUND:
        return "no ground under that point, or it is unstreamed";
    case SH_ERR_NOT_IN_GAME:
        return "not in game yet";
    case SH_ERR_NOT_STREAMED:
        return "too far from the player: collision only streams "
               "in within about 1500m, so query nearer or move "
               "the player there first";
    case SH_ERR_CONTROLLER:
        return "please use the native player controller "
               "velocity for this purpose: this entity is "
               "driven by a character controller, so its "
               "Havok body ignores velocity and writing it "
               "detaches the collision body from the player";
    default:               return "unknown";
    }
}

static void ApiLog(const char *fmt, ...) {
    va_list ap;
    if (!g_logReady) {
        LogInit("scripthook_api.log");
        g_logReady = 1;
    }
    va_start(ap, fmt);
    if (g_logFile) {
        vfprintf(g_logFile, fmt, ap);
        fputc('\n', g_logFile);
        fflush(g_logFile);
    }
    va_end(ap);
}

static int ShReadable(uint64_t addr, size_t len) {
    MEMORY_BASIC_INFORMATION mbi;
    if (addr < SH_HEAP_LO || addr >= SH_HEAP_HI) return 0;
    if (!VirtualQuery((void *)(uintptr_t)addr, &mbi, sizeof(mbi)))
        return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & PAGE_GUARD) return 0;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                         PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY)))
        return 0;
    {
        uint64_t end = (uint64_t)(uintptr_t)mbi.BaseAddress
                     + mbi.RegionSize;
        return addr + len <= end;
    }
}

int ShReadableAddr(uint64_t addr, size_t len) {
    return ShReadable(addr, len);
}

/* Kernel mediated, so a page freed between the check and
 * the read fails cleanly instead of faulting. Closes the
 * race every memcpy after VirtualQuery carried. */
int ShReadMem(uint64_t addr, void *out, size_t len) {
    SIZE_T got = 0;

    if (addr < SH_HEAP_LO || addr >= SH_HEAP_HI) return 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
                           (const void *)(uintptr_t)addr,
                           out, len, &got))
        return 0;
    return got == len;
}

static uint64_t ShQ(uint64_t addr) {
    uint64_t v;
    if (!ShReadMem(addr, &v, 8)) return 0;
    return v;
}

uint64_t ShReadQ(uint64_t addr) {
    return ShQ(addr);
}

/* Stub must land within rel32 of the hook site, so
 * probe upward and downward near the target.
 */
void *ShAllocNear(uint64_t target) {
    MEMORY_BASIC_INFORMATION mbi;
    uint64_t lo = target > 0x60000000ULL
                ? target - 0x60000000ULL : 0x10000ULL;
    uint64_t hi = target + 0x60000000ULL;
    uint64_t a;

    for (a = target & ~0xFFFFULL; a < hi; a += 0x10000ULL) {
        if (!VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof(mbi)))
            break;
        if (mbi.State == MEM_FREE) {
            void *p = VirtualAlloc((void *)(uintptr_t)a, 0x1000,
                                   MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    }
    for (a = target & ~0xFFFFULL; a > lo; a -= 0x10000ULL) {
        if (!VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof(mbi)))
            continue;
        if (mbi.State == MEM_FREE) {
            void *p = VirtualAlloc((void *)(uintptr_t)a, 0x1000,
                                   MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    }
    return NULL;
}

static int ShVec(uint64_t addr, ShVec3 *out) {
    return ShReadMem(addr, out, 12);
}

static int ShIsEntity(uint64_t obj) {
    return obj && ShQ(obj) == SH_VT_ENTITY;
}

static int ShIsSkeleton(uint64_t obj) {
    return obj && ShQ(obj) == SH_VT_SKELETON;
}

/* Resolve one parent hop. Returns 0 at the root, and
 * reports the link type: 1 entity, 2 bone attachment.
 */
static uint64_t ShParentOfT(uint64_t entity, uint32_t *outType) {
    uint64_t node, link, hp, val, parent;
    uint32_t type;
    int32_t flags;

    if (outType) *outType = 0;

    node = ShQ(entity + OFF_ENT_NODE);
    if (!node) return 0;
    link = node + OFF_NODE_LINK;
    if (!ShReadable(link, 0x20)) return 0;
    memcpy(&type, (void *)(uintptr_t)(link + LINK_TYPE), 4);

    if (type == 1)      hp = ShQ(link + LINK_HANDLE_T1);
    else if (type == 2) hp = ShQ(link + LINK_HANDLE_T2);
    else                return 0;
    if (outType) *outType = type;
    if (!hp || !ShReadable(hp, 0x10)) return 0;

    memcpy(&val, (void *)(uintptr_t)(hp + HANDLE_VALUE), 8);
    memcpy(&flags, (void *)(uintptr_t)(hp + HANDLE_FLAGS), 4);

    /* Negative flags means a raw pointer, otherwise
     * the value is masked with a per thread key.
     */
    if (flags >= 0) return 0;
    parent = val;
    if (!parent) return 0;

    if (ShIsSkeleton(parent)) {
        uint64_t owner = ShQ(parent + OFF_SKEL_OWNER);
        if (ShIsEntity(owner)) return owner;
        return 0;
    }
    if (ShIsEntity(parent)) return parent;
    return 0;
}

static uint64_t ShParentOf(uint64_t entity) {
    return ShParentOfT(entity, NULL);
}

/* The player is bone attached. A vehicle attached node
 * uses an entity link, which separates the two.
 */
static int ShIsBoneAttached(uint64_t obj) {
    uint32_t type = 0;
    return ShParentOfT(obj, &type) != 0 && type == 2;
}

SH_API int ShWalkToRoot(uint64_t entity, uint64_t *outRoot) {
    uint64_t cur = entity, seen[16];
    int n = 0, i;

    if (!outRoot) return ShFail(SH_ERR_BAD_ARG);
    if (!ShIsEntity(cur)) return ShFail(SH_ERR_NOT_ENTITY);
    while (n < 16) {
        uint64_t next;
        for (i = 0; i < n; i++)
            if (seen[i] == cur) { *outRoot = cur; return 1; }
        seen[n++] = cur;
        next = ShParentOf(cur);
        if (!next) { *outRoot = cur; return 1; }
        cur = next;
    }
    *outRoot = cur;
    return 1;
}

/* The transform is embedded at obj+0x30, not a pointer. */
SH_API int ShGetPlayerPosition(ShVec3 *out) {
    uint64_t obj;
    if (!out) return ShFail(SH_ERR_BAD_ARG);
    obj = ShQ(SH_PLAYER_GLOBAL);
    if (!obj) return ShFail(SH_ERR_NO_GLOBAL);
    if (!ShVec(obj + OFF_GLOBAL_TF + OFF_TF_POS, out))
        return ShFail(SH_ERR_NO_POSITION);
    g_lastError = SH_OK;
    return 1;
}

static int ShNear(const ShVec3 *a, const ShVec3 *b, float tol) {
    return fabsf(a->x - b->x) < tol && fabsf(a->y - b->y) < tol
        && fabsf(a->z - b->z) < tol;
}

/* The engine's own accessor, FUN_140D43530 and the two
 * calls after it. No scanning. See FINDINGS.md.
 */
#define SH_PLAYER_MGR    SH_IMG(0x4BB6438)
#define SH_SLOT_INDEX    SH_IMG(0x4D84E98)
#define OFF_MGR_OBJ      0x98
#define OFF_OBJ_TABLE    0xD10
#define OFF_TABLE_SLOTS  0x08
#define OFF_SLOT_IDX     0x170

static uint64_t ShPlayerFromStatic(void) {
    uint64_t mgr, obj, table, slots, idxp, root;
    uint32_t idx = 0;

    mgr = ShQ(SH_PLAYER_MGR);
    if (!mgr) return 0;
    obj = ShQ(mgr + OFF_MGR_OBJ);
    if (!obj) return 0;
    table = ShQ(obj + OFF_OBJ_TABLE);
    if (!table) return 0;
    slots = ShQ(table + OFF_TABLE_SLOTS);
    if (!slots) return 0;

    idxp = ShQ(SH_SLOT_INDEX);
    if (idxp && ShReadable(idxp + OFF_SLOT_IDX, 4))
        memcpy(&idx, (void *)(uintptr_t)(idxp + OFF_SLOT_IDX), 4);
    if (idx > 64) return 0;

    root = ShQ(slots + (uint64_t)idx * 8);
    return ShIsEntity(root) ? root : 0;
}

/* Match entities against the mirrored position, then
 * walk each candidate to its root.
 */
static int ShResolvePlayer(void) {
    ShVec3 want, got;
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *scan = (uint8_t *)0x1000000;
    uint64_t best = 0, root = 0;

    if (!ShGetPlayerPosition(&want)) {
        ApiLog("resolve: no player position, global=%p",
               (void *)(uintptr_t)ShQ(SH_PLAYER_GLOBAL));
        return 0;
    }
    /* Menus and loads park the position at the origin. */
    if (fabsf(want.x) < 1.0f && fabsf(want.y) < 1.0f
        && fabsf(want.z) < 1.0f)
        return ShFail(SH_ERR_NO_POSITION);
    ApiLog("resolve: want %.2f %.2f %.2f", want.x, want.y, want.z);

    while (VirtualQuery(scan, &mbi, sizeof(mbi))) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (next <= scan) break;
        if ((uint64_t)(uintptr_t)mbi.BaseAddress >= SH_HEAP_HI) break;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & PAGE_READWRITE) &&
            !(mbi.Protect & PAGE_GUARD))
        {
            uint8_t *b = (uint8_t *)mbi.BaseAddress;
            size_t sz = mbi.RegionSize, o, k, chunk;

            /* Chunked kernel reads: a page freed mid scan
             * skips instead of faulting. Chunks overlap so
             * no candidate spans a seam. */
            static uint8_t buf[0x10000];

            for (o = 0; o + 0x60 <= sz; o += sizeof(buf) - 0x60) {
                chunk = sz - o;
                if (chunk > sizeof(buf)) chunk = sizeof(buf);
                if (!ShReadMem((uint64_t)(uintptr_t)(b + o), buf,
                               chunk))
                    continue;
                for (k = 0; k + 0x60 <= chunk; k += 8) {
                    uint64_t vt;
                    uint64_t obj = (uint64_t)(uintptr_t)(b + o + k);

                    memcpy(&vt, buf + k, 8);
                    if (vt != SH_VT_ENTITY) continue;
                    memcpy(&got, buf + k + OFF_ENT_POS, 12);
                    /* In a vehicle the mirror sits metres
                     * off, so bone attachment disambiguates.
                     */
                    if (!ShNear(&got, &want, 12.0f)) continue;
                    if (!ShIsBoneAttached(obj)) continue;
                    if (!ShWalkToRoot(obj, &root)) continue;
                    if (root && root != obj) { best = obj; goto found; }
                }
            }
        }
        scan = next;
    }
found:
    if (!best) {
        ApiLog("resolve: no entity matched");
        return ShFail(SH_ERR_NO_CANDIDATE);
    }
    if (!ShWalkToRoot(best, &root)) return ShFail(SH_ERR_NO_ROOT);

    g_player.entity = best;
    g_player.node = ShQ(best + OFF_ENT_NODE);
    g_player.root = root;
    g_resolved = 1;
    g_lastError = SH_OK;
    ApiLog("resolved: entity %p node %p root %p",
        (void *)(uintptr_t)g_player.entity,
        (void *)(uintptr_t)g_player.node,
        (void *)(uintptr_t)g_player.root);
    return 1;
}

static int ShStillValid(void) {
    ShVec3 want, got;
    if (!g_resolved) return 0;
    if (!ShIsEntity(g_player.entity)) return 0;
    if (!ShIsEntity(g_player.root)) return 0;
    if (!ShGetPlayerPosition(&want)) return 0;
    if (!ShVec(g_player.entity + OFF_ENT_POS, &got)) return 0;
    return ShNear(&got, &want, 25.0f);
}

SH_API void ShInvalidate(void) {
    g_resolved = 0;
}

extern int ShRequireInGame(void);

SH_API int ShGetPlayer(ShPlayer *out) {
    uint64_t root = 0, ent;

    if (!out) return ShFail(SH_ERR_BAD_ARG);
    if (!ShRequireInGame()) return 0;

    /* Cheap and always current, so no cache to go stale.
     * The scan stays only as a fallback.
     */
    ent = ShPlayerFromStatic();
    if (ent) {
        g_player.entity = ent;
        g_player.node = ShQ(ent + OFF_ENT_NODE);
        g_player.root = ent;
        g_resolved = 1;
        g_lastError = SH_OK;
    } else if (!ShStillValid() && !ShResolvePlayer()) {
        return 0;
    }

    /* Entering or leaving a vehicle re-parents the player,
     * so the root is walked fresh rather than cached.
     */
    if (ShWalkToRoot(g_player.entity, &root) && root)
        g_player.root = root;
    *out = g_player;
    g_lastError = SH_OK;
    return 1;
}

SH_API int ShGetVersion(void) {
    return SH_API_VERSION;
}

/* Engine set transform: flags the entity dirty and
 * propagates to children, so it moves vehicles too.
 */
#define SH_SET_TRANSFORM   SH_IMG(0xC6BDE10)

typedef void (__attribute__((ms_abi)) *SetTransform_t)(uint64_t,
                                                       void *, char);

/* Keep the existing rotation, replace the translation. */
SH_API int ShPlaceEntity(uint64_t entity, const ShVec3 *pos,
                         const ShVec3 *orient) {
    float m[16];

    if (!entity || !pos) return ShFail(SH_ERR_BAD_ARG);
    if (!ShIsEntity(entity)) return ShFail(SH_ERR_NOT_ENTITY);
    if (!ShReadable(entity + OFF_ENT_MATRIX, 64))
        return ShFail(SH_ERR_UNWRITABLE);
    memcpy(m, (void *)(uintptr_t)(entity + OFF_ENT_MATRIX), 64);

    if (orient) {
        float f[3], r[3], u[3], len;
        f[0] = orient->x; f[1] = orient->y; f[2] = orient->z;
        len = sqrtf(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
        if (len > 1e-5f) {
            f[0] /= len; f[1] /= len; f[2] /= len;
            r[0] = f[1]; r[1] = -f[0]; r[2] = 0.0f;
            len = sqrtf(r[0]*r[0] + r[1]*r[1]);
            if (len > 1e-5f) {
                r[0] /= len; r[1] /= len;
                u[0] = r[1]*f[2] - r[2]*f[1];
                u[1] = r[2]*f[0] - r[0]*f[2];
                u[2] = r[0]*f[1] - r[1]*f[0];
                memcpy(m + 0, r, 12);
                memcpy(m + 4, f, 12);
                memcpy(m + 8, u, 12);
            }
        }
    }
    m[12] = pos->x;
    m[13] = pos->y;
    m[14] = pos->z;
    m[15] = 1.0f;

    ((SetTransform_t)SH_SET_TRANSFORM)(entity, m, 1);
    g_lastError = SH_OK;
    return 1;
}

/* Full orientation, which ShPlaceEntity cannot express:
 * it derives up from forward, so it can never roll a
 * thing onto its roof. Radians, game basis. */
SH_API int ShPlaceEntityRot(uint64_t entity, const ShVec3 *pos,
                            float yaw, float pitch, float roll) {
    float m[16];
    float cy = cosf(yaw), sy = sinf(yaw);
    float cp = cosf(pitch), sp = sinf(pitch);
    float cr = cosf(roll), sr = sinf(roll);
    float r[3], f[3], u[3];
    int k;

    if (!entity || !pos) return ShFail(SH_ERR_BAD_ARG);
    if (!ShIsEntity(entity)) return ShFail(SH_ERR_NOT_ENTITY);
    if (!ShReadable(entity + OFF_ENT_MATRIX, 64))
        return ShFail(SH_ERR_UNWRITABLE);
    memcpy(m, (void *)(uintptr_t)(entity + OFF_ENT_MATRIX), 64);

    r[0] = cy;       r[1] = -sy;      r[2] = 0.0f;
    f[0] = sy * cp;  f[1] = cy * cp;  f[2] = sp;
    u[0] = -sy * sp; u[1] = -cy * sp; u[2] = cp;

    /* Roll turns right and up about forward, so the body
     * tips without changing where it points.
     */
    for (k = 0; k < 3; k++) {
        float rr = r[k] * cr + u[k] * sr;
        float uu = -r[k] * sr + u[k] * cr;
        r[k] = rr;
        u[k] = uu;
    }

    memcpy(m + 0, r, 12);
    memcpy(m + 4, f, 12);
    memcpy(m + 8, u, 12);
    m[12] = pos->x;
    m[13] = pos->y;
    m[14] = pos->z;
    m[15] = 1.0f;

    ((SetTransform_t)SH_SET_TRANSFORM)(entity, m, 1);
    g_lastError = SH_OK;
    return 1;
}

/* The inverse of ShPlaceEntityRot, so a caller can turn a
 * thing relative to how it already sits.
 */
SH_API int ShGetEntityTransform(uint64_t entity, ShVec3 *pos,
                                float *yaw, float *pitch,
                                float *roll) {
    float m[16], f[3], r[3];
    float y, p, cy, sy, cp, sp;
    float r0[3], u0[3], s;

    if (!entity) return ShFail(SH_ERR_BAD_ARG);
    if (!ShIsEntity(entity)) return ShFail(SH_ERR_NOT_ENTITY);
    if (!ShReadMem(entity + OFF_ENT_MATRIX, m, 64))
        return ShFail(SH_ERR_UNWRITABLE);

    r[0] = m[0]; r[1] = m[1]; r[2] = m[2];
    f[0] = m[4]; f[1] = m[5]; f[2] = m[6];

    if (pos) {
        pos->x = m[12];
        pos->y = m[13];
        pos->z = m[14];
    }

    y = atan2f(f[0], f[1]);
    s = f[2];
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    p = asinf(s);

    /* Roll is what is left once yaw and pitch are taken
     * out: the angle between the actual right axis and
     * the one an unrolled basis would have. */
    cy = cosf(y); sy = sinf(y);
    cp = cosf(p); sp = sinf(p);
    r0[0] = cy;       r0[1] = -sy;      r0[2] = 0.0f;
    u0[0] = -sy * sp; u0[1] = -cy * sp; u0[2] = cp;

    if (yaw) *yaw = y;
    if (pitch) *pitch = p;
    if (roll)
        *roll = atan2f(r[0]*u0[0] + r[1]*u0[1] + r[2]*u0[2],
                       r[0]*r0[0] + r[1]*r0[1] + r[2]*r0[2]);
    g_lastError = SH_OK;
    return 1;
}

/* SetTransform reaches engine code that takes a thread
 * local scratch allocator. Off a game thread TlsGetValue
 * returns null and so does its fallback, and it dies. */
/* The physics ray callback is the wrong game thread spot:
 * it already holds the physics lock and re-entering it
 * deadlocks. The frame path is the window that works. */
#define XQ_MAX 32

typedef struct {
    uint64_t ent;
    ShVec3   pos;
    float    yaw, pitch, roll;
    volatile int ready;
} ShXForm;

static ShXForm g_xq[XQ_MAX];

SH_API int ShQueueTransform(uint64_t entity, const ShVec3 *pos,
                            float yaw, float pitch, float roll) {
    int i;

    if (!entity || !pos) return ShFail(SH_ERR_BAD_ARG);
    for (i = 0; i < XQ_MAX; i++) {
        if (g_xq[i].ready) continue;
        g_xq[i].ent = entity;
        g_xq[i].pos = *pos;
        g_xq[i].yaw = yaw;
        g_xq[i].pitch = pitch;
        g_xq[i].roll = roll;
        /* Published last, so the pump never sees a half
         * filled slot.
         */
        g_xq[i].ready = 1;
        g_lastError = SH_OK;
        return 1;
    }
    return ShFail(SH_ERR_NO_CANDIDATE);
}

/* Drained on the frame path, on the game thread. */
void ShTransformPump(void) {
    int i;

    for (i = 0; i < XQ_MAX; i++) {
        if (!g_xq[i].ready) continue;
        ShPlaceEntityRot(g_xq[i].ent, &g_xq[i].pos, g_xq[i].yaw,
                         g_xq[i].pitch, g_xq[i].roll);
        g_xq[i].ready = 0;
    }
}

/* An entity link in the parent chain means the player is
 * riding a vehicle. On foot every hop is a bone link.
 */
SH_API int ShIsInVehicle(void) {
    ShPlayer p;
    uint64_t cur;
    int n;

    if (!ShGetPlayer(&p)) return 0;
    cur = p.entity;
    for (n = 0; n < 16; n++) {
        uint32_t type = 0;
        uint64_t next = ShParentOfT(cur, &type);
        if (!next) return 0;
        if (type == 1) return 1;
        cur = next;
    }
    return 0;
}

/* Place an entity and confirm THAT entity moved. The
 * player mirror lags and sits metres off in a vehicle.
 */
static int ShPlaceVerified(uint64_t root, const ShVec3 *pos,
                           const ShVec3 *orient, int settleMs) {
    ShVec3 got;
    int spins;

    if (!ShPlaceEntity(root, pos, orient)) return 0;
    if (settleMs <= 0) {
        if (!ShVec(root + OFF_ENT_POS, &got)) return 1;
        return ShNear(&got, pos, 50.0f);
    }
    for (spins = 0; spins * 5 < settleMs; spins++) {
        Sleep(5);
        if (!ShVec(root + OFF_ENT_POS, &got)) continue;
        if (ShNear(&got, pos, 25.0f)) return 1;
    }
    return 0;
}

/* Place the root and confirm the player moved, so a
 * stale resolution cannot claim a false success.
 */
static int ShWriteRoot(const ShVec3 *pos, const ShVec3 *orient) {
    ShPlayer p;
    ShVec3 got;
    int spins;

    if (!ShGetPlayer(&p)) return 0;
    if (!ShPlaceEntity(p.root, pos, orient)) return 0;

    /* The mirror updates on the next frame. */
    for (spins = 0; spins < 40; spins++) {
        Sleep(5);
        if (!ShGetPlayerPosition(&got)) continue;
        if (ShNear(&got, pos, 25.0f)) return 1;
    }
    return 0;
}

/* The walk ends at the vehicle when the player is
 * riding one, so this carries the car and the camera.
 */
SH_API int ShTeleportPlayer(const ShVec3 *pos,
                            const ShVec3 *orient) {
    if (!pos) return ShFail(SH_ERR_BAD_ARG);
    if (ShWriteRoot(pos, orient)) return 1;
    /* A cold resolution can pick the wrong root. */
    ShInvalidate();
    if (ShWriteRoot(pos, orient)) return 1;
    return ShFail(SH_ERR_UNWRITABLE);
}

/* Long jumps crash in a vehicle, so cover the distance
 * in short hops. Proven at 300m with no delay at all.
 */
#define SH_HOP_DEFAULT   300.0f
#define SH_HOP_STOP      10.0f
#define SH_HOP_MAX       64

SH_API int ShTeleportPlayerHops(const ShVec3 *pos,
                                const ShVec3 *orient,
                                float hopMetres, int delayMs) {
    ShPlayer p;
    ShVec3 here, step;
    float hop = hopMetres > 1.0f ? hopMetres : SH_HOP_DEFAULT;
    int guard;

    if (!pos) return ShFail(SH_ERR_BAD_ARG);
    if (!ShGetPlayer(&p)) return 0;

    for (guard = 0; guard < SH_HOP_MAX; guard++) {
        float dx, dy, dist;

        if (!ShVec(p.root + OFF_ENT_POS, &here))
            return ShFail(SH_ERR_UNWRITABLE);
        dx = pos->x - here.x;
        dy = pos->y - here.y;
        dist = sqrtf(dx * dx + dy * dy);

        /* Close enough. Another hop is another drop. */
        if (dist <= SH_HOP_STOP) {
            g_lastError = SH_OK;
            return 1;
        }
        if (dist <= hop)
            return ShPlaceVerified(p.root, pos, orient, delayMs);

        step.x = here.x + dx / dist * hop;
        step.y = here.y + dy / dist * hop;
        /* The hop is inside the streamed radius, so wait
         * for its ground instead of moving blind.
         */
        {
            int t;
            for (t = 0; t < 200; t++) {
                if (ShGroundHeight(step.x, step.y, &step.z)) break;
                Sleep(5);
            }
            if (t >= 200) return ShFail(SH_ERR_NO_GROUND);
            step.z += 1.5f;
        }
        if (!ShPlaceVerified(p.root, &step, NULL, delayMs))
            return ShFail(SH_ERR_UNWRITABLE);

        /* A real pause. The verification above returns as
         * soon as it moves, so it is no delay at all.
         */
        if (delayMs > 0) Sleep((DWORD)delayMs);

        /* Re-resolve every hop. It is a few pointer reads
         * now, and the vehicle can be recreated mid route.
         */
        if (!ShGetPlayer(&p)) return 0;
    }
    return ShFail(SH_ERR_UNWRITABLE);
}

/* Vehicle teleport was removed. It never became
 * reliable, so use ShPlaceEntity with your own values.
 */
