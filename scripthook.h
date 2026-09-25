/** @file scripthook.h
 *  GRW ScriptHook plugin API from dinput8.dll. Addresses
 *  resolve once at init and are cached. */
#ifndef GRW_SCRIPTHOOK_H
#define GRW_SCRIPTHOOK_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

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

/** The plugin API's own version, apart from SH_VERSION: this one is only about
 *  what a plugin may call, and it goes up by one whenever that changes in a way
 *  a plugin can tell. A plugin declares the lowest version it can run against
 *  (SH_REQUIRES_API) and the loader refuses it on anything older, with a line
 *  in logs\scripthook.log and a toast on screen.
 *
 *  The rule for the number, so it stays usable:
 *
 *    - a new export, or a field added at the end of a struct, is +1 - it is
 *      what a plugin that wants it has to name;
 *    - a changed meaning, a changed unit or argument order, a field inserted
 *      in the middle of a struct, or a removed export, is +1 as well, and
 *      those are the ones that break: say so in the note;
 *    - a fix inside the framework that a plugin cannot tell apart from the
 *      outside, a log line, a comment, documentation: no change to this
 *      number;
 *    - it only ever goes up, by one, and a number is never reused - with one
 *      exception, taken on purpose: while the line that carries an addition is
 *      unreleased, the addition may stay under the number it is already on.
 *      The cost of that is written down here so it is not discovered later: a
 *      caller that names the same number for both additions has to read a NULL
 *      export as "this framework predates my feature", never as an error.
 *
 *  1  1.0-beta3 and before: everything up to and including ShGetAmmoRounds.
 *  2  ShGetAmmoObject: the weapon a rounds reading is about.
 *     ShNpcSpawnSetLayout / ShNpcSpawnGetLayout: how a batch is turned and
 *     which way it looks (see the exception above).
 */
#define SH_API_VERSION 2

/** What a plugin needs of the framework, declared once and outside every
 *  function:
 *
 *      SH_REQUIRES_API(2);                // only what version 2 added
 *      SH_REQUIRES_API(SH_API_VERSION);   // whatever this header is
 *
 *  Name the last thing you use, not the header you happened to build against:
 *  a plugin that only calls older entry points should stay loadable on older
 *  frameworks, and the loader refuses on "needs more than this one offers" and
 *  nothing else.
 *
 *  The loader reads this BEFORE the plugin's own code runs at all - it maps
 *  the image and reads the export without initialising it - so a plugin that
 *  needs more than the framework in front of it offers is refused instead of
 *  starting and failing somewhere inside itself. A plugin that does not write
 *  this declares nothing: silence means "no requirement", never "whatever is
 *  newest", so every plugin that predates this macro loads exactly as it did,
 *  and one that is switched off in scripthook.ini is not even read.
 *
 *  The export is a number, so anything else that loads .asi files can read it
 *  out of the file too. */
#define SH_REQUIRES_API(v) \
    __declspec(dllexport) uint32_t ShRequiresApi = (uint32_t)(v)

/** Whether this framework offers what version `v` added, for a plugin that was
 *  not loaded by this loader and would rather check for itself. */
#define SH_API_IS(v) (ShGetVersion() >= (int)(v))

/** The build's own version, in one place. The loader's start up line, the
 *  crash report header and the About page all carry this same string, so a
 *  bug report names the build it came from without anyone having to ask.
 *  Bump it here and nowhere else.
 *
 *  1.0-beta2 (2026-09-17 night) is the first beta with a load bearing fix:
 *  the overlay no longer creates a second D3D11 device and no longer waits
 *  for "any big window" - it captures the game's own swapchain - and the
 *  proxy carries the real dinput8's full export set. The 1.0-beta1 asset was
 *  replaced rather than left up, so the version string is what tells the two
 *  apart in a log.
 *
 *  1.0-beta3 (2026-09-19 night) adds the ninth shipped plugin, micfix: a game
 *  that cannot read a Chinese recording device name is handed an ASCII one,
 *  for this process only, plus the device picker the game has no option for.
 *  Two rules proven in the field are inside it - a device is renamed on both
 *  of the doors the game asks or on neither, and a name it can already read is
 *  never touched - and both were paid for in game launches.
 *
 *  It also picks the menu language on a first run. With no [Settings]
 *  Language= line - which is what a package now ships, the line is dropped
 *  when one is packed - the language comes from the Windows user language,
 *  matched against what the menu can actually show: the exact code, else the
 *  same language in another variant (zh-TW for zh-CN, en-GB for en-US), else
 *  English. The pick is written back to scripthook.ini, which is what makes
 *  it a first run only and what a player deletes to have it picked again.
 *
 *  1.0-beta4 (2026-09-22 night): the magazine stops being guessed at.
 *  AmmoControl reads the number the HUD is showing - found by its shape (the
 *  count that stops at the separator, beside the reserve), remembered by its
 *  place in the widget tree, one property read per poll and nothing read or
 *  searched outside play - with the capacity call kept as the fallback for when
 *  that number cannot be read. The framework grew ShGetAmmoObject, which is
 *  what lets a reading follow a weapon switch by the object it is about rather
 *  than by who moved last. The plugin API grew a version of its own
 *  (SH_API_VERSION 2, see SH_REQUIRES_API): a plugin that needs more of the API
 *  than the framework in front of it offers is refused by name in the log and
 *  on screen, instead of starting and failing somewhere inside itself. And
 *  firstperson gained the row that switches its view change toast on and off. */
#define SH_VERSION "1.0-beta4"

/** Where this build's source lives, in one place for the same reason the
 *  version is: the About page formats it in rather than typing it, so a
 *  move costs one line here. */
#define SH_REPO "https://github.com/GameXueRen/grw-scripthook"

#ifdef SH_BUILD
#define SH_API __declspec(dllexport)
#else
#define SH_API
#endif

/** Metres. x east, y north, z up. */
typedef struct { float x, y, z; } ShVec3;

/** @} */

/** @defgroup playmode Play mode selection
 *  Which PvP mode this session is in, read from the game's own mode
 *  manager.
 *
 *  The mode is the mode object, and the object is read from
 *  CreateGameMode - the description it is handed to build the mode from
 *  has a pointer into the game's own mode table as its first field, one
 *  entry per mode, stable across sessions (Mercenaries was 38DD178 in
 *  all three, the campaign 38DC7F0 every time). That call is hooked
 *  (`scripthook_playmode.c`), its identity is checked against the build
 *  before the hook is armed, its argument is read off the watcher thread
 *  - never inside the hook - and an object that is not in the
 *  framework's table is logged and left undecided rather than guessed at
 *  (`ShPlayModeFingerprint` hands the number out for exactly that case;
 *  that is how Guerrilla was found).
 *
 *  GameModeManager::SetCurrentGameMode is hooked too, and its argument is
 *  logged with every mode, but it is not what names the mode: measured,
 *  it is 3 for Ghost War and 2 for the three modes that share it, but it
 *  is also 0 for the campaign, Narco Road *and* 2 for Fallen Ghosts with
 *  the same campaign object. The same mode content can come with either
 *  number, so the argument is recorded and the object decides.
 *
 *  It has to be that call, because nothing else in the engine says it.
 *  Measured, in the main menu and in the Ghost War lobby: the GameFlow
 *  state name, the GameFlow sub object class hashes, the drawn scene set
 *  (empty in the front end), the UI state bits, the input context, the
 *  entity counts and the archives read are all identical - PvE is four
 *  player co-op, so other humans prove nothing, and every mode reads the
 *  same 23 archives because the engine indexes all of them at startup.
 *  The mode name in memory is already there in the main menu because it
 *  is the button's own label. The objects are reflected (32 byte entries
 *  of crc32(name), index and function, and that hash is plain CRC-32),
 *  but 312 front end methods resolved against a 24000 name dictionary
 *  came back with nothing but the scene interface, and the mode
 *  manager's own method names match no method table entry at all. The
 *  named lifecycle methods were hooked as a last resort: they fire, with
 *  a real dispatcher chain in the stack, but with the same signature for
 *  all three modes - page navigation, not a mode choice - and no mode id
 *  or name appears in their arguments either.
 *
 *  What this costs and what it does not promise:
 *
 *   - `NONE` until the game has set a mode, so asking early is safe and
 *     says "not yet" rather than guessing. `ShPlayModeHookArmed` says
 *     whether it ever will on this build.
 *   - The mode is measured, not documented: an object that is not in the
 *     framework's table is left `NONE` with a log line carrying it (and
 *     the argument the game used), rather than guessed at.
 *   - A different build whose layout differs will not arm the hook, and
 *     the answer stays `NONE`.
 *   - The mode is known from the moment the game sets it, in the lobby
 *     and in a match alike; nothing here needs a mouse, a resolution or
 *     a UI scale to be right.
 *  @{ */

enum ShPlayMode {
    SH_PLAYMODE_NONE = 0,      /**< the game has not set a mode yet */
    SH_PLAYMODE_GHOST_WAR = 1,
    SH_PLAYMODE_MERCENARIES = 2,
    /** The campaign - the story mode (mode object 38DC7F0). Campaign
     *  *content* is not a mode of its own: the base story, Narco Road,
     *  Fallen Ghosts and The Last Rites all report this same object,
     *  because they are the campaign game mode with different content
     *  loaded. Telling those apart needs something other than this call -
     *  the region, the mission, the save. */
    SH_PLAYMODE_CAMPAIGN = 3,
    /** Ghost Mode - 幽灵/魅影模式, the permadeath campaign (a death ends
     *  the run and the save goes with it), which is a different mode
     *  from Mercenaries: the eight player PvPvE one where a death only
     *  costs the gear the run had collected. */
    SH_PLAYMODE_GHOST_MODE = 4,
    /** Guerrilla Mode - 游击战模式: defending a camp against waves of
     *  attackers, alone or in co-op. */
    SH_PLAYMODE_GUERRILLA = 5
};

/** The mode this session is in, one of `ShPlayMode`.
 *
 *  `NONE` means not decided yet, which is also the honest answer while
 *  the mode object is still being read - the wait is a watcher tick, 200
 *  ms at most - or when the object is one the framework does not know. */
SH_API int ShSelectedPlayMode(void);

/** ShSelectedPlayMode() == the mode named. */
SH_API int ShIsGhostWarMode(void);
SH_API int ShIsMercenariesMode(void);
SH_API int ShIsGhostMode(void);
SH_API int ShIsGuerrillaMode(void);

/** The mode object's identity, as an RVA into the game's own mode table,
 *  or 0 while it is not known. This is the second half of the answer for
 *  the modes that share an argument: the table in
 *  `scripthook_playmode.c` maps each one to a mode, and an object that is
 *  not in it is logged by the framework - send that line in and it gets
 *  a row. */
SH_API int ShPlayModeFingerprint(void);

/** What the answer rests on, for a log line or a menu: "GameModeType 2
 *  with mode object 38DD178 (MERCENARIES)", or why there is none yet. 1
 *  when the mode is known. */
SH_API int ShPlayModeEvidence(char *buf, int len);

/** 1 when the game's mode manager is being read, so a caller can tell
 *  "not yet" from "not on this build" - and say so, rather than
 *  treating `NONE` as "no mode in particular". */
SH_API int ShPlayModeHookArmed(void);

/** The bit a mode occupies in a blacklist mask (see the plugin blacklist
 *  group below), or 0 for a value that is not a mode. */
SH_API uint32_t ShPlayModeBit(int mode);

/** A mode's name, spelled the way the framework logs it elsewhere:
 *  "Ghost War", "MERCENARIES", "campaign", "Ghost Mode", "Guerrilla", and
 *  "" for a value that is not a mode. It is a translation key: pass it
 *  through ShLang before showing it. */
SH_API const char *ShPlayModeName(int mode);

