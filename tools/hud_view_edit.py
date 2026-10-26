#!/usr/bin/env python3
"""hud_view_edit.py - 增删 HUD 视图里的元素.

游戏把所有 HUD 元素登记在三个「视图」里 (class 0x651F1E2E):

    GR_BasicHUD              48 个   正常游玩
    GR_CoopHUD                4 个   合作模式额外挂的
    GR_BenchmarkViewportHUD   1 个   benchmark 模式

视图结构 (已经 3/3 全量校验):

    +0x00  u64   自身 id
    +0x08  u32   class = 0x651F1E2E
    +0x0C  u8    1
    +0x0D  u32   元素个数 n
    +0x11  n × { u16 flag = 1, u64 creatorId }      每条 10 字节

    len = 0x11 + 10 * n

从视图里去掉一条记录, 那个 HUD 元素就**不会被创建** —— 这是游戏自己的做法:
benchmark 模式之所以没有 HUD, 就是因为它换成了只含一个元素的
GR_BenchmarkViewportHUD。2026-10-19 用删准星验证过: 准星消失, 其他 HUD 正常。

用法:

    # 看有哪些元素
    python hud_view_edit.py list <boot.data> [--view GR_BasicHUD]

    # 去掉几个 (按名字模糊匹配, 区分大小写)
    python hud_view_edit.py drop <in.data> <out.data> Crosshair Compass

    # 只保留几个 (其余全删)
    python hud_view_edit.py keep <in.data> <out.data> Crosshair Minimap

改的是 bootstrap 容器里的资源, 长度会跟着变, 写回走 wave_edit.write_resource。
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
import wave_edit as we                       # noqa: E402

be = we.be

CLASS_VIEW = 0x651F1E2E
HEADER = 0x11
RECORD = 10
COUNT_AT = 0x0D

VIEWS = ('GR_BasicHUD', 'GR_CoopHUD', 'GR_BenchmarkViewportHUD')


class View:
    """一个 HUD 视图: 一组 creator id, 顺序就是登记顺序."""

    def __init__(self, data: bytes, container):
        self.raw = bytes(data)
        self.container = container
        self.ident = int.from_bytes(self.raw[0:8], 'little')
        self.cls = struct.unpack_from('<I', self.raw, 8)[0]
        if self.cls != CLASS_VIEW:
            raise ValueError(f'class 0x{self.cls:08X} 不是 HUD 视图')
        if self.raw[0x0C] != 1:
            raise ValueError(f'+0x0C 是 {self.raw[0x0C]}, 期望 1')
        self.count = struct.unpack_from('<I', self.raw, COUNT_AT)[0]
        if HEADER + RECORD * self.count != len(self.raw):
            raise ValueError(f'count {self.count} 与长度 {len(self.raw)} 不符')
        self.items = []
        for k in range(self.count):
            flag, ident = struct.unpack_from('<HQ', self.raw, HEADER + k * RECORD)
            if flag != 1:
                raise ValueError(f'第 {k} 条记录 flag={flag}, 期望 1')
            self.items.append(ident)

    # -- 查询 --------------------------------------------------------------
    def names(self):
        return [self.container.name_of(i) or f'0x{i:012X}' for i in self.items]

    def describe(self):
        lines = [f'  {self.container.name_of(self.ident)}  {len(self.raw)} B  '
                 f'{self.count} 个元素']
        for k, (ident, name) in enumerate(zip(self.items, self.names())):
            lines.append(f'    [{k:2}] 0x{ident:012X}  {name}')
        return '\n'.join(lines)

    # -- 编辑 --------------------------------------------------------------
    def _rebuild(self, items):
        out = bytearray(self.raw[:HEADER])            # id / class / flags / count
        struct.pack_into('<I', out, COUNT_AT, len(items))
        for ident in items:
            out += struct.pack('<HQ', 1, ident)
        self.raw = bytes(out)
        self.items = list(items)
        self.count = len(items)
        if HEADER + RECORD * self.count != len(self.raw):
            raise AssertionError('重建后长度与 count 不符')

    def drop(self, idents):
        drop = set(idents)
        self._rebuild([i for i in self.items if i not in drop])

    def keep(self, idents):
        keep = set(idents)
        self._rebuild([i for i in self.items if i in keep])


def find_view(container, spec):
    """按名字或 id 找视图."""
    for ident, _s, _o in container.entries:
        if container.header_of(ident)[0] != CLASS_VIEW:
            continue
        if spec in (container.name_of(ident), f'0x{ident:012X}'):
            return View(container.data_of(ident), container)
    raise SystemExit(f'找不到视图 {spec}')


def match_names(container, tokens, view):
    """把命令行给的名字/片段解析成 creator id."""
    known = {n: i for i, n in zip(view.items, view.names())}
    out = []
    for tok in tokens:
        hits = [i for n, i in known.items() if tok.lower() in n.lower()]
        if not hits:
            raise SystemExit(f'视图里没有匹配 {tok!r} 的元素。'
                             f'用 list 看准确名字。')
        out += hits
    return out


def cmd_list(args):
    container = be.Container.open(args.container)
    views = VIEWS if not args.view else (args.view,)
    for name in views:
        try:
            v = find_view(container, name)
        except SystemExit:
            continue
        print(v.describe())
        print()
    return 0


def _edit(args, op):
    container = be.Container.open(args.src)
    view = find_view(container, args.view)
    idents = match_names(container, args.tokens, view)
    before = view.count
    op(view, idents)
    delta = we.write_resource(container, view.ident, view.raw)
    container.write(args.dst)
    again = be.Container.open(args.dst)
    check = View(again.data_of(view.ident), again)
    print(f'{again.name_of(view.ident)}: {before} -> {check.count} 个元素 '
          f'({len(view.raw)} B, {delta:+d})')
    for k, n in enumerate(check.names()):
        print(f'  [{k:2}] {n}')
    return 0


def cmd_drop(args):
    return _edit(args, lambda v, ids: v.drop(ids))


def cmd_keep(args):
    return _edit(args, lambda v, ids: v.keep(ids))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest='cmd', required=True)

    q = sub.add_parser('list', help='列出视图里的元素')
    q.add_argument('container')
    q.add_argument('--view', default=None)
    q.set_defaults(func=cmd_list)

    for name, func, helptext in (
            ('drop', cmd_drop, '去掉匹配的元素'),
            ('keep', cmd_keep, '只保留匹配的元素')):
        q = sub.add_parser(name, help=helptext)
        q.add_argument('src')
        q.add_argument('dst')
        q.add_argument('tokens', nargs='+', help='名字或名字片段，可给多个')
        q.add_argument('--view', default='GR_BasicHUD')
        q.set_defaults(func=func)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == '__main__':
    sys.exit(main())
