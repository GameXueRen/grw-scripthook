/* GRW ScriptHook plugin API, exported by dinput8.dll.
 * Addresses are resolved once at init and cached.
 */
#ifndef GRW_SCRIPTHOOK_H
#define GRW_SCRIPTHOOK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_API_VERSION 1

#ifdef SH_BUILD
#define SH_API __declspec(dllexport)
#else
#define SH_API
#endif

typedef struct { float x, y, z; } ShVec3;

enum ShGameState {
    SH_STATE_UNKNOWN = 0,
    SH_STATE_MENU,
    SH_STATE_LOADING,
    SH_STATE_LOBBY,
    SH_STATE_INGAME,
    SH_STATE_RELOADING
};

enum ShError {
    SH_OK = 0,
    SH_ERR_BAD_ARG,
    SH_ERR_NO_GLOBAL,
    SH_ERR_NO_POSITION,
    SH_ERR_NO_CANDIDATE,
    SH_ERR_NO_ROOT,
    SH_ERR_UNWRITABLE,
    SH_ERR_NOT_ENTITY,
    SH_ERR_HOOK_FAILED,
    SH_ERR_NO_PHYSICS,
    SH_ERR_NO_GROUND,
    SH_ERR_NOT_IN_GAME,
    SH_ERR_NOT_STREAMED
};

/* Collision exists only near the player. Measured live:
 * hits at 1500m, nothing at 2000m.
 */
#define SH_STREAM_RADIUS 1500.0f

/* root is the top of the parent chain, and the only
 * position that survives a direct write.
 */
typedef struct {
    uint64_t entity;
    uint64_t node;
    uint64_t root;
} ShPlayer;

typedef int (*ShLastError_t)(void);
typedef const char *(*ShErrorString_t)(int err);
typedef int (*ShGetVersion_t)(void);
typedef int (*ShGetPlayer_t)(ShPlayer *out);
typedef int (*ShTeleportPlayer_t)(const ShVec3 *pos,
                                  const ShVec3 *orient);
typedef int (*ShWalkToRoot_t)(uint64_t entity, uint64_t *outRoot);
typedef int (*ShGetPlayerPosition_t)(ShVec3 *out);
typedef void (*ShInvalidate_t)(void);

/* All return 1 on success, 0 on failure.
 * On failure ShLastError gives the reason.
 */
SH_API int  ShLastError(void);
SH_API const char *ShErrorString(int err);
SH_API int  ShGetVersion(void);
SH_API int  ShGetPlayer(ShPlayer *out);
SH_API int  ShGetPlayerPosition(ShVec3 *out);
/** Teleport the player, at any range. Verified 9.9km
 *  cross map, landing within 3m of the target.
 */
/** In a vehicle it carries the car and the camera, and
 *  that is verified cross map too.
 */
SH_API int  ShTeleportPlayer(const ShVec3 *pos,
                             const ShVec3 *orient);

/** Teleport in hops rather than one jump, for when a
 *  single long move misbehaves. 0 hopMetres gives 300m.
 */
/** delayMs is a real pause per hop. 300 is the value
 *  proven over long routes, 0 has crashed the game.
 */
SH_API int  ShTeleportPlayerHops(const ShVec3 *pos,
                                 const ShVec3 *orient,
                                 float hopMetres, int delayMs);

/** True while riding a vehicle, read from the entity
 *  link in the parent chain. Verified both ways.
 */
SH_API int  ShIsInVehicle(void);

/** Place any entity through the engine's set transform.
 *  Children follow, which is how vehicles carry riders.
 */
SH_API int  ShPlaceEntity(uint64_t entity, const ShVec3 *pos,
                          const ShVec3 *orient);
/* Entity kinds, from the engine's own net identity
 * component. SH_KIND_ANY matches every typed entity.
 */
enum ShEntityKind {
    SH_KIND_ANY = 0,
    SH_KIND_PLAYER,
    SH_KIND_NPC,
    SH_KIND_VEHICLE,
    SH_KIND_DRONE,
    SH_KIND_TEAMMATE,
    SH_KIND_TURRET,
    SH_KIND_MINE,
    SH_KIND_DOOR,
    SH_KIND_LOOTCHEST,
    SH_KIND_OTHER
};

/* Untyped entries are scenery logic and markers, so they
 * are hidden unless this flag is passed.
 */
#define SH_FIND_UNNAMED  0x1

typedef struct {
    uint64_t entity;
    ShVec3   pos;
    float    distance;
    int      kind;
    uint32_t maxHealth;
    char     name[32];
} ShEntity;

/* Fills out with entities within radius of the player,
 * nearest first. Returns how many were written.
 */
SH_API int  ShFindEntities(int kind, float radius, uint32_t flags,
                           ShEntity *out, int max);