/** @} */
/** @defgroup blacklist Plugin mode blacklist
 *  Which play modes a plugin must not run in, and what the framework does
 *  about it.
 *
 *  A plugin declares its modes once, from its own source:
 *
 *  ```c
 *  ShPluginBlacklist(SH_MODE_BLACKLIST_GHOST_WAR |
 *                    SH_MODE_BLACKLIST_MERCENARIES);
 *  ```
 *
 *  and the framework takes its row out of the F4 root menu, everything
 *  under it with it, for as long as the session is in one of those modes.
 *  The declaration is the only input: nothing in any ini can widen or
 *  narrow it.
 *
 *  A mask names play modes, and play modes are the only conditions. The
 *  front end was one of them until 2026-09-13 and is not any more: the
 *  reading it rested on (the GameFlow machine's mode-screen object) latches
 *  once a mode screen has been opened, so it was right at startup and wrong
 *  after a round trip through a campaign - and `ShGetGameState` puts the
 *  main menu and every lobby in one bucket, so there was no second reading
 *  to fall back on. Better no condition than one that is right half the
 *  time.
 *
 *  Three cases, and the third is the one that matters:
 *
 *   - declared bits       blocked in exactly those;
 *   - declared NONE (0)   blocked nowhere - declaring "none" is the only
 *                         way to be unrestricted;
 *   - not declared at all blocked in Ghost War and Mercenaries. Every
 *                         third party plugin is in this group by
 *                         construction, and so is any plugin of ours that
 *                         has not been taught otherwise. Built-in pages are
 *                         not plugins (their menu owner is "") and are never
 *                         blocked.
 *
 *  What is in force comes from one place: `ShSelectedPlayMode`, the mode
 *  the game's own manager was handed. When it cannot be read - the manager
 *  has not set a mode yet, or the hook is not armed on this build - nothing
 *  is in force and nobody is blocked: a menu that vanishes with no reason to
 *  give would be worse than one that stays.
 *
 *  What this cannot do: stop a plugin's code. Plugins run their own
 *  threads and carry their own MinHook copy, and the framework has no
 *  registry that could suspend them. Blocked means hidden and told; a
 *  plugin that wants its work to stop has to stop it. The usual shape is a
 *  tick thread that returns early plus a callback that tears down what it
 *  installed:
 *
 *  ```c
 *  static void OnBlocked(int allowed, int mode, void *user) {
 *      (void)mode; (void)user;
 *      if (!allowed) StopMyWork();      // drop hooks, park the thread
 *      else          StartMyWork();
 *  }
 *  ShPluginOnBlocked(OnBlocked, NULL);
 *  ```
 *
 *  The callback arrives on a framework thread (250 ms poll), once per
 *  change, and only when the answer actually flips. Keep it short; it is
 *  not holding any lock of ours, so calling back into the framework from
 *  it is allowed. See docs/plugin-blacklist.md.
 *  @{ */

/** Blocked in Ghost War, the 4v4 PvP mode. */
#define SH_MODE_BLACKLIST_GHOST_WAR   (1u << (SH_PLAYMODE_GHOST_WAR - 1))
/** Blocked in Mercenaries, the eight player PvPvE mode. */
#define SH_MODE_BLACKLIST_MERCENARIES (1u << (SH_PLAYMODE_MERCENARIES - 1))
/** Blocked in the campaign: the story mode, and the campaign content that
 *  runs in it (Narco Road, Fallen Ghosts, The Last Rites). */
#define SH_MODE_BLACKLIST_CAMPAIGN    (1u << (SH_PLAYMODE_CAMPAIGN - 1))
/** Blocked in Ghost Mode (幽灵/魅影模式), the permadeath campaign. */
#define SH_MODE_BLACKLIST_GHOST_MODE  (1u << (SH_PLAYMODE_GHOST_MODE - 1))
/** Blocked in Guerrilla (游击战), the camp defence mode. */
#define SH_MODE_BLACKLIST_GUERRILLA   (1u << (SH_PLAYMODE_GUERRILLA - 1))
/** Everything: every mode the game can be in. */
#define SH_MODE_BLACKLIST_ANY         (SH_MODE_BLACKLIST_GHOST_WAR | \
                                       SH_MODE_BLACKLIST_MERCENARIES | \
                                       SH_MODE_BLACKLIST_CAMPAIGN | \
                                       SH_MODE_BLACKLIST_GHOST_MODE | \
                                       SH_MODE_BLACKLIST_GUERRILLA)
/** Declared on purpose: no mode is blacklisted. A plugin that never calls
 *  ShPluginBlacklist is not this - it gets the default. */
#define SH_MODE_BLACKLIST_NONE        0u

/** Declare which conditions this plugin must not run in - a mask of the
 *  bits above. Call it once, from your own code: the caller's module is
 *  what identifies the plugin. 1 when accepted; 0 with ShLastError saying
 *  why - `SH_ERR_BAD_ARG` when the caller is not a plugin,
 *  `SH_ERR_REGISTRY_FULL` when 64 plugins are already registered. */
SH_API int      ShPluginBlacklist(uint32_t modes);

/** What is switching this plugin off right now, as the bits above
 *  (`SH_MODE_BLACKLIST_*`), or 0 when nothing is. */
SH_API int      ShPluginBlockedBy(void);

/** 1 when this plugin may run now. Cheap; safe to call every frame. */
SH_API int      ShPluginAllowed(void);

/** The conditions in force for this plugin: what it declared, or the
 *  default (Ghost War and Mercenaries) when it never declared anything.
 *  0 for a declarer of "none". */
SH_API uint32_t ShPluginBlacklistModes(void);

/** Be told when the answer changes: `allowed` is 1 when the plugin may run
 *  again, `blocked` the bits that decided it (0 when allowed). One callback
 *  per plugin, last call wins, and only called when the answer flips. */
typedef void (*ShPluginBlockedFn)(int allowed, int blocked, void *user);
SH_API int      ShPluginOnBlocked(ShPluginBlockedFn fn, void *user);

/** The name of one bit, for a log line or a menu: "Ghost War",
 *  "MERCENARIES", "campaign", "Ghost Mode", "Guerrilla"; "" for 0 or for
 *  more than one bit (use ShBlockedText for that). A translation key: pass
 *  it through ShLang before showing it. */
SH_API const char *ShBlacklistName(uint32_t bit);

/** The bits as one line, for a log: "Ghost War+MERCENARIES". 1 when
 *  something was written. */
SH_API int      ShBlockedText(uint32_t bits, char *buf, int cap);

/** The registry, for a settings page: how many plugins were seen, and one
 *  entry per index - its folder name, the conditions in force, and the bits
 *  blocking it now (0 for none). */
SH_API int      ShPluginBlacklistCount(void);
SH_API int      ShPluginBlacklistAt(int i, char *name, int cap,
                                    uint32_t *modes, int *blockedBy);

/** One translated line naming the plugins the current conditions have
 *  switched off: "Off now (Ghost War): firstperson, freecam
 *  (+3)". Short on purpose - the Plugins page shows it on the hint line it
 *  shares with the "needs a restart" note - and empty with a 0 return when
 *  there is nothing to say. */
SH_API int      ShPluginBlacklistNotice(char *buf, int cap);

/** Internal, not a plugin API: the folder name of the .asi a call came
 *  from; "" for the framework itself, the exe, or an unmapped address. */
void ShPluginOwnerFromAddress(void *address, char *out, int cap);

/** Internal, for the menu layer: 1 when that plugin's row must not be
 *  drawn, selected or entered right now. "" is never hidden. */
int  ShPluginHidden(const char *owner);

/** Internal: start the registry. The loader runs it before the plugins
 *  load, so a declaration has somewhere to go. */
void ShBlacklistStartup(void);

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
    SH_ERR_UI_ASSET,       /**< font or texture not loaded */
    SH_ERR_REGISTRY_FULL   /**< a fixed table is full (blacklist: 64) */
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
/** The live entity vtable. A game update moves it, and a stale value
 *  makes every entity test silently answer no - so the framework learns
 *  it from the player entity and this is where the rest of the code asks.
 *  Before it is known the pinned constant is answered.
 */
SH_API uint64_t ShEntityVtable(void);
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
/** True while the local player is actively swimming. Read from the
 *  locomotion component's state byte, held across its brief
 *  transitions; see ShGetOccupiedVehicle for the vehicle side. */
SH_API int  ShIsSwimming(void);

/* ---- player accuracy (ported from the Wildlands Immersion Suite) ---- */
/** Pin the player's spread to zero while enabled. Installs three
 *  verified mid-function hooks; 1 once they are in. The Active call
 *  pauses the effect without uninstalling, so a mode blacklist can
 *  hand the spread back frame by frame. */
SH_API int      ShSetSuperAccuracy(int enabled);
SH_API int      ShGetSuperAccuracy(void);
SH_API void     ShSetSuperAccuracyActive(int active);

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

enum {
    SH_VEHICLE_NONE = 0,
    SH_VEHICLE_GROUND = 1,
    SH_VEHICLE_AIR = 2,
    SH_VEHICLE_WATER = 3,
    SH_VEHICLE_UNKNOWN = 4
};
typedef struct {
    uint64_t entity;
    uint32_t id;
    int vehicleClass;
    int identified;
    char name[64];
} ShOccupiedVehicle;
/** Resolve the vehicle currently containing the local player. The class
 *  comes from the stable vehicle catalogue rather than a camera-distance
 *  fallback; identified says the id/name came from that catalogue. */
SH_API int ShGetOccupiedVehicle(ShOccupiedVehicle *out);

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
/** How that warm-up is going, for a status line: 1 while the
 *  walk runs, with *done of *total specs resolved, and 0 when
 *  nothing is running (the numbers are not written then). The
 *  walk takes about fifteen seconds and a dispatch that lands
 *  in the middle of it waits, so a caller that is about to
 *  wait can say what for. Either pointer may be NULL. */
SH_API int  ShSpawnWarmProgress(int *done, int *total);
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

/* ---- factions ----------------------------------------------------
 *
 * The engine keeps an archetype's faction private, and
 * ShNpcArchetype carries only {id, kind} with no name, so kind is
 * what the grouping is read from - measured on this build rather than
 * tabulated. The per-archetype id tables older versions carried came
 * from a third-party plugin and were removed at its author's request on
 * 2026-09-20, so an archetype whose kind does not name its faction now
 * reads as the group its kind names.
 */

#define SH_NPC_GROUP_SANTA_BLANCA 0
#define SH_NPC_GROUP_UNIDAD       1
#define SH_NPC_GROUP_REBELS       2
#define SH_NPC_GROUP_CIVILIANS    3
#define SH_NPC_GROUP_SPECIAL      4
#define SH_NPC_GROUP_MAX          5

/** SH_NPC_GROUP_MAX. */
SH_API int ShNpcGroupCount(void);
/** "Santa Blanca", "Unidad", "Rebels", "Civilians", "Special",
 *  or "" for a group out of range. The faction names are the game's, and
 *  they are translation keys: pass them through ShLang. */
SH_API const char *ShNpcGroupName(int group);

/** The group an archetype belongs to, or -1 for a kind no group claims
 *  (kinds 0..7 are the ones that do; see the note above). */
SH_API int ShNpcGroupOfArchetype(const ShNpcArchetype *a);

/** The same, looked up by id: walks the catalogue, so the first
 *  call waits for the registry like ShNpcCount does. -1 when no
 *  archetype carries that id. */
SH_API int ShNpcGroupOf(uint64_t archetypeId);

/** How many archetypes the group holds. 0 for a group out of
 *  range or a catalogue that is not readable yet. */
SH_API int ShNpcGroupSize(int group);

/** The index-th archetype of a group, 0 based, as the group is
 *  walked. Fills out when it is not NULL. Returns that
 *  archetype's index in the whole catalogue, or -1 past the end. */
SH_API int ShNpcAtInGroup(int group, int index, ShNpcArchetype *out);

/* ---- formations and batches ------------------------------------- */

/** The five layouts ShNpcPlanFormation lays out. */
enum ShNpcFormation {
    SH_NPC_FORMATION_LINE = 0,   /**< abreast, 2.5 m apart      */
    SH_NPC_FORMATION_SPREAD,     /**< a 3 column grid, 3 m      */
    SH_NPC_FORMATION_SEMICIRCLE, /**< an arc, radius 4.5 m      */
    SH_NPC_FORMATION_CIRCLE,     /**< a ring, radius 3.5 m      */
    SH_NPC_FORMATION_RANDOM      /**< jittered, radius 2-6.5 m  */
};

