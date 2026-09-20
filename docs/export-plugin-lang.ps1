Set-Location 'f:\UbisoftGames\GameXueRen\grw-scripthook'
$enc = [Text.UTF8Encoding]::new($false)

# The plugins whose text lives in their own .c table (kEn / kZh) and whose
# lang.ini is the override layer beside them. This script writes each one's
# lang.ini out again from those two sources, so the file a player reads
# carries both languages and can be copied for another.
$plugins = @('cnchat', 'firstperson', 'fov_changer', 'skipintro', 'spawner')

# C joins adjacent string literals; merge them so a wrapped row reads whole.
# The quote a join starts at must not be a backslash-escaped one: the two
# quotes at the end of `...over \"Apply the set time\""` are the row's closing
# quote plus its own, not two literals, and joining them drops the row - which
# is what happened to @tw.hint before this lookbehind was added.
function Merge-Lits([string]$t) {
    $prev = ''
    while ($prev -ne $t) { $prev = $t; $t = [regex]::Replace($t, '(?<!\\)"\s*"', '') }
    return $t
}

# <name>[] = { { "key", "text" }, ... };   (the element type does not matter)
function Get-Table([string]$text, [string]$name) {
    $d = [ordered]@{}
    $m = [regex]::Match($text,
        [regex]::Escape($name) + '\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;',
        [Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $m.Success) { return $d }
    foreach ($e in [regex]::Matches($m.Groups[1].Value,
            '\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}')) {
        $k = $e.Groups[1].Value
        if (-not $d.Contains($k)) { $d[$k] = $e.Groups[2].Value }
    }
    return $d
}

# The [zh-CN] rows a lang.ini already carries, which is what the menu shows
# for that key today - the file wins over the built-in text at run time, so
# it has to win here too. Commented lines are skipped: they are examples.
function Get-IniRows([string]$path) {
    $d = [ordered]@{}
    if (-not (Test-Path $path)) { return $d }
    $sec = ''
    foreach ($ln in [IO.File]::ReadAllLines($path)) {
        if ($ln -match '^\s*[#;]') { continue }
        if ($ln -match '^\s*\[(.+?)\]\s*$') { $sec = $Matches[1]; continue }
        if ($sec -ne 'zh-CN') { continue }
        if ($ln -match '^\s*"([^"]+)"\s*=\s*"?(.*?)"?\s*$') {
            $d[$Matches[1]] = $Matches[2]
        }
    }
    return $d
}

$head = @(
    '# {0} text - 本插件随包附带的文案文件。',
    '#',
    '# [en-US] 是插件自带的英文原文；[zh-CN] 是当前内置的中文译文。',
    '# 想改某一行：把那一行复制到本文件对应语言节里改值即可（本文件优先于内置文案）。',
    '# 想加一门语言：复制一整节，把节名改成语言代码（如 [ja-JP]），再逐行翻译。',
    '# 本文件与插件一起发布，框架只会读它、永远不会写它 —— 可以放心就地编辑。',
    '#',
    '# [en-US] is the text this plugin ships with; [zh-CN] is the built-in Chinese.',
    '# A row here overrides the built-in text for that key in that language.',
    '# To add a language, copy a whole section and rename it (e.g. [ja-JP]).',
    '# The framework only ever reads this file; it never writes one.'
)

$summary = @()
foreach ($p in $plugins) {
    $dir = Join-Path 'plugins' $p
    $c = Get-ChildItem $dir -Filter *.c -File | Select-Object -First 1
    if (-not $c) { Write-Warning "$p has no source in the tree - skipped"; continue }

    $t  = Merge-Lits ([IO.File]::ReadAllText($c.FullName, [Text.Encoding]::UTF8))
    $en = Get-Table $t 'kEn'
    $zh = Get-Table $t 'kZh'
    $old = Get-IniRows (Join-Path $dir 'lang.ini')

    # Every key three sources know about: the code's table order first, then
    # whatever the old file carried that the tables do not (a literal key
    # from the game, say), then sorted so two runs diff cleanly.
    $keys = @()
    foreach ($k in $en.Keys)  { if ($keys -notcontains $k) { $keys += $k } }
    foreach ($k in $zh.Keys)  { if ($keys -notcontains $k) { $keys += $k } }
    foreach ($k in $old.Keys) { if ($keys -notcontains $k) { $keys += $k } }
    $keys = @($keys | Sort-Object { $_.ToLowerInvariant() })

    $enLines = @()
    $zhLines = @()
    $skipEn = @()
    $skipZh = @()
    $onlyEn = 0
    $onlyZh = 0
    foreach ($k in $keys) {
        $e = ''
        $z = ''
        if ($en.Contains($k))         { $e = $en[$k] }   # the code's own English
        elseif ($k -notmatch '^@')    { $e = $k }        # a literal key IS the English
        if ($old.Contains($k))        { $z = $old[$k] }  # the file wins, as at run time
        elseif ($zh.Contains($k))     { $z = $zh[$k] }
        # A value carrying a double quote cannot be written into a lang.ini:
        # the framework's reader drops the line's outer quotes and knows no
        # escape, so `...over \"Apply the set time\"` would come back with the
        # backslashes still in it. Such a row is left out and named in the
        # file; the built-in text keeps applying to it.
        if ($e -match '"') { $skipEn += $k; $e = '' }
        if ($z -match '"') { $skipZh += $k; $z = '' }
        if ($e) { $enLines += ('"{0}" = "{1}"' -f $k, $e) }
        if ($z) { $zhLines += ('"{0}" = "{1}"' -f $k, $z) }
        if ($e -and -not $z) { $onlyEn++ }
        if ($z -and -not $e) { $onlyZh++ }
    }

    $body = New-Object System.Collections.ArrayList
    foreach ($l in $head) { [void]$body.Add(($l -f $p)) }
    [void]$body.Add('')
    [void]$body.Add('[en-US]')
    foreach ($l in $enLines) { [void]$body.Add($l) }
    if ($skipEn.Count) {
        [void]$body.Add('')
        [void]$body.Add('# 未列出的行：其英文含引号字符，本文件格式无法承载（内置英文照常生效）：')
        foreach ($k in $skipEn) { [void]$body.Add('#   ' + $k) }
    }
    [void]$body.Add('')
    [void]$body.Add('[zh-CN]')
    foreach ($l in $zhLines) { [void]$body.Add($l) }
    if ($skipZh.Count) {
        [void]$body.Add('')
        [void]$body.Add('# 未列出的行：其译文含引号字符，本文件格式无法承载（内置译文照常生效）：')
        foreach ($k in $skipZh) { [void]$body.Add('#   ' + $k) }
    }
    [void]$body.Add('')
    [IO.File]::WriteAllLines((Join-Path $dir 'lang.ini'), $body, $enc)

    $summary += ('{0,-18} en {1,3}  zh {2,3}  (en-only {3}, zh-only {4}, skipped {5})' -f `
                 $p, $enLines.Count, $zhLines.Count, $onlyEn, $onlyZh,
                 ($skipEn.Count + $skipZh.Count))
    if ($skipEn.Count -or $skipZh.Count) {
        Write-Warning ("{0}: {1} row(s) named in a comment instead (their text carries a quote)" -f `
                       $p, ($skipEn.Count + $skipZh.Count))
    }
}

'--- regenerated plugin lang.ini ---'
$summary | ForEach-Object { '  ' + $_ }
'files       : ' + $summary.Count + ' of ' + $plugins.Count
