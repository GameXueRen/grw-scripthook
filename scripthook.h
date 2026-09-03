/** @file scripthook.h
 *  GRW ScriptHook plugin API from dinput8.dll. Addresses
 *  resolve once at init and are cached. */
#ifndef GRW_SCRIPTHOOK_H
#define GRW_SCRIPTHOOK_H

#include <stdint.h>

/* Compiler shims so the same sources build with MSVC, which
 * has no __attribute__ and already uses the MS x64 ABI that
 * these engine callbacks are written for. GCC and Clang keep
 * the attribute; MSVC treats ms_abi as a no-op and gets its
 * alignment from __declspec(align) instead. */
#ifdef _MSC_VER
#define __attribute__(x) /* no-op */
#define SH_ALIGNED(x)    __declspec(align(x))
#else
#define SH_ALIGNED(x)    __attribute__((aligned(x)))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @defgroup core Core
 *  Errors, versioning and the shared types.
 *  @{ */

#define SH_API_VERSION 1

#ifdef SH_BUILD
#define SH_API __declspec(dllexport)
#else
#define SH_API
#endif

/** Metres. x east, y north, z up. */
typedef struct { float x, y, z; } ShVec3;

/** @} */

/** @defgroup state Game state
 *  The engine's GameFlow machine, tracked by the hook.
 *  @{ */

enum ShGameState {
    SH_STATE_UNKNOWN = 0,
    SH_STATE_MENU,
    SH_STATE_LOADING,
    SH_STATE_LOBBY,
    SH_STATE_INGAME,
    SH_STATE_RELOADING,

    /** In game with the pause menu, loadout, map or skills
     *  up. ShIsInGame stays true: the world is loaded. */
    SH_STATE_PAUSED,
    /** The game over screen is drawn. */
    SH_STATE_GAMEOVER,

    /** Live play with the engine driving its own camera.
     *  The game is running, so these are NOT paused, and
     *  ShIsInGame stays true for all three. */
    /** A mod that places the camera every frame has to let
     *  go here, or it fights the engine for it. */
    SH_STATE_DRONE,
    SH_STATE_BINOCULAR,
    SH_STATE_CINEMATIC
};

/** What the game's UI is showing, from the scenes it drew
 *  last frame. Several can be up at once. */
enum ShUiState {
    SH_UI_DRONE      = 0x0001,  /**< flying the drone */
    SH_UI_BINOCULAR  = 0x0002,
    SH_UI_VEHICLE    = 0x0004,  /**< vehicle HUD */
    SH_UI_PAUSE      = 0x0008,  /**< the pause menu (tabs) */
    SH_UI_LOADOUT    = 0x0010,
    SH_UI_MAP        = 0x0020,
    SH_UI_SKILLS     = 0x0040,
    SH_UI_COMWHEEL   = 0x0080,
    SH_UI_GAMEOVER   = 0x0100,
    SH_UI_LOADING    = 0x0200,
    SH_UI_CINEMATIC  = 0x0400,
    SH_UI_POPUP      = 0x0800   /**< a modal popup */
};
SH_API uint32_t ShGetUiState(void);
SH_API int      ShInDrone(void);
SH_API int      ShInLoadout(void);
SH_API int      ShInMap(void);
SH_API int      ShInBinocular(void);

/** @} */
/** @addtogroup core
 *  @{ */

/** Every int function returns 1 on success, 0 on failure,
 *  and on failure ShLastError carries one of these.
 */
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
    SH_ERR_NOT_STREAMED,
    SH_ERR_CONTROLLER,
    SH_ERR_UI_NOT_READY,   /**< in game, scene not up yet */
    SH_ERR_UI_PROP,        /**< no such property on the class */
    SH_ERR_UI_ASSET        /**< font or texture not loaded */
};

/** @} */
/** @defgroup ground Ground
 *  Height through engine physics, near the player.
 *  @{ */

/** Collision exists only near the player. Measured live:
 *  hits at 1500m, nothing at 2000m.
 */
#define SH_STREAM_RADIUS 1500.0f

/** @} */
/** @defgroup player Player
 *  The local player, via the engine's own component.
 *  @{ */

/** root is the top of the parent chain, and the only
 *  position that survives a direct write.
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

/** @} */
/** @addtogroup core
 *  @{ */

/** The last error for the calling context. */
SH_API int  ShLastError(void);
/** The readable name of an error code. */
SH_API const char *ShErrorString(int err);
/** SH_API_VERSION, for compatibility checks. */
SH_API int  ShGetVersion(void);

/** @} */
/** @addtogroup player
 *  @{ */

/** The local player. In a vehicle the root re-parents to
 *  the vehicle; the entity stays the soldier.
 */
SH_API int  ShGetPlayer(ShPlayer *out);
/** The soldier's own position, from its matrix. Use this
 *  for anything placed in the world. */
SH_API int  ShGetPlayerPosition(ShVec3 *out);
/** Where the player global says the view sits, about 1.9m
 *  behind and 1.6m above the body in third person. */
SH_API int  ShGetCameraEyePosition(ShVec3 *out);
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

/** @} */
/** @defgroup entities Entities
 *  Enumeration by kind, via the net identity component.
 *  @{ */

/** Place any entity through the engine's set transform.
 *  Children follow, which is how vehicles carry riders.
 */
SH_API int  ShPlaceEntity(uint64_t entity, const ShVec3 *pos,
                          const ShVec3 *orient);

/** Full orientation in radians, which ShPlaceEntity
 *  cannot express: roll puts a car on its roof.
 */
SH_API int  ShPlaceEntityRot(uint64_t entity, const ShVec3 *pos,
                             float yaw, float pitch, float roll);

/** Its inverse: where a thing is and how it is turned, so
 *  a caller can rotate relative to that.
 */
SH_API int  ShGetEntityTransform(uint64_t entity, ShVec3 *pos,
                                 float *yaw, float *pitch,
                                 float *roll);

/** Applied on the frame path rather than right now. Set
 *  transform wants a game thread's scratch allocator.
 */
