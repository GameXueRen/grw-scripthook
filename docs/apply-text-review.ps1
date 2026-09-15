param([switch]$DryRun)
$ErrorActionPreference = 'Stop'
Set-Location 'f:\UbisoftGames\GameXueRen\grw-scripthook'
$enc = [Text.UTF8Encoding]::new($false)
$reviewPath = 'docs\text-cn-review.ini'

# A table entry: { "key", "value" } - BOTH sides may be several adjacent
# literals (a long key is wrapped in the source too), and escapes stay RAW:
# the review file carries the same escape form the source does, so what is
# compared and written is the literal body.
$script:EntryRe = '\{\s*("(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*)\s*,\s*("(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*)\s*\}'
$script:SpecRe  = '%(?:[0-9]+\$)?[-+ #0]*[0-9]*(?:\.\d+)?[a-zA-Z%]'

function Get-ReviewRows([string]$path) {
    $sec = ''
    $map = [ordered]@{}
    foreach ($ln in [IO.File]::ReadAllLines($path)) {
        if ($ln -match '^\s*\[(.+?)\]\s*$') {
            $sec = $Matches[1]
            if (-not $map.Contains($sec)) { $map[$sec] = [ordered]@{} }
            continue
        }
        if (-not $sec) { continue }
        if ($ln -match '^\s*"((?:[^"\\]|\\.)*)"\s*=\s*(.*?)\s*$') {
            $map[$sec][$Matches[1]] = $Matches[2]
        }
    }
    return $map
}

function Value-Body([string]$pieces) {
    $parts = @()
    foreach ($m in [regex]::Matches($pieces, '"((?:[^"\\]|\\.)*)"')) { $parts += $m.Groups[1].Value }
    return ($parts -join '')
}

function Spec-Letters([string]$s) {
    $l = @()
    foreach ($m in [regex]::Matches($s, $script:SpecRe)) {
        if ($m.Value.EndsWith('%%')) { continue }
        $l += $m.Value.Substring($m.Value.Length - 1)
    }
    return ($l -join ' ')
}

function Split-Long([string]$s, [int]$limit) {
    $pieces = @()
    while ($s.Length -gt $limit) {
        $cut = -1
        # prefer a space at or before the limit (an English hint wraps there)
        for ($i = $limit; $i -gt [int]($limit / 2); $i--) {
            if ($s[$i] -eq ' ') { $cut = $i; break }
        }
        if ($cut -lt 0) {
            # a run with no space to break at (Chinese): break after a
            # punctuation mark if there is one in the window
            $punct = '；。，、！？：）】'
            for ($i = $limit; $i -gt [int]($limit / 2); $i--) {
                if ($punct.IndexOf($s[$i]) -ge 0) { $cut = $i + 1; break }
            }
        }
        if ($cut -lt 0) {
            # nothing to break at: cut on a character boundary, never inside a
            # surrogate pair and never right after a backslash of an escape
            $cut = $limit
            if ($cut -gt 1 -and [Char]::IsHighSurrogate($s[$cut - 1])) { $cut-- }
            if ($cut -gt 1 -and $s[$cut - 1] -eq '\') { $cut-- }
        }
        $pieces += $s.Substring(0, $cut)
        $s = $s.Substring($cut)
    }
    if ($s) { $pieces += $s }
    return $pieces
}

function Format-Literal([string]$v, [bool]$ownLine) {
    $s = $v -replace '(?<!\\)"', '\"' -replace "`r`n", '\n' -replace "`n", '\n'
    if ($s.Length -le 72) { return '"' + $s + '"' }
    $out = Split-Long $s 64
    # a wrapped value goes on its own line - unless it already is on one, in
    # which case the indentation in front of it stays and must not double
    $lead = if ($ownLine) { '' } else { "`n      " }
    return ($lead + (($out | ForEach-Object { '"' + $_ + '"' }) -join "`n      "))
}

function Read-IniRows([string]$path) {
    $rows = @{}
    if (-not (Test-Path $path)) { return $rows }
    $lines = [IO.File]::ReadAllLines($path)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*"((?:[^"\\]|\\.)*)"\s*=\s*"(.*)"\s*$') {
            if (-not $rows.ContainsKey($Matches[1])) {
                $rows[$Matches[1]] = @{ body = $Matches[2]; line = $i }
            }
        }
    }
    return $rows
}

$review = Get-ReviewRows $reviewPath

# Which sections exist here, and where each one's text lives.
$sections = @{}
$sections['framework'] = @{ table = 'scripthook_text.c'; name = 'kZhCN'; ini = '' }
foreach ($dir in (Get-ChildItem 'plugins' -Directory | Sort-Object Name)) {
    $table = ''
    $name = ''
    $c = Get-ChildItem $dir.FullName -Filter *.c | Select-Object -First 1
    if ($c) {
        $t = [IO.File]::ReadAllText($c.FullName)
        if ($t -match 'kZh\s*\[\s*\]\s*=') { $table = $c.FullName; $name = 'kZh' }
    }
    $ini = Join-Path $dir.FullName 'lang.ini'
    if (-not (Test-Path $ini)) { $ini = '' }
    if ($table -or $ini) { $sections[$dir.Name] = @{ table = $table; name = $name; ini = $ini } }
}

$same = 0; $blank = @(); $unknown = @(); $specs = @(); $absent = @(); $planned = @()
$stripped = @()

