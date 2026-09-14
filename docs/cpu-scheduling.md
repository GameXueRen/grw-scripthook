# CPU 调度控制（corefix 模块）

按游戏启动的三个阶段分别设置**处理器集合**与**进程优先级 / 效率模式**，并把当前状态以只读 API 提供给插件。
配置在 `scripthook.ini` 的 `[loader]` 段，界面在「ScriptHook 设置 → CPU 调度控制」。

| 键 | 阶段 | 取值 |
| --- | --- | --- |
| `cpu_boot` / `cpu_window` / `cpu_play` | Logo / 窗口加载 / 游玩 | 处理器集合档位（0～8，游玩档多出"禁 CPU0"组合）|
| `cpu_prio_play` | 游戏中 | `0` 不干涉 · `1` 正常 · `2` 高于正常 · `3` 高 |
| `cpu_eco_boot` | Logo + 窗口加载（这两个阶段共用）| `0` 关（默认）· `1` 开：这两个阶段保持效率模式；**本机不支持时改为保持「低」优先级类** |
| `cpu_cores` | 游戏中（仅此阶段）| 0～64，最大逻辑核心数上限 |

「CPU 调度控制」页上的六个行（按顺序）：`1-启动Logo窗口加载`（cpu_boot）、`2-游戏主窗口加载`（cpu_window）、`加载阶段使用效率模式`（cpu_eco_boot）、`3-游戏中`（cpu_play）、`游戏中的CPU优先级`（cpu_prio_play）、`游戏中的CPU核心数（0为不限制）`（cpu_cores）。

所有键都在**启动时读取一次**，改完要重启游戏；不提供"实时"优先级（会拖累系统输入与音频）。

## 效率模式（EcoQoS）是什么

套用 `SetProcessInformation(ProcessPowerThrottling, PROCESS_POWER_THROTTLING_EXECUTION_SPEED)`，也就是任务管理器里那一列「效率模式」：系统会倾向于把该进程安排到能效核、允许它降频，以更低功耗完成"不贡献前台体验"的工作。

- **它不是一个优先级类**，所以 `cpu_prio_*` 里的 `4` 与 `1..3` 走的是两条不同的路（前者不改变进程的优先级类）。
- 官方文档只保证**能效取向**，**不承诺提速**。它能加快启动只在部分机器/部分阶段成立，是否值得用请以自己机器上的实测（两次启动的日志时间戳）为准。

### 只在 Windows 11 上有效（重要）

微软在 `SetProcessInformation` 的说明里写明：**EcoQoS 这一级是 Windows 11 才有的**，在那之前的系统上，同样的请求只会把进程标成 **LowQoS**，并不是"效率模式"。

所以本模块的处理是**不假装成功**：

| 情形 | 行为 |
| --- | --- |
| Windows 11（build ≥ 22000） | 正常：那两个阶段保持效率模式，任务管理器「效率模式」列可作外部见证 |
| Win10 及更早（build < 22000） | **退化为「低」优先级类**：一次都不调用 `SetProcessInformation`，改为保持 `IDLE_PRIORITY_CLASS`；`ShCpuStatus.eco` 报 `SH_ECO_NA`，日志写一行（带 build 号）说明原因 |
| 系统没有 `Set/GetProcessInformation`（很老的系统） | 同上：退化「低」+ `SH_ECO_NA` + 一行日志；因为走 `GetProcAddress`，**不会让 `dinput8.dll` 加载失败** |
| 调用失败（例如结构版本/大小写错，`GetLastError() == 87`） | 报 `SH_ECO_FAILED` **并改持「低」**，**只记一次**日志（含错误码），这一局不再重试 |
| 系统版本探测不到 | 当作"足够新"照常尝试（宁可试一下，也不因为探测失败就废掉功能） |

一句话：**这个开关在任何机器上都会做点什么** —— 有办法做效率模式就做，没有就用「低」把机器让出去。**处理器集合与游玩阶段优先级完全不受影响**（它们没有系统版本要求）。

**菜单里的可见性**：只要开关是开的，而本机不支持（`SH_ECO_NA`）或设置失败（`SH_ECO_FAILED`），「CPU 调度控制」页的提示行会多一行写明**加载阶段已改用「低」优先级** —— 与"禁小核在不支持的 CPU 上"用的是同一套做法，避免"开了、以为生效了、其实什么都没发生"；底部状态行也会如实显示「低」。

