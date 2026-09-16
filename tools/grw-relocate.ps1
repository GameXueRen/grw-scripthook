<#
.SYNOPSIS
Byte level helper for re-locating the framework's engine offsets after a
game update.

Why this exists: the offsets in scripthook_*.c are RVAs into GRW.exe. When
the game is rebuilt, code and data move - by differing amounts in different
regions, so "old address plus a shift" is not a method. What is a method is
matching the old bytes against the new file: the instruction around a site
is unchanged except for its rel32/absolute operands, and a function's
callees map through the pairs that are already known.

Everything here is read only: it never writes to a game file.

.USAGE
  pwsh tools/grw-relocate.ps1 -Cmd info
  pwsh tools/grw-relocate.ps1 -Cmd dump  -Rva 9A51930 -Before 64 -After 16
  pwsh tools/grw-relocate.ps1 -Cmd calls -Rva 188BA20 -File old
  pwsh tools/grw-relocate.ps1 -Cmd find  -Pattern "FF 87 6C 05 00 00"
  pwsh tools/grw-relocate.ps1 -Cmd find  -Pattern "E8 ?? ?? ?? ??" -File old -Sec .text
  pwsh tools/grw-relocate.ps1 -Cmd ptr   -Rva 4B90638 -File old -Sec .rdata

-File old|new picks the executable; new is the one installed in the game
folder, old is the pre-update backup. Override either with -OldExe/-NewExe.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('info', 'dump', 'calls', 'jumps', 'reloc', 'reloccall', 'find', 'ptr', 'thunk', 'rtti', 'class', 'fn', 'fnx', 'batch', 'check')]
    [string]$Cmd,

    # RVA in hex, with or without 0x, for -Cmd dump/calls/ptr.
    [string]$Rva = '',

    # Bytes before and after -Rva for -Cmd dump.
    [int]$Before = 64,
    [int]$After = 16,

    # Byte pattern for -Cmd find: "E8 ?? ?? ?? ??" (? = any byte).
    [string]$Pattern = '',

    # Which executable to look at: old (pre-update backup) or new.
    [ValidateSet('old', 'new')]
    [string]$File = 'new',

    # Section to scan for find/ptr; empty means every section.
    [string]$Sec = '',

    # Cap on hits printed.
    [int]$Max = 40,

    # Optional address range (hex RVAs) that -Cmd find searches inside.
    [string]$From = '',
    [string]$To = '',

    [string]$OldExe = 'F:\UbisoftGames\backup\GRW-0915\GRW.exe',
    [string]$NewExe = "F:\UbisoftGames\Tom Clancy's Ghost Recon Wildlands\GRW.exe"
)

$ErrorActionPreference = 'Stop'