/* Kind of one entity, and its readable name. */
SH_API int  ShGetEntityKind(uint64_t entity);
SH_API const char *ShKindName(int kind);

/* Components. The class hash IS the component type, read
 * through the reflection descriptor, so no guessing.
 */
typedef struct {
    uint64_t component;
    uint64_t dataBlock;   /* shared definition, at +0x20 */
    uint32_t classHash;
    char     name[32];    /* set when the class is known */
} ShComponent;

/** Fills out with the entity's components, returning how
 *  many were written. A vehicle carries about fifty.
 */
SH_API int ShGetComponents(uint64_t entity, ShComponent *out,
                           int max);

/** The component of a given class, or 0. */
SH_API uint64_t ShFindComponent(uint64_t entity,
                                uint32_t classHash);

/** Run an engine call on the GAME THREAD. Calls that take
 *  engine locks deadlock from any other thread.
 */
/** Queue returns 1 when accepted. Poll ShQueueResult until
 *  it returns 1, which is usually the next frame.
 */
SH_API int  ShQueueCall(uint64_t fn, uint64_t a0, uint64_t a1,
                        uint64_t a2, uint64_t a3);
SH_API int  ShQueueResult(uint64_t *outRet);

/* Vehicle spawning. The engine has no SpawnVehicle: it has
 * a spec per vehicle and a spawner the manager owns.
 */
typedef struct {
    uint32_t    id;
    const char *name;
} ShVehicle;

/** The catalogue, named one spawn at a time by eye. Ids are
 *  stable across sessions, addresses are not.
 */
SH_API int ShVehicleCount(void);
SH_API const ShVehicle *ShVehicleAt(int index);
SH_API const char *ShVehicleName(uint32_t vehicleId);

/** Spawn a vehicle and return its ENTITY, or 0 on failure.
 *  Blocks until it exists, usually a frame or two.
 */
/** Two entries FREEZE the game if entered: the alpaca
 *  0x40081214 and the unused monster 0x40BA6E9D.
 */
SH_API uint64_t ShSpawnVehicle(uint32_t vehicleId,
                               const ShVec3 *pos);
SH_API void ShSpawnInvalidate(void);

SH_API int  ShWalkToRoot(uint64_t entity, uint64_t *outRoot);
SH_API void ShInvalidate(void);

/* Raw physics rays. Every cast the engine makes passes
 * through the hook, weapon traces included.
 */
/* dir is a full length vector, so origin + dir is the end
 * of the trace rather than a unit direction.
 */
/* The collector is an OUTPUT, empty when the call starts,
 * so results are read back one ray later.
 */
typedef struct {
    ShVec3   origin;
    ShVec3   dir;
    ShVec3   hitPos;      /* first record, when hits > 0 */
    uint32_t hits;        /* collector count */
    uint64_t descriptor;
    uint64_t collector;
    uint8_t  raw[32];     /* collector head, for layout work */

    /* Bullets cast with the descriptor at projectile+0x1D0,
     * so the projectile and its hits are reachable.
     */
    uint64_t projectile;  /* 0 when this is not a bullet */
    uint32_t projHits;    /* count at projectile+0xA6A */
    uint64_t hitObject;   /* first hit record, at +0x38 */
    float    hitDist;     /* first hit record, at +0x48 */
} ShRay;

enum ShRayMode {
    SH_RAY_OFF = 0,
    SH_RAY_ALL,        /* everything, tens of thousands a second */
    SH_RAY_DIRECTED    /* skip the straight down ground probes */
};

/* The engine casts about 100k rays a second, so an
 * unfiltered ring holds only a few milliseconds.
 */
/** Record only traces starting within radius of a point
 *  and at least minLength long. Radius 0 records all.
 */
SH_API void ShRayFilter(const ShVec3 *from, float radius,
                        float minLength);
/** Track the player instead. Radius 0 turns it off. */
SH_API void ShRayFilterPlayer(float radius, float minLength);

/** Recording is off by default: it costs a copy on every
 *  cast, and the engine casts a great many per frame.
 */
SH_API void ShRayLog(int mode);
SH_API uint32_t ShRayCount(void);
/** Fills out with the most recent rays, newest first. */
SH_API int ShGetRays(ShRay *out, int max);

/* A query over the recorded rays. Zeroed means match all,
 * so set only the fields you care about.
 */
typedef struct {
    float  minLength;    /* trace at least this long */
    float  maxLength;    /* 0 means no upper bound */
    int    hitsOnly;     /* only traces that hit something */
    ShVec3 from;         /* origin must be close to this */
    float  fromRadius;   /* 0 disables the origin test */
    ShVec3 through;      /* trace must pass near this point */
    float  throughRadius;/* 0 disables the through test */
} ShRayQuery;

