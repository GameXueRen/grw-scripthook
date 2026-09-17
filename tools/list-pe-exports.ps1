# 列出一个 PE 文件的导出表，并标出每个导出落在哪个节（.text = 代码，.rdata/.data = 数据）。
# 用途：代理 dinput8.dll 必须镜像真 dinput8 的全部导出；这个脚本用来核对有没有漏。
#
# 用法： pwsh -File tools\list-pe-exports.ps1 C:\Windows\System32\dinput8.dll
param([Parameter(Mandatory = $true)][string]$Path)

$full = (Resolve-Path -LiteralPath $Path).Path
$bytes = [System.IO.File]::ReadAllBytes($full)
$script:nSec = 0
$script:secTab = 0
$script:secInfo = @()

function GetU16([byte[]]$b, [int]$o) { return [BitConverter]::ToUInt16($b, $o) }
function GetU32([byte[]]$b, [int]$o) { return [BitConverter]::ToUInt32($b, $o) }

function GetOff([byte[]]$b, [int]$rva) {
    for ($i = 0; $i -lt $script:nSec; $i++) {
        $s = $script:secTab + $i * 40
        $vsz = GetU32 -b $b -o ($s + 8)
        $va = GetU32 -b $b -o ($s + 12)
        $rawSize = GetU32 -b $b -o ($s + 16)
        $raw = GetU32 -b $b -o ($s + 20)
        $span = [Math]::Max($vsz, $rawSize)
        if ($rva -ge $va -and $rva -lt ($va + $span)) { return [int]($raw + ($rva - $va)) }
    }
    return -1
}

function SectionOf([byte[]]$b, [int]$rva) {
    foreach ($s in $script:secInfo) {
        if ($rva -ge $s.Va -and $rva -lt ($s.Va + $s.Span)) { return $s.Name }
    }
    return '?'
}

if ($bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { throw "not a PE file: $full" }
$peOff = [int](GetU32 -b $bytes -o 0x3C)
if ((GetU32 -b $bytes -o $peOff) -ne 0x00004550) { throw "no PE signature: $full" }
$coff = $peOff + 4
$script:nSec = GetU16 -b $bytes -o ($coff + 2)
$optSize = GetU16 -b $bytes -o ($coff + 16)
$opt = $coff + 20
$is64 = ((GetU16 -b $bytes -o $opt) -eq 0x20B)
$dd = $opt + $(if ($is64) { 0x70 } else { 0x60 })
$exportRva = GetU32 -b $bytes -o $dd
$script:secTab = $opt + $optSize

$script:secInfo = for ($i = 0; $i -lt $script:nSec; $i++) {
    $s = $script:secTab + $i * 40
    $nameOff = $s
    $nm = ''
    while ($bytes[$nameOff] -ne 0 -and $nm.Length -lt 8) { $nm += [char]$bytes[$nameOff]; $nameOff++ }
    [PSCustomObject]@{
        Name = $nm
        Va   = GetU32 -b $bytes -o ($s + 12)
        Span = [Math]::Max((GetU32 -b $bytes -o ($s + 8)), (GetU32 -b $bytes -o ($s + 16)))
    }
}

"# file    : $full"
"# machine : 0x{0:X4}  {1}" -f (GetU16 -b $bytes -o $coff), ($(if ($is64) { 'PE32+' } else { 'PE32' }))
if ($exportRva -eq 0) { "# exports : none"; return }

$eo = GetOff -b $bytes -rva $exportRva
$nNames = GetU32 -b $bytes -o ($eo + 0x18)
$funcRva = GetU32 -b $bytes -o ($eo + 0x1C)
$namesRva = GetU32 -b $bytes -o ($eo + 0x20)
$ordRva = GetU32 -b $bytes -o ($eo + 0x24)
$fo = GetOff -b $bytes -rva $funcRva
$no = GetOff -b $bytes -rva $namesRva
$oo = GetOff -b $bytes -rva $ordRva

"# exports : $nNames"
$rows = for ($i = 0; $i -lt $nNames; $i++) {
    $nameRva = GetU32 -b $bytes -o ($no + $i * 4)
    $ord = GetU16 -b $bytes -o ($oo + $i * 2)
    $n = GetOff -b $bytes -rva $nameRva
    $e = $n
    while ($bytes[$e] -ne 0) { $e++ }
    $fnRva = GetU32 -b $bytes -o ($fo + $ord * 4)
    [PSCustomObject]@{
        Ordinal = $ord
        Name    = [System.Text.Encoding]::ASCII.GetString($bytes, $n, $e - $n)
        Rva     = ('0x{0:X}' -f $fnRva)
        Section = SectionOf -b $bytes -rva $fnRva
    }
}
$rows | Sort-Object Name | Format-Table -AutoSize | Out-String -Width 200
