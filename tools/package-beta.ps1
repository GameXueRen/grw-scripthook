<#
.SYNOPSIS
  Build the public beta package: the whitelist, and nothing else.

.DESCRIPTION
  Copies a fixed whitelist out of the deployed game folder into a clean tree
  (and optionally a zip).

  It exists because "zip the game folder" is the obvious thing to do and the
  wrong one: that folder also holds plugins_off\ (every development and probe
  plugin a -Beta build moved aside), logs\, mods\, and whatever an earlier
  build left behind. The whitelist is the only thing that decides what is in
  the package; anything else in the game folder is reported as skipped.

  The version in the package folder name is read from SH_VERSION in
  scripthook.h, so the package, the loader's start up line, the crash report
  header and the README all name the same build.

.EXAMPLE
  pwsh ./tools/package-beta.ps1
  pwsh ./tools/package-beta.ps1 -Zip
  pwsh ./tools/package-beta.ps1 -From 'D:\GRW' -OutDir 'D:\dist' -Zip
#>
[CmdletBinding()]
param(
    # The deployed game folder to copy from. Defaults to the sibling
    # installation the build script uses.
    [string]$From,

    # Where the package tree is written. Defaults to <repo>\out, which is
    # git-ignored.
    [string]$OutDir,

    # The plugins that ship. Everything else under plugins\ is left out.
    [string[]]$Plugins = @(
        'skipintro', 'spawner', 'firstperson', 'fov_changer', 'cnchat',
        'TimeWeatherControl', 'OpticalCamo', 'ammo_capacity'
    ),

    # Also produce a .zip beside the tree.
    [switch]$Zip
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not $From) {
    $candidate = Join-Path (Join-Path $root '..\..') "Tom Clancy's Ghost Recon Wildlands"
    if (Test-Path (Join-Path $candidate 'GRW.exe')) { $From = $candidate }
    else { $From = Join-Path $root '..\..' }
}
if (-not (Test-Path (Join-Path $From 'GRW.exe'))) {
    throw "not a game folder (no GRW.exe): $From"
}
if (-not $OutDir) { $OutDir = Join-Path $root 'out' }

# The deployed loader carries the version, but the source is what defines it:
# one string, read here rather than typed twice.
$header = Join-Path $root 'scripthook.h'
$m = Select-String -Path $header -Pattern '^\s*#define\s+SH_VERSION\s+"([^"]+)"' |
     Select-Object -First 1
if (-not $m) { throw "SH_VERSION not found in $header" }
$version = $m.Matches[0].Groups[1].Value

$name = "GRW-ScriptHook-$version"
$dst = Join-Path $OutDir $name
if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
New-Item -ItemType Directory -Force -Path $dst | Out-Null

Write-Host "GAMEDIR : $From"
Write-Host "VERSION : $version"
Write-Host "OUTPUT  : $dst"
Write-Host ""

$missing = @()
$copied = 0
$bytes = 0

function Copy-One([string]$rel, [string]$label) {
    $src = Join-Path $From $rel
    if (-not (Test-Path $src)) { $script:missing += "$rel  ($label)"; return }
    $target = Join-Path $dst $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item $src $target -Recurse -Force
    $f = Get-Item $target
    $script:copied++
    if (-not $f.PSIsContainer) { $script:bytes += $f.Length }
    else { $script:bytes += (Get-ChildItem $f -Recurse -File | Measure-Object -Property Length -Sum).Sum }
    Write-Host ("  + " + $rel)
}

# ---- the whitelist ---------------------------------------------------------

Copy-One 'dinput8.dll'    'the loader'
Copy-One 'scripthook.ini' 'the config'

foreach ($p in $Plugins) {
    $asi = Join-Path $From (Join-Path "plugins\$p" "$p.asi")
    if (-not (Test-Path $asi)) { $missing += "plugins\$p\$p.asi  (plugin not built or not deployed)"; continue }
    Copy-One "plugins\$p" "plugin"
}

# Repo-side files: the reference language file and the licences. lang.ini in
# the game folder is the player's own and is never shipped.
foreach ($rel in @('lang.example.ini', 'LICENSE')) {
    $src = Join-Path $root $rel
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $dst $rel) -Force
        $bytes += (Get-Item $src).Length
        Write-Host "  + $rel"
    } else {
        $missing += "$rel  (repo file)"
    }
}

# ---- third-party notices ---------------------------------------------------

