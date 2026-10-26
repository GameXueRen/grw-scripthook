"""快照 GRW.exe 里 HUD 相关内存区间，用于开关前后差分。

用法:
    python hud_memsnap.py snap <pid> <out.pkl>
    python hud_memsnap.py diff <a.pkl> <b.pkl>

只扫与 HUD 有关的两个区间（bootstrap 映射区 + 主堆区），避免读整个 5 GB 进程。
"""
import ctypes
import ctypes.wintypes as wt
import pickle
import sys

k32 = ctypes.windll.kernel32
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400

# 与 HUD 相关的区间。默认三段；模块基址可用 --base/--size 覆盖。
RANGES = [
    (0x7FF400000000, 0x7FF500000000),   # bootstrap 解包映射区（配置对象）
    (0x1C5C000000, 0x1C70000000),       # 主堆（元素实例、渲染节点）
    (0x7FF77E3F0000, 0x7FF7969AA000),   # GRW.exe 映像（.data 全局变量）
]

MEM_COMMIT = 0x1000


class MBI(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_void_p),
        ("AllocationBase", ctypes.c_void_p),
        ("AllocationProtect", wt.DWORD),
        ("RegionSize", ctypes.c_size_t),
        ("State", wt.DWORD),
        ("Protect", wt.DWORD),
        ("Type", wt.DWORD),
    ]


def snap(pid, out):
    h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    if not h:
        raise SystemExit(f"OpenProcess failed, err={k32.GetLastError()}")
    result = {}
    total = 0
    for lo, hi in RANGES:
        addr = lo
        while addr < hi:
            mbi = MBI()
            if not k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
                break
            size = mbi.RegionSize or 0x1000
            if mbi.State == MEM_COMMIT and (mbi.Protect & 0xFF) not in (0x01,):  # 跳过 NOACCESS
                buf = ctypes.create_string_buffer(size)
                got = ctypes.c_size_t(0)
                if k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)) and got.value:
                    result[addr] = buf.raw[:got.value]
                    total += got.value
            addr += size
    k32.CloseHandle(h)
    with open(out, "wb") as f:
        pickle.dump(result, f, protocol=4)
    print(f"snap ok: {len(result)} regions, {total/1e6:.1f} MB -> {out}")


def diff(pa, pb):
    A = pickle.load(open(pa, "rb"))
    B = pickle.load(open(pb, "rb"))
    rows = []
    for base, da in A.items():
        db = B.get(base)
        if db is None or len(db) != len(da):
            continue
        if da == db:
            continue
        for i in range(len(da)):
            if da[i] != db[i]:
                rows.append((base + i, da[i], db[i]))
    print(f"changed bytes: {len(rows)}")
    # 只打印 0<->1 的（开关最可能）
    flips = [r for r in rows if {r[1], r[2]} <= {0, 1}]
    print(f"0<->1 flips: {len(flips)}")
    for a, x, y in flips[:80]:
        print(f"  {a:012X}  {x} -> {y}")
    if len(flips) > 80:
        print(f"  ... {len(flips)-80} more")
    # 其它变化按区域聚合，便于看哪些对象被动过
    from collections import Counter
    c = Counter((a >> 12) for a, _, _ in rows)
    print("dirty pages (top 15):")
    for page, n in c.most_common(15):
        print(f"  0x{page<<12:012X}  {n} bytes")


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "snap":
        snap(int(sys.argv[2]), sys.argv[3])
    elif cmd == "diff":
        diff(sys.argv[2], sys.argv[3])