/** Newest first, only the rays matching the query. */
SH_API int ShQueryRays(const ShRayQuery *q, ShRay *out, int max);

/* Bullet hits. The engine keeps them on the PROJECTILE,
 * not on the physics collector.
 */
/* Bullets often strike a child part, so root is resolved
 * for you and is the one worth acting on.
 */
typedef struct {
    uint64_t entity;      /* validated against its own id */
    uint64_t root;        /* top of the parent chain */
    int      kind;        /* kind of root, an ShEntityKind */
    uint32_t id;          /* entity+0x138 */
    ShVec3   pos;         /* impact point */
    ShVec3   normal;      /* surface normal */
    float    distance;    /* along the bullet's flight */
    uint64_t shooter;     /* who fired it, 0 if unknown */
    int      byPlayer;    /* the local player fired it */
    uint64_t projectile;
    int      index;       /* record index in the list */
} ShHit;

/** Receivers run on a worker thread the API owns, so any
 *  API call is safe from inside one.
 */
typedef void (*ShHitFn)(const ShHit *hit, void *user);

/** Install the hook. Needed before any hit is reported. */
SH_API int  ShHitHookInstall(void);
SH_API int  ShHitHookReady(void);

/* Flags on the subscription. 0 delivers every event. */
/* MINE_ONLY depends on the shooter field, which is not
 * reliable on every projectile, so it is opt in.
 */
#define SH_EVT_MINE_ONLY     0x1
#define SH_EVT_NO_SELF       0x2

/** Register a receiver, up to eight. */
SH_API int  ShOnHit(ShHitFn fn, void *user, int flags);
/** Remove one receiver, or all of them when fn is NULL. */
SH_API int  ShOffHit(ShHitFn fn);

/* A ring, for callers that would rather poll. */
SH_API uint32_t ShHitCount(void);
SH_API int  ShGetHits(ShHit *out, int max);

/* A shot, reported on the projectile's first step. The
 * same hook feeds this, so no extra install is needed.
 */
typedef struct {
    uint64_t shooter;     /* entity that fired, or 0 */
    int      kind;        /* kind of shooter */
    int      byPlayer;    /* the local player fired it */
    ShVec3   origin;      /* muzzle, where the bullet began */
    ShVec3   dir;         /* unit vector of travel */
    float    yaw;         /* degrees, 0 along +X */
    float    pitch;       /* degrees, positive is up */
    float    range;       /* the weapon's max range */
    uint64_t projectile;
} ShShot;

typedef void (*ShFireFn)(const ShShot *shot, void *user);

/** Register a receiver for shots, up to eight. */
SH_API int  ShOnFire(ShFireFn fn, void *user, int flags);
SH_API int  ShOffFire(ShFireFn fn);
SH_API uint32_t ShShotCount(void);
SH_API int  ShGetShots(ShShot *out, int max);

/* Visibility. Render nodes carry a visible bit, so this is
 * a data write with no engine call behind it.
 */
/** persist rewrites the bit until you show it again. */
SH_API int  ShSetEntityVisible(uint64_t entity, int visible,
                               int persist);
SH_API int  ShEntityNodeCount(uint64_t entity);

/* The menu. One root owned by the API: every plugin adds a
 * submenu, so navigation and drawing are handled for you.
 */
typedef void (*ShMenuFn)(uint32_t menu, uint32_t item, int value,
                         void *user);

/** A top level entry for your plugin. F4 opens the root. */
SH_API uint32_t ShMenuCreate(const char *title);
SH_API uint32_t ShMenuSub(uint32_t parent, const char *label);
SH_API int  ShMenuAction(uint32_t menu, const char *label,
                         ShMenuFn fn, void *user);
SH_API int  ShMenuToggle(uint32_t menu, const char *label,
                         int initial, ShMenuFn fn, void *user);
SH_API int  ShMenuNumber(uint32_t menu, const char *label,
                         float initial, float lo, float hi,
                         float step, ShMenuFn fn, void *user);
/** opts must outlive the menu. String literals are fine. */
SH_API int  ShMenuList(uint32_t menu, const char *label,
                       const char **opts, int n, int initial,
                       ShMenuFn fn, void *user);
/** The line under the items. Empty text removes it. */
SH_API int  ShMenuStatus(uint32_t menu, const char *text);
SH_API void ShMenuSetKey(int vk);
SH_API int  ShMenuIsOpen(void);
SH_API void ShMenuOpen(int open);

/* The overlay. One window owned by the API, so plugins
 * never touch Win32 and slots pack with no gaps.
 */
enum {
    SH_HUD_TOPLEFT = 0,
    SH_HUD_TOPRIGHT,
    SH_HUD_BOTTOMLEFT,
    SH_HUD_BOTTOMRIGHT
};