SH_API int  ShQueueTransform(uint64_t entity, const ShVec3 *pos,
                             float yaw, float pitch, float roll);
/** Entity kinds, from the engine's own net identity
 *  component. SH_KIND_ANY matches every typed entity.
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

/** Untyped entries are scenery logic and markers, so they
 *  are hidden unless this flag is passed.
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

/** Fills out with entities within radius of the player,
 *  nearest first. Returns how many were written.
 */
SH_API int  ShFindEntities(int kind, float radius, uint32_t flags,
                           ShEntity *out, int max);

/** Kind of one entity. */
SH_API int  ShGetEntityKind(uint64_t entity);
/** The readable name of a kind. */
SH_API const char *ShKindName(int kind);

/** Components. The class hash IS the component type, read
 *  through the reflection descriptor, so no guessing.
 */
typedef struct {
    uint64_t component;
    uint64_t dataBlock;   /**< shared definition, at +0x20 */
    uint32_t classHash;
    char     name[32];    /**< set when the class is known */
} ShComponent;

/** Fills out with the entity's components, returning how
 *  many were written. A vehicle carries about fifty.
 */
SH_API int ShGetComponents(uint64_t entity, ShComponent *out,
                           int max);

/** The component of a given class, or 0. */
SH_API uint64_t ShFindComponent(uint64_t entity,
                                uint32_t classHash);

/** @} */
/** @defgroup engine Engine calls
 *  Queue your own finds onto the game thread.
 *  @{ */

/** Run an engine call on the GAME THREAD. Calls that take
 *  engine locks deadlock from any other thread.
 */
/** Queue returns 1 when accepted. Poll ShQueueResult until
 *  it returns 1, which is usually the next frame.
 */
SH_API int  ShQueueCall(uint64_t fn, uint64_t a0, uint64_t a1,
                        uint64_t a2, uint64_t a3);

/** Args three to five as floats, which the ABI passes in
 *  xmm2, xmm3 and the stack rather than the int registers.
 */
SH_API int  ShQueueCallF(uint64_t fn, uint64_t a0, uint64_t a1,
                         float f2, float f3, float f4);
SH_API int  ShQueueResult(uint64_t *outRet);

/** @} */
/** @defgroup vehicles Vehicles
 *  A spec per vehicle and a spawner the manager owns.
 *  @{ */

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
/** Warm the spec cache on a background thread so the first
 *  spawn does not scan the address space synchronously. Call
 *  once after the world is loaded; later calls do nothing. */
SH_API void ShSpawnWarm(void);
SH_API void ShSpawnInvalidate(void);

/** Guarded reads, for anything walking engine memory. They
 *  go through the kernel, so a freed page fails instead of
 *  killing the game. */
SH_API int      ShReadBytes(uint64_t addr, void *out, uint32_t len);
SH_API uint64_t ShReadU64(uint64_t addr, int *ok);
SH_API float    ShReadF32(uint64_t addr, int *ok);

/** A per frame hook on the game thread. Keep it fast: the
 *  frame waits for it. Registration fails past 16 slots. */
typedef void (*ShFrameFn_t)(void *user);
SH_API int  ShRegisterFrameCallback(ShFrameFn_t fn, void *user);
SH_API void ShUnregisterFrameCallback(ShFrameFn_t fn);

/** @} */
/** @defgroup domino World and entity calls from Domino
 *  Recovered from the mission script operators; see the
 *  map-domino files in scripthook/spawnmaps. @{ */

/** One lightning strike, queued in the weather system. */
SH_API int   ShTriggerLightning(void);
/** The live ground wetness. Writing it does nothing: the
 *  request slot is ignored, so there is no setter. */
SH_API float ShGetWetness(void);
/** Override the lightning rate, or 0 to hand it back. */
SH_API int   ShSetLightningFrequency(int enable, float value);

/** The player's own god and ghost bytes. */
SH_API int   ShSetGodMode(int on);
SH_API int   ShSetGhostMode(int on);
SH_API int   ShGetGodMode(void);

/** A sphere explosions ignore. NULL clears it. */
SH_API int   ShExplosionShield(const ShVec3 *at, float radius);

/** Physics bodies on or off for one entity. */
SH_API int   ShSetEntityPhysics(uint64_t entity, int on);
/** The child follows the parent at an offset. */
SH_API int   ShAttachEntity(uint64_t child, uint64_t parent,
                            const ShVec3 *offset);
SH_API int   ShDetachEntity(uint64_t child);

/** @} */
/** @defgroup npcs NPCs
 *  Standalone NPCs by archetype, built like the engine's
 *  spawn director does (FINDINGS "NPC SPAWN WORKS"). @{ */

typedef struct {
    uint64_t id;     /**< archetype id, stable across sessions */
    int      kind;   /**< 1 = spawnable NPC, 10 = none */
} ShNpcArchetype;

/** The archetype registry, read once per session on first
 *  use. About 530 entries, about 150 of them kind 1. */
SH_API int ShNpcCount(void);
SH_API const ShNpcArchetype *ShNpcAt(int index);

/** Spawn an NPC and return its ENTITY, or 0 on failure.
 *  Blocks until it exists, usually a frame or two. */
/** The NPC joins the population manager with the current
 *  Camp and Job, so its AI runs like a native spawn. */
SH_API uint64_t ShSpawnNpc(uint64_t archetypeId, const ShVec3 *pos);

/** Retire a spawned entity, NPC or vehicle, through the
 *  spawn manager. Entities built outside the spawn system
 *  have no spec and refuse. */
SH_API int ShDespawn(uint64_t entity);

/** @} */
/** @addtogroup player
 *  @{ */

/** Walk an entity's parent chain to its root. */
SH_API int  ShWalkToRoot(uint64_t entity, uint64_t *outRoot);
/** Drop the cached player, so the next call re-resolves. */
SH_API void ShInvalidate(void);

/** @} */
/** @defgroup rays Physics rays
 *  Every cast the engine makes passes through the hook.
 *  @{ */