/** Where a spawned batch looks. */
enum ShNpcFacing {
    /** Each one is turned to look at the player. */
    SH_NPC_FACING_PLAYER = 0,
    /** The orientation spawn already gave it, which is the
     *  player's own. Nothing is written. */
    SH_NPC_FACING_FORWARD
};

/** Most NPCs one batch may ask for. */
#define SH_NPC_SPAWN_MAX  50
/** Batches that may be in flight at once. */
#define SH_NPC_SPAWN_JOBS 4

/** Lay a formation out, and only that: no engine call, no lane
 *  to the game thread, and z is origin's own. The planning stays
 *  pure on purpose - a caller that wants the points as they are
 *  can have them - but ShNpcSpawnFormation does not spawn blind:
 *  each point is put on the ground with ShGroundHeightFrom first,
 *  so a batch stands on the terrain instead of at the player's own
 *  height. Writes up to max points, and at most SH_NPC_SPAWN_MAX.
 *  Returns how many were written, 0 on a bad argument. yaw is
 *  radians, world axes, x east y north z up. */
SH_API int ShNpcPlanFormation(int formation, int count, float distance,
                              const ShVec3 *origin, float yaw,
                              ShVec3 *out, int max);

/** One batch. id is an archetype id; count and facing are as the
 *  two enums above. */
typedef struct {
    uint64_t id;        /**< archetype id                        */
    int      count;     /**< 1 .. SH_NPC_SPAWN_MAX                */
    float    distance;  /**< metres ahead of the player           */
    int      formation; /**< an ShNpcFormation                    */
    int      facing;    /**< an ShNpcFacing                       */
} ShNpcSpawnRequest;

/** Spawn a whole batch and wait for it. Fills out with the
 *  entities that appeared, up to maxOut and at most
 *  SH_NPC_SPAWN_MAX, and returns how many. The count is clamped
 *  to those limits rather than refused.
 *
 *  BLOCKING, and each NPC is waited on separately, so a batch of
 *  50 can take a long time and the first spawn of a new
 *  archetype streams its assets in. NEVER call this from the
 *  game thread - a frame callback or a menu engine call - or
 *  from any thread the engine is waiting on: ShSpawnNpc waits
 *  for the physics pump, which runs on the game thread, and that
 *  would deadlock. Use ShNpcSpawnBegin from anywhere. */
SH_API int ShNpcSpawnFormation(const ShNpcSpawnRequest *req,
                               uint64_t *out, int maxOut);

/** The same batch on a worker thread of the API's own, with a
 *  handle to poll. Returns a job id, 0 when no job is free or
 *  the thread could not be started. */
SH_API uint32_t ShNpcSpawnBegin(const ShNpcSpawnRequest *req);

/** The entities a job has produced so far, copied into out, up
 *  to maxOut. Returns how many there are, or -1 for a job id
 *  that is not live. done is set to 1 once the worker has
 *  stopped, whether it finished, failed or was cancelled.
 *  Safe to call while the job is still running. */
SH_API int ShNpcSpawnPoll(uint32_t job, uint64_t *out, int maxOut,
                          int *done);

/** Ask a job to stop. The worker checks between NPCs, so a
 *  cancel lands within one spawn (a few seconds at worst); what
 *  it already produced stays readable through ShNpcSpawnPoll.
 *  Does not free the job - ShNpcSpawnEnd does. Safe, and a
 *  no-op, on a job that has already finished. */
SH_API int ShNpcSpawnCancel(uint32_t job);

/** Free a job. Refuses while it is still running, and asks it to
 *  stop first, so the caller polls until done and calls again.
 *  The job id is dead afterwards. */
SH_API int ShNpcSpawnEnd(uint32_t job);

/* ---- the layout a caller may ask for ----------------------------
 *
 * A batch is placed ahead of the player and left facing the way the
 * request says. That is all a plugin needs to spawn a wave, but it is
 * not enough to *look at* one: with the player standing still every
 * batch lands on the same spot, and a spawn that faces the player is
 * one whose reaction cannot be told apart from its patience.
 *
 * So a caller may install a policy instead. It is deliberately not a
 * field of ShNpcSpawnRequest: that struct is shared with plugins and
 * with jobs already running, and a caller that never installs a
 * policy has to see exactly what it saw before.
 */

/** Which way the spawns of a batch are made to look. */
enum ShNpcFacingMode {
    /** Facing the player, which is what SH_NPC_FACING_PLAYER means. */
    SH_NPC_FACING_MODE_PLAYER = 0,
    /** Left as spawned, so it carries the player's own heading. */
    SH_NPC_FACING_MODE_FORWARD,
    /** A different heading each, drawn where the spawn is placed. */
    SH_NPC_FACING_MODE_RANDOM,
    /** Every one turned to facing_angle_deg, in degrees, world axes. */
    SH_NPC_FACING_MODE_FIXED,
    /** Spun round: the i-th looks along facing_angle_deg * i, so a
     *  batch covers the circle in as many steps as it has members. */
    SH_NPC_FACING_MODE_SPIN
};

/** The policy ShNpcSpawnSetLayout installs. */
typedef struct {
    /** Every batch is placed this much further round the player than
     *  the last, in degrees. 0 leaves them all straight ahead, which
     *  is what the planner has always done; 40 puts nine batches
     *  round the circle. */
    float spread_step_deg;
    /** An ShNpcFacingMode. */
    int   facing_mode;
    /** The fixed heading, or the step of the spin, in degrees. */
    float facing_angle_deg;
} ShNpcSpawnLayout;

/** Install the policy every later batch obeys, until it is replaced.
 *  NULL clears it, returning the batch to the request's own fields
 *  (and to the straight-ahead placement). Refuses a facing_mode that
 *  is not one of the enum's, and returns 0 there. */
SH_API int ShNpcSpawnSetLayout(const ShNpcSpawnLayout *layout);

/** Read back what is installed, all zero when nothing is. */
SH_API int ShNpcSpawnGetLayout(ShNpcSpawnLayout *out);

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

/** Bullet physics: scale the muzzle velocity the engine stores into
 *  each round it fires, tracer and authoritative round alike. 1.0 is
 *  the game's own number; clamped to 0.10..10.0 and applied to shots
 *  fired after the call. Takes effect once ShBallisticsHookInstall
 *  has installed the trajectory patch. */
SH_API int  ShSetProjectileVelocityMultiplier(float multiplier);
/** The gravity half of the same idea, 0.0..10.0 (1.0 = vanilla). */
SH_API int  ShSetProjectileDropMultiplier(float multiplier);
SH_API float ShGetProjectileVelocityMultiplier(void);
SH_API float ShGetProjectileDropMultiplier(void);
/** How many trajectory steps the alternate sites have scaled. The
 *  shipped feature patches the trail, so this stays 0. */
SH_API uint32_t ShGetProjectileVelocityHookCount(void);
/** How many tracers the trail patch has scaled this session - the
 *  live proof the patch is running. */
SH_API uint32_t ShGetProjectileTrailHookCount(void);
/** Install the trajectory patch on its own; 1 once it is in (or
 *  already was). Independent of the hit hook above. */
SH_API int  ShBallisticsHookInstall(void);

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

/** Drop the cached head group pointers. A new session, a
 *  respawn, or the first aim of one can make the cached
 *  controller stale; the next ShGetHeadNodes rescans.
 */
SH_API void ShHeadInvalidate(void);
/** Clear only the "not found yet" note, keeping a controller
 *  that was already found for the player body.
 */
SH_API void ShHeadClearMiss(void);

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
/** opts must outlive the menu. String literals are fine, and so are
 *  numbers built once into a static buffer - an hour row is just "0" to
 *  "23" and needs no translation. The row WRAPS: one step past the last
 *  option is the first, which is what makes a clock row usable with the
 *  left/right keys at all. Up to 64 options; anything past that is
 *  dropped. */
SH_API int  ShMenuList(uint32_t menu, const char *label,
                       const char **opts, int n, int initial,
                       ShMenuFn fn, void *user);
/** Sync a toggle/list row's displayed value without firing its
 *  callback. A plugin that changed the state behind the menu's
 *  back (a hotkey flip) calls this so the next capture shows
 *  the truth. Matches on the label inside that one menu. */
SH_API int  ShMenuSetValue(uint32_t menu, const char *label,
                           int value);
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
/** Is that menu the page on screen right now?  1 when the menu is up and
 *  `menu` is exactly the page being shown; 0 when another page is
 *  showing, when the menu is closed (nothing is on screen then, whatever
 *  page was left open last), or when the id names no menu.
 *
 *  Exactly that page, not "somewhere on the way to it": the framework
 *  draws the status line of the current page and of no other, so while
 *  the player is inside a submenu the parent's page is not showing even
 *  though the player came through it.  Asking about the parent there
 *  answers 0; asking about the submenu answers 1.  A plugin whose status
 *  line lives in a submenu passes that submenu's id, and a plugin that
 *  wants to know whether any of several pages of its own is up asks
 *  about each of them.
 *
 *  This is the guard for a status line.  ShMenuStatus/ShMenuStatusF write
 *  into the menu model, and a plugin that keeps a line current while
 *  nobody can read it is spending work on nothing; gate the refresh on
 *  this and the line is only written while it can be seen.  The framework
 *  shows whatever was written last the moment the page comes up, so a
 *  plugin that wants the first frame to be right keeps a dirty flag of
 *  its own (any page with a live readout does exactly that).
 *
 *  0 for a menu that exists and is simply not showing is an answer, not a
 *  failure: ShLastError is set only when the id names no menu. */
SH_API int  ShMenuIsShowing(uint32_t menu);
SH_API void ShMenuOpen(int open);

/** One row of the menu, copied for the overlay renderer. The sizes here
 *  are the contract with the menu model: a row label is a full row, and
 *  the longest one the framework builds - a plugin switch, reading
 *  "<folder>(<page name>)" - has to fit. See LABEL in scripthook_menu.c. */
typedef struct ShMenuRow {
    char name[160];
    char value[48];
    int  selected;
} ShMenuRow;

/** One frame of the current menu, captured under the lock for
 *  the D3D11 overlay (scripthook_ovl.cpp) to draw. Sizes here are
 *  what the renderer gets: a longer title, hint or status line is
 *  cut on a character boundary by the capture, never mid-character. */
typedef struct ShMenuView {
    char title[160];  /**< sized with LABEL: a page title goes in here */
    char hint[384];   /**< control hints under the title, \n lines */
    char status[192];
    char footer[32];
    int  rows;
    int  sel;
    int  isRoot;      /**< 1 when the root menu is being shown */
    ShMenuRow row[12];
} ShMenuView;

/** Internal: snapshot the current menu for the overlay. */
void ShMenuCaptureView(ShMenuView *v);
/** Internal: drop every stored status line. The text layer calls this
 *  when the language changes, because a stored line is text in the
 *  language that was active when it was written. */
void ShMenuStatusResetAll(void);
/** Internal: tell the menu the overlay can render now. */
void ShMenuSetOverlayReady(int ready);

/** Internal: one root row the player can reorder, as the settings page
 *  sees it. `key` is the page key [MenuOrder] is keyed by; `owner` is
 *  the plugin folder the page belongs to. */
typedef struct ShMenuOrderRow {
    char key[96];        /* a page key can be a long literal title */
    char owner[48];
} ShMenuOrderRow;

/** Internal: copy the reorderable root rows in the order they are drawn.
 *  That is every page the root holds - a built-in feature page such as
 *  the Forge one, and the settings page itself, are ordered like any
 *  other - but only rows that are in the root right now, so a page the
 *  play mode has taken away is never listed and never renumbered. Writes
 *  at most `cap` rows and returns how many there are in total. */
int  ShMenuRootOrderRows(ShMenuOrderRow *out, int cap);
/** Internal: put the cursor on the row named `key` and scroll it into
 *  view: a page that rebuilds its own rows needs this afterwards. */
int  ShMenuSelectRow(uint32_t menu, const char *key);
/** Internal: the [MenuOrder] weights changed behind the menu's back, so
 *  the root re-sorts on the next capture instead of trusting a row count
 *  that has not changed. */
void ShMenuOrderDirty(void);

