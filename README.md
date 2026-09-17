# GRW ScriptHook 魔改版

此为《幽灵行动：荒野》ScriptHook模组的魔改版，以dinput8.dll代理的形式随游戏加载，不修改游戏本体文件的脚本扩展框架。

魔改版重写了游戏内菜单，多语言适配，性能及功能上的BUG修复和优化。

内置CPU调度控制功能、Forge资源侧载功能。新增和集成各种插件，并重新适配游戏最新版本。

---

### 原作者： **Phiality** | [grw-scripthook](https://github.com/PhialsBasement/grw-scripthook)

### 魔改版QQ交流群： **299177445**

---
## 功能完成状态

| 功能清单 | 状态 |
| --- | --- |
| D3D11/ImGui 覆盖层菜单 | 已完成 |
| 插件目录与配置 `plugins\` 规范化 | 已完成 |
| 菜单多语言适配框架 | 已完成 |
| 载具名称（65 种，已汉化） | 已完成 |
| 重写第一人称（含BUG修复及优化） | 已完成 |
| 增加根菜单行排序功能 | 已完成 |
| 游玩模式识别 | 已完成 |
| 插件模式黑名单 | 已完成，默认在Ghost War / Mercenaries中禁用 |
| Forge Mod Loader 游戏资源侧载热替换（mods\）| 部分完成，机制已验证 |
| Forge Mod Loader支持载具皮肤替换 | 已完成（机制待验证） |
| Forge Mod Loader支持武器数据修改替换 | 已完成（机制待验证） |
| 插件自绘 UI 公共 API（示例插件 `draw_sample`） | 已实现，见 [`docs/ui-drawing.md`](docs/ui-drawing.md) |
| 重写插件：光学迷彩加强（OpticalCamo） | 已完成 |
| 新插件开发：跳过启动时的动画视频（skipintro）| 已完成 |
| 新功能开发：CPU调度控制 | 已实现，待大规模验证 |
| 新插件开发：「最后的仪式」闪退修复（LastRites_dlcfix） | 已完成，官方已于9月15日更新修复 |
| 新插件开发：自绘输入框（支持输入中文），支持全屏模式使用 | 已完成 | 
| 重写插件：弹药上限 | 已完成 |
| 重写插件：天气 & 时间实时控制 | 已完成 |
---

## 待完成功能计划

> 此表由维护者持续更新；欢迎在 Q 群提出建议。

| 功能清单 | 状态 | 备注 |
| --- | --- | --- |
| 魅影模式不删档（GhostNoWipe） | 验证中，需重新适配游戏最新版本 | 新插件开发 |
| 敌人增援强化插件 | 验证中，需重新适配游戏最新版本 | 新插件开发 |
| 集成自动修复游戏麦识别故障方案 | 调研中 |
| 长按4键快速选择道具轮盘界面 | 计划中 |  |
| 按V键快速选择载具召唤界面 | 计划中 |  |
| 其他模式退回主菜单无需重启游戏 | 引擎机制，经调研不可行 |
| 全民公敌插件，反抗军也处于敌对状态 | 计划中 | 新插件开发 |



---

## 安装

1. 将 `dinput8.dll`、`plugins\`、`mods\`、`scripthook.ini` 放入游戏根目录（与 `GRW.exe` 同级）。
2. 各种插件放入对应的路径 `plugins\<插件名>\<插件名>.asi`（每个插件一个文件夹）。

```
Tom Clancy's Ghost Recon Wildlands/
├── GRW.exe
├── dinput8.dll        模组本体
├── scripthook.ini     主配置文件
├── logs/              运行日志
├── mods/              Forge Mod Loader 的 mod 目录（可选，见下）
└── plugins/           插件目录（每个插件一个子文件夹）
```

---

## 卸载

1. 关闭游戏。
2. 删除游戏根目录下的 `dinput8.dll`、`scripthook.ini` 、 `plugins\` 、`mods\`目录。
3. 以下目录按需处理：
   - `logs\` —— 运行日志，可直接删除；
4. 删完即可正常启动游戏，行为与未装本模组时一致。

---

## 主配置文件（scripthook.ini）说明

```ini
[loader]
load_plugins=1          ; 0 = 启动时不加载任何插件
cpu_boot=0              ; 启动Logo加载阶段：        0=不干涉 1=全部核心(强制) 2=禁超线程 3=禁小核 4=禁超线程+小核
cpu_window=0            ; 游戏窗口加载阶段：同上
cpu_eco_boot=0          ; 加载阶段(Logo+窗口加载)使用效率模式：0=不干预(默认) 1=开 2=关
cpu_play=0              ; 游玩阶段：0=不干涉 1=全部核心(强制) 2=禁超线程 3=禁小核 4=禁超线程+小核 5=仅禁CPU0 6=禁超线程+CPU0 7=禁小核+CPU0 8=三者全禁
cpu_prio_play=0         ; 游玩阶段优先级：  0=不干涉 1=正常 2=高于正常 3=高
cpu_cores=0             ; 游玩阶段最大逻辑核心数（0 = 不限制）

[plugins]
firstperson=0                 ; 每个插件一行：1 = 加载；0 或"没有这一行" = 不加载
                        ; 首次扫描会把缺行的插件自动补成 =0；删掉本配置文件可把插件全部重置为关闭