$libs = Get-ChildItem (Join-Path $root 'third_party') -Directory -ErrorAction SilentlyContinue
$notice = New-Object System.Text.StringBuilder
[void]$notice.AppendLine("GRW ScriptHook $version - third-party notices")
[void]$notice.AppendLine("=" * 60)
[void]$notice.AppendLine("")
[void]$notice.AppendLine("This package contains the following third-party software. Each")
[void]$notice.AppendLine("licence is reproduced in full below, as those licences require for")
[void]$notice.AppendLine("binary distribution.")
[void]$notice.AppendLine("")
foreach ($lib in $libs) {
    $lic = Get-ChildItem $lib.FullName -Recurse -File -Filter 'LICENSE*' |
           Select-Object -First 1
    if ($lic) { [void]$notice.AppendLine("  - $($lib.Name)  ($($lic.Name))") }
    else      { [void]$notice.AppendLine("  - $($lib.Name)  (NO LICENCE FILE FOUND)") }
}
foreach ($lib in $libs) {
    $lic = Get-ChildItem $lib.FullName -Recurse -File -Filter 'LICENSE*' |
           Select-Object -First 1
    if (-not $lic) { continue }
    [void]$notice.AppendLine("")
    [void]$notice.AppendLine("-" * 60)
    [void]$notice.AppendLine("$($lib.Name) - $($lic.FullName.Substring($root.Length + 1))")
    [void]$notice.AppendLine("-" * 60)
    [void]$notice.AppendLine((Get-Content $lic.FullName -Raw))
}
$noticePath = Join-Path $dst 'THIRD-PARTY-NOTICES.txt'
# UTF-8 without BOM, matching every other text file in the tree.
[System.IO.File]::WriteAllText($noticePath, $notice.ToString(),
                               (New-Object System.Text.UTF8Encoding($false)))
$bytes += (Get-Item $noticePath).Length
Write-Host "  + THIRD-PARTY-NOTICES.txt  (generated from third_party\*\LICENSE.txt)"

# ---- what was left behind --------------------------------------------------

$allowed = @('plugins')
$skipped = Get-ChildItem $From -Force |
    Where-Object { $_.Name -notin @('dinput8.dll', 'scripthook.ini') -and
                   $_.Name -notin $allowed } |
    Select-Object -ExpandProperty Name
$extras = Get-ChildItem (Join-Path $From 'plugins') -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -notin $Plugins } |
    Select-Object -ExpandProperty Name

Write-Host ""
if ($skipped) {
    Write-Host "skipped from the game folder (by design):"
    foreach ($s in $skipped) { Write-Host "  - $s" }
}
if ($extras) {
    Write-Host "skipped plugins (not in the shipped set):"
    foreach ($s in $extras) { Write-Host "  - plugins\$s" }
}
if ($missing) {
    Write-Host ""
    Write-Warning "expected but not found:"
    foreach ($m2 in $missing) { Write-Warning "  - $m2" }
}

# ---- result ----------------------------------------------------------------

$files = Get-ChildItem $dst -Recurse -File
$total = ($files | Measure-Object -Property Length -Sum).Sum
Write-Host ""
Write-Host ("package: {0} file(s), {1:N2} MB" -f $files.Count, ($total / 1MB))
$files | Sort-Object FullName | ForEach-Object {
    Write-Host ("  {0,10:N0}  {1}" -f $_.Length, $_.FullName.Substring($dst.Length + 1))
}

if ($Zip) {
    # The local is called $archive rather than $zip on purpose: PowerShell
    # variable names are case-insensitive, and a host shell that keeps a
    # typed $Zip (a switch) in its own scope makes every assignment to $zip
    # fail to convert - which looks like a failure of this script and is not.
    # .NET is used throughout for the same reason: it has no wrapper layer.
    try {
        Write-Host "writing archive"
        $archive = [System.IO.Path]::Combine($OutDir, ($name + '.zip'))
        Write-Host "  path   $archive"
        if ([System.IO.File]::Exists($archive)) {
            [System.IO.File]::Delete($archive)
            Write-Host "  stale copy removed"
        }
        [System.IO.Compression.ZipFile]::CreateFromDirectory(
            $dst, $archive,
            [System.IO.Compression.CompressionLevel]::Optimal, $true)
        $zb = [System.IO.FileInfo]::new($archive).Length
        Write-Host ("  ok     {0:N2} MB" -f ($zb / 1MB))
    } catch {
        Write-Warning ("archive failed at line {0}: {1}" -f `
                       $_.InvocationInfo.ScriptLineNumber, $_.Exception.Message)
    }
}