/** Internal: end a string one character earlier when its last bytes are
 *  only part of a character. Text cut mid-sequence is what a renderer
 *  draws as "?", so every copy that can cut display text calls this. */
void ShUtf8Trim(char *s);

/** Internal: format a translated template, letting the translation
 *  reorder the values with "%n$". `en` is the same template in en-US:
 *  the translation's conversions are checked against its types, and a
 *  translation that fails the check is logged and formatted from `en`
 *  instead (docs/i18n-refactor.md 3.3). */
int ShTextFormat(char *dst, size_t cap, const char *en, const char *tr,
                 ...);

/** Internal: the same, for a caller that already holds a va_list. */
int ShTextFormatV(char *dst, size_t cap, const char *en, const char *tr,
                  va_list ap);

/** Internal: the en-US text for a key, or NULL when this build has
 *  none. What a template is checked against, and what a rejected
 *  translation falls back to. */
const char *ShTextEnUS(const char *owner, const char *key);

/** @} */
/** @defgroup draw Plugin drawing
 *  A plugin's own window inside the game's overlay, drawn by the
 *  plugin's own code.
 *
 *  A plugin registers one callback by name (ShDrawAdd) and is called
 *  once per frame, on the render thread, inside an ImGui window titled
 *  with that name.  Inside the callback it draws with the primitives
 *  below - text, panels, buttons, toggles, numbers, sliders, a text
 *  box - using the same fonts, colours and resolution-scaled metrics as
 *  the framework's own menu, so a plugin's UI looks like it belongs.
 *  Nothing registered means nothing drawn and nothing paid: the layer is
 *  a single loop over an empty table.
 *
 *  The primitives are implemented by the overlay (ImGui lives there),
 *  but they are reached through a plain C ABI and a table of function
 *  pointers - ImGui is NOT part of the plugin API, so a plugin is
 *  ordinary C, links the import library (or GetProcAddress) and needs no
 *  C++ at all.  When the framework is built without the overlay - the
 *  MinGW chain has no D3D11 or ImGui - every primitive is a safe no-op
 *  and ShDrawReady() returns 0, so the same plugin source works in both
 *  builds.
 *
 *  The input box deserves its own paragraph, because the split of work
 *  is deliberate.  The framework owns the CHARACTERS and the IME
 *  session: the game's window is the only place a system IME can deliver
 *  composed text (it arrives as WM_CHAR / WM_IME_CHAR while the box is
 *  open), so the framework collects those, gives them to the owner
 *  through ShDrawInputTake(), and owns the composition string, the
 *  candidate list, the candidate-window anchoring and the "text is being
 *  composed, keep your hands off Enter / Esc / Backspace" test
 *  (ShDrawInputComposing).  The OWNER owns the buffer: it appends what
 *  ShDrawInputTake() hands it, deletes, pastes and decides what Enter
 *  means.  The framework never edits text and never interprets a command
 *  key, so there is exactly one writer per byte, and hotkeys and pulsing
 *  keys stay where the game's input model needs them - polled by the
 *  owner, not read out of the window's message queue.
 *
 *  The contract, in full:
 *   - One callback per frame, on the render thread, only while the
 *     overlay is up.  It runs on the game's Present path, so it must not
 *     block: no sleeps, no waiting on another thread, no I/O.  Drawing
 *     only.
 *   - Registration and ShDrawInput* may be called from any thread; the
 *     callback is the only part that is render-thread bound.
 *   - A drawer at the end of the frame is not drawn; a drawer registered
 *     during one starts on the next.
 *   - Sixteen drawers maximum, names up to 47 characters.  A seventeenth
 *     is refused with a log line rather than falling back silently.
 *   - Window positions and sizes are remembered for the session (the
 *     framework deliberately keeps no imgui.ini on disk).  Removing the
 *     drawer with ShDrawDel() removes the window with it; closing it with
 *     its own close box only hides it, and ShDrawShow() brings it back.
 *   - The layer will not protect the game from a plugin that crashes in
 *     its callback - it runs on the render thread.  Keep it trivial.
 *  @{ */

/** Current UI scale, 1.0 = the 1080p baseline.  The primitives already
 *  scale themselves; this is for a plugin that wants to lay out custom
 *  pixel values (a canvas, an image, manual spacing) against the same
 *  factor the menu uses.  Falls back to 1.0 while no renderer is up. */
SH_API float ShDrawScale(void);
/** Is the overlay attached and drawing?  0 on a build without the
 *  overlay, and for the first frames before ImGui comes up.  A plugin
 *  that only draws needs no test - the primitives are no-ops - but one
 *  that wants to skip work can ask. */
SH_API int   ShDrawReady(void);

/** A drawer's callback.  `user` is whatever was passed to ShDrawAdd. */
typedef void (*ShDrawFn)(void *user);

/** SH_DRAW_FRAMELESS - no title bar, no background, no move or resize,
 *  and the window hugs its content: this is the shape the framework's own
 *  text box uses, and what a HUD-style panel wants.
 *  SH_DRAW_NO_MOVE / SH_DRAW_NO_RESIZE - keep the title bar but pin the
 *  position or the size.
 *  SH_DRAW_POS_CENTER_X - centre the window horizontally on the screen.
 *  SH_DRAW_POS_Y_RATIO - read `y` as a fraction of the screen height
 *  (0.72 is the line the framework's own text box sits on) instead of
 *  scaled pixels.
 *  The last two together are "the classic box spot": a plugin cannot work
 *  that out for itself without knowing the resolution, and both a
 *  chat-style box and a readout pinned to a screen edge want it.
 *  The rest of the window behaves like any other: draggable by its title
 *  bar, collapsible, and closable (which only hides it - see
 *  ShDrawShow). */
enum {
    SH_DRAW_FRAMELESS    = 0x01,
    SH_DRAW_NO_MOVE      = 0x02,
    SH_DRAW_NO_RESIZE    = 0x04,
    SH_DRAW_POS_CENTER_X = 0x08,
    SH_DRAW_POS_Y_RATIO  = 0x10
};

/** How a drawer's window should behave.  `x`/`y` are where it first
 *  appears - scaled pixels, or a screen-centre line and a height fraction
 *  with SH_DRAW_POS_CENTER_X / SH_DRAW_POS_Y_RATIO (0 = let the overlay
 *  pick); after that the user's own placement wins for the rest of the
 *  session. */
typedef struct ShDrawOpts {
    int   flags;   /**< SH_DRAW_* above                          */
    float x, y;    /**< first position, 0 = overlay default      */
} ShDrawOpts;

/** Named colours from the framework's own palette, so a plugin's window
 *  matches the menu instead of guessing. */
enum {
    SH_DRAW_COL_TEXT  = 0xD2D2D2,  /**< body text                  */
    SH_DRAW_COL_DIM   = 0x8C9BA8,  /**< hints and secondary lines  */
    SH_DRAW_COL_HI    = 0x8CF0FF,  /**< highlight / selection      */
    SH_DRAW_COL_WARN  = 0xFFD25A,  /**< titles and warnings        */
    SH_DRAW_COL_GOOD  = 0xA0E6A0,  /**< confirmations              */
    SH_DRAW_COL_PANEL = 0x000000   /**< window / panel background  */
};

/** Register a drawer: `name` is the window title and the key for
 *  ShDrawDel; `fn` is called once per frame on the render thread.  A
 *  second call with a name already registered replaces its callback
 *  rather than adding a slot.  Returns 0 if the name is empty, longer
 *  than 47 characters, the callback is NULL, or all sixteen slots are
 *  taken - each of those is logged. */
SH_API int ShDrawAdd(const char *name, ShDrawFn fn, void *user);
/** Same, with window options (see ShDrawOpts). */
SH_API int ShDrawAddEx(const char *name, ShDrawFn fn, void *user,
                       const ShDrawOpts *opts);
/** Drop a drawer and its window.  Returns 0 if there was no such name.
 *  Safe to call from inside a drawer's own callback, and safe to call
 *  for a name that was never registered. */
SH_API int ShDrawDel(const char *name);
/** How many drawers are registered (visible or hidden). */
SH_API int ShDrawCount(void);
/** Hide or show a drawer's window without unregistering it.  A hidden
 *  drawer is not called at all.  Returns 0 if there was no such name. */
SH_API int ShDrawShow(const char *name, int on);
/** Is that drawer's window visible?  0 for a name that is not
 *  registered.  A drawer whose own close box the user clicked reads 0
 *  here until ShDrawShow() brings it back. */
SH_API int ShDrawShown(const char *name);

/** Fonts for ShDrawPushFont.  DEFAULT is the CJK-capable system font
 *  the whole overlay uses; BOLD is the heavier face the menu titles and
 *  the text box are drawn with (and falls back to DEFAULT when the
 *  system has no bold face).  The size is the frame's own scaled body
 *  size - the primitives scale, so plugins do not. */
enum {
    SH_DRAW_FONT_DEFAULT = 0,
    SH_DRAW_FONT_BOLD    = 1
};
/** Push / pop the font.  Everything drawn between the two calls uses
 *  it; pops must be balanced (the renderer restores the stack at the end
 *  of every callback regardless). */
SH_API void ShDrawPushFont(int which);
SH_API void ShDrawPopFont(void);

/** One line of text at the cursor. */
SH_API void ShDrawText(const char *utf8);
/** One line of text in a chosen colour (0xRRGGBB, alpha 0-255). */
SH_API void ShDrawTextColored(const char *utf8, unsigned rgb, int a);
/** Text wrapped to the window's width. */
SH_API void ShDrawTextWrapped(const char *utf8);
/** A small dim line, the way the menu shows its control hints. */
SH_API void ShDrawHint(const char *utf8);

/** Vertical gap, next widget on the same line, horizontal rule. */
SH_API void ShDrawSpacing(void);
SH_API void ShDrawSameLine(void);
SH_API void ShDrawSeparator(void);

/** A filled rounded panel / a plain filled rectangle of that size,
 *  advancing the cursor past it.  The colour is an 0xRRGGBB + alpha
 *  pair like ShDrawTextColored; the panel uses the overlay's own corner
 *  rounding. */
SH_API void ShDrawPanel(float w, float h, unsigned rgb, int a);
SH_API void ShDrawRect(float w, float h, unsigned rgb, int a);

/** A button.  Returns 1 on the frame it is clicked. */
SH_API int ShDrawButton(const char *label);
/** A checkbox bound to *v (any non-zero is on).  Returns 1 when the user
 *  flips it; *v is updated. */
SH_API int ShDrawToggle(const char *label, int *v);
/** An integer field with +/- steps, clamped to [mn, mx].  Returns 1 when
 *  it changed. */
SH_API int ShDrawNumber(const char *label, int *v, int step, int mn, int mx);
/** A float slider clamped to [mn, mx].  Returns 1 when it changed. */
SH_API int ShDrawSlider(const char *label, float *v, float mn, float mx);
/** A drop-down of `n` NUL-terminated labels; *idx is the selection.
 *  Returns 1 when it changed. */
SH_API int ShDrawList(const char *label, int *idx, const char *const *items,
                      int n);

/** Start an input session for the box called `id`: from here the
 *  framework collects the characters the game window receives and the
 *  IME session belongs to this box.  Only one session exists at a time -
 *  opening one while another is open moves it.  Returns 0 on a bad id. */
SH_API int  ShDrawInputOpen(const char *id);
/** End the input session (the box is gone, or the game is about to
 *  inject the text into its own field).  Any undrained characters are
 *  dropped.  Safe to call with no session open. */
SH_API void ShDrawInputClose(void);
/** Is a session live?  This is what the window hook and the IME probe
 *  key off, so an owner that polls its own hotkeys uses it to notice
 *  that something else (the menu, the console) took the keyboard away. */
SH_API int  ShDrawInputIsOpen(void);
/** Collect the characters that arrived since the last call, as UTF-8,
 *  NUL-terminated, at most `cap` bytes (leave room for the terminator -
 *  64 is plenty per frame at 60 fps).  Returns the byte count.  A
 *  character that does not fit stays queued: nothing is ever lost, the
 *  drain simply takes another frame.  Call it from wherever the owner
 *  edits its buffer; `out` may be a stack buffer. */