/** dir is a full length vector, so origin + dir is the end
 *  of the trace rather than a unit direction.
 */
/** The collector is an OUTPUT, empty when the call starts,
 *  so results are read back one ray later.
 */
typedef struct {
    ShVec3   origin;
    ShVec3   dir;
    ShVec3   hitPos;      /**< first record, when hits > 0 */
    uint32_t hits;        /**< collector count */
    uint64_t descriptor;
    uint64_t collector;
    uint8_t  raw[32];     /**< collector head, for layout work */

    /** Bullets cast with the descriptor at projectile+0x1D0,
     *  so the projectile and its hits are reachable.
     */
    uint64_t projectile;  /**< 0 when this is not a bullet */
    uint32_t projHits;    /**< count at projectile+0xA6A */
    uint64_t hitObject;   /**< first hit record, at +0x38 */
    float    hitDist;     /**< first hit record, at +0x48 */
} ShRay;

enum ShRayMode {
    SH_RAY_OFF = 0,
    SH_RAY_ALL,        /**< everything, tens of thousands a second */
    SH_RAY_DIRECTED    /**< skip the straight down ground probes */
};

/** The engine casts about 100k rays a second, so an
 *  unfiltered ring holds only a few milliseconds.
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

/** A query over the recorded rays. Zeroed means match all,
 *  so set only the fields you care about.
 */
typedef struct {
    float  minLength;    /**< trace at least this long */
    float  maxLength;    /**< 0 means no upper bound */
    int    hitsOnly;     /**< only traces that hit something */
    ShVec3 from;         /**< origin must be close to this */
    float  fromRadius;   /**< 0 disables the origin test */
    ShVec3 through;      /**< trace must pass near this point */
    float  throughRadius;/**< 0 disables the through test */
} ShRayQuery;

/** Newest first, only the rays matching the query. */
SH_API int ShQueryRays(const ShRayQuery *q, ShRay *out, int max);

/** @} */
/** @defgroup combat Combat events
 *  Shots and impacts from the projectile hook.
 *  @{ */

/** Bullets often strike a child part, so root is resolved
 *  for you and is the one worth acting on.
 */
typedef struct {
    uint64_t entity;      /**< validated against its own id */
    uint64_t root;        /**< top of the parent chain */
    int      kind;        /**< kind of root, an ShEntityKind */
    uint32_t id;          /**< entity+0x138 */
    ShVec3   pos;         /**< impact point */
    ShVec3   normal;      /**< surface normal */
    float    distance;    /**< along the bullet's flight */
    uint64_t shooter;     /**< who fired it, 0 if unknown */
    int      byPlayer;    /**< the local player fired it */
    uint64_t projectile;
    int      index;       /**< record index in the list */
} ShHit;

/** Receivers run on a worker thread the API owns, so any
 *  API call is safe from inside one.
 */
typedef void (*ShHitFn)(const ShHit *hit, void *user);

/** Install the hook. Needed before any hit is reported. */
SH_API int  ShHitHookInstall(void);
SH_API int  ShHitHookReady(void);

/** Flags on the subscription. 0 delivers every event. */
/** MINE_ONLY depends on the shooter field, which some
 *  projectiles omit, so it is opt in.
 */
#define SH_EVT_MINE_ONLY     0x1
#define SH_EVT_NO_SELF       0x2

/** Register a receiver, up to eight. */
SH_API int  ShOnHit(ShHitFn fn, void *user, int flags);
/** Remove one receiver, or all of them when fn is NULL. */
SH_API int  ShOffHit(ShHitFn fn);

/** A ring, for callers that would rather poll. */
SH_API uint32_t ShHitCount(void);
SH_API int  ShGetHits(ShHit *out, int max);

/** A shot, reported on the projectile's first step. The
 *  same hook feeds this, so no extra install is needed.
 */
typedef struct {
    uint64_t shooter;     /**< entity that fired, or 0 */
    int      kind;        /**< kind of shooter */
    int      byPlayer;    /**< the local player fired it */
    ShVec3   origin;      /**< muzzle, where the bullet began */
    ShVec3   dir;         /**< unit vector of travel */
    float    yaw;         /**< degrees, 0 along +X */
    float    pitch;       /**< degrees, positive is up */
    float    range;       /**< the weapon's max range */
    uint64_t projectile;
} ShShot;

typedef void (*ShFireFn)(const ShShot *shot, void *user);

/** Register a receiver for shots, up to eight. */
SH_API int  ShOnFire(ShFireFn fn, void *user, int flags);
SH_API int  ShOffFire(ShFireFn fn);
SH_API uint32_t ShShotCount(void);
SH_API int  ShGetShots(ShShot *out, int max);

/** @} */
/** @defgroup visibility Visibility
 *  Render node visible bits, applied on the game thread.
 *  @{ */

/** node 0 means the whole entity. persist rewrites the bit
 *  each frame until you show it again.
 */
SH_API int  ShSetVisible(uint64_t entity, uint64_t node,
                         int visible, int persist);
SH_API int  ShEntityNodeCount(uint64_t entity);

/** Enumerate parts, so a caller can hide any subset. The
 *  head group is what the camera hides while aiming.
 */
SH_API int  ShGetEntityNodes(uint64_t entity, uint64_t *out,
                             int max);
/** The group appears the first time the player aims, so 0
 *  is normal until then. Retry on a timer.
 */
SH_API int  ShGetHeadNodes(uint64_t entity, uint64_t *out,
                           int max);

/** @} */
/** @defgroup menu Menu
 *  One root owned by the API; every plugin adds a submenu.
 *  @{ */

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
/** Drop a menu's items, keeping the row, so a plugin can
 *  rebuild its own menu without stacking duplicates. */
SH_API int  ShMenuClear(uint32_t menu);
/** Remove the row and its subtree. */
SH_API int  ShMenuDestroy(uint32_t menu);
/** The line under the items. Empty text removes it. */
SH_API int  ShMenuStatus(uint32_t menu, const char *text);
/** Set the status line from a printf template. The English
 *  template is translated as the menu's own scope first, so the
 *  [lang.<menu>] value keeps the same % placeholders and the
 *  result is formatted once with the arguments. The language is
 *  fixed at startup, so translating here matches the capture
 *  path. The template and value must use the same conversion
 *  specifications. */