[forgemod]
enabled=0               ; Forge Mod Loader：1 = 加载 mods\（默认关）
dry_run=0               ; 1 = 只解析并写日志，不实际叠加
report_copies=1         ; 1 = 检测同一资源在其它归档里的副本并提示
apply_all_copies=0      ; 1 = 把 mod 自动叠加到那些副本
probe=0                 ; 1 = 安装取证探针（排查用）
log_reads=0             ; 1 = 记录每次读取（排查用）

[Settings]
LogLevel=warn           ; 日志等级：debug, info, warn, error
Languages=zh-CN,en-US   ; 设置菜单中可选的语言列表（逗号分隔，BCP-47 语言码）
Language=zh-CN          ; 当前菜单语言
```

---

## Forge Mod Loader（游戏资源侧载热替换）

把 `mods\` 目录下的松散文件**侧载**到已有的 `.forge` 条目上：**不解包、不重打包、
不改动原版游戏文件、不写盘**。

**开启**：在游戏根目录建 `mods\`，并在 `scripthook.ini` 里设 `[forgemod] enabled=1`
（默认关）。F4 菜单里的「Forge资源侧载（实验功能）」页有开关与状态行。

```
mods/<归档名>/<文件>                  扁平布局，优先级最高
mods/<mod名>/<归档名>/<文件>          按 mod 文件夹名升序，先者优先
mods/~<名字>/...                      "~" 前缀 = 该 mod 被禁用
.../<序号>_-_<条目名>.data            文件名里的序号指定条目
```

**两条硬性约束**（务必先读）：

1. **替换内容必须 ≤ 原条目长度**。该容器里 payload 是紧挨着存放的（没有空隙），
   所以「可用空间」就等于原长度；更大的替换需要移动其它条目，本版本不做，会被
   拒绝并写入日志。
2. **只替换已有条目**，不新增、不删除。`.delete` 后缀会被识别但暂不执行。

**同一资源常存在于多个归档**（例如 `W_ASR_AK47_body_LOD0` 同时存在于
`DataPC.forge`、`DataPC_patch_01.forge` 及 DLC 归档）。只改一份可能被另一份遮蔽，
看起来像「mod 没生效」。loader 会**按 FileDataID 跨归档检索**并把其它副本写进
日志；`apply_all_copies=1` 可自动一并覆盖。改动应优先打到 `*_patch_01`（它胜过其
base 文件）。

**做 mod**：用 WildlandsToolkit 的 `wlcli` 导出条目（`wlcli entry <forge> <index> <out>`）
→ 修改 → 放回 `mods\<归档名>\`。完整说明、归档角色对照表与实测结论见
[`docs/forge-mod-loader.md`](docs/forge-mod-loader.md)。

### 插件菜单的多语言（`lang.ini` 用法示例）

菜单里的**每一段文本都是一个键**，显示前按四层顺序查找：

```
① plugins\<插件名>\lang.ini   当前语言      ← 该插件的文案
② <gamedir>\lang.ini          当前语言      ← 框架文案
```

键有两种写法，按 `@` 前缀机械区分：

| 键 | 含义 |
|---|---|
| `@fp.hidehead` | **稳定 ID** |
| `Hide head` | **字面量**（英文原文）。给**没有源码、无法改代码的第三方插件**用 |

以 firstperson 为例 —— `plugins\firstperson\lang.ini`：

```ini
[zh-CN]
"@fp.page"           = 第一人称
"@fp.page.hint"      = W/S 选择，回车确认，Esc 返回
"@fp.hidehead"       = 隐藏头部
```

其中 ID 与英文同处，写在插件自己的 `.c` 里（每语言一张表，加语言 = 加一张表 + 一行注册）：

```c
static const ShText kEn[] = {
    { "@fp.page",      "First person" },
    { "@fp.page.hint", "W/S select, Enter confirm, Esc back" },
    { "@fp.hidehead",  "Hide head" },
};
```

**第三方插件（没有源码）同样能汉化**——它代码里的字面量本身就是键，只要在它的目录里放一份 `lang.ini`，它的 `.asi` 一个字节都不用改：

```ini
; plugins\DayNightVisibility\lang.ini
[zh-CN]
"Day/Night Visibility"      = 敌人昼夜感知
"Day/Night Visibility.hint" = 调整敌人视觉探测灵敏度。
```

## 构建

- **Windows（MSVC）**：`pwsh ./build_msvc.ps1`
- **Linux / Proton（MinGW）**：`make`
---

## 致谢

本项目的框架与插件参考源自多处社区作者，一并致谢：

- **Phiality** —— 上游框架 [grw-scripthook](https://github.com/PhialsBasement/grw-scripthook)。
- **Nexus 社区 MOD 作者** —— 下列插件的初版来自这些作品，本仓库的版本是重写与适配（含 BUG 修复与性能优化）：
  - 第一人称 —— [intifofo](https://www.nexusmods.com/ghostreconwildlands/mods/20)
  - 延展视野范围 —— [Phiality](https://www.nexusmods.com/ghostreconwildlands/mods/111)
  - Wildlands Toolkit —— [AlphaGlyph1371](https://www.nexusmods.com/ghostreconwildlands/mods/114)
  - 天气 & 时间实时控制 —— [IHateHUDClutter](https://www.nexusmods.com/ghostreconwildlands/mods/124)
  - 光学迷彩加强 —— [IHateHUDClutter](https://www.nexusmods.com/ghostreconwildlands/mods/126)
- **测试与反馈** —— 感谢内测群各位兄弟的反馈。

---

## 许可

GPL-3.0