/** Register a line. Lower priority sits nearer the edge. */
SH_API uint32_t ShHudCreate(const char *name, int anchor,
                            int priority);
/** Text may hold newlines. Empty text hides the slot. */
SH_API int  ShHudSet(uint32_t hud, const char *text);
SH_API int  ShHudColour(uint32_t hud, uint32_t rgb);
SH_API int  ShHudShow(uint32_t hud, int visible);
SH_API void ShHudDestroy(uint32_t hud);

/* The camera. Rows of the pose matrix: right, forward, up,
 * then position. mode is 0 for the player's camera.
 */
typedef struct {
    ShVec3 pos;
    ShVec3 right;
    ShVec3 forward;
    ShVec3 up;
    float  fov;
    int    mode;
    uint64_t camera;
} ShCamera;

/** Redirect the selector thunk. Done for you on demand. */
SH_API int  ShCameraHookInstall(void);
SH_API int  ShCameraReady(void);
SH_API uint64_t ShCameraCalls(void);
SH_API uint64_t ShCameraWrites(void);

/** Where the camera is, and how it is pointed. */
SH_API int  ShGetCamera(ShCamera *out);

/* The engine rebuilds the camera every frame, so an
 * override is reapplied until it is released.
 */
SH_API int  ShSetCamera(const ShVec3 *pos);
SH_API int  ShCameraOrbit(float back, float up);

/* Free camera. Radians, yaw 0 faces +y, pitch up positive.
 * ShCameraAngles reads the current view to start from.
 */
SH_API int  ShCameraFree(const ShVec3 *pos, float yaw, float pitch);
SH_API int  ShCameraAngles(float *yaw, float *pitch);
SH_API void ShCameraRelease(void);

/* Everything the camera object exposes, in one call. Set
 * only the bits you want; the engine keeps the rest.
 */
#define SH_CAM_POS    0x01
#define SH_CAM_ROT    0x02
#define SH_CAM_FOV    0x04
#define SH_CAM_SKEW   0x08
#define SH_CAM_MODE   0x10

typedef struct {
    uint32_t apply;
    ShVec3   pos;
    float    yaw, pitch, roll;
    float    fov;
    float    skewX, skewY;
    int      mode;
} ShCameraOverride;

/** Reapplied every frame until ShCameraRelease. */
SH_API int  ShCameraApply(const ShCameraOverride *o);

/* The nine derived matrices at camera+0x420, view and
 * projection and their inverses. Index 0 to 8, 16 floats.
 */
SH_API int  ShCameraMatrix(int index, float *out16);

/* Ground queries. The physics hook installs itself on the
 * first call that needs it, and is shared process wide.
 */

/* Game flow state. The hook installs on first use and
 * tracks the engine's own state transitions.
 */
SH_API int  ShGetGameState(void);
SH_API int  ShIsInGame(void);
SH_API int  ShGetGameStateName(char *buf, int len);

typedef int (*ShGetGameState_t)(void);
typedef int (*ShIsInGame_t)(void);

/* Predicate only, never installs anything. */
SH_API int  ShPhysicsReady(void);
SH_API int  ShGroundHeight(float x, float y, float *outZ);
SH_API int  ShGroundHeightFrom(float x, float y, float nearZ,
                               float *outZ);
SH_API int  ShTeleportPlayerToGround(float x, float y,
                                     float clearance);

/* Health, local player. The API owns the reference: it
 * resolves and caches internally, plugins never hold one.
 */
SH_API int  ShGetHealthPlayer(uint32_t *cur, uint32_t *max);
SH_API int  ShSetHealthPlayer(uint32_t value);
SH_API int  ShSetGodModePlayer(int on);
SH_API int  ShSetCannotDiePlayer(int on);
SH_API void ShInvalidateHealth(void);

/* Entity targeted variants. The entity comes from the
 * enumerator, never from a raw pointer a plugin invented.
 */
SH_API int  ShGetHealthEntity(uint64_t entity, uint32_t *cur,
                              uint32_t *max);
SH_API int  ShSetHealthEntity(uint64_t entity, uint32_t value);
SH_API int  ShSetGodModeEntity(uint64_t entity, int on);

typedef int (*ShGetHealthPlayer_t)(uint32_t *, uint32_t *);
typedef int (*ShSetHealthPlayer_t)(uint32_t);
typedef int (*ShSetGodModePlayer_t)(int);
typedef int (*ShPhysicsReady_t)(void);
typedef int (*ShGroundHeight_t)(float, float, float *);
typedef int (*ShTeleportPlayerToGround_t)(float, float, float);

#ifdef __cplusplus
}
#endif

#endif