SH_API int  ShMenuStatusF(uint32_t menu, const char *fmt, ...);
/** The hint shown under the title of this submenu, replacing the
 *  control hints the root menu shows. Empty text removes it. The
 *  text is translated as the menu's own scope, so it can be an
 *  English key in the [lang.<menu>] table. */
SH_API int  ShMenuHint(uint32_t menu, const char *text);
SH_API void ShMenuSetKey(int vk);
SH_API int  ShMenuIsOpen(void);
SH_API void ShMenuOpen(int open);

/** One row of the menu, copied for the overlay renderer. */
typedef struct ShMenuRow {
    char name[96];
    char value[32];
    int  selected;
} ShMenuRow;

/** One frame of the current menu, captured under the lock for
 *  the D3D11 overlay (scripthook_ovl.cpp) to draw. */
typedef struct ShMenuView {
    char title[48];
    char hint[128];   /**< control hints under the title, \n lines */
    char status[96];
    char footer[16];
    int  rows;
    int  sel;
    int  isRoot;      /**< 1 when the root menu is being shown */
    ShMenuRow row[12];
} ShMenuView;

/** Internal: snapshot the current menu for the overlay. */
void ShMenuCaptureView(ShMenuView *v);
/** Internal: tell the menu the overlay can render now. */
void ShMenuSetOverlayReady(int ready);

/** @} */
/** @defgroup hud HUD
 *  Drawn by the engine's own UI; slots pack per corner.
 *  @{ */

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

/** @} */
/** @defgroup camera Camera
 *  Per field ownership, so plugins compose.
 *  @{ */

/** Rows of the pose matrix: right, forward, up, then
 *  position. mode is 0 for the player's camera.
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

/** @} */
/** @addtogroup state
 *  @{ */

/** The game flow stays in Playing while paused, so this is
 *  read off the camera: the pause menu renders through a
 *  template pose no steered camera ever holds.
 */
SH_API int  ShInPauseMenu(void);

/** @} */
/** @addtogroup camera
 *  @{ */

/** The close range body blur, a hidden proximity fade the
 *  menu's DoF settings leave running. on=0 removes it,
 *  on=1 restores the engine default.
 */
SH_API int  ShSetCameraBlur(int on);
SH_API int  ShCameraBlurOff(void);

/** @} */

/** @defgroup motion Motion
 *  Push things rather than teleporting them.
 *  @{ */

/** Pass an entity, never a body id: an entity owns several
 *  bodies and these calls write every one of them.
 */

/** Metres a second, world axes, Z up. Clamped to 200. */
SH_API int  ShGetVelocity(uint64_t entity, ShVec3 *out);
SH_API int  ShSetVelocity(uint64_t entity, const ShVec3 *v);
SH_API int  ShAddVelocity(uint64_t entity, const ShVec3 *v);

/** Radians a second about each world axis. */
SH_API int  ShGetAngularVelocity(uint64_t entity, ShVec3 *out);
SH_API int  ShSetAngularVelocity(uint64_t entity, const ShVec3 *v);
SH_API int  ShAddAngularVelocity(uint64_t entity, const ShVec3 *v);

/** A push at a world point, shoving and spinning like a
 *  hit on a corner. NULL point gives a pure shove.
 */
SH_API int  ShShove(uint64_t entity, const ShVec3 *dir,
                    float strength, const ShVec3 *atWorldPos);

/** True when velocity moves this entity. Characters run on
 *  a controller that ignores it, resting bodies sleep.
 */
SH_API int  ShCanMove(uint64_t entity);

/** @} */

/** @defgroup havokdiag Havok diagnostics
 *  For debugging the mapping rather than for mods.
 *  @{ */

SH_API uint64_t ShHavokWorld(void);
SH_API uint32_t ShGetBodyId(uint64_t entity);
SH_API int  ShHavokScan(int *bodies, int *owners, int *mapped);

/** @} */

/** @defgroup input Input
 *  Blocked at the game's own import slots, since it polls.
 *  @{ */

#define SH_INPUT_KEYS  0x01  /**< every key */
#define SH_INPUT_MOVE  0x02  /**< WASD, space, shift, ctrl */
#define SH_INPUT_FIRE  0x04  /**< left mouse */
#define SH_INPUT_AIM   0x08  /**< right mouse */
#define SH_INPUT_LOOK  0x10  /**< mouse aiming */

/** Alt, Tab, Esc, F4 and Win always get through. */
SH_API int  ShBlockInput(uint32_t mask);
SH_API uint32_t ShBlockedInput(void);

/** @} */

/** Watch faults first and resume ones with a null path.
 *  Off by default: it breaks titles that fault on purpose.
 */
SH_API int  ShSetCrashIntercept(int on);
SH_API int  ShCrashInterceptOn(void);

/** @defgroup crash Crash reports
 *  Faults land in logs/scripthook_crash.log, annotated.
 *  @{ */

/** How many crashes have been caught this session. */
SH_API int  ShCrashCount(void);

/** How many were resumed on the engine's own null path,
 *  which the player never sees.
 */
SH_API int  ShCrashHealed(void);

/** The first report of the session, as text. */
SH_API int  ShCrashReport(char *buf, int len);

/** Freeze parks the faulting thread instead of dying,
 *  for a live post mortem. Opt in: a handled exception
 *  hangs rather than continues. */
SH_API int  ShSetCrashFreeze(int on);
SH_API int  ShCrashFreezeOn(void);

/** @} */

/** @addtogroup camera
 *  @{ */

/** Where the camera is, and how it is pointed. */
SH_API int  ShGetCamera(ShCamera *out);

/** The engine rebuilds the camera every frame, so an
 *  override is reapplied until it is released.
 */
SH_API int  ShSetCamera(const ShVec3 *pos);
SH_API int  ShCameraOrbit(float back, float up);