SH_API int  ShDrawInputTake(char *out, int cap);
/** Candidate window mode: 0 = the overlay draws the candidate list
 *  itself (default), 1 = the input method's own window is used and
 *  steered under the box.  Mirrors the menu's "Candidate window" row. */
SH_API void ShDrawInputSetMode(int mode);
SH_API int  ShDrawInputGetMode(void);
/** True while an IME composition (pinyin) or its candidate list is
 *  live.  The owner must leave Enter / Esc / Backspace to the IME in
 *  that state - they edit the composition, not the buffer.  Callable
 *  from any thread. */
SH_API int  ShDrawInputComposing(void);
/** True while the owner is injecting the text into the game's own chat
 *  field.  The window hook keeps swallowing the user's physical keys
 *  during that window: a leaked Enter keyup would make the game submit
 *  before the injection finished. */
SH_API int  ShDrawInputSending(void);
/** Tell the framework the owner started / finished injecting.  While it
 *  is set the hook swallows physical keys but lets the injected
 *  WM_CHAR characters through - that is the channel into the game. */
SH_API void ShDrawInputSetSending(int on);

/** Internal, used by the overlay's window hook: feed one window message
 *  from the subclassed game window.  Returns 1 when the message was
 *  consumed and the game must not see it.  Text arrives as WM_CHAR /
 *  WM_IME_CHAR and is queued for ShDrawInputTake(); every other keyboard
 *  message is swallowed while a box is open, except Tab. */
SH_API int  ShDrawInputWndMsg(uint64_t hwnd, uint32_t msg,
                              uint64_t wp, uint64_t lp);

/** What the owner learns about its box as it draws it. */
typedef struct ShDrawInput {
    int focused;     /**< this box owns the session                */
    int composing;   /**< IME composition live: hands off the
                          command keys (same as
                          ShDrawInputComposing)                  */
    int mode;        /**< candidate window mode in force           */
} ShDrawInput;

/** Draw the input box: panel, text, IME composition, caret, candidate
 *  list and the hint line above it, at the current cursor position,
 *  sized to its content.  This is the same widget the framework's own
 *  Chinese chat box is made of, so a plugin's box is identical to it.
 *
 *  `id` must be the name the session was opened with - that is what
 *  decides `out->focused`, and only a focused box takes the IME anchor.
 *  `text` is whatever the owner wants shown (UTF-8, NUL-terminated);
 *  `hint` is a small line drawn above the box and may be NULL or empty.
 *  Returns 1 if the box was drawn. */
SH_API int  ShDrawInputBox(const char *id, const char *text,
                           const char *hint, ShDrawInput *out);

/** @} */
/** @defgroup hud HUD
 *  Drawn by the engine's own UI; slots pack per corner.
 *  @{ */

enum {
    SH_HUD_TOPLEFT = 0,
    SH_HUD_TOPRIGHT,
    SH_HUD_BOTTOMLEFT,
    SH_HUD_BOTTOMRIGHT,
    /** Top centre, a little below the top edge. Several lines
     *  stack downwards from there, each centred on its own. */
    SH_HUD_TOPCENTER
};

/** Register a line. Lower priority sits nearer the edge. */
SH_API uint32_t ShHudCreate(const char *name, int anchor,
                            int priority);
/** Text may hold newlines. Empty text hides the slot. The line
 *  stays until it is changed or hidden. */
SH_API int  ShHudSet(uint32_t hud, const char *text);
/** As ShHudSet, but the line hides itself after ms milliseconds.
 *  ms 0 keeps it until it is changed or hidden. */
SH_API int  ShHudFlash(uint32_t hud, const char *text, uint32_t ms);
SH_API int  ShHudColour(uint32_t hud, uint32_t rgb);
SH_API int  ShHudShow(uint32_t hud, int visible);
SH_API void ShHudDestroy(uint32_t hud);

/* ---- status toast -------------------------------------------------
 * A short line across the top of the screen that a plugin puts up
 * for a moment and then forgets about: "first person on, looking
 * for the head", "third person on". It leaves on its own after a
 * couple of seconds, or after however long the caller asked for.
 *
 * These say what to show and for how long, and nothing about
 * where or how it is drawn, so the layer underneath can change
 * without a plugin noticing.
 */

/** How long a toast stays up when the caller does not say. */
#define SH_TOAST_MS_DEFAULT 2500u

/** Show text for SH_TOAST_MS_DEFAULT, or replace the text of the
 *  toast last shown. Returns the toast id, 0 when none is free. */
SH_API uint32_t ShToast(const char *text);

/** As ShToast with a colour (0xRRGGBB) and a duration. ms 0
 *  keeps it up until it is changed or hidden. */
SH_API uint32_t ShToastEx(const char *text, uint32_t rgb,
                          uint32_t ms);

/** Change the text of a toast already up and start its time
 *  again. This is how a state moves on - "scanning" to "hidden" -
 *  without a second line appearing. */
SH_API int  ShToastSet(uint32_t id, const char *text, uint32_t rgb,
                       uint32_t ms);

/** Take a toast down now and free it. */
SH_API int  ShToastHide(uint32_t id);

/** Take every toast down. */
SH_API void ShToastClear(void);

/** How many toasts can be up at once. Enough for a drawer's array. */
#define SH_TOAST_MAX 4

/** One toast line as whoever draws it reads it. rgb is 0xRRGGBB;
 *  alpha is 0..255 and already carries the fade in and out, so a
 *  drawer only has to put it on screen as it is. */
#define SH_TOAST_TEXT 256
typedef struct {
    char     text[SH_TOAST_TEXT];
    uint32_t rgb;
    int      alpha;
} ShToastView;

/** The lines to draw now: how many there are, and up to max of them
 *  written into out. Taking the snapshot is also what retires a line
 *  whose time is up, so this is called once a frame by the drawer
 *  and nothing else has to keep time.
 */
SH_API int  ShHudToastSnapshot(ShToastView *out, int max);

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

/** Which view the frame is showing.
 *
 *  SH_VIEW_FIRST_PERSON while the first person eye is the one on
 *  camera. That is true in two cases: we are placing that eye
 *  frame by frame (whatever offset the active preset or seat
 *  anchor asks for - the eye being off the head bone does not
 *  make the view third person), or, right after we let go, the
 *  engine's own aim camera is sitting on the head bone, which is
 *  the iron sight hand over.
 *
 *  Everything else is SH_VIEW_THIRD_PERSON: the engine pulled the
 *  view back, a drone, a cutscene, a stowed weapon, a parachute,
 *  or plain third person. SH_VIEW_UNKNOWN means nothing has been
 *  measured yet, and consumers must treat it as "not first
 *  person" - a head hidden on a guess shows a headless body.
 *
 *  Reading this keeps the head measurement alive for a couple of
 *  seconds, so a consumer that polls it on its own tick is enough.
 */
enum ShViewMode {
    SH_VIEW_UNKNOWN = 0,
    SH_VIEW_FIRST_PERSON,
    SH_VIEW_THIRD_PERSON
};

SH_API int  ShCameraViewMode(void);

/** ShCameraViewMode() == SH_VIEW_FIRST_PERSON. A consumer that
 *  hides the head only while first person is really on camera
 *  reads this, so the head comes back when the engine takes the
 *  view away.
 */
SH_API int  ShCameraFirstPersonActive(void);

/** Drop the "we just handed the camera back" grace, see
 *  ShCameraViewMode. Turning first person off is a change of view,
 *  not a hand over to the engine's aim camera.
 */
SH_API void ShCameraHandoverClear(void);

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
/** Enumerates only entities backed by a mapped Havok rigid body,
 *  within `radius` metres of the player, nearest first not
 *  guaranteed. Returns the count written. */
SH_API int  ShFindPhysicsEntities(float radius, ShEntity *out, int max);
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

/** @defgroup archives Loaded archives
 *  What this session read, not what is on disk.
 *  @{ */

/** How many distinct `.forge` archives the engine has READ so far.
 *  Read, not opened: an archive opened and never read is not loaded.
 *  The list is kept by the forge I/O layer (scripthook_forge_io.c),
 *  which has to resolve every read handle to a path anyway, and it is
 *  installed for this alone - `[forgemod] ledger=1` by default - even
 *  when nothing is being modded. 0 until the first archive is read.
 *
 *  It is worth asking because a mode that mounts an archive of its own
 *  reads a file the other modes never touch: a session that read it
 *  cannot have been in the other mode. The engine reads its archives
 *  rather than mapping them (measured: 47 opens, 646 reads, not one
 *  CreateFileMapping or MapViewOfFile on a .forge), so a mapped file
 *  name would have missed every one of them.
 */
SH_API int ShForgeReadCount(void);

/** File name of the index-th read archive, without its folder:
 *  "DataPC.forge", "DataPC_GRN_WorldMap.forge". "" for an index out of
 *  range. The order is the order the archives were first read in.
 */
SH_API const char *ShForgeReadName(int index);

/** 1 when any read archive's file name contains `name`, case
 *  insensitive - so "WorldMap" and "DataPC_GRN_WorldMap.forge" both
 *  work. This is the cheap question: it does not name a mode by itself
 *  (every mode reads archives every mode reads), but an archive only
 *  one mode reads settles which mode this is.
 */
SH_API int ShForgeReadSeen(const char *name);

/** @} */
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
/** The same orbit with a sideways axis: metres along the camera's
 *  right vector, which is what an over-the-shoulder offset is. */
SH_API int  ShCameraOrbitAdvanced(float back, float right, float up);

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
/** The first person eye claim `ShCameraFirstPerson` takes. It is separate
 *  from SH_CAM_POS on purpose (the eye is the engine's own answer, not an
 *  absolute position), so releasing it must not be done by passing
 *  SH_CAM_POS: that also clears the position and the orbit arm, which
 *  another plugin may be holding. */
#define SH_CAM_HEAD   0x200u

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
/** @defgroup fov Field of view
 *
 *  The engine computes the fov from the active camera behaviour and
 *  writes it into the camera manager every frame, so an override is a
 *  value the engine keeps being handed rather than a field written once.
 *  A value under 0.5 rad is a zoom optic - scopes and binoculars compute
 *  far below the 0.78 to 0.83 gameplay range - and those keep their own
 *  fov by default.
 *  @{
 */

/** The engine's own fov of the last frame, radians, before any
 *  replacement - 0 until the engine has run the site once. A plugin
 *  uses it to tell a narrowed aim (a mild zoom, still inside the
 *  gameplay range) apart from a magnified optic, which computes far
 *  below it.
 */
SH_API float ShFovEngine(void);

/** With the pin set, the override replaces the engine's value whatever
 *  it is, the zoom optics included - which is what "no zoom on ads"
 *  needs on the frames an aim would otherwise narrow the view. It stays
 *  set until cleared, and releasing SH_CAM_FOV clears it with the rest.
 */
SH_API void ShFovPin(int on);

/** @} */
/** @defgroup fpx First person, the engine's own way
 *
 *  The eye is the engine's own head position. The argument the
 *  engine hands its head function is captured once, then that
 *  same function is asked again every frame and the answer is
 *  written where the camera position goes. Nothing here is
 *  derived from the camera basis: no forward, no up, no easing
 *  - which is why it does not fight the engine on slopes, in
 *  vehicles or through a respawn.
 *
 *  A menu, the drone and iron sights each carry a byte the
 *  engine already maintains, and while any of them is up the
 *  camera is left alone.
 *
 *  ShCameraFirstPerson takes this path whenever it is
 *  available. Where these sites cannot be found - a build we
 *  have not seen - the older placement still answers, so the
 *  game behaves exactly as it did before.
 *  @{
 */

/** Install every site. Returns 1 when the capture site, the
 *  one the rest depends on, was found and patched. A partial
 *  install still works: the gates that could not be found
 *  simply never close.
 */
SH_API int  ShFp2Install(void);
SH_API int  ShFp2Ready(void);

/** The body and shoulder patches: always allow a shoulder swap,
 *  keep the body when it is pushed against a wall, and the two
 *  hooks that feed them. None of these is needed for the eye,
 *  and all of them change how the engine draws the body whether
 *  first person is on or not, so they are installed separately.
 *
 *  mask is one bit per site - 1 the body position hook, 2 body
 *  visibility, 4 the shoulder swap, 8 the wall push - so they
 *  can be taken one at a time. Returns 1 when every site that
 *  was asked for took.
 */