# The scanning engine is C#: a 400 MB byte array scanned from PowerShell
# would take minutes, from compiled code it takes about a second.
if (-not ('PeFile' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

public class PeFile {
    public string Path;
    public byte[] D;
    public long ImageBase;
    public int NSec;
    public string[] SecName;
    public long[] SecVA, SecVSize, SecPraw, SecRSize;

    public static PeFile Load(string path) {
        PeFile p = new PeFile();
        p.Path = path;
        p.D = File.ReadAllBytes(path);
        int peOff = BitConverter.ToInt32(p.D, 0x3C);
        if (BitConverter.ToUInt32(p.D, peOff) != 0x00004550) throw new Exception("not a PE: " + path);
        p.NSec = BitConverter.ToUInt16(p.D, peOff + 6);
        int optSize = BitConverter.ToUInt16(p.D, peOff + 20);
        int opt = peOff + 24;
        if (BitConverter.ToUInt16(p.D, opt) != 0x20B) throw new Exception("not PE32+: " + path);
        p.ImageBase = (long)BitConverter.ToUInt64(p.D, opt + 24);
        p.SecName = new string[p.NSec];
        p.SecVA = new long[p.NSec];
        p.SecVSize = new long[p.NSec];
        p.SecPraw = new long[p.NSec];
        p.SecRSize = new long[p.NSec];
        int sh = opt + optSize;
        for (int i = 0; i < p.NSec; i++) {
            int o = sh + i * 40;
            p.SecName[i] = Encoding.ASCII.GetString(p.D, o, 8).TrimEnd('\0');
            p.SecVSize[i] = BitConverter.ToUInt32(p.D, o + 8);
            p.SecVA[i] = BitConverter.ToUInt32(p.D, o + 12);
            p.SecRSize[i] = BitConverter.ToUInt32(p.D, o + 16);
            p.SecPraw[i] = BitConverter.ToUInt32(p.D, o + 20);
        }
        return p;
    }

    public int IndexOfSec(string name) {
        for (int i = 0; i < NSec; i++) if (SecName[i] == name) return i;
        return -1;
    }

    public long SecSize(int i) {
        return SecVSize[i] == 0 ? SecRSize[i] : SecVSize[i];
    }

    public int RvaToOff(long rva) {
        for (int i = 0; i < NSec; i++) {
            long va = SecVA[i], sz = SecSize(i);
            if (rva >= va && rva < va + sz) return (int)(SecPraw[i] + (rva - va));
        }
        return -1;
    }

    public string SecOf(long rva) {
        for (int i = 0; i < NSec; i++) {
            long va = SecVA[i], sz = SecSize(i);
            if (rva >= va && rva < va + sz) return SecName[i];
        }
        return "?";
    }

    public byte[] At(long rva, int len) {
        int off = RvaToOff(rva);
        if (off < 0 || off + len > D.Length) return null;
        byte[] b = new byte[len];
        Array.Copy(D, off, b, 0, len);
        return b;
    }

    public byte[] SecBytes(int i) {
        byte[] b = new byte[SecRSize[i]];
        Array.Copy(D, SecPraw[i], b, 0, (int)SecRSize[i]);
        return b;
    }
}

public class Scan {
    public static byte[] Hex(string s) {
        string[] t = s.Split(new char[] { ' ', ',', '-', ':' }, StringSplitOptions.RemoveEmptyEntries);
        byte[] b = new byte[t.Length];
        for (int i = 0; i < t.Length; i++)
            b[i] = t[i].IndexOf('?') >= 0 ? (byte)0 : Convert.ToByte(t[i], 16);
        return b;
    }

    // 1 = compare, 0 = wildcard, per token
    public static byte[] Mask(string s) {
        string[] t = s.Split(new char[] { ' ', ',', '-', ':' }, StringSplitOptions.RemoveEmptyEntries);
        byte[] m = new byte[t.Length];
        for (int i = 0; i < t.Length; i++)
            m[i] = t[i].IndexOf('?') >= 0 ? (byte)0 : (byte)1;
        return m;
    }

    // mask: 1 = compare, 0 = wildcard
    public static long[] Find(byte[] hay, long va, byte[] needle, byte[] mask) {
        List<long> hits = new List<long>();
        int n = needle.Length;
        for (int i = 0; i + n <= hay.Length; i++) {
            int j = 0;
            for (; j < n; j++) {
                if (mask != null && mask[j] == 0) continue;
                if (hay[i + j] != needle[j]) break;
            }
            if (j == n) hits.Add(va + i);
        }
        return hits.ToArray();
    }

    // every E8 rel32 whose target is the given RVA, in one section index
    public static long[] CallsToSec(PeFile pe, long target, int si) {
        if (si < 0) return new long[0];
        byte[] hay = pe.SecBytes(si);
        long va = pe.SecVA[si];
        List<long> hits = new List<long>();
        for (int i = 0; i + 5 <= hay.Length; i++) {
            if (hay[i] != 0xE8) continue;
            int rel = BitConverter.ToInt32(hay, i + 1);
            if (va + i + 5 + rel == target) hits.Add(va + i);
        }
        return hits.ToArray();
    }

    // every 8 byte little endian value equal to base+rva, in one section
    public static long[] Ptrs(PeFile pe, long rva, string secName) {
        int si = pe.IndexOfSec(secName);
        if (si < 0) return new long[0];
        byte[] hay = pe.SecBytes(si);
        byte[] needle = BitConverter.GetBytes(pe.ImageBase + rva);
        return Find(hay, pe.SecVA[si], needle, null);
    }

    // every E9 rel32 whose target is the given RVA, in one section index
    public static long[] JumpsToSec(PeFile pe, long target, int si) {
        if (si < 0) return new long[0];
        byte[] hay = pe.SecBytes(si);
        long va = pe.SecVA[si];
        List<long> hits = new List<long>();
        for (int i = 0; i + 5 <= hay.Length; i++) {
            if (hay[i] != 0xE9) continue;
            int rel = BitConverter.ToInt32(hay, i + 1);
            if (va + i + 5 + rel == target) hits.Add(va + i);
        }
        return hits.ToArray();
    }

    // .pdata is the exception directory: one 12 byte triple per function,
    // sorted by the function's start RVA. The section has the same address
    // and size in both builds, and the order is the order of the code, so
    // the n-th function of the old build is the n-th of the new one - which
    // maps a function across a rebuild without matching a byte of it.
    public static long[][] Pdata(PeFile pe) {
        int si = pe.IndexOfSec(".pdata");
        if (si < 0) return null;
        int n = (int)(pe.SecSize(si) / 12);
        long[][] t = new long[n][];
        long va = pe.SecVA[si];
        for (int i = 0; i < n; i++) {
            t[i] = new long[] {
                Dw(pe, va + (long)i * 12),
                Dw(pe, va + (long)i * 12 + 4),
                Dw(pe, va + (long)i * 12 + 8)
            };
        }
        return t;
    }

    // Index of the function that contains this RVA (0 if none).
    public static int FnIndex(long[][] t, long rva) {
        int lo = 0, hi = t.Length - 1, best = 0;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (t[mid][0] <= rva) { best = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        return best;
    }

    // The index alone drifts by a few entries when the build adds or
    // removes a function, so the neighbourhood is scored by how much of
    // the function body is byte for byte the same. Opcodes and structure
    // offsets survive a rebuild; only the operands move, so the right
    // function scores near the top and its neighbours do not.
    public static List<string> FnCandidates(PeFile oldPe, PeFile newPe, long rva,
                                           int span, int win) {
        List<string> res = new List<string>();
        long[][] to = Pdata(oldPe);
        long[][] tn = Pdata(newPe);
        if (to == null || tn == null) return res;
        int i = FnIndex(to, rva);
        int len = (int)Math.Min(to[i][1] - to[i][0], win);
        byte[] ob = oldPe.At(to[i][0], len);
        if (ob == null) return res;
        int from = Math.Max(0, i - span), to2 = Math.Min(tn.Length - 1, i + span);
        for (int k = from; k <= to2; k++) {
            int n = (int)Math.Min(tn[k][1] - tn[k][0], len);
            if (n < 8) continue;
            byte[] nb = newPe.At(tn[k][0], n);
            if (nb == null) continue;
            int same = 0;
            for (int j = 0; j < n; j++) if (ob[j] == nb[j]) same++;
            int pct = same * 100 / n;
            if (pct >= 70)
                res.Add(string.Format("idx {0} ({1:+0;-0;0}) rva {2:X} len {3} body {4}%",
                                      k, k - i, tn[k][0], tn[k][1] - tn[k][0], pct));
        }
        return res;
    }

    // The whole list at once, so a sweep costs one load of both files
    // instead of one per constant.
    public static List<string> Batch(PeFile oldPe, PeFile newPe, string[] lines,
                                     int span, int win) {
        List<string> res = new List<string>();
        long[][] to = Pdata(oldPe);
        long[][] tn = Pdata(newPe);
        foreach (string raw in lines) {
            string line = raw.Trim();
            if (line.Length == 0 || line[0] == '#') continue;
            int sp = line.IndexOfAny(new char[] { ' ', '\t' });
            if (sp <= 0) continue;
            string label = line.Substring(0, sp).Trim();
            string rest = line.Substring(sp).Trim();
            // The address is the second token and nothing else: the list
            // carries trailing notes ("(also entity.c ...)") on the entries
            // that exist in more than one file.
            int sp2 = rest.IndexOfAny(new char[] { ' ', '\t' });
            string hex = sp2 > 0 ? rest.Substring(0, sp2) : rest;
            long rva;
            try { rva = Convert.ToInt64(hex.StartsWith("0x") ? hex.Substring(2) : hex, 16); }
            catch { res.Add(label + " : bad address '" + hex + "'"); continue; }

            int i = FnIndex(to, rva);
            long[] oe = to[i];
            bool start = (oe[0] == rva);
            int len = (int)Math.Min(oe[1] - oe[0], win);
            byte[] ob = oldPe.At(oe[0], len);
            string best = "";
            int bestPct = 0;
            int bestIdx = 0;
            if (ob != null) {
                long[] range = new long[1];
                for (int k = Math.Max(0, i - span);
                     k <= Math.Min(tn.Length - 1, i + span); k++) {
                    int n = (int)Math.Min(tn[k][1] - tn[k][0], len);
                    if (n < 8) continue;
                    byte[] nb = newPe.At(tn[k][0], n);
                    if (nb == null) continue;
                    int same = 0;
                    for (int j = 0; j < n; j++) if (ob[j] == nb[j]) same++;
                    int pct = same * 100 / n;
                    if (pct > bestPct) { bestPct = pct; bestIdx = k - i; best = tn[k][0].ToString("X"); }
                }
            }
            string tag = start ? "" : "  (not a body start)";
            if (best == "")
                res.Add(string.Format("{0,-18} {1,-10} -> no body match{2}", label, rva.ToString("X"), tag));
            else if (!start)
                // An interior site, or data: .pdata only bounds the body it
                // sits in, so the answer is the neighbourhood and not an
                // address. Printing a score here read as precision the tool
                // does not have, which is worse than saying so.
                res.Add(string.Format("{0,-18} {1,-10} -> near {2,-10} (neighbourhood only){3}",
                                      label, rva.ToString("X"), best, tag));
            else
                res.Add(string.Format("{0,-18} {1,-10} -> {2,-10} ({3}% idx{4:+0;-0;0}){5}",
                                      label, rva.ToString("X"), best, bestPct, bestIdx, tag));
        }
        return res;
    }

    // A vtable and its class name are tied together by RTTI, and the tie
    // survives a rebuild even when every address in it moved: the slot at
    // vtable-8 points at a RTTICompleteObjectLocator, whose
    // pTypeDescriptor field is the RVA of a TypeDescriptor, whose name
    // (at +0x10 on x64) is the mangled class name. So the name can be read
    // out of an old build's vtable and then searched for in the new one.
    public static string Str(PeFile pe, long rva, int max) {
        int off = pe.RvaToOff(rva);
        if (off < 0) return "";
        int end = off;
        while (end < pe.D.Length && end - off < max && pe.D[end] != 0) end++;
        return Encoding.ASCII.GetString(pe.D, off, end - off);
    }

    public static long Q(PeFile pe, long rva) {
        int off = pe.RvaToOff(rva);
        if (off < 0 || off + 8 > pe.D.Length) return -1;
        return (long)BitConverter.ToUInt64(pe.D, off);
    }

    public static uint Dw(PeFile pe, long rva) {
        int off = pe.RvaToOff(rva);
        if (off < 0 || off + 4 > pe.D.Length) return 0xFFFFFFFF;
        return BitConverter.ToUInt32(pe.D, off);
    }

    // vtable -> "name|td|col", or "" when the slot is not RTTI.
    public static string NameOfVtable(PeFile pe, long vtRva) {
        long colVa = Q(pe, vtRva - 8);
        if (colVa <= 0) return "";
        long colRva = colVa - pe.ImageBase;
        if (colRva <= 0) return "";
        uint tdRva = Dw(pe, colRva + 0xC);           // x64: an RVA
        if (tdRva == 0xFFFFFFFF) return "";
        string nm = Str(pe, (long)tdRva + 0x10, 220);
        if (nm.Length == 0 || nm[0] != '.') nm = Str(pe, tdRva, 220);
        return nm + "|" + (pe.ImageBase + tdRva).ToString("X") + "|" + colVa.ToString("X");
    }

    // class name -> every vtable that names it.
    public static List<string> VtablesOfClass(PeFile pe, string name) {
        List<string> found = new List<string>();
        byte[] nm = Encoding.ASCII.GetBytes(name);
        for (int si = 0; si < pe.NSec; si++) {
            byte[] hay = pe.SecBytes(si);
            long va = pe.SecVA[si];
            foreach (long hit in Find(hay, va, nm, null)) {
                // the name sits inline in the TypeDescriptor: +0x10 on x64,
                // and at +0 for the older layout, so both are tried.
                long[] tdCand = new long[] { hit - 0x10, hit };
                foreach (long tdRva in tdCand) {
                    if (tdRva <= 0) continue;
                    byte[] tdNeedle = BitConverter.GetBytes((uint)tdRva);
                    for (int sj = 0; sj < pe.NSec; sj++) {
                        byte[] hay2 = pe.SecBytes(sj);
                        long va2 = pe.SecVA[sj];
                        foreach (long hit2 in Find(hay2, va2, tdNeedle, null)) {
                            long colRva = hit2 - 0xC;
                            if (colRva <= 0) continue;
                            byte[] colNeedle = BitConverter.GetBytes(pe.ImageBase + colRva);
                            for (int sk = 0; sk < pe.NSec; sk++) {
                                byte[] hay3 = pe.SecBytes(sk);
                                foreach (long hit3 in Find(hay3, pe.SecVA[sk], colNeedle, null)) {
                                    string s = string.Format(
                                        "vt={0:X} col={1:X} td={2:X} ({3})",
                                        hit3 + 8, colRva, tdRva, name);
                                    if (!found.Contains(s)) found.Add(s);
                                }
                            }
                        }
                    }
                }
            }
        }
        return found;
    }

    // The same search inside one address range: for the leaf helpers that
    // have no .pdata entry, a neighbourhood is the only thing that narrows
    // a pattern as generic as "load a global and return".
    public static long[] FindInRange(PeFile pe, long from, long to,
                                     byte[] needle, byte[] mask) {
        List<long> hits = new List<long>();
        for (int si = 0; si < pe.NSec; si++) {
            long va = pe.SecVA[si], sz = pe.SecSize(si);
            long lo = Math.Max(from, va), hi = Math.Min(to, va + sz);
            if (hi <= lo) continue;
            int a = pe.RvaToOff(lo), b = pe.RvaToOff(hi - 1);
            if (a < 0 || b < 0) continue;
            byte[] hay = new byte[b + 1 - a];
            Array.Copy(pe.D, a, hay, 0, hay.Length);
            foreach (long hit in Find(hay, lo, needle, mask)) hits.Add(hit);
        }
        return hits.ToArray();
    }

    // Every RIP relative operand that points at base+rva in one section.
    // Returns the address of the modrm byte (mod=00, rm=101), so the rel32
    // is at +1 and the instruction ends at +5. Any opcode is accepted: the
    // caller votes across the references it finds, which is what filters
    // the occasional byte pattern that is not really an operand.
    public static long[] RipRefs(PeFile pe, long rva, int si) {
        if (si < 0) return new long[0];
        byte[] hay = pe.SecBytes(si);
        long va = pe.SecVA[si];
        long want = pe.ImageBase + rva;
        List<long> hits = new List<long>();
        for (int i = 1; i + 5 <= hay.Length; i++) {
            if ((hay[i] & 0xC7) != 0x05) continue;      // mod=00 rm=101
            int rel = BitConverter.ToInt32(hay, i + 1);
            if (va + i + 5 + rel == want) hits.Add(va + i);
        }
        return hits.ToArray();
    }
}
'@
}

$exePath = if ($File -eq 'old') { $OldExe } else { $NewExe }
$pe = [PeFile]::Load($exePath)

function ToRva([string]$s) {
    if (-not $s) { throw '-Rva is required' }
    $s = $s -replace '^0x', ''
    return [Convert]::ToInt64($s, 16)
}

switch ($Cmd) {
    'info' {
        "file      : $($pe.Path)"
        "size      : $($pe.D.Length) bytes"
        "imagebase : $('{0:X}' -f $pe.ImageBase)"
        "sections  :"
        for ($i = 0; $i -lt $pe.NSec; $i++) {
            "  {0,-8} va={1:X8} vsize={2:X8} raw={3:X8} rsize={4:X8}" -f `
                $pe.SecName[$i], $pe.SecVA[$i], $pe.SecVSize[$i], $pe.SecPraw[$i], $pe.SecRSize[$i]
        }
    }

    'dump' {
        $rva = ToRva $Rva
        $start = $rva - $Before
        $len = $Before + $After
        $b = $pe.At($start, $len)
        if (-not $b) { throw "cannot read $($len) bytes at ${Rva}" }
        "dump {0:X} .. {1:X}  ({2})" -f $start, ($start + $len), $pe.SecOf($rva)
        for ($i = 0; $i -lt $len; $i += 16) {
            $hex = ($b[$i..([Math]::Min($i + 15, $len - 1))] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
            $mark = if ($start + $i -le $rva -and $rva -lt $start + $i + 16) { ' <=' } else { '' }
            "  {0:X}: {1}{2}" -f ($start + $i), $hex.PadRight(48), $mark
        }
    }

    'calls' {
        $rva = ToRva $Rva
        # Code is spread over more than one section in this executable, so
        # the default is every section; -Sec narrows it.
        $secs = if ($Sec) { @($Sec) } else { 0..($pe.NSec - 1) | ForEach-Object { $pe.SecName[$_] } }
        $total = 0
        foreach ($s in $secs) {
            $si = $pe.IndexOfSec($s)
            if ($si -lt 0) { continue }
            $hits = [Scan]::CallsToSec($pe, $rva, $si)
            if ($hits.Count -eq 0) { continue }
            $total += $hits.Count
            "$s : $($hits.Count) call site(s) to $('{0:X}' -f $rva)"
            $hits | Select-Object -First $Max | ForEach-Object { "  {0:X}" -f $_ }
        }
        "total: $total"
    }

    'find' {
        if (-not $Pattern) { throw '-Pattern is required' }
        $needle = [Scan]::Hex($Pattern)
        $mask = [Scan]::Mask($Pattern)
        if ($From -or $To) {
            $lo = if ($From) { ToRva $From } else { 0 }
            $hi = if ($To) { ToRva $To } else { 0x7FFFFFFF }
            $hits = [Scan]::FindInRange($pe, $lo, $hi, $needle, $mask)
            "hits in {0:X}..{1:X}: {2}" -f $lo, $hi, $hits.Count
            $hits | Select-Object -First $Max | ForEach-Object { "  {0:X}" -f $_ }
            break
        }
        $secs = if ($Sec) { @($Sec) } else { 0..($pe.NSec - 1) | ForEach-Object { $pe.SecName[$_] } }
        $total = 0
        foreach ($s in $secs) {
            $si = $pe.IndexOfSec($s)
            if ($si -lt 0) { continue }
            $hay = $pe.SecBytes($si)
            $hits = [Scan]::Find($hay, $pe.SecVA[$si], $needle, $mask)
            if ($hits.Count -eq 0) { continue }
            $total += $hits.Count
            "$s : $($hits.Count) hit(s)"
            $hits | Select-Object -First $Max | ForEach-Object { "  {0:X}" -f $_ }
            if ($hits.Count -gt $Max) { "  ... $($hits.Count - $Max) more" }
        }
        "total: $total"
    }

    # The engine's own thunks are a bare E9 rel32 between CC padding (an
    # incremental link leaves one per imported function). Reading the tail
    # of one by hand is where a digit gets dropped, so the tool does it.
    'thunk' {
        $rva = ToRva $Rva
        $b = $pe.At($rva, 5)
        if (-not $b) { throw "cannot read 5 bytes at ${Rva}" }
        $op = if ($b[0] -eq 0xE9) { 'jmp' } elseif ($b[0] -eq 0xE8) { 'call' } else { '' }
        if (-not $op) { throw ("not a jmp/call at {0:X}: first byte {1:X2}" -f $rva, $b[0]) }
        $rel = [BitConverter]::ToInt32($b, 1)
        $tgt = [int64]$rva + 5 + [int64]$rel
        $bytes = ($b | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
        $rvaHex = [Convert]::ToString([int64]$rva, 16)
        $relHex = [Convert]::ToString([int64]$rel, 16)
        $tgtHex = [Convert]::ToString([int64]$tgt, 16)
        "$op at $rvaHex ($($pe.SecOf($rva))) : $bytes  rel=$relHex  -> $tgtHex ($($pe.SecOf($tgt)))"
    }

    'jumps' {
        $rva = ToRva $Rva
        $secs = if ($Sec) { @($Sec) } else { 0..($pe.NSec - 1) | ForEach-Object { $pe.SecName[$_] } }
        $total = 0
        foreach ($s in $secs) {
            $si = $pe.IndexOfSec($s)
            if ($si -lt 0) { continue }
            $hits = [Scan]::JumpsToSec($pe, $rva, $si)
            if ($hits.Count -eq 0) { continue }
            $total += $hits.Count
            "$s : $($hits.Count) jump(s) to $('{0:X}' -f $rva)"
            $hits | Select-Object -First $Max | ForEach-Object { "  {0:X}" -f $_ }
        }
        "total: $total"
    }

    # Take every place the old build pointed at this address and find the
    # same place in the new build: the bytes around a reference are stable
    # (only its rel32 moves), so the window matches, and reading the new
    # rel32 out of the match gives the address the constant moved to. The
    # references vote, because one window can land on a lookalike.
    'reloc' {
        $rva = ToRva $Rva
        $old = [PeFile]::Load($OldExe)
        $new = [PeFile]::Load($NewExe)
        $votes = @{}
        $refs = 0
        $missed = 0
        for ($si = 0; $si -lt $old.NSec; $si++) {
            $refList = [Scan]::RipRefs($old, $rva, $si)
            if ($refList.Count -eq 0) { continue }
            # one run: parse a list of hex bytes with a mask, done once
            $newHay = @()
            for ($sj = 0; $sj -lt $new.NSec; $sj++) {
                $newHay += , @($sj, $new.SecBytes($sj), $new.SecVA[$sj])
            }
            foreach ($m in $refList) {
                $refs++
                $winLen = 20
                $b = $old.At($m - 6, $winLen)
                if (-not $b) { continue }
                $tol = ($b | ForEach-Object { '{0:X2}' -f $_ })
                for ($k = 7; $k -le 10; $k++) { $tol[$k] = '??' }
                $pat = $tol -join ' '
                $needle = [Scan]::Hex($pat)
                $mask = [Scan]::Mask($pat)
                $found = 0
                foreach ($h in $newHay) {
                    foreach ($hit in [Scan]::Find($h[1], $h[2], $needle, $mask)) {
                        $rel = [BitConverter]::ToInt32($new.D, $new.RvaToOff($hit + 7))
                        $tgt = $hit + 11 + $rel
                        $key = '{0:X}' -f $tgt
                        $votes[$key] = 1 + $votes[$key]
                        $found++
                    }
                }
                if ($found -eq 0) { $missed++ }
            }
        }
        "old refs: $refs, refs whose window did not match: $missed"
        $votes.GetEnumerator() | Sort-Object -Property Value -Descending |
            Select-Object -First 12 | ForEach-Object { "  {0} : {1} vote(s)" -f $_.Key, $_.Value }
    }

    # Where a function (or a thunk) moved to, voted by the code that calls
    # it: a prologue is a common shape and can match in several places, but
    # the twenty bytes around each of its call sites are unique to it. Every
    # old call site is looked up in the new build and the call it finds
    # there votes for a target; the target with the most votes is the one.
    'reloccall' {
        $rva = ToRva $Rva
        $old = [PeFile]::Load($OldExe)
        $new = [PeFile]::Load($NewExe)
        $newHay = @()
        for ($sj = 0; $sj -lt $new.NSec; $sj++) {
            $newHay += , @($sj, $new.SecBytes($sj), $new.SecVA[$sj])
        }
        $votes = @{}
        $refs = 0
        $missed = 0
        for ($si = 0; $si -lt $old.NSec; $si++) {
            $use = [Scan]::CallsToSec($old, $rva, $si)
            $refs += $use.Count
            # A handful of call sites is enough for the vote to land, and
            # each one costs a pass over the new file.
            foreach ($c in ($use | Select-Object -First 8)) {
                $b = $old.At($c - 10, 23)
                if (-not $b) { continue }
                $tol = ($b | ForEach-Object { '{0:X2}' -f $_ })
                for ($k = 10; $k -le 14; $k++) { $tol[$k] = '??' }
                $pat = $tol -join ' '
                $needle = [Scan]::Hex($pat)
                $mask = [Scan]::Mask($pat)
                $found = 0
                foreach ($h in $newHay) {
                    foreach ($hit in [Scan]::Find($h[1], $h[2], $needle, $mask)) {
                        $rel = [BitConverter]::ToInt32($new.D, $new.RvaToOff($hit + 11))
                        $tgt = $hit + 15 + $rel
                        $key = '{0:X}' -f $tgt
                        $votes[$key] = 1 + $votes[$key]
                        $found++
                    }
                }
                if ($found -eq 0) { $missed++ }
            }
        }
        "old call sites: $refs (windows that matched nowhere: $missed)"
        $votes.GetEnumerator() | Sort-Object -Property Value -Descending |
            Select-Object -First 10 | ForEach-Object { "  {0} : {1} vote(s)" -f $_.Key, $_.Value }
    }

    # Which function this address is, and where that function is now. The
    # section is sorted by address and its address and size are the same in
    # both builds, so the index carries the function across a rebuild.
    'fn' {
        $rva = ToRva $Rva
        $old = [PeFile]::Load($OldExe)
        $new = [PeFile]::Load($NewExe)
        $to = [Scan]::Pdata($old)
        $tn = [Scan]::Pdata($new)
        if (-not $to -or -not $tn) { throw '.pdata not found in one of the files' }
        "pdata entries: old $($to.Length), new $($tn.Length)"
        $i = [Scan]::FnIndex($to, $rva)
        $o = $to[$i]
        "old[$i] " + [Convert]::ToString($o[0], 16) + " .. " +
            [Convert]::ToString($o[1], 16) + "   (asked for " +
            [Convert]::ToString($rva, 16) + ")"
        for ($k = $i - 2; $k -le $i + 2; $k++) {
            if ($k -lt 0 -or $k -ge $tn.Length) { continue }
            $n = $tn[$k]
            $d = $n[0] - $to[$k][0]
            $mark = if ($k -eq $i) { '   <-' } else { '' }
            "  new[$k] " + [Convert]::ToString($n[0], 16) + " .. " +
                [Convert]::ToString($n[1], 16) + "   delta " +
                [Convert]::ToString($d, 16) + $mark
        }
    }

    # Sweep a list of old addresses: one "LABEL <hex>" per line, '#' comments.
    'batch' {
        if (-not $Pattern) { throw '-Pattern is the path of the list file' }
        $lines = Get-Content -LiteralPath $Pattern
        $old = [PeFile]::Load($OldExe)
        $new = [PeFile]::Load($NewExe)
        # 192 bytes of body, not 64: the UI and scene helpers are small
        # siblings whose first lines look alike, and a short window scores
        # one of them as a match for another.
        $res = [Scan]::Batch($old, $new, [string[]]$lines, 60, 192)
        $res | ForEach-Object { $_ }
    }

    # The sweep, then the comparison that makes it a checklist: for every
    # entry the tool maps precisely - a body start - read what the sources
    # carry for that label right now and line the two up. A copy that
    # disagrees is a constant that was never re-pinned, which is the worklist
    # for the next update; agreement is the confirmation that a re-pin landed.
    #
    # EVERY definition is read, not the first: the anchors that exist in more
    # than one file are the ones that go wrong, because re-pinning one copy
    # leaves the other walking from the old address. A label with one copy
    # that agrees and one that does not is exactly that story, and the line
    # says so.
    #
    # Entries whose old address was an interior site or data are skipped:
    # .pdata bounds the body they sit in, not the address, so there is
    # nothing to compare - those need a content anchor.
    'check' {
        if (-not $Pattern) { throw '-Pattern is the path of the list file' }
        $srcRoot = Split-Path -Parent $PSScriptRoot
        $lines = Get-Content -LiteralPath $Pattern
        $old = [PeFile]::Load($OldExe)
        $new = [PeFile]::Load($NewExe)

        $defs = @{}
        Get-ChildItem -LiteralPath $srcRoot -Recurse -File -Include *.c, *.h |
            Where-Object { $_.FullName -notmatch '\\third_party\\' } |
            Select-String -Pattern '^\s*#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(.+)$' |
            ForEach-Object {
                $n = $_.Matches[0].Groups[1].Value
                $v = $_.Matches[0].Groups[2].Value
                if ($v -match '0x([0-9A-Fa-f]+)') {
                    if (-not $defs.ContainsKey($n)) {
                        $defs[$n] = New-Object System.Collections.ArrayList
                    }
                    [void]$defs[$n].Add(@{
                        File  = (Split-Path -Leaf $_.Path)
                        Line  = $_.LineNumber
                        Value = [Convert]::ToInt64($Matches[1], 16)
                    })
                }
            }

        $agree = 0; $stale = 0; $unmapped = 0; $skipped = 0
        foreach ($r in [Scan]::Batch($old, $new, [string[]]$lines, 60, 192)) {
            if ($r -notmatch '^(\S+)\s+[0-9A-F]+\s+->') { continue }
            $label = $Matches[1]
            if ($r -match 'not a body start') { $skipped++; continue }
            if ($r -match '-> no body match') {
                $unmapped++
                "no map        $label"
                continue
            }
            if ($r -notmatch '->\s+([0-9A-F]+)\s+\((\d+)%') { continue }
            $mapped = [Convert]::ToInt64($Matches[1], 16)
            $score = $Matches[2]
            if (-not $defs.ContainsKey($label)) {
                $unmapped++
                "no define     $label  (tool $($mapped.ToString('X')) at $score%)"
                continue
            }
            $copies = $defs[$label]
            $right = @($copies | Where-Object { $_.Value -eq $mapped }).Count
            if ($right -eq $copies.Count) { $agree++; continue }
            foreach ($c in ($copies | Where-Object { $_.Value -ne $mapped })) {
                $stale++
                $note = if ($right -gt 0) { "   (another copy already agrees)" } `
                        else { "" }
                "STALE?  {0,-16} {1}:{2} holds {3:X}, the tool maps it to {4:X} at {5}%{6}" -f `
                    $label, $c.File, $c.Line, $c.Value, $mapped, $score, $note
            }
        }
        ""
        "agree $agree, stale $stale, unmapped $unmapped, skipped (interior or data) $skipped"
    }

    # The same lookup, but scored by the function body: .pdata gets the
    # neighbourhood right, the bytes pick the entry out of it.
    'fnx' {
        $rva = ToRva $Rva
        $old = [PeFile]::Load($OldExe)
        $new = [PeFile]::Load($NewExe)
        $hits = [Scan]::FnCandidates($old, $new, $rva, 40, 96)
        "body candidates for " + [Convert]::ToString($rva, 16) + ": $($hits.Count)"
        $hits | ForEach-Object { "  $_" }
    }

    # What class a vtable belongs to, read through RTTI.
    'rtti' {
        $rva = ToRva $Rva
        $s = [Scan]::NameOfVtable($pe, $rva)
        if (-not $s) { "no RTTI behind $('{0:X}' -f $rva) ($($pe.SecOf($rva)))"; break }
        $p = $s.Split('|')
        # Spelled out rather than through -f: an Int64 reaches that format
        # operator as a string often enough that the address came out in
        # decimal, which reads as a different address entirely.
        "vtable " + [Convert]::ToString([int64]$rva, 16) + " (" + $pe.SecOf($rva) + ")"
        "  name " + $p[0]
        "  td   " + $p[1] + "    col " + $p[2]
    }

    # Where a class's vtable is, looked up by its mangled name.
    'class' {
        if (-not $Pattern) { throw '-Pattern is required (part of the mangled class name)' }
        $hits = [Scan]::VtablesOfClass($pe, $Pattern)
        if ($hits.Count -eq 0) { "no vtable naming '$Pattern' in $($pe.Path)"; break }
        "class '$Pattern' ($File build): $($hits.Count) hit(s)"
        $hits | Select-Object -First $Max | ForEach-Object { "  $_" }
    }

    'ptr' {
        $rva = ToRva $Rva
        $secs = if ($Sec) { @($Sec) } else { 0..($pe.NSec - 1) | ForEach-Object { $pe.SecName[$_] } }
        foreach ($s in $secs) {
            $hits = [Scan]::Ptrs($pe, $rva, $s)
            if ($hits.Count -eq 0) { continue }
            "pointers to {0:X} in {1}: {2}" -f $rva, $s, $hits.Count
            $hits | Select-Object -First $Max | ForEach-Object { "  {0:X}" -f $_ }
        }
    }
}