/** Free camera. Radians, yaw 0 faces +y, pitch up positive.
 *  ShCameraAngles reads the current view to start from.
 */
SH_API int  ShCameraFree(const ShVec3 *pos, float yaw, float pitch);
SH_API int  ShCameraAngles(float *yaw, float *pitch);
SH_API void ShCameraRelease(void);

/** Ownership is per field, so plugins compose. Release only
 *  what you took and another plugin's fields keep running.
 */
SH_API void ShCameraReleaseFields(uint32_t fields);
SH_API uint32_t ShCameraOwned(void);

/** First person: the eye tracks the head bone every frame
 *  and eases onto the aim ray during ADS, so sights stay
 *  centered. forward clears the face.
 */
SH_API int  ShCameraFirstPerson(float forward, float up);

/** The head in world space, from the bone named Head. This
 *  is the eye. ShGetPlayerPosition is NOT: it was measured
 *  1.71m above the feet once and 2.72m another time.
 */
SH_API int  ShGetHeadPosition(ShVec3 *out);
SH_API int  ShHeadBone(void);

/** Everything the camera object exposes, in one call. Set
 *  only the bits you want; the engine keeps the rest.
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

/** The nine derived matrices at camera+0x420, view and
 *  projection and their inverses. Index 0 to 8, 16 floats.
 */
SH_API int  ShCameraMatrix(int index, float *out16);

/** @} */
/** @addtogroup state
 *  @{ */

/** Game flow state. The hook installs on first use and
 *  tracks the engine's own state transitions.
 */
SH_API int  ShGetGameState(void);
/** Stays true while paused: the world is loaded and every
 *  read keeps working, so callers keep their state on Esc.
 */
SH_API int  ShIsInGame(void);
SH_API int  ShGetGameStateName(char *buf, int len);

typedef int (*ShGetGameState_t)(void);
typedef int (*ShIsInGame_t)(void);

/** @} */
/** @addtogroup ground
 *  @{ */

/** Predicate only, never installs anything. */
SH_API int  ShPhysicsReady(void);
SH_API int  ShGroundHeight(float x, float y, float *outZ);
/** Probe from a given height, for stacked geometry like
 *  bridges and caves.
 */
SH_API int  ShGroundHeightFrom(float x, float y, float nearZ,
                               float *outZ);
SH_API int  ShTeleportPlayerToGround(float x, float y,
                                     float clearance);

/** @} */
/** @defgroup stats Stats and ammo
 *  Health, resources, skill points, stealth and ammo.
 *  @{ */

/** Health, local player. The API owns the reference: it
 *  resolves and caches internally, plugins never hold one.
 */
SH_API int  ShGetHealthPlayer(uint32_t *cur, uint32_t *max);
SH_API int  ShSetHealthPlayer(uint32_t value);
SH_API int  ShSetGodModePlayer(int on);

/** Damage through the engine's own path, so death runs
 *  its real sequence. Queued onto the game thread.
 */
SH_API int  ShDamagePlayer(uint32_t amount);
SH_API int  ShKillPlayer(void);
/** Floors at the downed state instead of dying. */
SH_API int  ShSetCannotDiePlayer(int on);
SH_API void ShInvalidateHealth(void);

/** Entity targeted variants. The entity comes from the
 *  enumerator, never from a raw pointer a plugin invented.
 */
SH_API int  ShGetHealthEntity(uint64_t entity, uint32_t *cur,
                              uint32_t *max);
SH_API int  ShSetHealthEntity(uint64_t entity, uint32_t value);
SH_API int  ShSetGodModeEntity(uint64_t entity, int on);

/** Protected ints, the general stat storage: four bit planes
 *  and an XOR key. Health, resources, skill points, XP.
 */
SH_API int  ShStatRead(uint64_t stat, uint32_t *out);
SH_API int  ShStatWrite(uint64_t stat, uint32_t value);

/** The four crafting resources, by name. */
#define SH_RES_FOOD      0
#define SH_RES_GASOLINE  1
#define SH_RES_MEDICINE  2
#define SH_RES_COMMS     3
SH_API int  ShGetResource(int which, uint32_t *out);
SH_API int  ShSetResource(int which, uint32_t value);
SH_API int  ShSetAllResources(uint32_t value);

/** Skill points, a plain int rather than a protected one. */
SH_API int  ShGetSkillPoints(uint32_t *out);
SH_API int  ShSetSkillPoints(uint32_t value);

/** Visibility to enemies. 1 normal, 0 invisible, 0.5 halves
 *  the detection range, above 1 is easier to spot.
 */
SH_API int  ShSetVisibility(float factor);
SH_API int  ShGetVisibility(float *out);

/** Ammo by weapon slot: 0 primary, 1 second, 2 sidearm. */
SH_API int  ShGetAmmo(int slot, uint32_t *out);
SH_API int  ShSetAmmo(int slot, uint32_t value);

/** @} */
/** @defgroup weather Weather and time
 *  The environment object, via the engine's transition.
 *  @{ */

/** Weather type, blended by the engine over its default
 *  ten seconds. Ambient stays off until ShReleaseWeather.
 */
enum ShWeather {
    SH_WEATHER_SUNNY = 0,
    SH_WEATHER_CLOUDS_LIGHT,
    SH_WEATHER_CLOUDS_HEAVY,
    SH_WEATHER_FOG,
    SH_WEATHER_RAIN_LIGHT,
    SH_WEATHER_RAIN_HEAVY
};
SH_API int  ShSetWeather(int type);
/** The same, blended over seconds. 0 changes at once. */
SH_API int  ShSetWeatherBlend(int type, float seconds);
/** Hand the weather back to the ambient system. */
SH_API int  ShReleaseWeather(void);
SH_API int  ShGetWeather(int *out);

/** Time of day in hours past midnight, 0 to 24. The clock
 *  keeps running from the new hour.
 */
SH_API int  ShSetTime(float hours);
SH_API int  ShGetTime(float *out);

/** Clock rate. 1 is normal, 0 stops it, 100 runs a day in
 *  about fifteen minutes. Holds until set again.
 */
