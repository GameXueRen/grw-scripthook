# 逆向：Time&Weather.asi（无源码插件）→ 重写规格

对象：`plugins\Time&Weather\Time&Weather.asi`（17,408 字节），出处见同目录
`作者.txt`：<https://www.nexusmods.com/ghostreconwildlands/mods/124>。

> **后续**：该插件已由 `TimeWeatherControl`（原名 `TimeWeather2`）**正式替代** ——
> 旧插件目录（含 `.asi`、`Time&Weather.ini`、`DynamicTime&Weather.ini`、`lang.ini`、
> `作者.txt`）与 `scripthook.ini` 里的开关都已从仓库与游戏目录删除，仓库侧
> `plugins\Time&Weather\lang.ini` 也已删除。因此上面这些路径只存在于本文
> —— 它们是**当时的取证清单**，不是当前目录结构。

本文是**静态逆向**（`dumpbin /imports` + `/disasm` + 原始字节）的结论，也是重写它的规格。
所有 RVA 都相对 `Time&Weather.asi` 自己的映像基址 `0x180000000`。

## 0. 一句话结论 —— 这个插件不需要任何逆向出来的"手段"

**它一个钩子都不装。** 它的导入表里没有 `VirtualProtect`、没有 `VirtualAlloc`、没有
`WriteProcessMemory`，只有读 ini、开线程和 `GetProcAddress`：

```
GetModuleHandleA  CreateThread  Sleep  GetProcAddress
GetPrivateProfileStringA  WritePrivateProfileStringA  GetCurrentProcessId
```

它做的全部事情，是**用本框架自己的 API** 控时间与天气（字符串表里逐个出现的
`GetProcAddress` 目标）：

```
ShGetVersion            ShIsInGame            ShMenuCreate      ShMenuSub
ShMenuAction            ShMenuList            ShMenuStatus      ShMenuDestroy
ShGetTime               ShSetTime             ShGetTimeSpeed    ShSetTimeSpeed
ShSetWeatherBlend       ShReleaseWeather
```

所以它是本框架的**纯消费者**：重写这件事因此不是"复刻手段"，而是"用同一套 API
把界面与调度重做一遍"，硬件上零风险 —— 这也是本轮把它排在 AmmoCapacity 之后
的理由（后者必须逆向出钩点，前者只需要读出语义）。

## 1. 它读写的文件

| 文件 | 内容 |
|---|---|
`plugins\Time&Weather\DynamicTime&Weather.ini` | `[DynamicTimeWeather]` `DaySpeed=2.0` / `NightSpeed=2.0` / `Weather=Default` |
`plugins\Time&Weather\Time&Weather.ini` | 它的 `[zh_cn]` 文本段（旧形态）|
仓库侧 `plugins\Time&Weather\lang.ini` | i18n 迁移后的文本（语言码段 + 英文字面量键）|

**实测印证**：用户那份 `DaySpeed=2.0`，而框架侧的探针在同一局里读到
`ShGetTimeSpeed` 恰为 `2.000`（见 A 轮日志）—— 说明它把 ini 的**档位值原样**下发给
`ShSetTimeSpeed`，不做缩放、不做换算。这条把"语义"这一栏直接钉死了。

## 2. 它的文本面（重写要保持这些词不变）

`lang.ini` 里 22 行，逐条对应它的菜单结构：

```
页面   Time & Weather
行     Day Speed / Night Speed / Weather
子页   Set Time  →  Hour / Minute / AM / PM / Apply Time
读数   Time / Rate
天气   Default / Sunny / Light Clouds / Heavy Clouds / Fog / Light Rain / Heavy Rain
其他   Waiting for game...
```

## 3. 昼夜调度：常量表读出来的规则

它的浮点常量集中在 `.rdata` 的 `RVA 0x4654`–`0x4694`（连续 16 个 dword），
按反汇编里引用它们的指令读回来：

| 常量 | 引用处 | 读出来的意思 |
|---|---|---|
| 5.0 / 5.5 / 6.5 / 7.0 | `movss xmm13/xmm0/xmm5` | **黎明的渐变窗**：5:00 → 7:00 |
| 18.0 / 18.5 / 19.5 / 20.0 | `movss xmm5/xmm15` + 一处 `comiss xmm1,20.0` | **黄昏的渐变窗**：18:00 → 20:00 |
| 0.5 | `mulss xmm2,0.5` | 过渡中点的插值因子 |
| 1.0 | `movss xmm11,1.0` | 满速基准 |
| 0.001 / 0.01 | `comiss xmm0,0.001` | 小到可视为"暂停"的速率判定 |
| 60.0 | `divss xmm0,60.0` | 读数换算（秒 ↔ 分）|
| 3600.0 | `mulss xmm6,3600.0` | 时速换算（小时 ↔ 秒）|
| −24.0 / −1.0 | `movss xmm1,-24.0` | 跨零点的回绕 / 边界判定 |

