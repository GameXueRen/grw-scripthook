<#
.SYNOPSIS
Build GRW ScriptHook with MSVC.

Output mirrors the Makefile: dinput8.dll next to GRW.exe, the
import library libscripthook.lib in the source folder, and every
plugin in scripts\<name>\<name>.asi.

Usage:
  pwsh ./build_msvc.ps1
  pwsh ./build_msvc.ps1 -Gamedir "D:\Games\GRW"
  pwsh ./build_msvc.ps1 -Clean
#>
[CmdletBinding()]
param(
    # Folder containing GRW.exe. Auto-detected when omitted:
    # the game folder is looked up as a sibling of this
    # repo's parent, falling back to two up from the script.
    [string]$Gamedir = '',

    # When set, the currently deployed mod files (dinput8.dll,
    # scripthook.ini and scripts\) are copied to
    # $BackupDir\<yyMMdd_HHmmss>\ before anything is overwritten,
    # one unique folder per build.
    [string]$BackupDir = '',

    # Remove the built dinput8.dll, scripts output and the
    # import library instead of building.
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$root   = $PSScriptRoot
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat'

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
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $Gamedir 'scripts')
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
    $srcScripts = Join-Path $Gamedir 'scripts'
    if (Test-Path $srcScripts) {
        Copy-Item $srcScripts (Join-Path $dst 'scripts') -Recurse -Force
    }
    Write-Host "backed up old mod files to $dst"
}

if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found: $vcvars"
}

# Load the MSVC x64 build environment into this process.
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}

$out     = Join-Path $Gamedir 'dinput8.dll'
$scripts = Join-Path $Gamedir 'scripts'
$tmp     = Join-Path $env:TEMP 'grw_msvc_build'
New-Item -ItemType Directory -Force -Path $tmp, $scripts | Out-Null

# Shared flags for every compile unit.
$c = @(
    '/nologo', '/O2', '/W3', '/LD', '/std:c17',
    "/I$root",
    '/D_CRT_SECURE_NO_WARNINGS',
    "/Fo$tmp\"
)

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
    param([string]$Name, [string]$Source, [string[]]$LinkArgs)
    $dir = Join-Path $scripts $Name
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $dll = Join-Path $dir "$Name.asi"
    & cl @c (Join-Path $root $Source) "/Fe:$dll" /link $LinkArgs
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
    'scripthook_physics.c', 'scripthook_health.c',
    'scripthook_state.c', 'scripthook_entity.c',
    'scripthook_spawn.c', 'scripthook_npc.c',
    'scripthook_domino.c', 'scripthook_hit.c',
    'scripthook_camera.c', 'scripthook_head.c',
    'scripthook_fov.c', 'scripthook_blur.c',
    'scripthook_stat.c', 'scripthook_resource.c',
    'scripthook_stealth.c', 'scripthook_ammo.c',
    'scripthook_weather.c', 'scripthook_crash.c',
    'scripthook_input.c', 'scripthook_havok.c',
    'scripthook_reflect.c', 'scripthook_ui.c',
    'scripthook_scene.c', 'scripthook_uiprop.c',
    'scripthook_uiinput.c', 'scripthook_dinput.c',
    'scripthook_hud.c', 'scripthook_menu.c', 'guard.c'
) | ForEach-Object { Join-Path $root $_ }

$fwLink = @(
    "$tmp\guard_pad.obj",
    "/Fe:$out",
    '/link',
    "/DEF:$root\proxy.def",
    "/IMPLIB:$root\libscripthook.lib",
    'dinput8.lib', 'dxguid.lib', 'gdi32.lib', 'user32.lib'
)
Invoke-FrameworkBuild -Sources $fwSources -LinkArgs $fwLink
Write-Host "built $out"

# ---- plugins
$libPath = "/LIBPATH:$root"

Build-Plugin 'ui_sample'    'ui_sample.c'    @($libPath, 'libscripthook.lib', 'user32.lib')
Build-Plugin 'hitfling'     'hitfling.c'     @('gdi32.lib', 'user32.lib')
Build-Plugin 'freecam'      'freecam.c'      @('gdi32.lib', 'user32.lib')
Build-Plugin 'firstperson'  'firstperson.c'  @('gdi32.lib', 'user32.lib')
Build-Plugin 'chaos'        'chaos.c'        @($libPath, 'libscripthook.lib', 'gdi32.lib', 'user32.lib', 'winmm.lib')
Build-Plugin 'fov_changer'  'fov_changer.c'  @($libPath, 'libscripthook.lib', 'gdi32.lib', 'user32.lib')
Build-Plugin 'spawner'      'spawner.c'      @('gdi32.lib', 'user32.lib')
Build-Plugin 'CrazyCars'    'crazycars.c'    @('gdi32.lib', 'user32.lib')
Build-Plugin 'tpgun'        'tpgun.c'        @('gdi32.lib', 'user32.lib')
Build-Plugin 'tp_roulette'  'tp_roulette.c'  @($libPath, 'libscripthook.lib', 'gdi32.lib', 'user32.lib')
Build-Plugin 'test_plugin'  'test_plugin.c'  @('ws2_32.lib', 'gdi32.lib', 'user32.lib')

# cl generates a .lib/.exp beside any plugin that exports
# symbols (chaos exports ChaosCount & friends). They are not
# loaded by the game, so keep the scripts tree clean.
Get-ChildItem $scripts -Recurse -Include *.lib, *.exp |
    Remove-Item -Force

Write-Host 'build complete'
