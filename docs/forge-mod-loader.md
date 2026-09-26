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

- **A replacement larger than the room the entry already has takes the room of
  the entries that follow it**, with `strict=0`. Payloads in this container are
  stored back to back with no gaps, so an entry's room is its own length; a
  larger payload therefore covers the payloads after it, and their own bytes
  are spliced back into the payload's patch at the offsets they came from. Every
  offset the engine asks for is still an offset the file really holds, nothing
  is invented past the end of the archive, and a read that spans the boundary
  (the engine streams, it does not read one entry at a time) gets the right
  bytes everywhere. With `strict=1` - the default - such a replacement is still
  refused with a line in the log. It may not run past the end of the archive, it
  may not cover an entry that a mod of its own is replacing, and it may not
  cover more than 32 entries.
- **Entries are replaced, not added or deleted.** There is no `grow` and no
  new entry. A `.delete` file is recognised and skipped.
- **The whole entry payload is the unit.** A mod file is a complete forge
  entry (what `wlcli entry` writes out), not a single resource inside one.
  Rebuilding the payload after changing a resource inside it is a job for
  the toolkit; this loader only swaps payloads.
- **Off by default.** Nothing is loaded until `[forgemod] enabled=1`.
- **Nothing is served in the two PvP modes.** While Ghost War (4v4) or
  Mercenaries (the eight player PvPvE mode) is the selected mode the loader
  stands down: reads go back to the engine untouched, no overlay is built,
  and the log says which way it went (`mode gate: Ghost War - mods\ is not
  served`, `mode gate: campaign - mods\ is served again`). The declaration
  lives in the source (`FORGE_BLOCKED_MODES` in `scripthook_forge_io.c`), so
  no ini can widen or narrow it. The menu page is deliberately **not**
  hidden: a mod that stops working is announced by the log, not by a missing
  page. The front end, and any mode the framework has not read yet, block
  nobody.

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
| `+GR_PLAYER_Template.data` | an entry the archive never held: read, checked, then **refused** (see [Additions](#additions-the-engines-own-patch-archives)) |

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
strict=1           ; 1 = refuse a replacement that does not fit; 0 = it takes
                   ;     the room of the entries that follow it
report_copies=1    ; 1 = name the other archives a resource also lives in
apply_all_copies=0 ; 1 = override those copies as well
probe=0            ; 1 = install the evidence probe (diagnostics)
report_reads=0     ; 1 = report every read of a modded archive (diagnostics)
report_opens=0     ; 1 = report every .forge the engine asks for, refused ones too
```

The in-game page (**F4 → Forge Mod Loader**) has the two switches, a status
line and a hint; changes need a restart, like the rest of the loader's keys.

---

## Logs

| File | What is in it |
|---|---|
| `logs\scripthook_forge.log` | config, archives found, every mod resolved or rejected, conflicts, the cross-archive copy report |
| `logs\scripthook_forge_io.log` | the hooks, each archive served and how many entries it patches, and a line per patched read |

Both files are a framework module's logs, so they only exist at `[Settings]
LogLevel=info` or above - a release build defaults to `warn`, where the module
logs are not created at all. The `report_*` keys in `[forgemod]` say which
extra lines to write when the file is there; the level says whether it is
there. Setting one of those keys without raising the level looks like it did
nothing.

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

The larger-payload path was verified on 2026-09-26 against the same install:
a 175,094-byte replacement for `DataPC.forge` entry 25192 (101,046 bytes of
room) took the room of the 236-byte entry after it and the first 73,812 bytes
of the streamed mip after that, the game showed the new payload, the covered
entries stayed correct, and no patch was refused.

Known gaps: deletions (`.delete` is recognised but does not act). Additions are
not a gap in that sense - they are refused on purpose, because the two payloads
that make an entry real are not written here. The section below says why, and
what to use instead.

---

## Additions: the engine's own patch archives

This loader replaces bytes. It does not add an entry, and a file named
`+<entry name>.data` is read, checked, and then refused rather than served.
That refusal is the useful part of the feature, so it is worth writing down
what stands behind it.

**The row is the easy half, and it is already there.** An archive lays its
tables out for one row more than it holds. Measured on `DataPC.forge`: the meta
says 29,640 entries used with a limit of 29,641, the location table ends
exactly where the info table begins, and the info table ends exactly where the
trailing set begins - and `DataPC_extra_patch_01` is laid out the same way, to
the byte. So an addition appends one 20-byte location record, one 192-byte info
record copied from a container of the same class, and five small counts. No
pointer moves and no table is rebuilt, and both rows land exactly where an
offline reading of the tables says they will.

**A row is not an entry.** The engine also finds entries through the global
meta (`GlobalMetaFile`, entry 16) and the prefetch registry
(`PrefetchingFileInfos`, entry 145), and it reads both while it starts. With
the row appended and nothing else, the game went from the integrity warning it
shows for any mod straight to a dead process, with no archive read reaching the
I/O layer in between. In `DataPC.forge` those two payloads are 139,510 and
429,560 bytes (the registry is a compressed blob); in a patch archive they are
296 and 131 bytes, because a patch archive's copies describe only that archive.

**Which is why additions belong to the engine.** A mod of the game's own kind
ships the two companions beside its payloads - one of the reference patch
archives holds a single weapon file and then entries 16 and 145, and nothing
else - and the engine merges the file and updates both registries itself. The
toolkit builds exactly that, so the job needs no loader:

```
toolkit: Add -> install as a patch archive
  AddonArchiveService.SlotPath / NextSlot   -> <family>_patch_NN.forge
  GlobalMetaFile.CreatePatch(identity)      -> the companion record (ForgeArchive.cs)
  ForgeArchive.Rebuild(out, replacements, additions, removals)
```

Drop the result in the game directory and it is picked up; that is how the mods
of this kind in the wild are installed.

### The two mechanisms are layers, not rivals

They do not conflict. The engine merges archives by its own priority first
(base game < `*_patch_NN`), and the loader patches the bytes of whichever
archive is read, after that. So both apply, in a fixed order. Two consequences
are worth knowing:

- A replacement only lands if it names the archive that wins for that entry.
  `report_copies=1` names the other archives a resource also lives in, and
  `apply_all_copies=1` overrides those too. An entry only a patch archive
  provides is not in the base archive at all.
- A patch archive in the game directory is an archive like any other to this
  loader, so `mods\DataPC_patch_02\` replaces inside it - an added entry can be
  retextured the same way a base-game one can.

| The mod changes | How |
|---|---|
| textures, meshes, any bytes of an entry that exists | a loose file in `mods\<archive>\` |
| a new entry (a weapon, a body, anything the archive never held) | a `<family>_patch_NN.forge` from the toolkit, in the game directory |
| an added entry's own resources | `mods\<that patch archive's name>\` |

On names, honestly: `GRW.exe` mentions exactly one patch archive,
`DataPC_patch_01.forge` - no `_02`, `_03` or `_04` anywhere in 405 MB - and
with `report_opens=1` the engine was watched never asking for an archive that is
not there. How a newly added root archive is found is therefore not pinned
down; that it works when dropped in is the field evidence of the mods that ship
that way. `report_opens` is the way to watch it again if this is ever revisited.

