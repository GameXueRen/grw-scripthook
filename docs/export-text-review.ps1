Set-Location 'f:\UbisoftGames\GameXueRen\grw-scripthook'
$enc = [Text.UTF8Encoding]::new($false)

# C joins adjacent string literals; merge them so a wrapped hint reads whole.
function Merge-Lits([string]$t) {
    $prev = ''
    while ($prev -ne $t) { $prev = $t; $t = $t -replace '"\s*"', '' }
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

$out = New-Object System.Collections.ArrayList
$counts = [ordered]@{}
$missing = 0

function Emit-Owner([string]$owner, $en, $zh, [string]$note) {
    $keys = @()
    if ($en) { $keys += $en.Keys }
    if ($zh) { foreach ($k in $zh.Keys) { if ($keys -notcontains $k) { $keys += $k } } }
    if ($keys.Count -eq 0) { return }
    [void]$out.Add("[$owner]")
    if ($note) { [void]$out.Add("; $note") }
    foreach ($k in $keys) {
        [void]$out.Add("")
        $e = ''
        $z = ''
        if ($en -and $en.Contains($k)) { $e = $en[$k] }
        elseif ($k -notmatch '^@') { $e = $k }
        if ($zh -and $zh.Contains($k)) { $z = $zh[$k] }
        if ($e) { [void]$out.Add("; en: $e") }
        [void]$out.Add('"' + $k + '" = ' + $z)
        if (-not $z) { $script:missing++ }
    }
    [void]$out.Add("")
    $script:counts[$owner] = $keys.Count
}

# ---- the framework ------------------------------------------------------
$fw = Merge-Lits ([IO.File]::ReadAllText('scripthook_text.c'))
Emit-Owner 'framework' (Get-Table $fw 'kEnUS') (Get-Table $fw 'kZhCN') `
    'the framework own text: menu furniture, the settings tree, CPU, Forge, the report'

# ---- plugins ------------------------------------------------------------
foreach ($dir in (Get-ChildItem 'plugins' -Directory | Sort-Object Name)) {
    $c = Get-ChildItem $dir.FullName -Filter *.c | Select-Object -First 1
    $en = $null
    $zh = $null
    $note = ''
    if ($c) {
        $t = Merge-Lits ([IO.File]::ReadAllText($c.FullName))
        $en = Get-Table $t 'kEn'
        $zh = Get-Table $t 'kZh'
    }
    if ((-not $en -or $en.Count -eq 0) -and (-not $zh -or $zh.Count -eq 0)) {
        $note = 'no source for this plugin: its lang.ini is the only place its text lives'
    }

    # A plugin's own lang.ini may carry rows its tables do not: spawner keeps
    # the framework's vehicle catalogue there (the keys are the framework's
    # entity names), and a plugin without source has nothing else at all.
    $li = Join-Path $dir.FullName 'lang.ini'
    if (Test-Path $li) {
        $sec = ''
        foreach ($ln in [IO.File]::ReadAllLines($li)) {
            if ($ln -match '^\s*\[(.+?)\]') { $sec = $Matches[1]; continue }
            if ($sec -ne 'zh-CN') { continue }
            if ($ln -match '^\s*"([^"]+)"\s*=\s*"(.*)"\s*$') {
                if (-not $zh) { $zh = [ordered]@{} }
                if (-not $zh.Contains($Matches[1])) { $zh[$Matches[1]] = $Matches[2] }
            }
        }
    }
    if ((-not $en -or $en.Count -eq 0) -and (-not $zh -or $zh.Count -eq 0)) { continue }
    Emit-Owner $dir.Name $en $zh $note
}

$head = @'
; 菜单与插件文本的中文校对表 —— 从源码基线导出，供逐条润色。
;
; 英文是权威文本，中文是当前屏幕上的样子：
;   [framework]  -> scripthook_text.c 的 kEnUS / kZhCN
;   [插件目录名]  -> 该插件 .c 里的 kEn / kZh
;   无源码插件    -> plugins\<名>\lang.ini 的 [zh-CN]（键就是英文原文）
;
; 用法：
;   只改等号右边的中文。键不要动 —— 键是代码里的锚点，改键会打断译文。
;   上一行 '; en: ...' 是该条的英文原文，供对照，不要改。
;   值为空 = 这一条还没有中文：可以填，也可以留空。
;   值里的换行写成 \n（与 lang.ini 同规）。
;   删掉某一行、或把右值留空，都表示"这条保持现状"。
;
; 回写规则：按节落到 scripthook_text.c 的 kZhCN / 插件的 kZh 表 /
; 无源码插件的 plugins\<名>\lang.ini；不动键、不动英文侧。
;
; 本文件由 docs\export-text-review.ps1 生成（在仓库根运行它即可重导）——
; 润色前先另存一份，重新生成会覆盖它。
'@

$body = New-Object System.Collections.ArrayList
[void]$body.Add($head)
foreach ($l in $out) { [void]$body.Add($l) }
[IO.File]::WriteAllLines('docs\text-cn-review.ini', $body, $enc)

'--- sections ---'
foreach ($k in $counts.Keys) { '  {0,-20} {1,4} rows' -f $k, $counts[$k] }
'total rows  : ' + (($counts.Values | Measure-Object -Sum).Sum)
'empty zh    : ' + $missing
'file        : docs\text-cn-review.ini  (' + $body.Count + ' lines)'