SH_API int  ShSetTimeSpeed(float multiplier);
SH_API int  ShGetTimeSpeed(float *out);

/** @} */
/** @defgroup reflect Reflected objects
 *  The engine's own method tables, callable by name.
 *  @{ */

/** An object is [vtable, methodTable, ...]; the table is
 *  32 byte entries of crc32(name), index and function.
 */
typedef struct {
    uint32_t nameHash;
    int      index;
    uint64_t fn;
} ShMethod;

/** The function behind a method name, 0 when absent. */
SH_API int  ShReflectMethod(uint64_t obj, uint32_t nameHash,
                            uint64_t *outFn);
/** Every method of the object's class, in table order. */
SH_API int  ShReflectMethods(uint64_t obj, ShMethod *out, int max);
/** The class hash, through the descriptor getter. */
SH_API uint32_t ShReflectClassHash(uint64_t obj);

/** Call a method by name on the game thread and wait.
 *  obj is this; up to three more integer arguments.
 */
SH_API int  ShReflectCall(uint64_t obj, uint32_t nameHash,
                          uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t *outRet);

/** crc32 of the method names the scene calls rely on. */
#define SH_HASH_ENTER     0x78B1EF6Au
#define SH_HASH_EXIT      0x343B2B30u
#define SH_HASH_INIT      0x66464B4Au
#define SH_HASH_SHUTDOWN  0x6CD4BC94u
#define SH_HASH_GAMEOVER  0xCA671D0Au  /**< GameFlow, reason */

/** A scene is any reflected object with Enter and Exit.
 *  Enter refuses objects without Exit, so it can be undone.
 */
SH_API int  ShSceneEnter(uint64_t obj);
SH_API int  ShSceneExit(uint64_t obj);

/** The GR_GameFlow machine, identity checked. */
SH_API uint64_t ShGameFlow(void);

/** Its sub objects, slots 0 to 16. Slot 9 is the game over
 *  sequence: Enter plays the death with no reload, Exit
 *  restores. Verified in game. */
#define SH_FLOW_GAMEOVER_SCENE  9
#define SH_FLOW_ALT_SCENE       11
SH_API uint64_t ShGameFlowObject(int slot);

/** The HybridMenu, the shell every menu page lives in. */
SH_API uint64_t ShHybridMenu(void);

/** The real thing: death, card and checkpoint reload.
 *  Reason 1 is the one verified in game.
 */
SH_API int  ShTriggerGameOver(int reason);

/** @} */
/** @defgroup ui Native UI
 *  Engine widgets in scenes of our own; see docs/ui.md.
 *  @{ */

/** 1 once the world is up; poll before building. It also
 *  fires the reset callbacks after a world reload. */
SH_API int      ShUiReady(void);
/** Kill switch, on by default. */
SH_API void     ShUiEnable(int on);
/** Font and plate texture for new widgets, by asset GUID
 *  ("873fe53f-3b90-db4d-9887-d3cc6edeaba9", storage order).
 *  Defaults: the HUD font and the white 16x16 texture. */
SH_API int      ShUiSetDefaultFont(const char *guid);
SH_API int      ShUiSetDefaultImage(const char *guid);
/** Changes when the scene reloads; older ids are dead. */
SH_API int      ShUiGen(void);
/** The phoenix::Scene handle that hosts every ShUi widget,
 *  built and driven by the DLL itself; 0 until in game. */
SH_API uint64_t ShSceneHandle(void);

/** A container with a translucent quad behind it. */
SH_API uint32_t ShUiPanel(float x, float y, float w, float h,
                          uint32_t rgb, float alpha);
/** A line of text in the HUD font. panel 0 is the root. */
SH_API uint32_t ShUiLabel(uint32_t panel, float x, float y, float w,
                          float h, const char *text, uint32_t rgb);
/** A tinted quad, for bars and highlights. */
SH_API uint32_t ShUiImage(uint32_t panel, float x, float y, float w,
                          float h, uint32_t rgb, float alpha);

SH_API int  ShUiSetText(uint32_t id, const char *text);
SH_API int  ShUiSetPos(uint32_t id, float x, float y);
SH_API int  ShUiSetSize(uint32_t id, float w, float h);
SH_API int  ShUiSetColour(uint32_t id, uint32_t rgb);
SH_API int  ShUiSetAlpha(uint32_t id, float alpha);
/** Hiding a panel hides its children too. */
SH_API int  ShUiShow(uint32_t id, int visible);
/** Detaches the widget and its children from the tree. */
SH_API int  ShUiDestroy(uint32_t id);

/** Properties by the engine's own ids, typed from its
 *  property tables at runtime. Any id the class has works.
 */
#define SH_PT_FLOAT   1
#define SH_PT_BOOL    2
#define SH_PT_UINT    3
#define SH_PT_VEC2    4
#define SH_PT_VEC3    5
#define SH_PT_STRING  6

#define SH_P_POSITION  0x01   /**< Widget vec3, local position */
#define SH_P_ROTATION3 0x02   /**< Widget vec3 */
#define SH_P_ROTATION  0x03   /**< Widget float, degrees */
#define SH_P_COLOUR    0x05   /**< Widget vec3, 0..255 */
#define SH_P_ALPHA     0x06   /**< Widget float, 0..1 */
#define SH_P_VISIBLE   0x07   /**< Widget bool */
#define SH_P_SCALE     0x35   /**< Widget vec3 */
#define SH_P_TEXT      0x08   /**< Label string */
#define SH_P_AUTOSIZE  0x09   /**< Label bool */
#define SH_P_STYLE     0x0A   /**< Label string */
#define SH_P_FONTSIZE  0x0C   /**< Label float */
#define SH_P_LINEGAP   0x0E   /**< Label float */
#define SH_P_SIZE      0x0F   /**< Label vec2 */
#define SH_P_IMAGE     0x2A   /**< Image string */
#define SH_P_UV0       0x38   /**< Image vec2 */
#define SH_P_UV1       0x39   /**< Image vec2 */
#define SH_P_CONTSIZE  0x3E   /**< Container vec2 */