## 三个阶段怎么判、什么时候走

三个阶段是**依次走过、不会回头**的三步，同一次游戏里各进入一次；判据还是两个（游戏自己的窗口、游戏自己的状态），其中窗口这一半由**类名与标题两个特征**共同判定：

| 阶段 | 从 | 到 | 判据 |
| --- | --- | --- | --- |
| **Logo** | 进程启动 | 游戏主窗口出现 | 两个窗口靠**两个特征**分辨：**窗口类**（启动画面 `ScimitarSplashScreenWindow`，主窗口 `ScimitarEngineWindowClass`）与**标题**（启动画面里注册商标是乱码 `Ghost Recon?Wildlands`，主窗口正常 `Ghost Recon(R) Wildlands`）。**先看类名**（类名是结构性的，乱码只是编码意外），类名不认识时退回只看标题 —— 所以"主窗口还没起来"是精确的 |
| **窗口加载** | 主窗口出现 | 进主菜单 | 上一步之后的全部时间：这是第一次加载，引擎自己还在忙 |
| **游戏中** | **首次进主菜单** | 退出游戏 | 引擎自己的状态说 `MenuOrLobby`（主菜单和所有大厅是同一个桶，所以大厅不是单独一档）|

之后的一切 —— 主菜单、大厅、之后的加载界面、暂停菜单、世界 —— 都算**游戏中**：过了前端，"引擎还在启动吗""世界的任何一部分起来了吗"这两个问题都已经问完、答完，不值得再分。**阶段只会前进**（代码里用"地板"实现，连看门狗都只能抬升地板），所以插件可以放心在第一次收到 `SH_STAGE_PLAY` 时锁存。

两个特征**不一致**时（某个 build 改了类名，或修好了标题乱码），日志会记一行 `the class and the title disagree about which window this is`，并**以类名为准**；类名不认识时退回只按标题判，与引入类名之前的行为完全一致。判定走通之后，日志还会写明这次是**谁认出来的**（`named by its class` / `named by the mis-encoded mark in its title`），一眼能看出类名是否真的在起作用。

两个 **120 秒看门狗**只为"信号永远不来"兜底（两个窗口都改名/改标题了的 build；状态机 hook 没装上的 build）：各自把地板抬到下一档并记一行日志。第二个只在状态机**从未读到过**时才触发 —— 能读到就说明信号是真的，阶段就一直等它，第一次加载再慢也等。

规则：

1. 加载阶段（Logo / 窗口加载）→ 开关开着就保持效率模式（本机不支持则保持「低」），并记下"这份是我们开的"；
2. 游戏中 → **先**把加载阶段的那份收掉（效率模式关掉并还原成进程进来时的状态；若之前是退化的「低」优先级类，则把类还给进程进来时的状态，通常就是"正常"），**再**应用「游玩阶段优先级」，或按"不干涉"什么都不设。顺序是刻意的：进入游戏中的那一刻，进程不会停留在"效率模式还开着、游玩优先级已经设好"的中间态；
3. **进程启动前/运行中被外部开启的效率模式一律不动** —— 我们只关自己开的那一份；
4. 与优先级类一样，每 250 ms **复查一次**：该开的没开就重新开，我们自己开过、现在不该开的就关掉（外部程序可以直接改，我们的钩子看不到）。

> 一个边角写在这里，因为它是唯一的例外：如果进程**进来时就已经是开启状态**（比如你用任务管理器或 Process Lasso 给它开了），而某个阶段的档位又要求"效率模式"，那么到了不要求该模式的阶段，我们**不会把"开启"这个状态还原回去**（那等于把该阶段要丢掉的降频又装回来），而是明确关闭。此时日志会写明原因。

## 公共 API（`@defgroup cpu`，scripthook.h）

只读：框架是进程优先级/亲和性的唯一所有者，不开放给插件改写（两个所有者会互相打架）。

