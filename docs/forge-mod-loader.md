# Forge Mod Loader

Load loose files from `mods\` over entries that already exist in the game's
`.forge` archives, **without unpacking, without repacking, and without
touching a single byte of the shipped archives**.

The idea is the one FusionFix ModLoader and GRB Tweaks use: the game is not
given a modified file, it is *redirected*. Here that redirect happens at the
last possible moment - when the engine reads the archive - so the archive on
disk stays exactly as shipped and nothing is written anywhere.

---

## Requirements and limits, read this first

- **A replacement must fit the room the entry already has.** In this
  container the payloads are stored back to back with no gaps, so an
  entry's room is its own length. A replacement that is the same size or
  smaller works; a larger one would need the entry moved, which this build
  does not do, and is refused with a line in the log.
- **Entries are replaced, not added or deleted.** There is no `grow` and no
  new entry. A `.delete` file is recognised and skipped.
- **The whole entry payload is the unit.** A mod file is a complete forge
  entry (what `wlcli entry` writes out), not a single resource inside one.
  Rebuilding the payload after changing a resource inside it is a job for
  the toolkit; this loader only swaps payloads.
- **Off by default.** Nothing is loaded until `[forgemod] enabled=1`.

---

## Layout

```
<gamedir>/
├── GRW.exe
├── scripthook.ini
├── mods/
│   ├── DataPC_patch_01/                     <- archive name, flat, wins
│   │   └── 1234_-_GR_PLAYER_Template.data
│   └── My Vest/                             <- mod folder
│       └── DataPC_GRN_WorldMap/
│           └── 55_-_Some_Cosmetic.data
└── logs/
```

- **`mods\<archive name>\<file>`** - flat. The folder is named after an
  archive without its extension (`DataPC_patch_01`, `DataPC_GRN_WorldMap`,
  `DataPC_20_dlc`, ...). This layout has the highest priority.
- **`mods\<mod name>\<archive name>\<file>`** - one folder per mod. Ties are
  broken by the mod folder name, earlier first, with digit runs compared as
  numbers (`2 mod` sorts before `10 mod`).
- A folder whose name starts with **`~`** is switched off and skipped.
- A file whose name ends with **`.delete`** is recognised and skipped
  (deletion is not implemented yet; the line in the log says so).
- Files may sit one or more folders deeper under the archive folder; those
  levels are organisational, the file name is what counts.

### Naming the entry

Two forms are accepted for a mod file:

| File name | Resolved as |
|---|---|
| `1234_-_GR_PLAYER_Template.data` | entry index 1234, name checked against the file name |
| `GR_PLAYER_Template.data` | entry name |

Entry names are **not** unique inside an archive, so the index form is the
reliable one - it is also what the toolkit's own export produces. A file
whose index and name disagree is used by index, and the mismatch is logged.

---

## Making a mod

`wlcli` (built from the WildlandsToolkit sources) is the offline half:

```powershell
# what is in an archive
wlcli list "F:\...\DataPC_GRN_TitleScreen.forge" 20

# write one entry out - this is the shape a mod file has
wlcli entry "F:\...\DataPC_GRN_TitleScreen.forge" 9 out\9_-_DBG_Override_3DGrid_DiffuseMap.data

# ... change that entry with the rest of the toolkit's commands ...

# check it still fits before shipping it
powershell -c "(Get-Item out\9_-_....data).Length"
```

Then drop it under `mods\<archive name>\` and restart the game.

---

## Which archive to patch

The same resource is often present in several archives - `W_ASR_AK47_body_LOD0`
exists in `DataPC.forge`, `DataPC_patch_01.forge` and two DLC archives - and
changing one copy can simply be masked by another. The loader therefore looks
up every FileDataID a mod targets across **all** installed archives and names
the other copies in the log:

```
copies: mods\DataPC\1234_-_X.data also lives in DataPC_patch_01, DataPC_20_dlc
        - change those too, or the game may keep loading them