SH_API int  ShFp2InstallExtras(uint32_t mask);

/** Which sites did not take, as a mask. 0 means all of them.
 *  Bit 0 the capture site, 1/2/3 the three menu sites, 4 the
 *  drone, 5 aim down sight, 6 the body reading, 7 body
 *  visibility, 8 the shoulder swap, 9 the wall push.
 */
SH_API uint32_t ShFp2Missing(void);

/** 1 to take the camera, 0 to hand it back. */
SH_API void ShFp2Enable(int on);

/** The eye offset in metres, in the eye's own axes: right
 *  along the camera's right, forward along its forward
 *  flattened to the horizon, up in world Z.
 */
SH_API void ShFp2SetOffset(float right, float forward, float up);

/** The live gate bytes. Any argument may be NULL. */
SH_API void ShFp2Gate(int *menu, int *drone, int *ads, int *fresh);

/** 1 while the head is reachable through the engine's own
 *  visibility call, so a caller knows whether it has to hide
 *  the head by some other means.
 */
SH_API int  ShFp2HeadOk(void);

/** Force the head visible (non zero) or hidden (0), now and
 *  for every frame until said otherwise. While first person
 *  holds the camera the hidden state is restated once a
 *  frame, because the engine keeps reasserting its own.
 */
SH_API void ShFp2HeadShow(int show);



/** Why the last frame placed no eye: 0 it placed, 1 not
 *  asked, 2 a menu, 3 the drone, 4 an aim, 5 stale, 6 no
 *  argument, 7 the answer was not a position.
 */
SH_API int  ShFp2Bow(void);

/** Milliseconds since the last frame an eye was placed. */
SH_API uint32_t ShFp2Age(void);

/** @} */

/* Not exported. Called from the camera manager's own frame,
 * inside the engine's call chain, by scripthook_camera.c -
 * once a frame whether first person runs or not, so the head
 * is held down while taken and restated while handed back.
 */
int ShFp2PlaceEye(uint64_t cm, float *m, float *p);
void ShFp2HeadFrame(void);

/** @defgroup cpu Processor scheduling
 *  The processor set and the process priority the framework holds for each
 *  stage of the game's start up, the efficiency mode, and the read-only
 *  queries a plugin can use to see them.
 *
 *  Why a plugin would want to: while a dial is in force the framework
 *  answers the whole process for the processor count, the topology, the
 *  affinity and the priority. That is what keeps the engine from spreading
 *  itself back over every core, and it means `GetSystemInfo` and friends
 *  report the trimmed set to every caller, a plugin's included - a thread
 *  pool sized from them is sized for the set that is really allowed, which
 *  is usually what a caller wants, but it is no longer the machine's own
 *  answer. These calls say what the framework is doing instead. With every
 *  dial left alone nothing is hooked and every answer is the machine's
 *  own; the queries below still work and report that truthfully.
 *  @{ */

/** Outcome of one scheduling trim, for the log and the menu.
 *  The values double as the state of the whole switch: 0 it was
 *  never asked for, 1 it is in force, 2 it does not apply to
 *  this CPU (not an Intel hybrid), 3 it does not apply (an Intel
 *  CPU with no E-cores), 4 the detection failed, 5 it was asked
 *  for but the result would have been an empty set.
 */
#define SH_CF_OFF           0
#define SH_CF_APPLIED       1
#define SH_CF_NA_NOT_INTEL  2
#define SH_CF_NA_NO_ECORE   3
#define SH_CF_FAILED        4
#define SH_CF_SKIPPED_EMPTY 5

/** The stage a dial belongs to, and the value ShCpuStage reports.
 *
 *  The three are steps, not modes: within one session they only ever move
 *  forwards, once each, and none of them comes back.
 *
 *   - BOOT    the logo screen, until the game's own window appears;
 *             the framework knows it by the window itself, on two features:
 *             the class the game gives it (ScimitarSplashScreenWindow,
 *             against ScimitarEngineWindowClass for the window that ends
 *             the step) and the title, where the registered mark comes
 *             through mis-encoded ("Ghost Recon?Wildlands", against
 *             "Ghost Recon(R) Wildlands"). The class is read first and the
 *             title is the fallback, so a renamed build behaves as it did
 *             before the class was used at all;
 *   - WINDOW  the game's own window, until the main menu is reached: the
 *             first load, where the engine does its own start-up work;
 *   - PLAY    everything from the first main menu on - the menu, a lobby,
 *             a later load screen, the pause menu and the world - for the
 *             rest of the session. Past the front end the two questions a
 *             stage answers (is the engine still starting, is any of the
 *             world up) have both been asked and answered.
 */
#define SH_STAGE_BOOT   0   /* the logo screen                      */
#define SH_STAGE_WINDOW 1   /* the window, before the front end     */
#define SH_STAGE_PLAY   2   /* from the first main menu on          */

/** The efficiency mode (Windows 11 EcoQoS - the switch Task Manager shows
 *  as "Efficiency mode"), as ShCpuStatus.eco reports it. Not a priority
 *  class: the framework sets it through the process' power-throttling
 *  class, and offers it for the two start-up stages only, where the work is
 *  the kind the hint is documented for.
 *
 *  Windows 11 only. The calls exist earlier, but the level this names does
 *  not - Microsoft's page for SetProcessInformation says such a process was
 *  marked LowQoS before Windows 11 - so on anything older the dial reads as
 *  SH_ECO_NA and nothing is set: a dial that cannot be honoured must not
 *  read as one that was. The settings page says so on its hint line.
 */
#define SH_ECO_OFF     0    /**< not on                                 */
#define SH_ECO_ON      1    /**< on right now                           */
#define SH_ECO_NA      2    /**< not on this system (needs Windows 11)  */
#define SH_ECO_FAILED  3    /**< the call failed; see the corefix log   */

/** One plugin's stage callback, handed the new SH_STAGE_*. */
typedef void (*ShCpuStageFn)(int stage, void *user);

/** The values ShCpuStatus.prio[] carries: the four the play dial offers,
 *  and the states the loading stages' dial can resolve to. The efficiency
 *  ones are never a choice in themselves - they are only ever the state
 *  that is in force: SH_PRIO_LOW is what SH_PRIO_ECO falls back to on a
 *  machine which cannot do efficiency mode, and SH_PRIO_ECO_OFF is the
 *  dial's "off" (cpu_eco_boot=2), where the mode is dropped outright.
 */
#define SH_PRIO_LEAVE  0
#define SH_PRIO_NORMAL 1
#define SH_PRIO_ABOVE  2
#define SH_PRIO_HIGH   3
#define SH_PRIO_ECO    4
#define SH_PRIO_LOW    5

/** What the CPU scheduling dials are doing right now. The fields are
 *  written by the framework's stage thread and read from anywhere, so a
 *  copy may mix two ticks a quarter of a second apart - each field is true
 *  of some moment, which is what a status query is for.
 */
typedef struct {
    int      active;        /* any stage does something (0 = disabled) */
    int      stage;         /* the stage in force (SH_STAGE_*)        */
    int      dial[3];       /* the three core dials from the ini      */
    int      prio[3];       /* the priority each stage holds (SH_PRIO_*):
                             * [0] and [1] are what the loading switch
                             * resolved to, [2] is the play dial        */
    int      ecoreState;    /* SH_CF_* for this CPU's applicability   */
    int      eco;           /* SH_ECO_* : the efficiency mode now     */
    int      ecoOurs;       /* 1 when that switch is one we turned on */
    int      ecoBoot;       /* the loading dial as it was set:
                             * 0 leave alone, 1 efficiency mode, 2 off  */
    unsigned origCount;     /* processors this process started with   */
    unsigned sysCount;      /* processors the machine has             */
    unsigned reportCount;   /* what the engine is told (0 = as-is)    */
    unsigned keepCount;     /* schedulable now (0 = as it came)       */
    unsigned long long mask;/* the set in force (0 = as it came)      */
} ShCpuStatus;

/** Fill *out with the launch's scheduling result. 0 only for a NULL
 *  argument: the dials are read on the attach path, so there is always
 *  something to say. */
SH_API int      ShCpuGetStatus(ShCpuStatus *out);

/** The stage the framework is in now (SH_STAGE_*), cheap and safe to poll.
 *  True whether or not any dial is set, and it only ever moves forwards, so
 *  a caller can latch on the first SH_STAGE_PLAY and stay latched. This is
 *  the framework's own reading of the start up and not a second game state:
 *  the engine's states put the main menu and every lobby in one bucket and
 *  do not tell the logo window from the game's own, and those two are
 *  exactly the boundaries drawn here. */
SH_API int      ShCpuStage(void);

/** The processors the process may run on while a dial is in force, or 0
 *  when nothing trims the set. This is the mask the affinity hooks answer
 *  with, so a caller can place its own work inside it on purpose. */
SH_API uint64_t ShCpuAllowedMask(void);

/** How many processors the engine is told the machine has (0 = as-is).
 *  The number the count hooks report: lower than the machine's own while a
 *  dial trims the set. */
SH_API unsigned ShCpuReportedCount(void);

/** Be told when the stage changes: called once per change with the new
 *  SH_STAGE_*, never with the stage already in force - at most twice in a
 *  session, since the stages only move forwards. One slot per plugin (the
 *  caller's module is the identity, as with ShPluginOnBlocked) and eight
 *  plugins can subscribe; calling in with NULL clears this plugin's slot.
 *  1 when accepted, 0 with ShLastError saying why. The callback runs on the
 *  framework's stage thread and may call back into the framework. */
SH_API int      ShCpuOnStageChange(ShCpuStageFn fn, void *user);

/** @} */

/** Internal, not a plugin API: the play-time half of the trims, started by
 *  the loader on its own thread (never from DllMain, where creating a
 *  thread deadlocks). It is what puts the stage thread up, and that thread
 *  runs whether or not a dial asks for anything - the stage above is part
 *  of the API, so it is read and reported either way.
 */
void ShCoreFixLateStartup(void);

/** @defgroup files File interception
 *  One owner for the file APIs, and rules instead of hooks.
 *
 *  Five parts of this repository used to intercept the same kernel32 file
 *  calls with five private arrangements: skipintro patched the main
 *  module's import table by hand, while GhostNoWipe, GhostWipeProbe,
 *  forgeprobe and scripthook_forge_io each built their own MinHook set.
 *  MinHook keeps one hook per target per module, so those arrangements
 *  have been shaping the code around them - forge_io asks a handle where
 *  its file is with GetFinalPathNameByHandleW because CreateFileW was
 *  taken, and skipintro wrote a PE parser because a slot was easier to own
 *  than a target.
 *
 *  This layer is that owner. It hooks the file APIs once, in the framework
 *  DLL, and everything else - framework modules and plugins alike -
 *  registers a rule. A rule says which file it is about, which calls it
 *  covers, and what to do when one matches:
 *
 *   - SH_FILE_HIDE     answer "no such file" without calling anything;
 *   - SH_FILE_REDIRECT run the call against another path;
 *   - SH_FILE_DECIDE   hand the call to desc->before, which either answers
 *                      it or lets it through.
 *
 *  plus an optional desc->after, which sees the call once it has run (or
 *  been answered) and may change what the caller gets - ReadFile's buffer
 *  among it. A watcher is a rule with only an after callback: it looks at
 *  everything and changes nothing.
 *
 *  More than one rule can match one call. They are weighed, not raced:
 *  HIDE beats REDIRECT beats DECIDE, and within one action the rule
 *  registered first wins. Hiding first is deliberate - "the file is not
 *  there" is the most conservative answer available, and the only one that
 *  cannot hand a caller something wrong.
 *
 *  The hooks are inline, so they answer for the whole process: the game,
 *  every plugin and the framework's own modules go through this one place,
 *  and a pointer taken with GetProcAddress is intercepted exactly like a
 *  static import. That is the point - and it is why the layer also has to
 *  know when NOT to look: its own pass is marked, a thread can mark its
 *  own I/O with ShFileOwn, and a callback's own file calls pass straight
 *  through untouched.
 *
 *  Nothing is installed until a rule is registered, and the last
 *  unregistration takes every hook and trampoline back out: a session that
 *  registers nothing runs with not one intercepted call. That is what lets
 *  a one-shot user keep its promise - skipintro releases its rule when it
 *  is done, and the layer uninstalls itself if nobody else is left.
 *
 *  The callbacks run on the calling thread, inside the file call they are
 *  about, with the caller's stack below them: keep them short, never
 *  block, and never wait for another thread that needs the same thread to
 *  make a file call. A callback may call any file API it likes - the layer
 *  recognises its own threads and passes those calls through - but it
 *  should not expect its own rules to apply to them: they do not, on
 *  purpose, which is what keeps "look at the real state of the disk"
 *  possible from inside a rule.
 *
 *  Not here, on purpose: no priority field (three actions and registration
 *  order are the whole ordering rule), no directory virtualization, no way
 *  for a plugin to hold or call the real functions, and no global switch.
 *  A rule is a registration, and two writers of one answer is the thing
 *  this layer exists to prevent.
 *  @{ */