/** Widget classes for ShUiCreate. A panel is a container
 *  with a tinted quad behind it; a container is bare. */
#define SH_W_CONTAINER 1
#define SH_W_LABEL     2
#define SH_W_IMAGE     3
#define SH_W_PANEL     4

/** Any widget under any container (0 = the scene root), at
 *  any depth. Text, colour, alpha, UVs and the rest go
 *  through the property calls below. */
SH_API uint32_t ShUiCreate(uint32_t parent, int cls, float x, float y,
                           float w, float h);

/** 0 when the widget's class has no such property. */
SH_API int  ShUiPropType(uint32_t id, uint32_t prop);
SH_API int  ShUiSetF(uint32_t id, uint32_t prop, float v);
SH_API int  ShUiSetU(uint32_t id, uint32_t prop, uint32_t v);
SH_API int  ShUiSetV(uint32_t id, uint32_t prop, const float *v, int n);
SH_API int  ShUiSetS(uint32_t id, uint32_t prop, const char *utf8);
SH_API int  ShUiGetF(uint32_t id, uint32_t prop, float *out);
SH_API int  ShUiGetU(uint32_t id, uint32_t prop, uint32_t *out);
SH_API int  ShUiGetV(uint32_t id, uint32_t prop, float *out, int n);
SH_API int  ShUiGetS(uint32_t id, uint32_t prop, char *out, int n);
/** Label size follows its text on the axes set to 1. */
SH_API int  ShUiSetAutoSize(uint32_t id, int autoW, int autoH);
/** The label's laid out box: its text bounds once an axis
 *  is automatic, otherwise the size that was set. */
SH_API int  ShUiMeasure(uint32_t id, float *w, float *h);

/** RGBA8 pixels to an engine texture, stride in bytes.
 *  Returns an id, 0 on failure; lives for the session. */
SH_API uint32_t ShUiTextureCreate(int w, int h, const uint8_t *rgba,
                                  int stride);
/** Shows a texture on an image widget or panel plate. */
SH_API int  ShUiImageSet(uint32_t id, uint32_t texture);
/** The resolved property records: class, id, type. */
SH_API int  ShUiPropCount(void);
SH_API int  ShUiPropAt(int i, char *cls, int n, uint32_t *prop,
                       int *type);

/** Scenes: layers of your own. Scene 1 is the default.
 *  Order below 0 draws under the game's UI, the rest over
 *  it, lowest first. */
SH_API uint32_t ShUiSceneCreate(const char *name, int order);
SH_API int      ShUiSceneSetOrder(uint32_t scene, int order);
SH_API int      ShUiSceneShow(uint32_t scene, int visible);
/** Every widget of the scene, then the scene. */
SH_API int      ShUiSceneDestroy(uint32_t scene);
/** A widget in a scene; parent 0 is that scene's root. */
SH_API uint32_t ShUiCreateIn(uint32_t scene, uint32_t parent, int cls,
                             float x, float y, float w, float h);
/** After a world reload, once the scene is back: the old
 *  widgets are gone, rebuild inside. */
SH_API int      ShUiSetReset(uint32_t scene,
                             void (*fn)(uint32_t scene, void *user),
                             void *user);
/** Edits between Begin and Commit run as one job. Creates
 *  and reads still run at once. Per thread. */
SH_API int      ShUiBegin(void);
SH_API int      ShUiCommit(void);
SH_API int      ShUiAbort(void);
/** Commit from a worker; done(ok, user) when it landed. */
SH_API int      ShUiCommitAsync(void (*done)(int ok, void *user),
                                void *user);

/** Tree: move a widget under another container of the same
 *  scene at a sibling index (draw order); list children. */
SH_API int      ShUiReparent(uint32_t id, uint32_t parent, int index);
SH_API int      ShUiChildCount(uint32_t id);
SH_API uint32_t ShUiChildAt(uint32_t id, int index);

/** Input: the focused scene gets keys and pointer moves and
 *  captures the keyboard (only Esc, Alt, Tab, F4 and the
 *  Windows keys reach the game). Coordinates 1920 x 1080. */
#define SH_UI_EV_DOWN 1
#define SH_UI_EV_UP   2
#define SH_UI_EV_MOVE 3
typedef struct { int type; int key; int x; int y; } ShUiEvent;
typedef int (*ShUiInputFn)(uint32_t scene, const ShUiEvent *e,
                           void *user);
SH_API int      ShUiSetInput(uint32_t scene, ShUiInputFn fn, void *user);
SH_API int      ShUiFocus(uint32_t scene, int take);
SH_API uint32_t ShUiFocused(void);
/** One virtual key hidden from the game until released. */
SH_API int      ShBlockKey(int vk, int on);
/** Every key but the escapes hidden, focus uses this. */
SH_API int      ShCaptureKeys(int on);

/** The game's UI state, from the scenes it drew last frame.
 *  Names are the scene's own; the common ones below. */
#define SH_SCENE_DRONE      "HUD_Drone"
#define SH_SCENE_BINOCULAR  "HUD_Binocular"
#define SH_SCENE_VEHICLE    "HUD_Vehicle"
#define SH_SCENE_PAUSE      "Menu_TabbedPage"
#define SH_SCENE_LOADOUT    "MENU_LoadoutV2"
#define SH_SCENE_MAP        "MENU_Map_Cursor"
#define SH_SCENE_SKILLS     "MENU_Skills"
#define SH_SCENE_COMWHEEL   "HUD_ComWheel"
#define SH_SCENE_GAMEOVER   "MENU_GameOver"
#define SH_SCENE_LOADING    "MENU_LoadingScreen"
#define SH_SCENE_CINEMATIC  "HUD_Cinematic"
/** 1 when a scene of that name was drawn last frame. */
SH_API int      ShGameSceneActive(const char *name);
/** Comma separated names drawn last frame; the count. */
SH_API int      ShGameScenes(char *buf, int n);