**规则**（照此实现）：白天在 **7:00–18:00** 之间用 `DaySpeed`，夜里在 **20:00–5:00**
之间用 `NightSpeed`，**5:00–7:00 与 18:00–20:00 两个窗内线性过渡**（这正是它带 0.5 与
中间点 5.5/6.5/18.5/19.5 的原因），速率小到 0.001 量级时按"暂停"处理。

**节拍**：`Sleep` 的调用点是 **500 ms（两处）与 250 ms（一处）**（`mov ecx,1F4h` /
`mov ecx,0FAh` → `call [0x180004010]`）。重写取 250 ms 即可覆盖昼/夜切换与读数刷新。

## 4. 天气

七项对应框架的 `ShWeather` 枚举（`SUNNY / CLOUDS_LIGHT / CLOUDS_HEAVY / FOG /
RAIN_LIGHT / RAIN_HEAVY`）+ `Default`。它用的是 **`ShSetWeatherBlend`**（不是
`ShSetWeather`）：即每次切换都带一段过渡，而不是硬切；`Default` 走
`ShReleaseWeather`（它解析了该符号，且这是"交还给引擎"的唯一入口）。

## 5. 与重写（`TimeWeatherControl`）的对应

| 旧插件 | 重写 |
|---|---|
零钩子，全部走框架 API | **同上**：只用 `ShGetTime/ShSetTime/ShGetTimeSpeed/ShSetTimeSpeed/ShSetWeatherBlend/ShReleaseWeather` + 菜单 API |
`[DynamicTimeWeather] DaySpeed/NightSpeed/Weather` | 全新键名 `[TimeWeather] enabled / day_speed / night_speed / weather / hour / minute`（**老配置需手工迁移两行速度**）|
菜单：一页 + 一个 `Set Time` 子页 | 重新设计为一页，全部行同一种读法：`启用`（列表 关/开）、`天气`（七档）、`小时`（**24 小时制** 0-23）、`分钟`（0-59）、`应用当前设置的时间`（动作行）、`白天时间流逝速度` / `夜晚时间流逝速度`（**0.00~10.00，步进 0.25**，1.00 = 原版）|
文本：英文字面量键 | 编译期 `kEn`/`kZh` 基线 + **`@tw.*` 稳定 ID**，`lang.ini` 只作覆盖层 |
`Waiting for game...` | 用 `ShIsInGame()` gating，未进游玩时显示同一句话且**不下发任何写操作** |
（无）| 声明 `ShPluginBlacklist(SH_MODE_BLACKLIST_GHOST_WAR | SH_MODE_BLACKLIST_MERCENARIES)`，被禁时**主动交还**（`ShReleaseWeather()` + `ShSetTimeSpeed(1.0f)`）|
（无）| 昼/夜调度照 §3 的窗内插值实现；**读数放状态行**（`时:分` + 四个时段名、方括号标出当前时段 + **实际流速**读回），不占用任何行的值 —— 行的值归玩家输入，实时去写它就会和玩家的手打架；总开关关闭时该行显示 `已关闭`/`Off` |

## 6. 未确认（重写时用日志补齐，不影响开工）

- §3 的"渐变窗"是从**常量组合**读出来的（中间点 5.5/6.5/18.5/19.5 + 0.5 因子），
  没有逐条追指令确认插值公式；若重写后的观感与原版有差，以实测读数为准微调。
- 重写时踩到的坑，记下来免得再犯：**两位小数的倍率必须按小数读** ——
  `GetPrivateProfileInt` 会把 `1.00` 读成 `1`，除以 100 就变成 `0.01x`，
  表现是"时钟几乎停住"而 ini 上明明写着 `1.00`。读小数一律
  `GetPrivateProfileStringA` + `atof`（重写里的 `IniSpeed`）。
- `Apply Time` 的下发顺序（先 `ShSetTime` 再读回、还是同时设速率）未逐条确认；
  重写按"先设时间，再按当前小时重算速率"实现。
- 它解析了 `ShMenuDestroy`，说明页面可被重建；重写用同一套做法（切语言时重建）。