/** What a rule does when a call matches it. */
#define SH_FILE_HIDE     1  /**< answer "no such file" without a call   */
#define SH_FILE_REDIRECT 2  /**< run the call on desc->to instead        */
#define SH_FILE_DECIDE   3  /**< ask desc->before: answer, or let it run */

/** Which calls a rule is about; or the bits together. A group with a file
 *  name in it (OPEN, ATTR, MOVE, DELETE, FIND) is one a HIDE or a REDIRECT
 *  can apply to; the others carry a handle instead, and a rule for them is
 *  either a decision or a watch. */
#define SH_FILE_OPEN    0x0001u  /**< CreateFileA/W                     */
#define SH_FILE_ATTR    0x0002u  /**< GetFileAttributesA/W/ExA/ExW      */
#define SH_FILE_MOVE    0x0004u  /**< MoveFileA/W, MoveFileExA/W        */
#define SH_FILE_DELETE  0x0008u  /**< DeleteFileA/W, RemoveDirectoryA/W */
#define SH_FILE_FIND    0x0010u  /**< FindFirstFileA/W/ExA/ExW,
                                      FindNextFileA/W                   */
#define SH_FILE_READ    0x0020u  /**< ReadFile, and where a read will
                                      start or end: SetFilePointer(Ex),
                                      GetFileSize(Ex), CreateFileMapping,
                                      MapViewOfFile, UnmapViewOfFile   */
#define SH_FILE_WAIT    0x0040u  /**< the completion path: the WaitFor*
                                      calls, GetOverlappedResult(Ex),
                                      CloseHandle                       */
#define SH_FILE_INFO    0x0080u  /**< SetFileInformationByHandle,
                                      CopyFileA/W                       */
#define SH_FILE_ANY     0x00FFu  /**< every group of them               */

/** One file call, as the layer hands it to a rule's callbacks. Filled in
 *  by the layer on the caller's stack - nothing here is ever allocated,
 *  and none of it outlives the call. */
typedef struct {
    const char    *api;      /**< the call itself: "CreateFileW",
                                  "MoveFileExW", "DeleteFileA" ... the
                                  kernel32 name, which is what a watcher
                                  logs and a rule keys on              */
    uint32_t       group;    /**< the SH_FILE_* bit this call is in      */
    int            wide;     /**< 1 for the W variant of the call, and
                                  so for `path`, `asked` and `to`       */
    const wchar_t *path;     /**< the file the call is about, as it will
                                  be used - the redirect target, after a
                                  redirect. NULL on an A call.          */
    const char    *pathA;    /**< the same for an A call: NULL when wide */
    const wchar_t *asked;    /**< the file the caller named, before any
                                  redirect (NULL on an A call)          */
    const char    *askedA;   /**< the same for an A call: NULL when W    */
    const wchar_t *to;       /**< the second path, on the calls that have
                                  one: where a move or a copy is going
                                  (NULL on an A call)                    */
    const char    *toA;      /**< the same for an A call: NULL when W    */
    DWORD          access;   /**< an open's own arguments, so a rule that
                                  answers one can open something else
                                  (SH_FILE_OPEN)                       */
    DWORD          share;
    DWORD          disp;
    DWORD          flags;
    HANDLE         handle;   /**< the handle, on the calls that carry one */
    void          *buffer;   /**< ReadFile's buffer                      */
    DWORD          bytes;    /**< the size the call was made with: what
                                  ReadFile was asked for, a mapping's
                                  length, an info size, a wait's timeout  */
    DWORD          done;     /**< how much it says it did - the bytes a
                                  ReadFile transferred, when the call
                                  reports one (0 when it does not)      */
    void          *overlapped; /**< the OVERLAPPED, when there is one    */
    uint64_t       offset;   /**< where the I/O starts, when the call or
                                  the layer knows it                    */
    void          *result;   /**< what the call returned, or what a
                                  callback answered it with             */
    DWORD          error;    /**< GetLastError at that moment, or the
                                  error a callback answered with       */
    int            answered; /**< 1: no real call was made - a rule
                                  answered (see `result` and `error`)   */
    int            matched;  /**< 1 when a rule matched this call, and so
                                  when the after callbacks were run      */
    void          *user;     /**< the rule's own pointer, for a callback */
} ShFileCall;

/** A rule's before callback, asked only for SH_FILE_DECIDE: 1 when it has
 *  answered the call (the layer returns `result` and raises `error`), 0 to
 *  let the real call run. */
typedef int (*ShFileDecideFn)(ShFileCall *call, void *user);

/** A rule's after callback, run once per matching call - whether it ran or
 *  a rule answered it - and able to change `result`, `error`, and for
 *  ReadFile the data in `buffer`. It runs for every matching rule that has
 *  one, in registration order. */
typedef void (*ShFileAfterFn)(ShFileCall *call, void *user);

/** One rule. `name` is matched against the file's own name, the part after
 *  the last separator, case insensitively; NULL or "" matches every file
 *  the group covers (which is how a watcher is written). `suffix`, when
 *  given, must also match the end of the whole path - for a file that
 *  moves around inside a tree but keeps its tail. `to` is read only for
 *  SH_FILE_REDIRECT, and `before` only for SH_FILE_DECIDE. Every string is
 *  copied, so the caller's own may go away as soon as this returns. */
typedef struct {
    const wchar_t  *name;
    const wchar_t  *suffix;
    uint32_t        group;   /**< SH_FILE_* bits                         */
    int             action;  /**< SH_FILE_HIDE / _REDIRECT / _DECIDE     */
    const wchar_t  *to;      /**< the redirect target, SH_FILE_REDIRECT  */
    ShFileDecideFn  before;
    ShFileAfterFn   after;
    void           *user;
} ShFileRuleDesc;

/** A registration. Opaque: hand it back to ShFileRuleDel and forget it. */
typedef struct ShFileRule ShFileRule;

/** Register a rule. 1 = installed, and the layer's hooks go up with it.
 *  0 = refused, with the reason in logs\scripthook_files.log (no slot
 *  left, a group with no bit in it, SH_FILE_REDIRECT with no `to`,
 *  SH_FILE_DECIDE with no `before` and no `after`, or a name longer than
 *  the table holds). The caller's module is the identity the log and
 *  ShFileRuleOwner report, so a plugin does not name itself. */
SH_API ShFileRule *ShFileRuleAdd(const ShFileRuleDesc *desc);

/** Take a rule back out. 1 = it was live and is not any more; 0 = it was
 *  already gone (never fails otherwise). The hooks come down with the last
 *  rule, so this is also how a one-shot user puts the process back the way
 *  it found it. */
SH_API int      ShFileRuleDel(ShFileRule *rule);

/** How many rules are registered right now. */
SH_API int      ShFileMatchCount(void);

/** Whether the layer's hooks are in place - 1 while at least one target is
 *  hooked, 0 after the last rule has gone. */
SH_API int      ShFileInstalled(void);

/** How many calls the layer has looked at this session: every call that
 *  reached the dispatch, whether a rule matched it or not. */
SH_API uint32_t ShFileCallCount(void);

/** Mark this thread's own file calls. While `on`, the layer does not look
 *  at anything this thread does: no rule is applied and no observer sees
 *  it. For a module that reads files as part of serving one (the forge
 *  loader's own reads are the reason this exists), which would otherwise
 *  be fed back through its own rules. Every turn on must be matched by one
 *  off, and it does not nest. */
SH_API void     ShFileOwn(int on);

/** The name of one action, for a log line or a menu row: "hide",
 *  "redirect", "decide", "watch", "" for anything else. A translation key:
 *  pass it through ShLang before showing it. */
SH_API const char *ShFileActionName(int action);

/** One line about what the layer is doing, for a status row or a log:
 *  how many rules, which of them are watching or deciding, whether the
 *  hooks are in place and how many calls have been looked at. Returns the
 *  length written, 0 when `buf` is too small. */
SH_API int      ShFileStatus(char *buf, int n);

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

/** What the player is doing, from the engine's own input
 *  context dispatcher (AnvilNext notes, file 12). One index
 *  is active at a time and covers on foot, driving, riding,
 *  flying and menus. Read cheap, no engine call.
 */
enum ShInputContextIdx {
    SH_CTX_EMPTY = 0,
    SH_CTX_ONFOOT,
    SH_CTX_FASTTUNING,
    SH_CTX_BINOCULARS,
    SH_CTX_VEHICLE_PASSENGER,
    SH_CTX_VEHICLE,
    SH_CTX_HELICOPTER,
    SH_CTX_AIRPLANE,
    SH_CTX_DRONE,
    SH_CTX_SQUADTACTICS,
    SH_CTX_MENU,
    SH_CTX_POPUP
};

/** The active input context index, or -1 when it cannot be
 *  read (not in game, engine objects not up). A consumer
 *  that changes behaviour per stance or per vehicle reads
 *  this: OnFoot for walking, Vehicle for ground vehicles,
 *  Helicopter/Airplane while flying, VehiclePassenger while
 *  riding along, Menu for the pause screens.
 */
SH_API int  ShInputContext(void);
typedef int (*ShInputContext_t)(void);

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
typedef struct {
    ShVec3   hitPos;
    uint32_t hits;
    uint8_t  record[128];
} ShSurfaceProbe;
/** Casts the ScriptHook-owned downward ray and copies its first
 *  collision record before the physics scratch storage can be
 *  reused. */
SH_API int  ShProbeSurface(float x, float y, float nearZ,
                           ShSurfaceProbe *out);
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

/** One entry of the resource node, by its own order rather than by name - for
 *  a probe. The node the four named resources live in holds EIGHT entries, and
 *  what the other four are is a question worth answering from a log instead of
 *  a heap scan: a count that moves by one per shot would be cheap to reach
 *  there, with no hook at all.
 *
 *  i runs from 0 to the count the node reports. *value is the decoded int
 *  behind the entry, *prot the address of the protected int itself; either
 *  may be NULL. 1 when the row exists, 0 with SH_ERR_NO_CANDIDATE past the end
 *  or while the manager is not up. */
SH_API int  ShGetResourceSlot(int i, uint32_t *spec, uint32_t *value,
                              uint64_t *prot);

/** Skill points, a plain int rather than a protected one. */
SH_API int  ShGetSkillPoints(uint32_t *out);
SH_API int  ShSetSkillPoints(uint32_t value);

/** Visibility to enemies. 1 normal, 0 invisible, 0.5 halves
 *  the detection range, above 1 is easier to spot.
 */
SH_API int  ShSetVisibility(float factor);
SH_API int  ShGetVisibility(float *out);

/** The MAGAZINE CAPACITY the game computes for a weapon, scaled by a
 *  num/den pair (2,1 double, 1,2 half, 1,1 the game's own value).
 *
 *  Capacity is the return value of one engine function (RVA 0x614CB0) and
 *  is stored nowhere, so this hook is the only way to change it - reading
 *  memory cannot find it. See docs/ammocapacity-reverse.md (kept out of
 *  the repository).
 *
 *  The hook installs on the first call that asks for something other than
 *  1,1; asking for 1,1 first leaves the game alone and installs nothing.
 *  A change takes effect at the next refill: an ammo crate is what puts the
 *  number to use. 1 on success, 0 with ShLastError saying why
 *  (SH_ERR_BAD_ARG, SH_ERR_HOOK_FAILED). */