/** @} */
/** @defgroup widgets The engine's widget tree
 *  Every widget the engine has, ours and the game's own,
 *  by its engine handle. @{ */

/** Read only. Writing into a tree you do not own is how
 *  a scene gets corrupted. */

/** The scenes drawn last frame, as handles. Walk one with
 *  ShSceneRoot and the ShWidget calls below. */
SH_API int      ShGameSceneCount(void);
SH_API uint64_t ShGameSceneAt(int i);
SH_API int      ShGameSceneName(uint64_t scene, char *buf, int n);

/** The root widget of any scene, ours or the game's. */
SH_API uint64_t ShSceneRoot(uint64_t scene);

/** Children in the engine's own draw order. */
SH_API int      ShWidgetChildCount(uint64_t widget);
SH_API uint64_t ShWidgetChildAt(uint64_t widget, int i);

/** The class name, "LabelWidget" and the like. */
SH_API int      ShWidgetClass(uint64_t widget, char *out, int n);

/** Any property the class has, by the same ids the SH_P_
 *  defines carry. 0 when the class lacks it. */
SH_API int      ShWidgetPropType(uint64_t widget, uint32_t prop);
SH_API int      ShWidgetGetF(uint64_t widget, uint32_t prop, float *out);
SH_API int      ShWidgetGetU(uint64_t widget, uint32_t prop,
                             uint32_t *out);
SH_API int      ShWidgetGetV(uint64_t widget, uint32_t prop, float *out,
                             int n);
SH_API int      ShWidgetGetS(uint64_t widget, uint32_t prop, char *out,
                             int n);

/** The handle behind one of our own ids, so a widget made
 *  with ShUiCreate reads back the same way. */
SH_API uint64_t ShUiHandle(uint32_t id);

/** @} */
/** @addtogroup core
 *  @{ */

typedef int (*ShGetHealthPlayer_t)(uint32_t *, uint32_t *);
typedef int (*ShSetHealthPlayer_t)(uint32_t);
typedef int (*ShSetGodModePlayer_t)(int);
typedef int (*ShPhysicsReady_t)(void);
typedef int (*ShGroundHeight_t)(float, float, float *);
typedef int (*ShTeleportPlayerToGround_t)(float, float, float);

/** @} */
/** @defgroup config Config and paths
 *  The on-disk layout: the main scripthook.ini, one folder
 *  per plugin, and a single logs directory.
 *
 *  ```
 *  <gamedir>/
 *  ├── GRW.exe
 *  ├── dinput8.dll
 *  ├── scripthook.ini        main config
 *  ├── logs/                 every log file
 *  └── plugins/<name>/
 *      ├── <name>.asi
 *      └── <name>.ini        the plugin's own config
 *  ```
 *
 *  Every path is anchored to the folder holding GRW.exe, so
 *  it stays correct no matter what the working directory is.
 *  @{ */

/** Parse scripthook.ini. The loader runs this before any
 *  plugin loads; calling it again is harmless. */
SH_API void ShConfigInit(void);

/** Integer setting from the main config; def when missing. */
SH_API int  ShConfigGetInt(const char *section, const char *key,
                           int def);

/** Boolean setting: 1/0, true/false, yes/no, on/off. */
SH_API int  ShConfigGetBool(const char *section, const char *key,
                            int def);

/** String setting; copies the value or def. Returns 1. */
SH_API int  ShConfigGetStr(const char *section, const char *key,
                           const char *def, char *out, int size);

/** Write a value back to scripthook.ini. The on-disk file is
 *  updated in place (comments and other sections preserved) and
 *  the in-memory copy is refreshed, so later ShConfigGet*
 *  calls see the new value. Most loader/plugins keys only take
 *  effect on the next launch. */
SH_API int  ShConfigSetStr(const char *section, const char *key,
                           const char *value);
SH_API int  ShConfigSetInt(const char *section, const char *key,
                           int value);
SH_API int  ShConfigSetBool(const char *section, const char *key,
                            int value);

/** The folder containing GRW.exe, no trailing backslash. */
SH_API int  ShGameDir(char *buf, int size);

/** <gamedir>\plugins\ (with trailing backslash); every .asi
 *  plugin lives in its own folder under it. */
SH_API int  ShPluginsDir(char *buf, int size);

/** Compatibility alias for ShPluginsDir: older third-party .asi
 *  plugins resolve this name by GetProcAddress. It returns the same
 *  plugins\ directory. New code should use ShPluginsDir. */
SH_API int  ShScriptsDir(char *buf, int size);

/** <gamedir>\logs\<name>; the logs directory is created if
 *  missing. Name may include a subfolder. */
SH_API int  ShLogPath(const char *name, char *buf, int size);

/** <gamedir>\plugins\<name>\<name>.ini, the config file that
 *  belongs beside a plugin of the same name.
 *
 *  Plugin config convention (follow this in every plugin):
 *  the .ini lives in the SAME folder as the .asi and uses the
 *  SAME base name, so plugins\foo\bar.asi reads and writes
 *  plugins\foo\bar.ini. Derive <name> from your own module
 *  path rather than hardcoding it, so the pairing survives a
 *  rename: GetModuleFileNameA(instance, path, MAX_PATH), take
 *  the file part and strip the ".asi". */
SH_API int  ShPluginIniPath(const char *plugin, char *buf, int size);

/** @} */
/** @defgroup lang Localization
 *  Menu text is translated at capture time. The [Settings]
 *  Language key in scripthook.ini selects the language; for
 *  language "zh_cn" the [zh_cn] and [zh_cn.<scope>] sections
 *  hold translations keyed by the original English text. Lookup
 *  order: [lang.scope] -> [lang] -> [en.scope] -> [en] -> the
 *  original text. @{ */

/** Translate without a scope (framework text, plugin HUD text). */
SH_API const char *ShLang(const char *text);
/** Translate within a menu's scope (its English title). NULL
 *  scope is the same as ShLang. */
SH_API const char *ShLangFor(const char *scope, const char *text);
/** The active language name, from [Settings] Language. */
SH_API const char *ShLangGet(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif
