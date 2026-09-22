<#
.SYNOPSIS
Build GRW ScriptHook with MSVC.

Output mirrors the Makefile: dinput8.dll next to GRW.exe, the
import library libscripthook.lib in the source folder, and every
plugin in plugins\<name>\<name>.asi.

Usage:
  pwsh ./build_msvc.ps1
  pwsh ./build_msvc.ps1 -Gamedir "D:\Games\GRW"
  pwsh ./build_msvc.ps1 -Clean
  pwsh ./build_msvc.ps1 -Beta        # public beta: only the shipped plugins
  pwsh ./build_msvc.ps1 -Release     # -Beta plus diagnostics compiled out
#>
[CmdletBinding()]
param(
    # Folder containing GRW.exe. Auto-detected when omitted:
    # the game folder is looked up as a sibling of this
    # repo's parent, falling back to two up from the script.
    [string]$Gamedir = '',

    # When set, the currently deployed mod files (dinput8.dll,
    # scripthook.ini and plugins\) are copied to
    # $BackupDir\<yyMMdd_HHmmss>\ before anything is overwritten,
    # one unique folder per build.
    [string]$BackupDir = '',

    # Remove the built dinput8.dll, plugins output and the
    # import library instead of building.
    [switch]$Clean,

    # Dear ImGui source folder (imgui.h / imgui.cpp / backends\),
    # compiled into dinput8.dll for the menu overlay. Vendored under
    # third_party/imgui; -Imgui overrides for a newer checkout.
    [string]$Imgui = (Join-Path $PSScriptRoot 'third_party\imgui'),

    # Public beta: build and deploy only the plugins that ship in it, and
    # move every other plugin folder out of the game's plugins\ into
    # plugins_off\<stamp>\ - moved, never deleted, so a plain build puts
    # them back. Their source and their build entries stay in the tree.
    [switch]$Beta,

    # Same set as -Beta, and in addition the framework's own module-level
    # diagnostics are compiled out (SH_RELEASE): only logs\scripthook.log
    # and the crash report are still written.
    [switch]$Release
)

$ErrorActionPreference = 'Stop'

$root   = $PSScriptRoot
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat'

# The plugin set the first public beta ships. Everything else is built by a
# plain run, and left out of a -Beta / -Release one.
#
# Four left the tree entirely on 2026-09-20, at the request of the author whose
# plugins they re-implemented: OpticalCamo, TimeWeatherControl, ammo_capacity
# and NPCSpawner. Nothing of his is built, shipped or kept here any more. One
# plugin is out of the shipped set and still in the tree, for a reason of its
# own:
#
#   AmmoProbe         read-only evidence: it answered the question it was for
#                     (docs\ammocapacity-reverse.md, section 9), so a plain
#                     run builds it and a -Beta does not - and a -Beta also
#                     moves its folder out of plugins\ like any other plugin
#                     that is not in the set below.
#
# AmmoControl is IN the set: the capacity multiplier and the auto reload are
# part of the release, and leaving it out of this list would quietly drop the
# plugin from every beta build.
#
# AllLanguages is IN the set: the pair-shipped plugin set should be able to
# give a RU/CN player the language list back.
$betaSet = @(
    'skipintro', 'spawner', 'firstperson', 'fov_changer', 'cnchat', 'micfix',
    'TimeWeatherControl', 'AmmoControl', 'AllLanguages'
)
$script:BetaOnly  = if ($Beta -or $Release) { $betaSet } else { $null }
$releaseBuild     = [bool]$Release

if (-not $Gamedir) {
    $candidate = Join-Path (Join-Path $root '..\..') "Tom Clancy's Ghost Recon Wildlands"
    if (Test-Path (Join-Path $candidate 'GRW.exe')) {
        $Gamedir = $candidate
    } else {
        $Gamedir = Join-Path $root '..\..'
    }
}
Write-Host "GAMEDIR: $Gamedir"

if ($Clean) {
    Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $Gamedir 'dinput8.dll')
    Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $root 'libscripthook.lib')
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $Gamedir 'plugins')
    Write-Host "cleaned $Gamedir"
    return
}

# Back up the currently deployed mod files before they are
# replaced, keeping the same relative layout as the game
# folder. The folder name is stamped to the second, so every
# build gets its own unique backup directory and older ones
# sort naturally by time.
if ($BackupDir) {
    $stamp = Get-Date -Format 'yyMMdd_HHmmss'
    $dst = Join-Path $BackupDir $stamp
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    foreach ($rel in @('dinput8.dll', 'scripthook.ini')) {
        $src = Join-Path $Gamedir $rel
        if (Test-Path $src) { Copy-Item $src $dst -Force }
    }
    $srcPlugins = Join-Path $Gamedir 'plugins'
    if (Test-Path $srcPlugins) {
        Copy-Item $srcPlugins (Join-Path $dst 'plugins') -Recurse -Force
    }
    Write-Host "backed up old mod files to $dst"
}

if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found: $vcvars"
}

if (-not (Test-Path (Join-Path $Imgui 'imgui.h'))) {
    throw "imgui.h not found under '$Imgui'"
}

# Load the MSVC x64 build environment into this process.
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}

$out     = Join-Path $Gamedir 'dinput8.dll'
# Two folders named plugins, kept apart on purpose: the game's is the
# build output, and the repo's holds one source folder per plugin.
$outPlugins = Join-Path $Gamedir 'plugins'
$srcPlugins = Join-Path $root 'plugins'
$tmp     = Join-Path $env:TEMP 'grw_msvc_build'
New-Item -ItemType Directory -Force -Path $tmp, $outPlugins | Out-Null

# Shared flags for every compile unit.
## /MT, not the /MD default: this is a proxy DLL injected into somebody
## else's process on somebody else's machine, and /MD makes every artifact
## depend on the Visual C++ runtime DLLs being installed there. They are not
## part of Windows - the universal CRT is, MSVCP140 and VCRUNTIME140 are not
## - and a machine without them fails at LoadLibrary, which looks like "the
## mod does nothing at all" with no log, because the DLL never ran. Static
## linking removes the question; the cost is size.
$c = @(
    '/nologo', '/O2', '/W3', '/LD', '/MT', '/std:c17', '/utf-8',
    "/I$root",
    '/D_CRT_SECURE_NO_WARNINGS',
    "/Fo$tmp\"
)
if ($releaseBuild) { $c += '/DSH_RELEASE=1' }

# The framework DLL and the import library plugins link.
function Invoke-FrameworkBuild {
    param(
        [string[]]$Sources,
        [string[]]$LinkArgs
    )
    & cl @c $Sources $LinkArgs
    if ($LASTEXITCODE -ne 0) { throw 'cl failed for dinput8.dll' }
}

function Build-Plugin {
    param([string]$Name, [string]$Source, [string[]]$LinkArgs,
          [string[]]$ExtraSources)
    if ($script:BetaOnly -and ($script:BetaOnly -notcontains $Name)) {
        Write-Host "skipped (not in the -Beta set): $Name"
        return
    }
    $dir = Join-Path $outPlugins $Name
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $dll = Join-Path $dir "$Name.asi"
    # The source of a plugin named $Name is plugins\<Name>\<Source>,
    # next to its config and its text file, and it deploys as
    # plugins\<Name>\<Name>.asi. MinHook sources stay where they are.
    $src = @((Join-Path $srcPlugins "$Name\$Source"))
    if ($ExtraSources) { $src += $ExtraSources }
    & cl @c $src "/Fe:$dll" /link $LinkArgs
    if ($LASTEXITCODE -ne 0) { throw "cl failed for $Name" }
    Write-Host "built $dll"
}

# ---- guard pad: MSVC has no inline assembler on x64, so the
# pad is emitted by ml64 and linked into the framework DLL.
# The name differs from cl's own guard.obj (the C side of
# guard.c) so the two do not collide.
& ml64 /nologo /c "/Fo$tmp\guard_pad.obj" (Join-Path $root 'guard.asm')
if ($LASTEXITCODE -ne 0) { throw 'ml64 failed for guard.asm' }

$fwSources = @(
    'loader.c', 'scripthook_api.c', 'scripthook_config.c',
    'scripthook_text.c', 'scripthook_tick.c',
    'scripthook_physics.c', 'scripthook_health.c',
    'scripthook_state.c', 'scripthook_playmode.c',
    'scripthook_blacklist.c', 'scripthook_entity.c',
    'scripthook_spawn.c', 'scripthook_npc.c',
    'scripthook_domino.c', 'scripthook_hit.c',
    'scripthook_accuracy.c',
    'scripthook_camera.c', 'scripthook_head.c',
    'scripthook_fov.c', 'scripthook_blur.c',
    'scripthook_fpx.c',
    'scripthook_stat.c', 'scripthook_resource.c',
    'scripthook_stealth.c',
    'scripthook_ammocap.c',
    'scripthook_weather.c', 'scripthook_crash.c',
    'scripthook_input.c', 'scripthook_havok.c',
    'scripthook_reflect.c', 'scripthook_ui.c',
    'scripthook_scene.c', 'scripthook_uiprop.c',
    'scripthook_uiinput.c', 'scripthook_dinput.c',
    'scripthook_hud.c', 'scripthook_menu.c',
    'scripthook_draw.c', 'guard.c',
    'scripthook_corefix.c', 'scripthook_modsettings.c',
    'forge.c', 'scripthook_forge.c', 'scripthook_forge_io.c',
    'scripthook_forgeprobe.c', 'scripthook_files.c',
    'third_party/minhook/src/buffer.c',
    'third_party/minhook/src/hook.c',
    'third_party/minhook/src/trampoline.c',
    'third_party/minhook/src/hde/hde64.c'
) | ForEach-Object { Join-Path $root $_ }

# ---- menu overlay: Dear ImGui + the D3D11 overlay (C++) ----
$cpp = @(
    '/nologo', '/O2', '/W3', '/c', '/MT', '/std:c++17', '/utf-8',
    '/D_CRT_SECURE_NO_WARNINGS', '/DSH_BUILD=1',
    "/I$root", "/I$Imgui", "/I$Imgui\backends",
    "/Fo$tmp\"
)
if ($releaseBuild) { $cpp += '/DSH_RELEASE=1' }
$cppSources = @(
    (Join-Path $Imgui 'imgui.cpp'),
    (Join-Path $Imgui 'imgui_draw.cpp'),
    (Join-Path $Imgui 'imgui_tables.cpp'),
    (Join-Path $Imgui 'imgui_widgets.cpp'),
    (Join-Path $Imgui 'backends\imgui_impl_dx11.cpp'),
    (Join-Path $Imgui 'backends\imgui_impl_win32.cpp'),
    (Join-Path $root 'scripthook_ovl.cpp')
)
& cl @cpp $cppSources
if ($LASTEXITCODE -ne 0) { throw 'cl failed for the imgui overlay sources' }
$cppObjs = $cppSources | ForEach-Object {
    Join-Path $tmp ("{0}.obj" -f [IO.Path]::GetFileNameWithoutExtension($_))
}

$fwLink = @("$tmp\guard_pad.obj") + $cppObjs + @(
    "/Fe:$out",
    '/link',
    "/DEF:$root\proxy.def",
    "/MAP:$tmp\framework.map",
    "/IMPLIB:$root\libscripthook.lib",
    'dinput8.lib', 'dxguid.lib', 'gdi32.lib', 'user32.lib',
    'd3d11.lib', 'dxgi.lib', 'dwmapi.lib'
)
Invoke-FrameworkBuild -Sources $fwSources -LinkArgs $fwLink
Write-Host "built $out"

# ---- plugins
$libPath = "/LIBPATH:$root"

Build-Plugin 'ui_sample'    'ui_sample.c'    @($libPath, 'libscripthook.lib', 'user32.lib')
# The mode blacklist worked example: one declaration, one callback that
# stops its own work, one query in its tick, one HUD line. It ships on
# because it is what docs/plugin-blacklist.md points at, and it declares
# itself blocked in Ghost Mode - the single player one - so it can be tried
# without a second player. Late binds, so it needs no import library.
Build-Plugin 'blacklist_sample' 'blacklist_sample.c' @()
# The file interception worked example: one watcher, one hide rule and the
# query calls, so docs/file-interception.md points at something runnable.
Build-Plugin 'file_watch_sample' 'file_watch_sample.c' @($libPath, 'libscripthook.lib')
# The drawing-API worked example: a drawer window, a frameless corner
# readout and the input box, so docs/ui-drawing.md points at something
# runnable.
Build-Plugin 'draw_sample'  'draw_sample.c'  @($libPath, 'libscripthook.lib', 'user32.lib')
# In-game Chinese text input: the input-box primitive's real user, and
# the worked example of "a plugin owns the buffer, the framework owns the
# IME". It ships off by default (its own ini: enabled=0); see
# docs/ui-drawing.md section 5 for the split of work.
Build-Plugin 'cnchat'       'cnchat.c'       @($libPath, 'libscripthook.lib', 'user32.lib')
Build-Plugin 'hitfling'     'hitfling.c'     @('gdi32.lib', 'user32.lib')
Build-Plugin 'freecam'      'freecam.c'      @('gdi32.lib', 'user32.lib')
Build-Plugin 'firstperson'  'firstperson.c'  @('gdi32.lib', 'user32.lib')
# Reinforcement prototype is parked outside the tree until its
# combat/lock logic is verified; see reinf_boost/ next to the repo.
# Enabling it means putting reinf_boost.c in plugins\Reinforcement\.
#Build-Plugin 'Reinforcement' 'reinf_boost.c' @($libPath, 'libscripthook.lib', 'gdi32.lib', 'user32.lib')
Build-Plugin 'chaos'        'chaos.c'        @($libPath, 'libscripthook.lib', 'gdi32.lib', 'user32.lib', 'winmm.lib')
Build-Plugin 'fov_changer'  'fov_changer.c'  @($libPath, 'libscripthook.lib', 'gdi32.lib', 'user32.lib')
Build-Plugin 'skipintro'     'skipintro.c'    @($libPath, 'libscripthook.lib')
# AllLanguages hooks the function's own entry point rather than the
# game's lookup of it, so the hook does not depend on being installed
# before the game asks. That needs MinHook, which the framework already
# carries; the plugin target picks up its four sources here.
Build-Plugin -Name 'AllLanguages' -Source 'AllLanguages.c' -LinkArgs @() -ExtraSources @(
    (Join-Path $root 'third_party/minhook/src/buffer.c'),
    (Join-Path $root 'third_party/minhook/src/hook.c'),
    (Join-Path $root 'third_party/minhook/src/trampoline.c'),
    (Join-Path $root 'third_party/minhook/src/hde/hde64.c')
)

# GhostWipeProbe is the second round of evidence, for the question
# GhostNoWipe cannot answer on its own: why the slot stays hidden for the
# rest of the session even when nothing of the wipe is left on disk. It
# records the APIs the save list could be built from - reads included -
# and stamps each line with the engine state.
#
# Not built by default: it and GhostNoWipe both hook MoveFileExW, and
# MinHook keeps its state per DLL, so the two plugins tread on each
# other. Run it with the other one switched off.
Build-Plugin -Name 'GhostWipeProbe' -Source 'GhostWipeProbe.c' -LinkArgs @($libPath, 'libscripthook.lib')

# GhostNoWipe keeps a Ghost Mode save when a death ends the run: the
# rename the game performs is turned into a copy. MinHook as well.
Build-Plugin -Name 'GhostNoWipe' -Source 'GhostNoWipe.c' -LinkArgs @($libPath, 'libscripthook.lib')

# GhostRevive asked whether a Ghost Mode death could be sent down the
# reviving path instead of the run-ending one. It cannot: the branch is
# decided by whether the squad is aboard, inside a flow no export reaches
# - ShTriggerGameOver is ignored and refilling the health changes nothing.
# That is answered, and the long form is in
# .codebuddy/plans/ghost-revive-findings.md and the source's own header.
#
# Built anyway, because it is the probe that answered the question and a
# probe is only useful ready to run - but it ships switched off in its own
# ini, so it registers nothing until asked.
Build-Plugin -Name 'GhostRevive' -Source 'GhostRevive.c' -LinkArgs @()

