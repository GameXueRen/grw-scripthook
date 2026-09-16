<#
.SYNOPSIS
  Turn a Markdown document into plain text a text editor can show.

.DESCRIPTION
  The documents in this repository are Markdown because that is what is read,
  diffed and reviewed here. The package, though, is handed to somebody on
  another machine who opens a file by double-clicking it - and a .md often has
  nothing bound to it at all. So the package carries a .txt next to each .md,
  and this is what makes that copy.

  It only ever removes markup, and it never reflows a sentence: the words, the
  order and the line breaks are the source's. What changes is what a reader
  does not need - table separator rows, code fences, asterisks, backticks, the
  blockquote markers - plus one addition, table rows padded to the widest cell
  of their column so the columns still line up in a monospaced window. A fence
  body is copied verbatim, four spaces in, so code still reads as code.

  Tables and headings are measured in display columns, not characters, so a
  Chinese heading gets an underline as long as it looks.

  The caller writes the file (see Write-DocTxt in tools/package-beta.ps1),
  which is also where the encoding decision lives: UTF-8 with a BOM, so that a
  legacy Notepad - which reads a BOM-less UTF-8 file as ANSI - does not turn
  every Chinese character into mojibake.

.EXAMPLE
  pwsh ./tools/md-to-txt.ps1 -Path docs/beta-test-plan.md
  pwsh ./tools/md-to-txt.ps1 -Path docs/*.md -OutDir out\readable
#>
[CmdletBinding()]
param(
    # The Markdown file(s) to convert. When given, this script acts as a
    # command line tool; when the file is dot-sourced instead, the parameter
    # block simply binds nothing and only the functions below are taken.
    [string[]]$Path,

    # Where the .txt files go. Defaults to beside their .md.
    [string]$OutDir
)

# How many columns a string takes in a monospaced window: East Asian wide
# characters take two, everything else one.
function Get-DisplayWidth {
    param([string]$s)
    $w = 0
    foreach ($ch in $s.ToCharArray()) {
        $c = [int]$ch
        if (($c -ge 0x1100 -and $c -le 0x115F) -or
            ($c -ge 0x2E80 -and $c -le 0xA4CF) -or
            ($c -ge 0xAC00 -and $c -le 0xD7A3) -or
            ($c -ge 0xF900 -and $c -le 0xFAFF) -or
            ($c -ge 0xFE30 -and $c -le 0xFE6F) -or
            ($c -ge 0xFF00 -and $c -le 0xFF60) -or
            ($c -ge 0xFFE0 -and $c -le 0xFFE6)) { $w += 2 } else { $w += 1 }
    }
    return $w
}

function Convert-MarkdownToText {
    param([string]$Text)

    $out = New-Object 'System.Collections.Generic.List[string]'
    $table = New-Object 'System.Collections.Generic.List[object]'
    $inFence = $false
    $ruleWidth = 72

    # Everything that is markup inside a line, and nothing else.
    function Strip-Inline([string]$s) {
        $s = [regex]::Replace($s, '\*\*(.+?)\*\*', '$1')
        $s = [regex]::Replace($s, '`([^`]+)`', '$1')
        $s = [regex]::Replace($s, '\[([^\]]+)\]\(\1\)', '$1')
        $s = [regex]::Replace($s, '\[([^\]]+)\]\(([^)]+)\)', '$1 ($2)')
        return $s.TrimEnd()
    }

    function Add-Blank {
        if ($out.Count -gt 0 -and $out[$out.Count - 1] -ne '') { $out.Add('') }
    }

    # A table becomes its rows with the columns padded to line up; the header
    # row gets the rule the Markdown separator row was standing in for.
    function Flush-Table {
        if ($table.Count -eq 0) { return }
        $cols = 0
        foreach ($r in $table) { if ($r.Count -gt $cols) { $cols = $r.Count } }
        $width = New-Object 'int[]' $cols
        foreach ($r in $table) {
            for ($i = 0; $i -lt $r.Count; $i++) {
                $cw = Get-DisplayWidth $r[$i]
                if ($cw -gt $width[$i]) { $width[$i] = $cw }
            }
        }
        $first = $true
        foreach ($r in $table) {
            $cells = @()
            for ($i = 0; $i -lt $r.Count; $i++) {
                $pad = $width[$i] - (Get-DisplayWidth $r[$i])
                if ($pad -lt 0) { $pad = 0 }
                $cells += ($r[$i] + (' ' * $pad))
            }
            $out.Add(($cells -join '  ').TrimEnd())
            if ($first) {
                $total = (($width | Measure-Object -Sum).Sum) + (($r.Count - 1) * 2)
                if ($total -gt $ruleWidth) { $total = $ruleWidth }
                $out.Add('-' * $total)
                $first = $false
            }
        }
        Add-Blank
        $table.Clear()
    }

    $lines = ($Text -replace "`r`n", "`n").Replace("`r", "`n") -split "`n"

    foreach ($raw in $lines) {
        $line = $raw.TrimEnd()

        if ($line -match '^\s*```') {
            if (-not $inFence) { Flush-Table }
            $inFence = -not $inFence
            continue
        }
        if ($inFence) {
            # Verbatim, indented, so it still reads as code - and so a
            # character that means markup elsewhere means nothing here.
            if ($line -ne '') { $out.Add('    ' + $line) }
            continue
        }

        if ($line -match '^\s*\|') {
            if ($line -match '^\s*\|[\s\-:|]+\|\s*$') { continue }
            $cells = @($line.Trim().Trim('|') -split '\|' |
                       ForEach-Object { (Strip-Inline $_).Trim() })
            $table.Add($cells)
            continue
        }
        Flush-Table

        if ($line -match '^\s*$') { Add-Blank; continue }
        if ($line -match '^\s*-{3,}\s*$') { $out.Add('-' * $ruleWidth); continue }

        if ($line -match '^(#{1,6})\s+(.*)$') {
            $level = $Matches[1].Length
            $head = Strip-Inline $Matches[2]
            Add-Blank
            $out.Add($head)
            if ($level -le 2) {
                $w = Get-DisplayWidth $head
                if ($w -gt $ruleWidth) { $w = $ruleWidth }
                if ($w -lt 3) { $w = 3 }
                if ($level -eq 1) { $out.Add('=' * $w) } else { $out.Add('-' * $w) }
            }
            continue
        }

        if ($line -match '^\s*>\s?(.*)$') {
            $out.Add('  ' + (Strip-Inline $Matches[1]))
            continue
        }

        $out.Add((Strip-Inline $line))
    }
    Flush-Table

    return (($out -join "`r`n").TrimEnd() + "`r`n")
}

# ---- command line, when this file is run rather than dot-sourced -----------

if ($PSBoundParameters.ContainsKey('Path') -and $Path) {
    foreach ($p in $Path) {
        $full = [System.IO.Path]::GetFullPath($p)
        if (-not [System.IO.File]::Exists($full)) {
            Write-Warning "not found: $p"
            continue
        }
        $dir = if ($OutDir) { $OutDir } else { [System.IO.Path]::GetDirectoryName($full) }
        [System.IO.Directory]::CreateDirectory($dir) | Out-Null
        $dest = [System.IO.Path]::Combine($dir,
                    ([System.IO.Path]::GetFileNameWithoutExtension($full) + '.txt'))
        $text = Convert-MarkdownToText ([System.IO.File]::ReadAllText($full, [System.Text.Encoding]::UTF8))
        # UTF-8 with a BOM: this copy is for a human double-clicking it.
        [System.IO.File]::WriteAllText($dest, $text, (New-Object System.Text.UTF8Encoding($true)))
        Write-Host ("{0} ({1:N0} B)  ->  {2} ({3:N0} B)" -f `
            [System.IO.Path]::GetFileName($full), (Get-Item $full).Length,
            $dest, [System.IO.FileInfo]::new($dest).Length)
    }
}