```

`apply_all_copies=1` makes it do that automatically, per archive, still
refusing any copy where the replacement does not fit.

Archive roles, for choosing where a change belongs:

| Archive | Contents |
|---|---|
| `DataPC` | base game assets |
| `DataPC_patch_01` | patches and updates for the base game - **wins over `DataPC`** |
| `DataPC_extra`, `DataPC_extra_patch_01` | DLC and major updates (meshes, mission parameters) |
| `DataPC_GRN_GhostRoom(_patch_01)` | in-game menus: gunsmith, lobby |
| `DataPC_GRN_TitleScreen(_patch_01)` | title screen |
| `DataPC_GRN_WorldMap(_patch_01)` | world map, 2D and 3D - **most cosmetic 3D models live here** |
| `dlc_N\DataPC_N_dlc`, `dlc_N\DataPC_GRN_WorldMap_N_dlc` | DLC archives |

Resource name prefixes worth knowing: `W` weapon parts, `CP`/`FCP` male/female
character parts, `UI` interface, `VEG` vegetation, `ENV` environment, `CIV`
civilian, `UNP` named-NPC parts.

---

## Configuration (`scripthook.ini`)

```ini
[forgemod]
enabled=0          ; 1 = load mods\ (off by default)
dry_run=0          ; 1 = resolve and log only, serve nothing
strict=1           ; 1 = refuse a replacement that does not fit
report_copies=1    ; 1 = name the other archives a resource also lives in
apply_all_copies=0 ; 1 = override those copies as well
probe=0            ; 1 = install the evidence probe (diagnostics)
log_reads=0        ; 1 = log every read of a modded archive (diagnostics)
```

The in-game page (**F4 → Forge Mod Loader**) has the two switches, a status
line and a hint; changes need a restart, like the rest of the loader's keys.

---

## Logs

| File | What is in it |
|---|---|
| `logs\scripthook_forge.log` | config, archives found, every mod resolved or rejected, conflicts, the cross-archive copy report |
| `logs\scripthook_forge_io.log` | the hooks, each archive served and how many entries it patches, and a line per patched read |

---

## How it works, and why it looks like this

Measured on a retail install (2026-09-12) with the probe, before any of this
was relied on:

- **Archives are read, never mapped.** 47 `.forge` opens and 646 reads in one
  session, and not one `CreateFileMappingA/W` or `MapViewOfFile` on a
  `.forge`. A handful of `UnmapViewOfFile` calls belong to other files.
- **Each archive is opened twice, and the pair behaves differently:**
  - `share=0x7 flags=0x10000000` (`FILE_FLAG_RANDOM_ACCESS`) - buffered, read
    synchronously after a `SetFilePointer`.
  - `share=0x1 flags=0x60000000` (`NO_BUFFERING | OVERLAPPED`) - read
    asynchronously. `ReadFile` returns `ERROR_IO_PENDING`, the file pointer
    never moves, and the offset exists only in the `OVERLAPPED`. Payload
    streaming goes through this one.
- **The hooks go in from the loader thread** about 3.3 seconds before the first
  archive is opened, so `DllMain` is not needed.
- `FindFirstFile` is used on `dlc_*` folders (`dlc_10\*.forge` and so on), so
  archives dropped into a `dlc_N` folder are discovered; the root archives come
  from a fixed table in the executable, which is why a new `DataPC_patch_02.forge`
  in the root is *not* picked up.

So the loader:

1. reads the archive's tables at the first read of that file (a few MB even
   for a 19.89 GB archive, because the tables sit below the first payload);
2. resolves every mod in `mods\` against them, by index or by name;
3. keeps the game's own handle on the vanilla file and answers reads from it,
   overwriting only the byte ranges a mod replaces.

What is hooked, and nothing else:

| Hook | Why |
|---|---|
| `ReadFile` | patches the caller's buffer for the ranges a mod replaces, and records asynchronous reads |
| `GetOverlappedResult` / `Ex` | the completion point for asynchronous reads |
| `WaitForSingleObject(Ex)` / `WaitForMultipleObjects(Ex)` | the completion point this engine actually uses - see below |
| `CloseHandle` | so a later file reusing the same handle value cannot inherit an archive's patches |

Which handle belongs to which archive is not learned from `CreateFile`:
`CreateFileW` would collide with GhostNoWipe and skipintro, which both want
it, and MinHook keeps one hook per target. A handle is resolved **late**, the
first time it is read, with `GetFinalPathNameByHandleW`, and the answer is
cached - so an archive that is never read costs nothing.

### Two things that had to be got right

**Asynchronous reads cannot be patched when `ReadFile` returns.** The data is
not in the buffer yet. The loader records the request and patches it once the
transfer has finished - and "finished" is deliberately not tied to one API:
after any wait returns, and before every read, the outstanding requests are
swept and those whose `OVERLAPPED::Internal` is no longer `STATUS_PENDING` are
patched from `InternalHigh`. Waiting inside `ReadFile` instead would consume
the signal on the engine's own event, and the engine waits on exactly that -
the first attempt at this stalled the patch, which is how the wait hook was
arrived at.

**Nothing is written blind.** Every patch carries a copy of the archive's own
bytes for that range, read when the overlay is built. Before the patch is
written the buffer is compared against it; if it does not match, the read did
not land where it was believed to and the patch is refused rather than written
into live data. Over a whole session of testing the count was zero, which is
what makes the offset arithmetic trustworthy.

Reads that cover no patch are passed through completely untouched, so ordinary
loading and its asynchronous shape are unchanged.

---

## Status

Verified end to end on a retail install, with the game running normally:

- three entries in `DataPC.forge` taken over (one from a flat folder, one from
  a mod folder, one folder disabled with `~`), one of them 139 KB;
- a synchronous read and an asynchronous read both patched
  (`read/sync ... (fixup 1)`, `read/async-done ... (fixup 2)`);
- zero refusals from the safety check.

Test payloads were the entries' own bytes, so game content was unchanged by
design - what was verified is the mechanism. A real texture or data edit is
the next step, and that is a toolkit job (change the resource, rebuild the
entry, drop it in `mods\`).

Known gaps: additions and deletions (`.delete` is recognised but does not act);
any replacement larger than its entry; the root-directory "new archive" route,
which the executable's fixed table appears not to support.