foreach ($sec in $review.Keys) {
    if (-not $sections.ContainsKey($sec)) {
        foreach ($k in $review[$sec].Keys) { $unknown += "[$sec] $k  (no such section in the tree)" }
        continue
    }
    $s = $sections[$sec]

    # the compiled-in table: key -> value body + the value's span in the file
    $code = [ordered]@{}
    if ($s.table) {
        $t = [IO.File]::ReadAllText($s.table)
        $span = [regex]::Match($t, [regex]::Escape($s.name) + '\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;',
            [Text.RegularExpressions.RegexOptions]::Singleline)
        if ($span.Success) {
            $body = $span.Groups[1].Value
            $base = $span.Groups[1].Index
            foreach ($e in [regex]::Matches($body, $script:EntryRe)) {
                $k = Value-Body $e.Groups[1].Value
                if ($code.Contains($k)) { continue }
                $code[$k] = @{
                    body = (Value-Body $e.Groups[2].Value)
                    idx  = $base + $e.Groups[2].Index
                    len  = $e.Groups[2].Length
                }
            }
        }
    }
    # a plugin's own lang.ini: it carries what the table does not (spawner
    # keeps the framework's vehicle catalogue there, a plugin without source
    # has nothing else at all)
    $ini = if ($s.ini) { Read-IniRows $s.ini } else { @{} }

    foreach ($k in $review[$sec].Keys) {
        $new = $review[$sec][$k]
        # The file does not quote the right-hand side, so a value that
        # arrives wrapped in its own quotes was meant as text: strip the pair
        # instead of writing the quote characters into the literal.
        if ($new.Length -ge 2 -and $new.StartsWith('"') -and $new.EndsWith('"')) {
            $stripped += "[$sec] $k  (" + $new + ")"
            $new = $new.Substring(1, $new.Length - 2)
        }
        $where = $null
        if ($code.Contains($k)) { $where = 'table' }
        elseif ($ini.ContainsKey($k)) { $where = 'ini' }
        if (-not $where) { $unknown += "[$sec] $k"; continue }

        $cur = if ($where -eq 'table') { $code[$k].body } else { $ini[$k].body }
        if ($new -eq '') {
            if ($cur -ne '') { $blank += "[$sec] $k  (was: $cur)" }
            continue
        }
        if ($new -eq $cur) { $same++; continue }

        $a = Spec-Letters $cur
        $b = Spec-Letters $new
        if ($a -ne $b) { $specs += "[$sec] $k`n      old: $a`n      new: $b"; continue }

        if ($where -eq 'table') {
            $ownLine = [regex]::IsMatch(
                [IO.File]::ReadAllText($s.table).Substring(0, $code[$k].idx), "`n\s*$")
            $planned += @{ path = $s.table; idx = $code[$k].idx; len = $code[$k].len;
                           text = (Format-Literal $new $ownLine) }
        } else {
            $planned += @{ path = $s.ini; line = $ini[$k].line; text = $new }
        }
    }

    foreach ($k in $code.Keys) { if (-not $review[$sec].Contains($k)) { $absent += "[$sec] $k" } }
}

$total = (($review.Values | ForEach-Object { $_.Keys.Count }) | Measure-Object -Sum).Sum
"review rows with a value : $total"
"  identical to code      : $same"
"  EDITED (will be applied): " + $planned.Count
"  empty value            : " + $blank.Count
''
if ($stripped.Count) {
    '--- STRIPPED QUOTES: the value arrived wrapped in " ", applied without them ---'
    $stripped | ForEach-Object { '  ' + $_ }
    ''
}
if ($blank.Count) { '--- EMPTY VALUE (kept as it is) ---'; $blank | ForEach-Object { '  ' + $_ }; '' }
if ($unknown.Count) { '--- UNKNOWN KEY: no anchor in the code ---'; $unknown | ForEach-Object { '  ' + $_ }; '' }
if ($specs.Count) { '--- PLACEHOLDER MISMATCH: NOT applied, decide these ---'; $specs | ForEach-Object { '  ' + $_ }; '' }
if ($absent.Count) { '--- NOT IN THE REVIEW FILE (kept) ---'; $absent | ForEach-Object { '  ' + $_ }; '' }

if ($DryRun) { 'dry run: nothing written.'; exit 0 }

$byFile = @{}
foreach ($p in $planned) {
    if (-not $byFile.ContainsKey($p.path)) { $byFile[$p.path] = @() }
    $byFile[$p.path] += $p
}
$written = 0
foreach ($path in $byFile.Keys) {
    if ($path -like '*lang.ini') {
        $lines = [IO.File]::ReadAllLines($path)
        foreach ($p in $byFile[$path]) {
            $lines[$p.line] = $lines[$p.line] -replace '^(\s*"(?:[^"\\]|\\.)*"\s*=\s*)".*"(\s*)$',
                                                     ('$1"' + $p.text + '"$2')
            $written++
        }
        [IO.File]::WriteAllLines($path, $lines, $enc)
    } else {
        $t = [IO.File]::ReadAllText($path)
        foreach ($p in ($byFile[$path] | Sort-Object -Property idx -Descending)) {
            $t = $t.Substring(0, $p.idx) + $p.text + $t.Substring($p.idx + $p.len)
            $written++
        }
        [IO.File]::WriteAllText($path, $t, $enc)
    }
    'written: ' + $path
}
"total values written: $written"