```c
#include "scripthook.h"

ShCpuStatus st;
ShCpuGetStatus(&st);              /* 档位、当前阶段、允许集、核数、效率模式状态 */

int      stage = ShCpuStage();    /* SH_STAGE_BOOT / WINDOW / PLAY */
uint64_t mask  = ShCpuAllowedMask();     /* 允许运行的处理器集合，0 = 没裁剪 */
unsigned n     = ShCpuReportedCount();   /* 引擎被告知的核数，0 = 原样 */

ShCpuOnStageChange(OnStage, NULL);       /* 阶段变化时回调（每个插件一个槽，共 8 个） */
```

`st.eco` 就是上表的 `SH_ECO_*`（`SH_ECO_ON` 且 `st.ecoOurs == 0` 表示"开着，但不是我们开的"）。

`ShCpuStage()` 是**框架自己的启动阶段读数**，不是第二个游戏状态：它与 `ShGetGameState()` / `ShGetUiState()` 回答的问题不同（后两者不区分 Logo 窗口与游戏窗口）。用法举例：Logo/窗口加载阶段别做大动作、进入 `SH_STAGE_PLAY` 再做。

**即使一个档位都没设**（全部"不干涉"），阶段照常跟踪、回调照常触发、查询照常有效 —— 只是不装任何钩子、不改任何设置。

## 启用档位后，进程里所有调用者看到的答案会被框架代答

这是本模块最重要的副作用，写清楚比让人意外好：只要有一个档位在生效，下面这些 API 对**整个进程**（包括其它插件、包括游戏自己）都返回被裁剪后的答案：

| 挂钩 | 答案 |
| --- | --- |
| `GetSystemInfo` / `GetNativeSystemInfo` / `GetActiveProcessorCount` / `GetMaximumProcessorCount` / `GetLogicalProcessorInformation(Ex)` / `NtQuerySystemInformation`（class 0、73）| 裁剪后的核数与拓扑（`GetActiveProcessorGroupCount` 恒为 1）|
| `SetProcessAffinityMask` / `SetThreadAffinityMask` / `SetThreadIdealProcessor(Ex)` / `NtSetInformationProcess`（21）| 请求被求交或按"第 i 个允许的处理器"重映射 |
| `SetPriorityClass` / `NtSetInformationProcess`（18）| 有档位持有时被改写成本框架的类（调用返回成功，类被顶回）|

所以：**插件按 `GetSystemInfo()` 的核数开线程池是对的** —— 拿到的是真正可用的那个集合；想知道"这是不是被改过"，用 `ShCpuAllowedMask()` / `ShCpuReportedCount()`。

**全部"不干涉"时 `g_keepMask == 0 && g_prioNow == 0`，每个挂钩原样透传**，一个字节都不改。

> 与插件的边界：这些是进程级挂钩，插件自己的线程与 MinHook 副本框架不接管；第三方插件二进制里没有任何调度 API 的引用（23 个 `.asi` 全扫过），仓库内其它模块的挂钩目标与这 15 个零重合。

## 怎么验证

- **任务管理器 → 详细信息**：右键列头勾出「效率模式」，启动阶段 GRW.exe 显示"已启用"，进入游玩后变为关闭（**仅 Win11 有此列**）；
- `logs\scripthook_corefix.log` 应能看到：
  - `efficiency mode: available (Windows build 22621)` 或 `not applicable - ... build 19045`（一行，带 build 号）；
  - `efficiency mode: found it off (control 0x0, state 0x0); it is left alone unless a dial asks for it`（来时快照）；
  - `priorities loading=efficiency mode (the switch is on) play=leave alone (found 0x20)`；本机不支持时则是 `loading=low (the switch is on)`；
  - 进主菜单（游戏中阶段开始）时：`... efficiency mode OFF - an earlier stage turned it on and this one does not ask for it (put back as it was found)`；本机不支持、退化过「低」的那条路则是 `... priority put back as it was found (0x20) - the low the loading switch fell back to is given back with it`；
  - 任何失败有一条带 `GetLastError()` 的行（`87` = 参数不对，即结构版本或大小写错）；
- 菜单「CPU 调度控制」底部状态行：`当前：<行名> · 核心控制 <档位> · 优先级 <档位>`（阶段名用的是同一套行名，例如 `当前：3-游戏中 · 核心控制 禁CPU0 · 优先级 效率模式`）；加载阶段开着开关时优先级显示「效率模式」，本机不支持时显示「低」（提示行另有说明）；
- 想判断"到底快不快"：把两次启动（开 / 关效率模式）的日志时间戳对比，结论以实测为准。
