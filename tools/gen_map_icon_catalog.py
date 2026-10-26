#!/usr/bin/env python3
"""gen_map_icon_catalog.py - 把大地图图标做成可对照、可标注的清单.

输入：
    plugins/MoreHudControl/hud_icon_ids.h    (id -> kind + 资源名)
    <png 目录>                                (Wildlands.Cli texout 导出的 PNG)

输出（默认写到 documents/scripthook-docs-20261016/）：
    hud-map-icons/                     PNG 副本（HTML 相对引用）
    hud-map-icons-catalog.html         分组画廊 + 白名单勾选 + 导出所选
    hud-map-icons-catalog.csv          同一份数据的表格，便于批量标注

用法：
    python tools/gen_map_icon_catalog.py --png /tmp/mapicons
    python tools/gen_map_icon_catalog.py --png /tmp/mapicons --out-dir <目录>

PNG 用导出命令得到（每个归档跑一次，filter 见下）：
    Wildlands.Cli.exe texout <forge> "UI_Map" <out>
    Wildlands.Cli.exe texout <forge> "UI_HUD_WorldIcon" <out>
    Wildlands.Cli.exe texout <forge> "UI_HUD_WorldIntel" <out>
    Wildlands.Cli.exe texout <forge> "UI_HUD_Map_Icon" <out>
    Wildlands.Cli.exe texout <forge> "UI_HUD_Icon_" <out>
"""
from __future__ import annotations

import argparse
import html
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ICON_H = os.path.join(ROOT, "plugins", "MoreHudControl", "hud_icon_ids.h")
PLUGIN_C = os.path.join(ROOT, "plugins", "MoreHudControl", "MoreHudControl.c")
DOCS = os.path.join(os.path.dirname(ROOT), "documents", "scripthook-docs-20261016")

# kind -> 中文组名（顺序与 MoreHudControl.c 的 g_icons[] 一致）
KIND_LABEL = {
    0: "小地图点：收集品", 1: "小地图点：敌人", 2: "小地图点：其他",
    3: "小地图箭头与目标", 4: "小地图图标", 5: "大地图设施",
    6: "大地图：资源", 7: "大地图：载具", 8: "大地图：任务",
    9: "大地图：联合军", 10: "大地图：队友", 11: "大地图：传奇",
    12: "大地图：干扰器", 13: "大地图：收集品", 14: "大地图：据点与地标",
    15: "大地图活动", 16: "大地图地点（其他）", 17: "大地图情报",
    18: "世界标记", 19: "兵种与威胁标记", 20: "物件标记", 21: "小地图折角",
    22: "信标", 23: "载具与物品图标", 24: "大地图活动与奖励",
    25: "大地图据点与省份", 26: "大地图图标（变体/奖励）", 27: "大地图任务图标",
    28: "小地图底图与干扰", 29: "信标 / 重部署 / 阵营", 30: "大地图光标",
    31: "大地图界面元素", 32: "键鼠提示",
}
# 只列可能出现在大地图上的组，顺序即页面顺序
KIND_ORDER = [5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 22, 23, 24,
              25, 26, 27, 29, 30, 31]
ROW_RE = re.compile(r"\{ (0x[0-9A-Fa-f]+)ULL,\s*(\d+) \},\s*/\* (.*?) \*/")
GUID_RE = re.compile(r"_[0-9A-Fa-f]{8}-[0-9A-Fa-f-]+$")


def base_of(name: str) -> str:
    """去掉变体后缀与 DLC 前缀，得到"同一个图标"的基名。"""
    s = GUID_RE.sub("", name)            # GUID 变体先剥掉，否则后面的后缀都被它挡住
    s = re.sub(r"^DLC\d+_", "", s)
    s = re.sub(r"_MapDesc$", "", s)
    s = re.sub(r"_Simple_Map$", "", s)
    s = re.sub(r"_Map$", "", s)
    s = re.sub(r"_Simple$", "", s)       # 低缩放级别的简化版：并进母图标那一张卡
    return s


