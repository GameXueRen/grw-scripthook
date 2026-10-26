#!/usr/bin/env python3
"""gen_hud_icon_ids.py - 把 forge 清单里的图标资源 ID 编成一张 C 查表.

为什么需要这一步：HUD 图标控件的 SH_P_IMAGE 不是贴图名，是**资源 ID 的十六进制
字符串**（实测 2026-10-20）：

    0x106dedde40   UI_HUD_Vehicle_Icon_MapDesc
    0x43b8404948   UI_Map_Icon_Vehicle_Bike_MapDesc
    0x1099198655   GRN_HUD_MiniMapDot_Collectible_AmmoBox_MapDesc

所以"这一类图标"在运行时只能靠 ID 认。名字只在清单里有，于是这张表在构建时由
名字生成：清单 → (id, name) → 按前缀归到某一类 → 排序后写成头文件。

用法：
    python tools/gen_hud_icon_ids.py                 # 写 plugins/MoreHudControl/hud_icon_ids.h
    python tools/gen_hud_icon_ids.py --list          # 只统计每一类多少条

类别顺序必须和 MoreHudControl.c 里 g_icons[] 一致，头文件里带了
HC_ICON_KIND_COUNT，插件用编译期断言对住它。
"""
from __future__ import annotations

import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def _listings():
    """The forge listings live outside the repo (they are a dump of the game,
    not part of the source), so the folder is looked up the same way
    wave_edit.py finds bootstrap_edit.py."""
    for rel in ("../third_party/WildlandsToolkit/_analysis",
                "../../third_party/WildlandsToolkit/_analysis",
                "third_party/WildlandsToolkit/_analysis"):
        path = os.path.abspath(os.path.join(ROOT, rel, "*.forge.txt"))
        if glob.glob(path):
            return path
    return ""


LISTINGS = _listings()
OUT = os.path.join(ROOT, "plugins", "MoreHudControl", "hud_icon_ids.h")

# (前缀, 类名里的中文标签只用于 --list 的时候看)  顺序即 kind 下标
KINDS = [
    ("GRN_HUD_MiniMapDot_Collectible", "小地图点：收集品"),
    ("GRN_HUD_MiniMapDot_Enemy",       "小地图点：敌人与声音"),
    ("GRN_HUD_MiniMapDot",             "小地图点：其他"),
    ("UI_HUD_MiniMap_",                "小地图箭头与目标"),
    ("UI_HUD_Minimap_",                "小地图图标"),
    ("UI_HUD_Map_Icon_",               "大地图设施图标"),
    # The map draws 14+ icons and every one of them used to land in this single
    # bucket, because "UI_Map_Icon_" matches all of them. Splitting it by the
    # second name segment (which the archives do consistently) is what makes the
    # map switchable kind by kind. Order inside the family matters: the
    # catch-all goes last.
    ("UI_Map_Icon_Resource",           "大地图：资源"),
    ("UI_Map_Icon_Vehicle",            "大地图：载具"),
    ("UI_Map_Icon_Mission",            "大地图：任务"),
    ("UI_Map_Icon_Unidad",             "大地图：联合军"),
    ("UI_Map_Icon_Teammate",           "大地图：队友"),
    ("UI_Map_Icon_Legend",             "大地图：传奇"),
    ("UI_Map_Icon_Jammer",             "大地图：干扰器"),
    ("UI_Map_Icon_Collectible",        "大地图：收集品"),
    ("UI_Map_Icon_",                   "大地图：据点与地标"),
    ("UI_HUD_WorldIcon_Activity",      "大地图活动图标"),
    ("UI_HUD_WorldIcon_",              "大地图地点图标（其他）"),
    ("UI_HUD_WorldIntel_",             "大地图情报图标"),
    ("UI_HUD_MarkIcon",                "世界标记（敌/友/中立）"),
    ("UI_HUD_Mark_",                   "兵种与威胁标记"),
    ("UI_HUD_Marker_",                 "物件标记"),
    ("GRN_HUD_MiniMapCorner",          "小地图折角"),
    ("UI_HUD_Beacon",                  "信标"),
    ("UI_HUD_Vehicle_Icon|UI_HUD_Icon_", "载具与物品图标"),
    # --- added 2026-10-22: families the first pass missed ---------------------
    # Order matters: a name takes the first prefix that matches, so everything
    # already classified above keeps its kind. These are appended.
    ("UI_Map_Activity|UI_Map_Icons|UI_Map_Temp", "大地图活动与奖励"),
    ("UI_Map_Settlement|UI_Map_Province|UI_Map_Zone", "大地图据点与省份"),
    ("UI_MAP_Icon|UI_Map_icon", "大地图图标（大小写变体）"),
    ("UI_Map_Mission|UI_Map_MissionObjective|UI_Map_MissionSquareHollow"
     "|UI_Map_MissionOpportunity", "大地图任务图标"),
    ("GRN_HUD_MiniMapSkin|GRN_HUD_MiniMap|GRN_HUD_MiniMapJammer", "小地图底图与干扰"),
    ("UI_Map_BeaconGeneric|UI_Map_BeaconIcon|UI_Map_Redeploy"
     "|UI_Map_FactionState|UI_HUD_Worldicon", "信标 / 重部署 / 阵营"),
    ("UI_Map_cursorDefault|UI_Map_cursorCenter|UI_Map_CursorLine", "大地图光标"),
    ("UI_HUD_Map|UI_Map_BGGradient|UI_Map_DirtBg|UI_Map_WhiteDirtBg"
     "|UI_Map_DifficultyIcon", "大地图界面元素"),
    # The one gap the runtime census found: the map draws a mouse glyph
    # (UI_COM_Mouse_RightClick) that no earlier prefix claimed, and UI_COM_ was
    # never collected at all. Its sibling families are the key and mouse hint
    # icons shown next to prompts, so they get one switch of their own rather
    # than being dumped into a map bucket.
    ("UI_COM_Mouse|UI_COM_Key", "键鼠提示"),
]

