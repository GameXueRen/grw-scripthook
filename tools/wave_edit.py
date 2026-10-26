#!/usr/bin/env python3
"""wave_edit.py - 增援 / 追击「波次与载具编组」读写器.

覆盖 bootstrap 里两个资源类:

  0xB0F8536E  WaveSetting / DBWaveSpawner
      一次增援 / 一次追击由若干「槽位」组成, 每个槽位引用一个伏击模板,
      槽位自己还带三个浮点参数。槽位数是 +0x1D 的一个字节。

  0x76BBA011  DBAmbush / DBOnDemandVehicle
      一个「载具编组」: 头部 + 若干 {u16 flag=1, u64 id} 记录 + 尾部。

已由全库样本反向确认的布局 (详见 docstring 末尾的「依据」):

  WaveSetting
    +0x00  u64   自身 id
    +0x08  u32   class hash (0xB0F8536E)
    +0x0C  u32   1
    +0x10  u32   0xF8
    +0x14  8 B   ...
    +0x1C  5 B   ...
    +0x1D  u8    槽位数                         <-- 唯一的计数
    +0x1E  2 B   ...
    +0x44  槽位记录 #0
          每个记录 = [u64 模板 id][若干 0][3 x float][...]
          记录长度不固定: 地面模板 82 B, 空中模板 64 B
          所以本工具按「相邻槽位 id 的偏移差」推断每条记录的长度,
          最后一个槽位用 len(data) - 偏移 收尾。

  DBAmbush
    +0x00  u64   自身 id
    +0x08  u32   class hash (0x76BBA011)
    +0x22  载具记录 #0 {u16 flag=1, u64 vehId} + {u16 flag=1, u64 crewId}
    ...
    载具组之间/尾部还夹着 4 字节字段, 尾部 20~25 B 不是定长;
    因此本工具对这类资源**只做等长 id 替换**, 不做插记录。

用法
  python wave_edit.py list     <container> <resource>
  python wave_edit.py set      <container> <resource> <slot> <new-id>
  python wave_edit.py grow     <container> <resource> <n> [<template-id>]
  python wave_edit.py clone    <container> <dst-resource> <src-resource> [<template-id>]
  python wave_edit.py ambush   <container> <resource>
  python wave_edit.py selftest <container>

  <slot> 是槽位下标 (0 起), <new-id> / <template-id> 支持 0x0123456789AB 或资源名。

自检 (`selftest`) 会对库里所有 0xB0F8536E 资源做一次解析,
校验「+0x1D 的槽位数 == 实际扫到的槽位引用数」, 并报告例外。
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

# --- bootstrap_edit 的位置 --------------------------------------------------
_HERE = os.path.dirname(os.path.abspath(__file__))
_CANDIDATES = [
    os.path.join(_HERE, "..", "out", "nightvision", "_work", "doc-backup"),
    os.path.join(_HERE, "..", "..", "out", "nightvision", "_work", "doc-backup"),
    r"F:/UbisoftGames/GameXueRen/grw-scripthook/out/nightvision/_work/doc-backup",
]
for _p in _CANDIDATES:
    if os.path.isfile(os.path.join(_p, "bootstrap_edit.py")):
        sys.path.insert(0, os.path.abspath(_p))
        break
else:                                        # noqa: PLW0120
    raise SystemExit("找不到 bootstrap_edit.py —— 用 PYTHONPATH 指到 doc-backup 目录")
import bootstrap_edit as be                   # noqa: E402

CLASS_WAVE = 0xB0F8536E
CLASS_VEHICLE_TEMPLATE = 0x76BBA011

HEADER_LEN = 0x44          # WaveSetting: 第一个槽位之前
COUNT_OFFSET = 0x1D        # WaveSetting: 槽位数 (u8)
FIRST_SLOT = 0x44          # WaveSetting: 第一个槽位记录
FLOAT_OFFSET = 0x20        # WaveSetting 槽位: 三个 float 的起点 (50 / 100 / 500)
ROLE_OFFSET = 0x30         # WaveSetting 槽位: 链上角色字段 (03=中间 05=最后有效 全0=终止)
VEH_FIRST = 0x22           # DBAmbush: 第一条载具记录


# ---------------------------------------------------------------------------
# 底层: 在资源数据里找「id 引用」
# ---------------------------------------------------------------------------

def scan_ids(data: bytes, container, start: int = 0x08):
    """扫出所有落在容器 id 表里的 8 字节引用.

    返回 [(offset, id)]; 按偏移升序。id 空间很稀疏, 偶然命中几乎不会发生,
    但对 0xB0F8536E 这种含浮点/计数的结构仍按 8 字节严格匹配。
    """
    out = []
    for at in range(start, len(data) - 7):
        ident = int.from_bytes(data[at:at + 8], "little")
        if ident > 0xFFFF and ident in container.by_id:
            out.append((at, ident))
    return out


def _u8(data, at):
    return data[at]


def read_f32_halfswap(buf, at):
    """读「高 16 位在前」的 float32.

    槽位里的三个参数存成 [hi16 LE][lo16 LE]:
        50.0  -> 48 42 00 00      100.0 -> c8 42 00 00
    直接当 LE float 读会得到 2.4e-41 这种垃圾值。
    """
    hi, lo = struct.unpack_from("<HH", buf, at)
    return struct.unpack("<f", struct.pack("<I", (hi << 16) | lo))[0]


def write_f32_halfswap(buf, at, value):
    bits = struct.unpack("<I", struct.pack("<f", value))[0]
    struct.pack_into("<HH", buf, at, bits >> 16, bits & 0xFFFF)


# ---------------------------------------------------------------------------
# WaveSetting
# ---------------------------------------------------------------------------

class WaveSetting:
    """一次增援 / 一次追击的「槽位表」。"""

    def __init__(self, data: bytes, container):
        self.raw = bytes(data)
        self.container = container
        self.ident = int.from_bytes(self.raw[0:8], "little")
        self.slots = scan_ids(self.raw, container, FIRST_SLOT)
        self.count = _u8(self.raw, COUNT_OFFSET)

    # -- 查询 ---------------------------------------------------------------
    def slot_offsets(self):
        return [at for at, _ in self.slots]

    def slot_lengths(self):
        offs = self.slot_offsets()
        return [offs[i + 1] - offs[i] for i in range(len(offs) - 1)] + \
               [len(self.raw) - offs[-1]] if offs else []

    def record_geometry(self):
        """(步长, 记录长度, 分隔长度) —— 槽位记录的排布.

        实测 222 B / 304 B 的排布是:
            记录 72 B + 分隔 10 B + 记录 72 B (+ 分隔 10 B + 记录 72 B)
        所以「相邻槽位 id 的间隔」= 82, 而最后一个槽位只有 72 B。
        """
        offs = self.slot_offsets()
        if len(offs) < 2:
            raise ValueError("只有一个槽位, 无法推断步长")
        steps = {offs[i + 1] - offs[i] for i in range(len(offs) - 1)}
        if len(steps) != 1:
            raise ValueError(f"槽位步长不一致: {sorted(steps)}")
        step = steps.pop()
        record = len(self.raw) - offs[-1]
        return step, record, step - record

    def templates(self):
        return [ident for _, ident in self.slots]

    def describe(self):
        lines = [f"  class 0x{CLASS_WAVE:08X}  {len(self.raw)} B  "
                 f"槽位数(+0x1D)={self.count}  实扫到 {len(self.slots)} 个引用"]
        for index, at in enumerate(self.slot_offsets()):
            ident = self.raw[at:at + 8]
            ident = int.from_bytes(ident, "little")
            tail = len(self.raw) - at if index == len(self.slots) - 1 \
                else self.slot_offsets()[index + 1] - at
            lines.append(f"    [{index}] @0x{at:03X}  {tail:>3} B  角色 {self.slot_role(index):<4} "
                         f"0x{ident:012X}  {self.container.name_of(ident) or '?'}")
        # 槽位里的三个浮点 (实测 50 / 100 / 500, 与"距离/时间"有关)
        if self.slots:
            at = self.slot_offsets()[0]
            floats = [read_f32_halfswap(self.raw, at + FLOAT_OFFSET + 4 * i)
                      for i in range(3)]
            lines.append("    槽位浮点(+0x%X): " % FLOAT_OFFSET
                         + " / ".join(f"{v:g}" for v in floats))
        try:
            step, record, sep = self.record_geometry()
            lines.append(f"    排布: 记录 {record} B + 分隔 {sep} B = 步长 {step} B")
        except ValueError as error:
            lines.append(f"    排布: {error}")
        return "\n".join(lines)

    # -- 修改 ---------------------------------------------------------------
    def set_slot(self, index: int, ident: int):
        if not 0 <= index < len(self.slots):
            raise IndexError(f"槽位下标 {index} 越界 (0..{len(self.slots) - 1})")
        at = self.slot_offsets()[index]
        self.raw = bytearray(self.raw)
        struct.pack_into("<Q", self.raw, at, ident)
        self.raw = bytes(self.raw)

    def slot_floats(self, index: int):
        at = self.slot_offsets()[index] + FLOAT_OFFSET
        return tuple(read_f32_halfswap(self.raw, at + 4 * i) for i in range(3))

    def set_slot_floats(self, index: int, a: float, b: float, c: float):
        at = self.slot_offsets()[index] + FLOAT_OFFSET
        self.raw = bytearray(self.raw)
        for i, value in enumerate((a, b, c)):
            write_f32_halfswap(self.raw, at + 4 * i, value)
        self.raw = bytes(self.raw)

    def grow(self, extra: int, template_id: int | None = None):
        """在末尾追加 `extra` 个槽位（复制最后一个槽位，可换模板 id）。

        **不推荐**：槽位的 +0x30 处带「链上角色」字段（实测 03 = 中间、
        05 = 最后一个有效、全 0 = 终止记录），复制终止记录当中间槽位会让
        引擎解析崩在 `exec of 0`（2026-10-19 实测过一次）。
        优先用 `clone_from()` 从原生多槽样本复制结构。
        """
        if extra <= 0:
            return
        step, record, sep = self.record_geometry()
        if sep < 0:
            raise ValueError(f"记录 {record} B 大于步长 {step} B, 结构不对")
        offs = self.slot_offsets()
        last = offs[-1]
        if self.raw[last + ROLE_OFFSET:last + ROLE_OFFSET + 6] == b"\0" * 6:
            raise ValueError(
                "最后一个槽位是终止记录（+0x30 处全 0），复制它会破坏结构。\n"
                "  请改用 clone：从一个原生多槽样本复制结构。")
        piece = bytearray(self.raw[last - sep:])       # 分隔 + 记录
        if template_id is not None:
            struct.pack_into("<Q", piece, sep, template_id)
        out = bytearray(self.raw)
        for _ in range(extra):
            out += piece
        out[COUNT_OFFSET] = self.count + extra
        self.raw = bytes(out)
        self.count = out[COUNT_OFFSET]
        self.slots = scan_ids(self.raw, self.container, FIRST_SLOT)

    def clone_from(self, sample: bytes, template_id: int | None = None,
                   floats=None):
        """用另一个资源的槽位表替换本资源的结构，只保留自己的 id。

        `sample` 必须是同类的原生数据；`floats` 给定时按槽位依次设置
        （跳过终止记录 —— 它的 +0x20 区不是浮点）。
        """
        data = bytearray(sample)
        if len(data) < FIRST_SLOT:
            raise ValueError("样本太短")
        struct.pack_into("<Q", data, 0, self.ident)
        self.raw = bytes(data)
        self.count = data[COUNT_OFFSET]
        self.slots = scan_ids(self.raw, self.container, FIRST_SLOT)
        if template_id is not None:
            for index in range(len(self.slots)):
                self.set_slot(index, template_id)
        if floats is not None:
            self.set_all_floats(*floats)

    def slot_role(self, index: int) -> str:
        """槽位在链上的角色（+0x30 的两个字节）。"""
        at = self.slot_offsets()[index] + ROLE_OFFSET
        tag = self.raw[at] | (self.raw[at + 1] << 8)
        return "终止" if tag == 0 and self.raw[at + 2:at + 6] == b"\0" * 4 \
            else f"0x{tag:04X}"

    def set_all_floats(self, a: float, b: float, c: float):
        """给所有「有效」槽位设同一组浮点，跳过终止记录。"""
        for index in range(len(self.slots)):
            if self.slot_role(index) == "终止":
                continue
            self.set_slot_floats(index, a, b, c)


# ---------------------------------------------------------------------------
# 0x76BBA011 载具编组
# ---------------------------------------------------------------------------

class VehicleTemplate:
    """伏击 / 按需载具的载具编组; 只做等长 id 替换。"""

    def __init__(self, data: bytes, container):
        self.raw = bytes(data)
        self.container = container
        self.items = scan_ids(self.raw, container, 0x08)

    def vehicles(self):
        return [(at, i) for at, i in self.items
                if (self.container.name_of(i) or "").startswith("TrafficVehicle_")]

    def crews(self):
        return [(at, i) for at, i in self.items
                if "Crew" in (self.container.name_of(i) or "")]

    def others(self):
        return [(at, i) for at, i in self.items
                if not (self.container.name_of(i) or "").startswith("TrafficVehicle_")
                and "Crew" not in (self.container.name_of(i) or "")]

    def describe(self):
        lines = [f"  class 0x{CLASS_VEHICLE_TEMPLATE:08X}  {len(self.raw)} B  "
                 f"载具 {len(self.vehicles())}  车组 {len(self.crews())}"]
        for at, i in self.items:
            kind = "车" if (self.container.name_of(i) or "").startswith("TrafficVehicle_") \
                else ("组" if "Crew" in (self.container.name_of(i) or "") else "  ")
            lines.append(f"    {kind} @0x{at:03X}  0x{i:012X}  {self.container.name_of(i) or '?'}")
        return "\n".join(lines)

    def set_reference(self, offset: int, ident: int):
        self.raw = bytearray(self.raw)
        struct.pack_into("<Q", self.raw, offset, ident)
        self.raw = bytes(self.raw)

    def replace_vehicles(self, ident: int):
        for at, _old in self.vehicles():
            self.set_reference(at, ident)

    def replace_crews(self, ident: int):
        for at, _old in self.crews():
            self.set_reference(at, ident)


# ---------------------------------------------------------------------------
# 写回容器
# ---------------------------------------------------------------------------

def write_resource(container, ident: int, new_data: bytes):
    """把资源数据写回容器, 支持长度变化.

    长度变化的三步 (与仓库里既有约定一致):
      1. Blob.grow 重排压缩块
      2. Container.set_size 改索引里的 size 并重算后续偏移
      3. 资源头 +4 的 u32 dataLength 同步
    """
    old_data = bytes(container.data_of(ident))
    old_size = container.by_id[ident][0]
    pos = container.data_offset_of(ident)
    delta = len(new_data) - len(old_data)
    if delta == 0:
        container.payload.write_range(pos, new_data)
        return 0
    container.payload.grow([(pos, len(old_data), new_data)])
    # 资源头是 [u32 class][u32 dataLength][u32 nameLength][name][padding],
    # dataLength 在「资源起点 + 4」, 不是「数据起点 + 4」。插点在数据起点,
    # 所以资源起点 +4 不受影响, 可以直接写。
    res_at = container.by_id[ident][1]
    container.payload.write_range(res_at + 4, struct.pack("<I", len(new_data)))
    container.set_size(ident, old_size + delta)
    return delta


def open_container(path):
    return be.Container.open(path)


def resolve(container, spec: str) -> int:
    if spec.lower().startswith("0x"):
        ident = int(spec, 16)
        if ident not in container.by_id:
            raise SystemExit(f"容器里没有 0x{ident:012X}")
        return ident
    return container.find(spec)


# ---------------------------------------------------------------------------
# 自检
# ---------------------------------------------------------------------------

def cmd_selftest(args):
    container = open_container(args.container)
    waves, vehicles = [], []
    for ident, _size, _off in container.entries:
        cls = container.header_of(ident)[0]
        if cls == CLASS_WAVE:
            waves.append(ident)
        elif cls == CLASS_VEHICLE_TEMPLATE:
            vehicles.append(ident)

    print(f"0x{CLASS_WAVE:08X} 资源 {len(waves)} 个")
    bad = 0
    lengths = {}
    for ident in waves:
        ws = WaveSetting(container.data_of(ident), container)
        lengths.setdefault(tuple(ws.slot_lengths()[:-1]), 0)
        lengths[tuple(ws.slot_lengths()[:-1])] += 1
        if ws.count != len(ws.slots):
            bad += 1
            if bad <= 8:
                print(f"   ! {container.name_of(ident)}: +0x1D={ws.count} "
                      f"但扫到 {len(ws.slots)} 个引用 ({ws.slot_lengths()})")
    print(f"  计数与扫描一致: {len(waves) - bad} / {len(waves)}")
    print("  槽位记录长度组合:", {k: v for k, v in sorted(
        lengths.items(), key=lambda kv: -kv[1])[:6]})

    print(f"\n0x{CLASS_VEHICLE_TEMPLATE:08X} 资源 {len(vehicles)} 个")
    sizes = {}
    for ident in vehicles:
        vt = VehicleTemplate(container.data_of(ident), container)
        sizes.setdefault((len(vt.raw), len(vt.vehicles()), len(vt.crews())), 0)
        sizes[(len(vt.raw), len(vt.vehicles()), len(vt.crews()))] += 1
    for (size, nveh, ncrew), count in sorted(sizes.items()):
        print(f"  {size:>4} B  载具 {nveh}  车组 {ncrew}   x{count}")
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _kind(container, ident):
    cls = container.header_of(ident)[0]
    if cls == CLASS_WAVE:
        return "wave"
    if cls == CLASS_VEHICLE_TEMPLATE:
        return "veh"
    return f"0x{cls:08X}"


def cmd_list(args):
    container = open_container(args.container)
    ident = resolve(container, args.resource)
    kind = _kind(container, ident)
    print(f"0x{ident:012X}  {container.name_of(ident)}  [{kind}]")
    if kind == "wave":
        print(WaveSetting(container.data_of(ident), container).describe())
    elif kind == "veh":
        print(VehicleTemplate(container.data_of(ident), container).describe())
    else:
        raise SystemExit(f"class {kind} 不是本工具处理的类型")
    return 0


def cmd_set(args):
    container = open_container(args.container)
    ident = resolve(container, args.resource)
    target = resolve(container, args.target)
    ws = WaveSetting(container.data_of(ident), container)
    before = ws.templates()
    ws.set_slot(args.slot, target)
    delta = write_resource(container, ident, ws.raw)
    container.write(args.output or args.container)
    print(f"槽位 {args.slot}: 0x{before[args.slot]:012X} -> 0x{target:012X}  "
          f"({delta:+d} B)  -> {args.output or args.container}")
    return 0


def cmd_grow(args):
    container = open_container(args.container)
    ident = resolve(container, args.resource)
    ws = WaveSetting(container.data_of(ident), container)
    print(f"原: {ws.count} 槽位  长度 {len(ws.raw)}")
    template = resolve(container, args.template) if args.template else ws.templates()[-1]
    ws.grow(args.n, template)
    delta = write_resource(container, ident, ws.raw)
    out = args.output or args.container
    container.write(out)
    again = be.Container.open(out)
    check = WaveSetting(again.data_of(ident), again)
    print(f"新: {check.count} 槽位  长度 {len(check.raw)}  ({delta:+d} B)")
    for line in check.describe().splitlines()[1:]:
        print(line)
    return 0


def cmd_clone(args):
    container = open_container(args.container)
    dst = resolve(container, args.dst)
    src = resolve(container, args.source)
    template = resolve(container, args.template) if args.template else None
    floats = None
    if args.floats:
        parts = [float(x) for x in args.floats.split(",")]
        if len(parts) != 3:
            raise SystemExit("--floats 要三个数，例如 75,100,500")
        floats = tuple(parts)
    ws = WaveSetting(container.data_of(dst), container)
    ws.clone_from(container.data_of(src), template, floats)
    delta = write_resource(container, dst, ws.raw)
    container.write(args.output or args.container)
    print(f"{container.name_of(dst)} <- 结构来自 {container.name_of(src)}  "
          f"({delta:+d} B)")
    return 0


def cmd_ambush(args):
    container = open_container(args.container)
    ident = resolve(container, args.resource)
    vt = VehicleTemplate(container.data_of(ident), container)
    print(f"0x{ident:012X}  {container.name_of(ident)}")
    print(vt.describe())
    if args.vehicle:
        vt.replace_vehicles(resolve(container, args.vehicle))
        write_resource(container, ident, vt.raw)
        container.write(args.output or args.container)
        print(f"  载具全部改成 {args.vehicle} -> {args.output or args.container}")
    if args.crew:
        vt.replace_crews(resolve(container, args.crew))
        write_resource(container, ident, vt.raw)
        container.write(args.output or args.container)
        print(f"  车组全部改成 {args.crew} -> {args.output or args.container}")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("list", help="列出一个资源的槽位 / 引用")
    p.add_argument("container")
    p.add_argument("resource")
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("set", help="改一个槽位指向")
    p.add_argument("container")
    p.add_argument("resource")
    p.add_argument("slot", type=int)
    p.add_argument("target")
    p.add_argument("-o", "--output")
    p.set_defaults(func=cmd_set)

    p = sub.add_parser("grow", help="给波次表加槽位")
    p.add_argument("container")
    p.add_argument("resource")
    p.add_argument("n", type=int)
    p.add_argument("template", nargs="?")
    p.add_argument("-o", "--output")
    p.set_defaults(func=cmd_grow)

    p = sub.add_parser("clone", help="把另一个资源的槽位表结构复制过来")
    p.add_argument("container")
    p.add_argument("dst")
    p.add_argument("source")
    p.add_argument("template", nargs="?")
    p.add_argument("--floats", help="三个浮点，例如 75,100,500（跳过终止记录）")
    p.add_argument("-o", "--output")
    p.set_defaults(func=cmd_clone)

    p = sub.add_parser("ambush", help="读 / 改载具编组")
    p.add_argument("container")
    p.add_argument("resource")
    p.add_argument("--vehicle")
    p.add_argument("--crew")
    p.add_argument("-o", "--output")
    p.set_defaults(func=cmd_ambush)

    p = sub.add_parser("selftest", help="全库校验解析规则")
    p.add_argument("container")
    p.set_defaults(func=cmd_selftest)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())


# ---------------------------------------------------------------------------
# 依据 (2026-10-19 从 v250u 的 bootstrap 反向确认)
#
# 1) 222 B / 304 B 的 WaveSetting 逐字节只差三处:
#      0,1 (自身 id 低字节) 与 0x1D (0x02 -> 0x03)
#    其余 0x44.. 完全同构, 说明 +0x1D 就是槽位数。
#
# 2) 槽位记录长度随模板类别变化:
#      WaveSetting_Hunt_SB_D01              0x44 / 0x96 / 0xE8   间隔 82
#      WaveSetting_Hunt_UNI_..._Level03     0x44 / 0x84 / 0xC4   间隔 64
#    所以工具用「相邻槽位偏移差」推长度, 不用固定常量。
#
# 3) 槽位记录里 +0x2C 起是三个 float, 实测 50 / 100 / 500。
#
# 4) 0x76BBA011 的载具/车组是 {u16 flag=1, u64 id} 记录, 但载具组之间
#    夹着 4 字节、尾部长度也随组数变化 (Easy 83 / Medium 103 差 20),
#    因此**不做插记录**, 只做等长 id 替换。
# ---------------------------------------------------------------------------