SH_API int  ShSetAmmoScale(int num, int den);
/** What is in force, as the pair. 1/1 when nothing was ever set. */
SH_API void ShGetAmmoScale(int *num, int *den);
/** 1 while a scale other than 1/1 is in force. */
SH_API int  ShAmmoScaleActive(void);

/** One capacity look the hook saw, for a probe.
 *
 *  This exists because the engine function's ARGUMENTS are the only place the
 *  weapon - or whatever the engine passes - can be reached from, and nothing
 *  in this API has ever said what they are: the scale only needs the return
 *  value. A probe samples this while firing, reloading and refilling and reads
 *  the answer out of its own log; a plugin that ships has no reason to call
 *  it.
 */
typedef struct {
    uint64_t args[4];   /**< rcx, rdx, r8, r9 as the engine passed them */
    uint32_t raw;       /**< the capacity it computed: low 16 bits, UNSCALED */
    uint32_t count;     /**< looks recorded since the hook went in */
    uint64_t tick;      /**< GetTickCount64 of the last one */
} ShAmmoLook;

/** The last capacity look, or 0 with SH_ERR_NO_CANDIDATE before the first one
 *  (and when no hook is installed at all - see ShSetAmmoScale: asking for 1/1
 *  leaves the game alone and installs nothing, so a probe asks for some other
 *  scale first and puts 1/1 back once it has what it came for). */
SH_API int  ShGetAmmoLook(ShAmmoLook *out);

/** Rounds left in the magazine of the weapon the engine last asked for a
 *  capacity - the local player's own weapon, and the number the HUD shows.
 *
 *  How it is reached, because finding it took a while: the engine hands that
 *  weapon to the one function that computes a magazine's capacity (the one
 *  ShSetAmmoScale hooks), ShGetAmmoLook reports which object that was, and the
 *  rounds are a protected int 0x180 into it - 0x130 for the weapons that keep
 *  them there, which is the fallback the removed ShGetAmmo already used. No
 *  heap scan, no class constant, nothing cached.
 *
 *  0 with SH_ERR_NO_CANDIDATE until the engine has asked once, which firing a
 *  round or switching a weapon makes it do - so the capacity hook has to be
 *  installed (asking for 1/1 alone installs nothing: see ShSetAmmoScale).
 *  SH_ERR_BAD_ARG for a NULL out. This is the magazine, not the reserve:
 *  reloading puts it back to the capacity that ShSetAmmoScale scales.
 *
 *  A weapon switch is followed within a frame or two: the engine asks about
 *  the weapon coming up (the HUD redraws its number, a shot asks) and stops
 *  asking about the one that went down, and the reading follows the newest of
 *  those calls that is the player's own before it follows anything else.
 *  ShGetAmmoObject names the object the answer came from. */
SH_API int  ShGetAmmoRounds(int *rounds);

/** Which weapon the last ShGetAmmoRounds reading was about.
 *
 *  The rounds alone cannot say that the weapon in hand changed - two weapons
 *  read the same number often, and the value a switch shows is the one the
 *  plugin was just told - so this is what a caller watches to notice that the
 *  magazine it was following has been put away. Set by ShGetAmmoRounds, which
 *  has to have answered at least once: 0 with SH_ERR_NO_CANDIDATE until then,
 *  and SH_ERR_BAD_ARG for a NULL out. */
SH_API int  ShGetAmmoObject(uint64_t *obj);

/** One entry of the call trace: what the engine asked a capacity about, and
 *  when. */
typedef struct {
    uint64_t obj;       /**< the object the engine passed */
    uint64_t tick;      /**< GetTickCount64 of that call */
    uint64_t seq;       /**< its order: 1, 2, 3 ... since the hook went in */
} ShAmmoCall;

/** The last calls that changed WHICH object was asked about, oldest first, up
 *  to max of them. A weapon switch runs both the weapon going down and the one
 *  coming up through this function, and nothing about those objects says which
 *  is which - so the order they were asked in is the only evidence there is.
 *  Only transitions are recorded, so one switch is one burst. A probe reads
 *  this while switching weapons; a plugin that ships has no reason to. */
SH_API int  ShGetAmmoCalls(ShAmmoCall *out, int max);

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
/** Declare that a virtual key needs event polling (1) or withdraw it (0).
 *  While nothing has been declared the poll thread sweeps the whole
 *  keyboard, exactly as it always did; once anything is declared it sweeps
 *  the declared keys plus every key it is currently holding, and nothing
 *  else. Declaring is therefore how a consumer says what it listens for -
 *  and a consumer that declares nothing keeps working unchanged. */
SH_API int      ShUiInputWatch(int vk, int on);
/** One virtual key hidden from the game until released. */
SH_API int      ShBlockKey(int vk, int on);
/** Every key but the escapes hidden, focus uses this. */
SH_API int      ShCaptureKeys(int on);

/** Is the game's own window the one in front? Any window of this
 *  process counts, so windowed and borderless fullscreen both answer
 *  yes, and a backgrounded game answers no.
 *
 *  Ask this before acting on a key read with GetAsyncKeyState: that
 *  reads the PHYSICAL key, so without the check a hotkey fires on a
 *  press meant for whichever window the player switched to. One
 *  GetForegroundWindow, answered uncached - a stale "yes" is the one
 *  case this exists to prevent. */
SH_API int      ShGameFocused(void);

/** Queue full press/release taps of a virtual key into the game's
 *  DirectInput keyboard reports.  The phases advance in real time
 *  (60 ms per phase), so the engine sees a human-length press. */
SH_API void     ShFakeKey(int vk, int taps);
/** Same, but phases advance per GetState report with no wall-clock
 *  delay, so N taps burst as fast as the engine polls the device. */
SH_API void     ShFakeKeyFast(int vk, int taps);
/** 1 while a fake-key tap sequence is still in flight. */
SH_API int      ShFakeKeyBusy(void);
/** Diag: what the game last saw on this keyboard device, or -1
 *  before any report was captured. */
SH_API int      ShKeyState(int vk);

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
/** Show or suppress the game-owned HUD_* scenes. Framework and menu
 *  scenes remain visible either way. */
SH_API int      ShGameHudShow(int visible);

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

/** What ShLogLevel() answers with. The same numbers log.h uses for the
 *  framework's own lines; a plugin with a log of its own reads them from
 *  here rather than including log.h. */
#define SH_LOG_NONE   0
#define SH_LOG_ERR    1
#define SH_LOG_WARN   2
#define SH_LOG_INFO   3
#define SH_LOG_DBG    4

/** The [Settings] LogLevel in force, as one of the SH_LOG_* values:
 *  0 none, 1 error, 2 warn, 3 info, 4 debug. Parsed once and
 *  kept, so editing the ini takes effect on the next launch. Every
 *  module - and every plugin that logs - asks this one function, which
 *  is what keeps a session's logs\ folder telling one story.
 *
 *  The files that follow the level are the framework's own module logs:
 *  those exist at SH_LOG_INFO and SH_LOG_DBG, and not below. The two the
 *  support flow asks for - the loader's log and the crash report - are
 *  written whatever it says, and a plugin's own log is written at every
 *  level except SH_LOG_NONE, so a plugin that failed is still on record
 *  in an otherwise quiet session. */
SH_API int  ShLogLevel(void);

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

/** <gamedir>\plugins\<name>\lang.ini, the text file that belongs
 *  beside a plugin of the same name. The framework reads it for a
 *  menu that plugin created (see @ref lang); a plugin does not have
 *  to ship one, and should not write one - the framework never does.
 *  Handy for a tool that wants to seed or inspect it. */
SH_API int  ShPluginLangPath(const char *plugin, char *buf, int size);

/** @} */
/** @defgroup lang Localization
 *  Every piece of text a player can read goes through one lookup.
 *
 *  **Keys.** A key starting with '@' is a stable ID
 *  ("@camo.page.visibility"); anything else is a literal and is its
 *  own key. IDs survive a renamed row or menu title - which is what
 *  the old "[lang.<menu title>]" scheme could not - and a literal is
 *  how a plugin whose source you do not have is translated.
 *
 *  **Sources**, in order:
 *    1. plugins\<owner>\lang.ini, section [<language>]   (the owner's)
 *    2. <gamedir>\lang.ini,      section [<language>]   (shared override)
 *    3. the compiled-in baseline this module declared with
 *       ShLangDeclare, for the active language
 *    4. the same baseline, for "en-US"
 *    5. an ID with no text anywhere is shown readable ("@a.b" ->
 *       "A B") and logged once; a literal falls through to itself
 *
 *  A lang.ini row overrides the baseline for that one key, so a file
 *  only needs the lines it changes. Nothing in the framework ever
 *  writes a lang.ini: the settings file is ours, the text is yours.
 *
 *  Language codes are standard BCP-47 tags ("zh-CN", "en-US") and are
 *  compared case-insensitively and exactly. [Settings] Language is the
 *  active language, [Settings] Languages the list the picker shows
 *  (default: the languages this build ships text for).
 *  @{ */

/** One row of compiled-in text: a key and its text in one language. */
typedef struct ShText { const char *id; const char *text; } ShText;

/** Declare this module's text for ONE language; call once per
 *  language (call again later to add a language - code that already
 *  declares two needs no change). `rows` must stay alive for the life
 *  of the process: a static const array is what this is for. owner
 *  NULL or "" means the framework. The languages declared here are
 *  what ShLangBuiltin reports and what [Settings] Languages falls
 *  back to. Returns 1 when kept. */
SH_API int ShLangDeclare(const char *owner, const char *lang,
                         const ShText *rows, int n);
/** One of the languages this build ships text for, in declaration
 *  order. Fills the tab-separated pair "code<TAB>label" and returns
 *  1; 0 when i is out of range or buf is too small. */
SH_API int ShLangBuiltin(int i, char *buf, int size);
/** The label to show for a language code: its [LanguageNames] entry
 *  when a lang.ini carries one, else the code itself. Never NULL. */
SH_API const char *ShLangLabel(const char *code);

/** Translate one key for one owner (NULL or "" = framework text).
 *  Never NULL and never a failure: text that is missing everywhere
 *  comes back readable rather than empty. */
SH_API const char *ShLangText(const char *owner, const char *key);

/** 1 when a source can answer for this key: a lang.ini row or a
 *  baseline row, in the active language or in en-US. Ask this before
 *  showing text that must stay empty when there is none (a hint line,
 *  say): ShLangText always returns something, which is right for a
 *  label and wrong for a line that takes room. */
SH_API int ShLangHas(const char *owner, const char *key);

/** Translate framework text: the same as ShLangText(NULL, text). */
SH_API const char *ShLang(const char *text);
/** Retained spellings for callers that still pass a scope. The scope
 *  is ignored - a key says what text a row gets, not a menu path. */
SH_API const char *ShLangFor(const char *scope, const char *text);
SH_API const char *ShLangForOwned(const char *owner,
                                  const char *scope,
                                  const char *text);
/** The framework's own language-code comparison, for callers that keep
 *  a code of their own: standard BCP-47 tags, case-insensitive. */
SH_API int ShLangMatch(const char *a, const char *b);
/** The active language, from [Settings] Language. */
SH_API const char *ShLangGet(void);
/** Internal, for the loader: one line saying where this session's menu
 *  language came from, on the run that picked it because the settings file
 *  had no [Settings] Language= row. Returns 0 - and empties buf - on every
 *  other run. */
SH_API int ShLangPickLine(char *buf, int size);
/** Switch the active language now: what was read for the old language is
 *  dropped and the next lookup reads the files again, so the menu - which
 *  translates as it captures - is in the new language on the next frame.
 *  Text that was already written into a line (a status line, a toast) is
 *  put away instead of left in the old language; a page that keeps its own
 *  line current fills it again on the next tick. This does not write
 *  scripthook.ini: the caller decides whether to persist the choice. */
SH_API int ShLangSet(const char *code);

/** @} */

#ifdef __cplusplus
}
#endif

#endif