# ModeExitProbe answered its question - the mode-switch exit is the
# engine's design, not a defect; see its header - so it is no longer
# deployed. The source stays for the next question of this kind.
#Build-Plugin -Name 'ModeExitProbe' -Source 'ModeExitProbe.c' -LinkArgs @() -ExtraSources @(
#    (Join-Path $root 'third_party/minhook/src/buffer.c'),
#    (Join-Path $root 'third_party/minhook/src/hook.c'),
#    (Join-Path $root 'third_party/minhook/src/trampoline.c'),
#    (Join-Path $root 'third_party/minhook/src/hde/hde64.c')
#)
Build-Plugin 'spawner'      'spawner.c'      @('gdi32.lib', 'user32.lib')
# TimeWeatherControl is time and weather from the menu: every change goes
# through the framework's own engine calls (ShSetTime, ShSetTimeSpeed,
# ShSetWeatherBlend, ShReleaseWeather), so it installs no hook, patches no code
# and writes no engine memory of its own. The clock rate is per window -
# dawn 05-07, day 07-18, dusk 18-20, night 20-05 - with the time of day sent
# only when the player asks for it. Off by default, and turning the switch off
# hands the weather and the clock rate back to the engine.
Build-Plugin 'TimeWeatherControl' 'TimeWeatherControl.c' @($libPath, 'libscripthook.lib')
# AmmoProbe is a read only evidence tool, and the answer it is after is "where
# does the game keep the rounds left in the magazine". Where that is, is known
# (the inventory object's vtable and owner handle, from the module that used to
# read ammo); what is not known is a cheap way to reach the object, since that
# module's finder swept every read/write page of the game (~23 s cold). So the
# probe tries the objects at hand - the player root and entity, their
# components, and the fire path's own projectile and shooter - and only sweeps
# when that comes back empty and the player asks for it. Not in the beta set:
# it ships to nobody.
Build-Plugin 'AmmoProbe'    'AmmoProbe.c'    @($libPath, 'libscripthook.lib')
# AmmoControl is the capacity multiplier and an automatic reload. Every change
# goes through the framework's own engine calls (ShSetAmmoScale for the
# capacity, ShFakeKey for the reload), so it installs no hook of its own and
# writes no engine memory. The rounds come from ShGetAmmoRounds, which reads
# the weapon the engine hands its capacity function - so the capacity hook has
# to be installed for auto reload to have anything to read, and switching it on
# is what installs it. Off by default: 1.00x installs nothing at all.
Build-Plugin 'AmmoControl'  'AmmoControl.c'  @($libPath, 'libscripthook.lib')
# EnemyReinforce sends reinforcements while a fight is on and hardens
# the enemies it can prove are fighting. It late-binds as well, and
# keeps its defaults in EnemyReinforce.ini and its text in lang.ini,
# both beside the source and both seeded next to the .asi below.
Build-Plugin 'EnemyReinforce' 'EnemyReinforce.c' @()
# ModeProbe is a read only evidence tool: it samples every candidate
# the framework can reach (GameFlow objects, the shell, the scene set,
# the PVP entity names) into logs\ModeProbe.log so the play mode can
# be pinned to a field. It late-binds and hooks nothing.
Build-Plugin 'ModeProbe'    'ModeProbe.c'    @('user32.lib')
# ModeCallProbe dumps the method tables of the front end objects (the
# GameFlow machine, its sub objects, the HybridMenu) and names as many
# entries as a crc32 dictionary resolves, so the function behind the
# mode selection can be found by name and hooked. Read only.
Build-Plugin -Name 'ModeCallProbe' -Source 'ModeCallProbe.c' `
    -LinkArgs @('user32.lib') -ExtraSources @(
    (Join-Path $root 'third_party/minhook/src/buffer.c'),
    (Join-Path $root 'third_party/minhook/src/hook.c'),
    (Join-Path $root 'third_party/minhook/src/trampoline.c'),
    (Join-Path $root 'third_party/minhook/src/hde/hde64.c')
)
Build-Plugin 'CrazyCars'    'crazycars.c'    @('gdi32.lib', 'user32.lib')
Build-Plugin 'tpgun'        'tpgun.c'        @('gdi32.lib', 'user32.lib')
Build-Plugin 'tp_roulette'  'tp_roulette.c'  @($libPath, 'libscripthook.lib', 'gdi32.lib', 'user32.lib')
Build-Plugin 'test_plugin'  'test_plugin.c'  @('ws2_32.lib', 'gdi32.lib', 'user32.lib')
# micfix hands the game ASCII names for recording devices whose own name is
# not ASCII - the in-process form of the community fix for "the game finds no
# microphone on a Chinese Windows" - and adds the device picker the game has
# none of. It hooks ole32!CoCreateInstance with MinHook and patches the vtable
# slots of the DirectShow objects that call returns; see the source header.
# It links the import library for the menu/text/paths, oleaut32 for the BSTR
# it writes back into a VARIANT, and ole32 for CoInitializeEx/CoUninitialize -
# its scan runs on a plugin thread and has to initialize COM there itself.
# CoCreateInstance is still resolved by name at run time rather than linked,
# so the plugin's own scan can reach the original with the hook installed.
Build-Plugin -Name 'micfix' -Source 'micfix.c' `
    -LinkArgs @($libPath, 'libscripthook.lib', 'ole32.lib', 'oleaut32.lib') `
    -ExtraSources @(
    (Join-Path $root 'third_party/minhook/src/buffer.c'),
    (Join-Path $root 'third_party/minhook/src/hook.c'),
    (Join-Path $root 'third_party/minhook/src/trampoline.c'),
    (Join-Path $root 'third_party/minhook/src/hde/hde64.c')
)

# Ballistics and CameraPresets came in with PR #2 (R4333).  They were wired into
# the MinGW Makefile only, and the MSVC build is the one that ships here: without
# these two lines the plugin folders would be deployed with an ini seeded beside
# them and no .asi to load, which reads as "the plugin is broken" rather than
# "the plugin was never built".
Build-Plugin 'Ballistics'    'Ballistics.c'    @($libPath, 'libscripthook.lib')
Build-Plugin 'CameraPresets' 'CameraPresets.c' @($libPath, 'libscripthook.lib')

# A plugin's own files are seeded next to its .asi the first time only:
# a later build must never overwrite settings changed in game, and never
# a lang.ini that was edited in place. Both sit in the plugin's source
# folder under the names they carry once deployed, so the tree and the
# game folder line up file for file.
foreach ($dir in (Get-ChildItem $srcPlugins -Directory)) {
    $name = $dir.Name
    if ($script:BetaOnly -and ($script:BetaOnly -notcontains $name)) { continue }
    $dst  = Join-Path $outPlugins $name
    foreach ($file in @("$name.ini", 'lang.ini')) {
        $from = Join-Path $dir.FullName $file
        $to   = Join-Path $dst $file
        if ((Test-Path $from) -and -not (Test-Path $to)) {
            New-Item -ItemType Directory -Force -Path $dst | Out-Null
            Copy-Item $from $to -Force
            Write-Host "seeded $to"
        }
    }
}

# <gamedir>\lang.ini is NOT seeded, and an existing one is never touched.
#
# It is the player's own file: the place to change a line of the built-in
# text, to add a language, or to translate a plugin that ships no source.
# Everything a fresh install shows is compiled in - the framework's tables
# in scripthook_text.c and each plugin's own - so the menu reads in English
# and Chinese with no file there at all, and one created by the build would
# only be a file to explain away. lang.example.ini in the repository root is
# the reference to copy out by hand if a player wants one.

# cl generates a .lib/.exp beside any plugin that exports
# symbols (chaos exports ChaosCount & friends). They are not
# loaded by the game, so keep the plugins tree clean.
# Each path is removed explicitly: piping these objects straight into
# Remove-Item fails to bind under PowerShell 7 ("the input object cannot
# be bound to any parameters"), which left the files in place and made a
# successful build report a failure at its last step.
# -Beta / -Release: the game's plugins\ has to match what is released, so
# every plugin folder outside the set is moved aside - never deleted - into
# <gamedir>\plugins_off\<stamp>\. A plain build puts them back in place.
$offDir = $null
if ($script:BetaOnly) {
    $offDir = Join-Path $Gamedir ('plugins_off\' + (Get-Date -Format 'yyMMdd_HHmmss'))
    foreach ($dir in (Get-ChildItem $outPlugins -Directory)) {
        if ($script:BetaOnly -contains $dir.Name) { continue }
        New-Item -ItemType Directory -Force -Path $offDir | Out-Null
        Move-Item -LiteralPath $dir.FullName -Destination $offDir -Force
        Write-Host "moved out of plugins\: $($dir.Name) -> $offDir"
    }
    Write-Host "beta plugin set deployed: $($script:BetaOnly -join ', ')"
}

$cleanDirs = @($outPlugins)
if ($offDir -and (Test-Path $offDir)) { $cleanDirs += $offDir }
foreach ($d in $cleanDirs) {
    Get-ChildItem $d -Recurse -Include *.lib, *.exp -ErrorAction SilentlyContinue |
        ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }
}

Write-Host 'build complete'