# DLC 归档里的图标不在 hud_icon_ids.h 里（生成器只扫了本体归档），所以按名字前缀
# 兜底归类；顺序与 MoreHudControl.c 的 g_icons[] 一致，长的前缀在前面。
PREFIX_KIND = [
    ("UI_HUD_WorldIcon_Activity", 15),
    ("UI_Map_Activity", 24), ("UI_Map_Icons", 24), ("UI_Map_Temp", 24),
    ("UI_Map_Followers", 24), ("UI_Map_Informer", 24),
    ("UI_Map_Followers", 24), ("UI_Map_Informer", 24),
    ("UI_HUD_WorldIcon", 16),
    ("UI_HUD_WorldIntel", 17),
    ("UI_HUD_Map_Icon", 5),
    ("UI_Map_Icon_", 14),
    ("UI_Map_Settlement", 25), ("UI_Map_Province", 25), ("UI_Map_Zone", 25),
    ("UI_MAP_Icon", 26), ("UI_Map_icon", 26),
    ("UI_Map_Mission", 27),
    ("UI_Map_Beacon", 29), ("UI_Map_Redeploy", 29), ("UI_Map_FactionState", 29),
    ("UI_Map_cursor", 30),
    ("UI_Map_BGGradient", 31), ("UI_Map_DirtBg", 31),
    ("UI_Map_WhiteDirtBg", 31), ("UI_Map_DifficultyIcon", 31),
    ("UI_HUD_Vehicle_Icon", 23), ("UI_HUD_Icon_", 23),
]


def kind_by_prefix(name: str) -> int:
    n = re.sub(r"^DLC\d+_", "", name).lower()
    for pre, k in PREFIX_KIND:
        if n.startswith(pre.lower()):
            return k
    return -1


MAP_ROWS = [("g_mapVehicleIds", "大地图·载具"),
            ("g_mapDocIds", "大地图·文档"),
            ("g_mapUnknownIds", "大地图·未知地点")]


def load_map_blacklist():
    """id -> 行名，读插件里的三张黑名单（插件只控制这些，其余一律不碰）。"""
    out = {}
    if not os.path.exists(PLUGIN_C):
        return out
    src = open(PLUGIN_C, encoding="utf-8", errors="replace").read()
    for arr, label in MAP_ROWS:
        m = re.search(r"static const unsigned long long %s\[\] = \{(.*?)\};"
                      % arr, src, re.S)
        if not m:
            continue
        for hexid in re.findall(r"0x([0-9A-Fa-f]+)ULL", m.group(1)):
            out[int(hexid, 16)] = label
    return out


MAP_ROWS = [("g_mapVehicleIds", "大地图·载具"),
            ("g_mapDocIds", "大地图·文档"),
            ("g_mapUnknownIds", "大地图·未知地点")]


def load_map_blacklist():
    """id -> 行名，读插件里的三张黑名单（插件只控制这些，其余一律不碰）。"""
    out = {}
    if not os.path.exists(PLUGIN_C):
        return out
    src = open(PLUGIN_C, encoding="utf-8", errors="replace").read()
    for arr, label in MAP_ROWS:
        m = re.search(r"static const unsigned long long %s\[\] = \{(.*?)\};"
                      % arr, src, re.S)
        if not m:
            continue
        for hexid in re.findall(r"0x([0-9A-Fa-f]+)ULL", m.group(1)):
            out[int(hexid, 16)] = label
    return out