ENTRY = re.compile(r"(0x[0-9A-Fa-f]{12})\s+(\S.*?)\s*$")


def kind_of(name: str) -> int:
    for i, (spec, _label) in enumerate(KINDS):
        for prefix in spec.split("|"):
            if name.startswith(prefix):
                return i
    return -1


def collect():
    ids: dict[int, int] = {}
    names: dict[int, str] = {}
    for path in sorted(glob.glob(LISTINGS)):
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = ENTRY.search(line)
                if not m:
                    continue
                rid = int(m.group(1), 16)
                name = m.group(2)
                if name.lower().endswith(".data"):
                    name = name[:-5]
                k = kind_of(name)
                if k < 0 or rid in ids:
                    continue
                ids[rid] = k
                names[rid] = name
    return ids, names


def main(argv):
    ids, names = collect()
    if "--list" in argv:
        per = {}
        for rid, k in ids.items():
            per.setdefault(k, []).append(rid)
        for k, (spec, label) in enumerate(KINDS):
            got = per.get(k, [])
            print("%2d  %-34s %4d ids   %s" % (k, spec, len(got), label))
            for rid in sorted(got)[:2]:
                print("        0x%012X %s" % (rid, names[rid]))
        print("total %d ids, %d kinds" % (len(ids), len(KINDS)))
        return 0

    lines = [
        "/* Generated by tools/gen_hud_icon_ids.py - do not edit.",
        " *",
        " * Every HUD icon texture the game's archives name, sorted by resource id,",
        " * with the kind it belongs to. A HUD icon widget carries its texture as an",
        " * id string in SH_P_IMAGE, not as a name, so this is what the filter",
        " * matches on: id -> kind -> is that kind switched off.",
        " *",
        " * Rebuild after a game update changes the archives:",
        " *     python tools/gen_hud_icon_ids.py",
        " */",
        "#ifndef HC_ICON_IDS_H",
        "#define HC_ICON_IDS_H",
        "",
        "#define HC_ICON_KIND_COUNT %d" % len(KINDS),
        "",
        "typedef struct { unsigned long long id; unsigned char kind; } HcIconId;",
        "",
        "static const HcIconId g_iconIds[] = {",
    ]
    for rid in sorted(ids):
        lines.append("    { 0x%012XULL, %2d },   /* %s */"
                     % (rid, ids[rid], names[rid][:52]))
    lines += [
        "};",
        "",
        "#endif",
        "",
    ]
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))
    print("wrote %s: %d ids, %d kinds" % (OUT, len(ids), len(KINDS)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