def load_table():
    text = open(ICON_H, encoding="utf-8", errors="replace").read()
    rows = ROW_RE.findall(text)
    by_name, by_id = {}, {}
    for hexid, kind, name in rows:
        by_name[name] = (int(hexid, 16), int(kind))
        by_id[int(hexid, 16)] = (name, int(kind))
    return by_name, by_id


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--png", required=True, help="texout 导出的 PNG 目录")
    ap.add_argument("--out-dir", default=DOCS)
    a = ap.parse_args()

    by_name, _ = load_table()
    by_base = {}
    for nm, (idv, kind) in by_name.items():
        b = base_of(nm)
        cur = by_base.get(b)
        if cur is None or kind < cur[0]:
            by_base[b] = (kind, {idv})
        else:
            cur[1].add(idv)
    pngs = [f for f in os.listdir(a.png) if f.lower().endswith(".png")]
    if not pngs:
        print("no png in", a.png)
        return 1

    # base -> {kind, ids, names, png}
    items = {}
    unclassified = []
    for f in pngs:
        name = f[:-4]
        b = base_of(name)
        hit = by_base.get(b)
        if hit:
            kind, ids_known = hit[0], hit[1]
        else:
            kind, ids_known = kind_by_prefix(name), set()
            if kind < 0:
                unclassified.append(name)
                continue
        idv = None
        it = items.setdefault(b, {"kind": kind, "ids": set(), "names": [],
                                  "png": None, "score": 9})
        it["ids"] |= ids_known
        it["names"].append(name)
        # 选图优先级：<base>_Map（地图上真正画的那张）> _Simple_Map > _MapDesc > 其它
        score = 0 if name == b + "_Map" else (
            1 if name == b + "_Simple_Map" else (
                2 if name == b + "_MapDesc" else 3))
        if it["png"] is None or score < it["score"]:
            it["png"], it["score"] = f, score
    for b, it in items.items():
        it["names"].sort()

    groups = {}
    for b, it in items.items():
        groups.setdefault(it["kind"], []).append((b, it))
    for k in groups:
        groups[k].sort(key=lambda x: x[0].lower())

    black = load_map_blacklist()

    def badge_of(it):
        for idv in it["ids"]:
            if idv in black:
                return black[idv]
        return "不控制（黑名单外）"

    black = load_map_blacklist()

    def badge_of(it):
        for idv in it["ids"]:
            if idv in black:
                return black[idv]
        return "不控制（黑名单外）"

    out_icons = os.path.join(a.out_dir, "hud-map-icons")
    if os.path.isdir(out_icons):
        shutil.rmtree(out_icons)     # 重新生成时不留上次的陈旧图
    os.makedirs(out_icons, exist_ok=True)
    for b, it in items.items():
        shutil.copy2(os.path.join(a.png, it["png"]), os.path.join(out_icons, it["png"]))

    order = [k for k in KIND_ORDER if k in groups]
    for k in sorted(groups):
        if k not in order:
            order.append(k)

    total = len(items)
    parts = ["""<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GRW 大地图图标清单</title>
<style>
:root{--bg:#15171b;--panel:#1d2026;--line:#2c313a;--fg:#e6e8ec;--dim:#98a0ad;--hi:#4ea1ff}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.5 "Segoe UI","Microsoft YaHei",sans-serif}
header{position:sticky;top:0;z-index:9;background:#101216f2;border-bottom:1px solid var(--line);padding:14px 20px;backdrop-filter:blur(6px)}
h1{margin:0 0 6px;font-size:18px}
.sub{color:var(--dim);font-size:12px}
.bar{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin-top:10px}
input[type=search]{background:var(--panel);border:1px solid var(--line);color:var(--fg);padding:7px 10px;border-radius:6px;width:260px}
button{background:var(--panel);border:1px solid var(--line);color:var(--fg);padding:7px 12px;border-radius:6px;cursor:pointer}
button:hover{border-color:var(--hi)}
nav a{color:var(--dim);text-decoration:none;margin-right:12px;font-size:12px}
nav a:hover{color:var(--hi)}
main{padding:16px 20px 60px}
h2{font-size:15px;margin:26px 0 10px;padding-bottom:6px;border-bottom:1px solid var(--line)}
h2 span{color:var(--dim);font-weight:400;font-size:12px;margin-left:8px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(178px,1fr));gap:10px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:8px;display:flex;flex-direction:column;gap:6px}
.card.on{border-color:#e5b567;box-shadow:0 0 0 1px #e5b56755 inset}
.thumb{height:64px;display:flex;align-items:center;justify-content:center;background:#0f1114;border-radius:6px}
.thumb img{max-width:64px;max-height:64px;image-rendering:pixelated}
.nm{font-size:11px;color:var(--dim);word-break:break-all;line-height:1.35}
.nm b{color:var(--fg);font-weight:600;display:block;font-size:12px;margin-bottom:2px}
.row{display:flex;align-items:center;gap:6px;font-size:11px;color:var(--dim)}
.row input{accent-color:#e5b567}
.id{font-size:10px;color:#6f7885;word-break:break-all}
.bg{font-size:10px;color:#8fb8e8;background:#1b2733;border:1px solid #243444;border-radius:4px;padding:2px 5px;align-self:flex-start}
.bg{font-size:10px;color:#8fb8e8;background:#1b2733;border:1px solid #243444;border-radius:4px;padding:2px 5px;align-self:flex-start}
footer{color:var(--dim);font-size:12px;padding:20px;border-top:1px solid var(--line);margin-top:30px}
</style></head><body>
<header>
<h1>GRW 大地图图标清单<span class="sub">—— 逐张对照，勾选要加入「不要隐藏」白名单的图标</span></h1>
<div class="sub">来源：游戏归档 DataPC_extra / DataPC_extra_patch_01 / DataPC_2·10·20·30_dlc，用 Wildlands.Cli 的 texout 导出原图后用本工具归档。<b>同一图标的 _Map 与 _MapDesc 是一对，卡片标题是去掉后缀的基名。</b></div>
<div class="bar">
  <input id="q" type="search" placeholder="搜索名称 / 资源 ID（如 Airport、0x002E84154764）">
  <button id="copy">复制已勾选（可直接粘回对话）</button>
  <button id="clear">清空勾选</button>
  <span class="sub" id="stat"></span>
</div>
<nav>"""]

    for k in order:
        parts.append('<a href="#g%d">%s (%d)</a>' % (k, html.escape(KIND_LABEL.get(k, str(k))),
                                                     len(groups[k])))
    parts.append('</nav></header><main>')

    for k in order:
        label = KIND_LABEL.get(k, "kind %d" % k)
        parts.append('<h2 id="g%d">%s<span>kind %d · %d 个图标</span></h2><div class="grid">'
                     % (k, html.escape(label), k, len(groups[k])))
        for b, it in groups[k]:
            ids = sorted(it["ids"])
            idtxt = ", ".join("0x%012X" % i for i in ids)
            variants = " · ".join(it["names"])
            parts.append(
                '<div class="card" data-k="%s">'
                '<label class="row"><input type="checkbox" class="pick" value="%s"> 白名单</label>'
                '<div class="thumb"><img loading="lazy" src="hud-map-icons/%s" alt=""></div>'
                '<div class="nm"><b>%s</b>%s</div>'
                '<div class="id">%s</div><div class="bg">%s</div>'
                '</div>' % (b.lower(), html.escape(b), html.escape(it["png"]),
                            html.escape(b), html.escape(variants), html.escape(idtxt),
                            html.escape(badge_of(it))))
        parts.append('</div>')

    if unclassified:
        parts.append('<h2 id="gX">未分类<span>%d 张（不在 hud_icon_ids.h 里的贴图）</span></h2>'
                     '<div class="grid">' % len(unclassified))
        for name in sorted(unclassified)[:300]:
            shutil.copy2(os.path.join(a.png, name + ".png"),
                         os.path.join(out_icons, name + ".png"))
            parts.append('<div class="card"><div class="thumb"><img loading="lazy" src="hud-map-icons/%s" alt=""></div>'
                         '<div class="nm"><b>%s</b></div></div>'
                         % (html.escape(name + ".png"), html.escape(name)))
        parts.append('</div>')

    parts.append('''</main><footer>
本清单由 tools/gen_map_icon_catalog.py 生成。勾选状态保存在本机浏览器（localStorage），刷新不丢；
点「复制已勾选」得到 <code>白名单: 基名, 基名, …</code> 一行文本，直接粘回对话即可。
</footer>
<script>
const KEY='grw-map-icon-whitelist';
const boxes=[...document.querySelectorAll('input.pick')];
function load(){const s=new Set(JSON.parse(localStorage.getItem(KEY)||'[]'));
  boxes.forEach(b=>{if(s.has(b.value)){b.checked=true;b.closest('.card').classList.add('on');}});stat();}
function save(){localStorage.setItem(KEY,JSON.stringify(boxes.filter(b=>b.checked).map(b=>b.value)));stat();}
function stat(){const n=boxes.filter(b=>b.checked).length;document.getElementById('stat').textContent='已勾选 '+n+' / '+boxes.length;}
boxes.forEach(b=>b.addEventListener('change',()=>{b.closest('.card').classList.toggle('on',b.checked);save();}));
document.getElementById('clear').onclick=()=>{boxes.forEach(b=>{b.checked=false;b.closest('.card').classList.remove('on');});save();};
document.getElementById('copy').onclick=async()=>{
  const sel=boxes.filter(b=>b.checked).map(b=>b.value);
  const txt='白名单('+sel.length+'): '+sel.join(', ');
  try{await navigator.clipboard.writeText(txt);}catch(e){prompt('复制下面这行：',txt);}
  document.getElementById('copy').textContent='已复制 '+sel.length+' 项';
  setTimeout(()=>document.getElementById('copy').textContent='复制已勾选（可直接粘回对话）',1500);
};
document.getElementById('q').oninput=e=>{
  const v=e.target.value.toLowerCase();
  document.querySelectorAll('.card').forEach(c=>{
    c.style.display=(!v||c.textContent.toLowerCase().includes(v))?'':'none';});
};
load();
</script></body></html>''')

    os.makedirs(a.out_dir, exist_ok=True)
    html_path = os.path.join(a.out_dir, "hud-map-icons-catalog.html")
    open(html_path, "w", encoding="utf-8", newline="\n").write("".join(parts))

    csv_path = os.path.join(a.out_dir, "hud-map-icons-catalog.csv")
    with open(csv_path, "w", encoding="utf-8-sig", newline="\n") as f:
        f.write("组,基名,图片,资源ID,变体名,白名单\n")
        for k in order:
            for b, it in groups[k]:
                f.write("%s,%s,%s,%s,%s,\n" % (
                    KIND_LABEL.get(k, k), b, it["png"],
                    " ".join("%012X" % i for i in sorted(it["ids"])),
                    " ".join(it["names"])))
    print("wrote %s (%d icons)" % (html_path, total))
    print("wrote %s" % csv_path)
    for k in order:
        print("  %-24s %3d" % (KIND_LABEL.get(k, k), len(groups[k])))
    if unclassified:
        print("  未分类 %d（未写入 CSV）" % len(unclassified))
    return 0


if __name__ == "__main__":
    sys.exit(main())
