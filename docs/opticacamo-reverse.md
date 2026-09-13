# OpticalCamo.asi 逆向报告（光学迷彩加强）

日期：2026-09-14
对象：`<gamedir>\plugins\OpticalCamo\OpticalCamo.asi`（第三方插件，17408 字节，PE32+ x64 DLL，无导出）
结论：**它是运行在本项目 ScriptHook 之上的第三方 MSVC C++ 插件**，靠
`GetModuleHandleA("dinput8.dll")` + `GetProcAddress` 晚绑定 11 个框架导出；
它判断"光学迷彩是否生效"的办法是**逐部件读玩家实体节点 `+0x54` 处的 2 字节，
按 bit7 投票**。行为已完整还原，并按本框架 API 重写为同目录下的
`OpticalCamo.c`（见本报告第十节）。

---

## 一、结论摘要（先读这一段）

1. **它不静态导入 dinput8**。导入表只有 `KERNEL32.dll` / `VCRUNTIME140.dll` /
   `api-ms-win-crt-*`。11 个 `Sh*` 名字是 `.rdata` 里的字符串，由
   `GetProcAddress` 逐个绑定（解析块 `0x180001770`）。
2. **"识别光学迷彩"不是一个 API，而是一次读内存的投票**：
   `ShIsInGame()` → `ShGetPlayer()` → 取 `ShPlayer.entity` →
   `ShGetEntityNodes(entity, nodes, 64)` → 对每个节点
   `ShReadBytes(node + 0x54, &u16, 2)` → 统计 bit7。**全部置位 = 未激活；
   全部清零 = 激活；读失败或结果不一致 = 状态不可用**（三态，见第四节）。
3. **它只做一件事**：在"激活"那一态，把框架的可见度因子
   （`ShSetVisibility`；原版四档 `0.0/0.25/0.50/0.75`，重写版九档，见第十节）
   乘到玩家身上；未激活时把因子复位成
   `1.0`。可见度因子本身就是框架 `scripthook_stealth.c` 里那条
   `mulss xmm8, xmm9`（`SH_IMG(0x14393508)`）旁路乘数。
4. 它还有一套**跨插件共享内存总线**（`W_VisibilityBus_v2`，44 字节，tag `"VIS2"`），
   用来和同进程内的"消费者"交接：有消费者时它只发布状态并读回因子（**Linked**），
   没有消费者时它自己应用因子（**Direct**）。状态行前缀就是这个意思，不是两套读法。
5. 它自带**版本门**（`ShGetVersion() == 1`）与**游戏构建门**
   （PE 头 `TimeDateStamp == 0x6A7C5143` 且 `SizeOfImage == 0x18B09000`）。
   两者都是为了让"直接写内存"的旧实现认构建；**本框架里没有意义**。

---

## 二、取证方法与工具

本机无 IDA / Ghidra，`objdump` / `gdb` 不在 PATH；用 MSVC 自带的 `dumpbin`：

| 步骤 | 命令 |
|---|---|
| 找工具 | `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\dumpbin.exe` |
| 头/节表 | `dumpbin /headers OpticalCamo.asi` |
| 导入表 | `dumpbin /imports OpticalCamo.asi` |
| 反汇编 | `dumpbin /disasm OpticalCamo.asi` |
| 数据段与字符串地址 | `dumpbin /all OpticalCamo.asi` |

**关键手法**：`dumpbin /disasm` **不打印 IAT 符号名**（只给
`call qword ptr [0000000180004018h]` 这种形式）。所以：

1. 用 `/imports` 拿到 IAT 基址（`0x180004000`）与**按顺序**的导入名，
   即可把每个 `[0x1800040xx]` 槽还原成函数：
   `4000 GetModuleFileNameA`、`4008 UnmapViewOfFile`、`4010 GetModuleHandleA`、
   `4018 Sleep`、`4020 GetLastError`、`4028 DisableThreadLibraryCalls`、
   `4030 CloseHandle`、`4038 WritePrivateProfileStringA`、`4040 CreateThread`、
   `4048 GetProcAddress`、`4050 GetModuleHandleW`、`4058 CreateFileMappingW`、
   `4060 MapViewOfFile`、`4068 GetPrivateProfileStringA`、`4070 GetTickCount`、
   `4078 QueryPerformanceCounter`、`4080 GetCurrentProcessId`、
   `4088 GetCurrentThreadId`、`4090 GetSystemTimeAsFileTime`、`4098 InitializeSListHead`；
   VCRUNTIME 段 `40A8 __std_type_info_destroy_list`、`40B0 __C_specific_handler`、
   `40B8 strrchr`、`40C0 memcpy`、`40C8 memset`（CRT 段另有 `strtof`/`sprintf` 助手）。
   三处交叉验证过：解析器用 `[4008+?]`…即 `GetModuleHandleA("dinput8.dll")`、
   `GetProcAddress`、`Sleep(250)`、`GetTickCount`、`CreateFileMappingW/MapViewOfFile`，
   与代码语义完全吻合。
2. 用 `/all` 的 `.rdata` 十六进制转储拿到**字符串的 VA**，再在 `/disasm` 里搜
   `lea r??,[<VA>]`，就能把"哪段代码在用哪条字符串"逐一对上。
3. 框架导出的名字→指针槽是**按解析顺序**成对出现的，直接读连续的
   `lea rdx,[名字]` / `mov [槽],rax` 即可建表（3.1 节）。

### 二进制指纹

| 项 | 值 |
|---|---|
| 大小 | 17408 字节 |
| 类型 | PE32+ x64 DLL，image base `0x180000000`，`.text` 仅 `0x2028` 字节 |
| 时间戳 | `Sat Sep 5 12:02:19 2026` |
| PDB | `C:\Users\crazy\Documents\Visual Studio\source\Repo\OpticalCamo\x64\Release\OpticalCamo.pdb` |
| 导出 | 无（纯插件） |
| 导入 | KERNEL32 / VCRUNTIME140 / api-ms-win-crt-*（**无 dinput8**） |

---

## 三、API 绑定与全局变量表

### 3.1 绑定顺序（解析块 `0x180001770`，位于线程函数 `0x180001520` 内）

`GetModuleHandleA("dinput8.dll")`（字符串 `0x180004298`）→ 存入 `0x1800062A0`；
随后每项为 `lea rdx,[名字]` + `GetProcAddress(hMod, 名字)` + `mov [槽],rax`：

| # | 导出名 | 名字 VA | 指针槽 | 本插件怎么用 |
|---|---|---|---|---|
| 1 | `ShGetVersion` | `0x1800042A8` | `0x1800062A8` | `0x1800018F4`：必须返回 `1`，否则"Unsupported ScriptHook version" |
| 2 | `ShMenuCreate` | `0x1800042B8` | `0x180006270` | `0x180001875`，标题 `"Optical Camo"` |
| 3 | `ShMenuList` | `0x1800042C8` | `0x180006278` | `0x18000188B`，项 `"Camo Visibility"`，4 档 |
| 4 | `ShMenuStatus` | `0x1800042D8` | `0x180006150` | 全部状态行（含 4 个错误文案） |
| 5 | `ShMenuDestroy` | `0x1800042E8` | `0x180006140` | `0x1800018D3`（列表注册失败时）与退出 `0x180001F39` |
| 6 | `ShIsInGame` | `0x1800042F8` | `0x180006268` | 轮询第一步 `0x180001C30` |
| 7 | `ShGetPlayer` | `0x180004308` | `0x180006298` | `0x180001C58`，填 `ShPlayer`（24 字节） |
| 8 | `ShGetEntityNodes` | `0x180004318` | `0x180006290` | `0x180001CA0`，`(entity, nodes, 64)` |
| 9 | `ShReadBytes` | `0x180004330` | `0x1800062B0` | `0x180001CF3`，`(node+0x54, &u16, 2)` |
| 10 | `ShSetVisibility` | `0x180004340` | `0x1800062B8` | `0x1800013F1`（应用档位）与退出时 `1.0f` 复位 |
| 11 | `ShGetVisibility` | `0x180004350` | `0x180006288` | `0x1800012D1` / `0x18000141A`，读回当前生效系数 |

绑定分两批：**1–5 在菜单注册前**（缺任何一个 → `Sleep(250)` 后整段重试，`0x180001834`），
**6–11 在游戏构建门通过之后**（`0x18000198E`–`0x180001A37`），随后 `0x180001A29`
逐一判空，缺任何一个 → 状态行 `"Required ScriptHook API unavailable"`
（`0x180004488`，`0x180001F4D`）。

### 3.2 关键全局（`.data`，段范围 `0x180006000`–`0x1800062D7`）

| 全局 | 初值 | 含义 |
|---|---|---|
| `0x180006078` | `2` | **档位索引**（0..3），`lock cmpxchg`/`xchg` 原子读写，越界一律钳成 `2` |
| `0x180006088` | `1.0f` | 当前生效系数缓存（`float`） |
| `0x18000608C` | `1` | 强制重画标志（菜单回调置 1） |
| `0x180006094` | `-1000.0f` | 上次**画出**的系数（保证第一帧必画；`0x1800012ED` 处做差值比较） |
| `0x18000607C` / `0x180006080` / `0x180006084` / `0x180006090` | `-1` | 上次的 active / linked / available / interval，用于"变了才重画" |
| `0x18000614C` | `0` | "本轮已把因子应用下去"的字节标志 |
| `0x180006148` | `0` | **菜单句柄**（`ShMenuCreate` 的返回值；0 = 没有菜单，状态行直接跳过） |
| `0x180006264` | `0` | **停止标志**：`DllMain` 在 `DLL_PROCESS_DETACH` 时置 1，线程各等待点看它退出 |
| `0x180006158` | — | 自身 `hInstance`（`DllMain(ATTACH)` 存入，用来算 ini 路径） |
| `0x180006160` | — | ini 全程路径 `...\plugins\OpticalCamo\OpticalCamo.ini` |
| `0x180006280` | — | `CreateFileMappingW` 的映射句柄 |
| `0x1800062C0` | — | `MapViewOfFile` 得到的**总线视图基址** |
| `0x180006098` | 4 个指针 | 给 `ShMenuList` 的选项数组：`"0.0x"`/`"0.25x"`/`"0.50x"`/`"0.75x"`（`0x180004228`–`0x180004243`） |

`.rdata` 里的数值常量（原样列出，重写时不再需要）：
`0x1800044D8` 起 4 个 `float` = `{0.0, 0.25, 0.5, 0.75}`（就等于选项数组）；
`0x1800044F0` = `0.0025f`（比较用 epsilon）；`0x1800044F4/4F8/4FC` = `0.25/0.50/0.75`（认档位用）；
`0x180004500` = `1.0f`（复位值）；`0x180004508` = `0.5`（double，默认串来源）；
`0x180004510` = `1000.0f`；`0x180004520` = 4×`0x7FFFFFFF`（取绝对值掩码）。

---

## 四、状态判定（本报告核心）

### 4.1 判定链重建源码

投票线程每轮（`0x180001C30` 起）做的事，等价重写如下：

```c
/* 0x180001C30：不在游戏里 → 直接报"状态不可用" */
if (!ShIsInGame()) { Pump(0 /*available*/, 0 /*active*/); goto next_round; }

ShPlayer pl;                       /* typedef struct { entity; node; root; } */
memset(&pl, 0, sizeof pl);
if (!ShGetPlayer(&pl) || !pl.entity) { Pump(0, 0); goto next_round; }   /* 0x1C58 / 0x1C70 */

uint64_t nodes[64];
memset(nodes, 0, sizeof nodes);                                        /* 0x1C86 */
int n = ShGetEntityNodes(pl.entity, nodes, 64);                        /* 0x1CA0 */
if (n < 1 || n > 64) { Pump(0, 0); goto next_round; }                  /* 0x1CA5 */

int failed = 0, flagged = 0, clear = 0;                                /* r? */
for (int i = 0; i < n; i++) {                                          /* 0x1CD0 */
    uint16_t v = 0;                                                    /* 0x1CE4 */
    if (ShReadBytes(nodes[i] + 0x54, &v, 2)) {                         /* 0x1CF3 */
        if (v & 0x80) flagged++; else clear++;                         /* 0x1DCB */
    } else {
        failed++;                                                      /* esi++ 0x1CFE */
    }
}

if (failed != 0)        Pump(0, 0);   /* 有部件读不到 → 状态不可用  0x1DE4-0x1DE6 */
else if (flagged == n)  Pump(1, 0);   /* 全部置位     → 未激活      0x1DE8-0x1DF2 */
else if (clear == n)    Pump(1, 1);   /* 全部清零     → 激活        0x1DF7-0x1DFC */
else                    Pump(0, 0);   /* 结果不一致   → 状态不可用  0x1DED-0x1DF0 */
```

逐条地址依据：

| 断言 | 地址 |
|---|---|
| `ShIsInGame` 门 | `0x180001C30` `call [0x180006268]`，`test eax,eax / je 0x180001E00` |
| 取玩家、失败即 `Pump(0,0)` | `0x180001C51` `call [0x180006298]`；`0x180001C69` `mov rbx,[rbp+150h]`（= `ShPlayer.entity`，结构第一个字段） |
| 取部件（上限 64） | `0x180001C8B` `call [0x180006290]`，`r8d = 0x40` |
| 逐部件读 `+0x54`，长度 2 | `0x180001CEF` `add rcx,54h`；`0x180001CF3` `call r9`（`ShReadBytes`），`r8d = 2` |
| `ShReadBytes` 返回 1 = 成功 | `scripthook_api.c:309`（`return 1`；失败返回 0）——所以 `0x180001CF8 jne 0x1DCB` 走的是**成功**分支 |
| bit7 统计 | `0x180001DCB` `test byte ptr [rsp+48h],80h` → `r14d++` / `edi++` |
| 三态判决 | `0x180001DE4`–`0x180001E03`（详见上面注释的地址） |
| 每轮间隔 50 ms | `0x180001E0B` `mov ecx,32h` + `Sleep` |

### 4.2 三态的含义，以及"装备/未装备"为什么不可区分

- **激活（Active）**：玩家的**每一个**部件在 `+0x54` 处的 bit7 都是 0。
- **未激活（Inactive）**：每一个部件该位都是 1。
- **状态不可用（State unavailable）**：进不了游戏、拿不到玩家/部件、某个部件读失败，
  或**部件之间不一致**（例如正在切换、部分部件未加载）。

读的是"部件级标志"，**不是背包/装备槽**。所以这份插件**只能知道"迷彩此刻有没有生效"**，
而**无法区分"没装光学迷彩"和"装了但没开"**——两者都会落到未激活或状态不可用。
（这也是本轮不做"通用当前装备 API"的直接原因，见第十节。）

---

## 五、菜单、ini 与本地化

**菜单**（`0x180001875` 起）

```c
uint32_t menu = ShMenuCreate("Optical Camo");                 /* 0x180004420 */
if (!menu) goto wait_stop;
idx = clamp([0x180006078], 0, 3);                             /* 0x18000185B */
ShMenuList(menu, "Camo Visibility",                           /* 0x180004430 */
           opts /* "0.0x","0.25x","0.50x","0.75x" @0x180006098 */,
           4, idx, OnCamoVisibility, NULL);                   /* 回调 0x1800014F0 */
if (!ok) { ShMenuDestroy(menu); goto wait_stop; }              /* 0x1800018C1 */
ShMenuStatus(menu, "Initializing");                            /* 0x180004440 */
```

**选项回调** `0x1800014F0(menu, item, value, user)`：

```c
if ((unsigned)value > 3) return;          /* 越界忽略，不落盘 */
[0x180006078] = value;                    /* 原子写档位 */
SaveSetting(value);                       /* 0x180001080 */
[0x18000608C] = 1;                        /* 强制下一帧重画状态行 */
```

**SaveSetting** `0x180001080(idx)`：`idx = min(idx, 3)`（越界取 2）→
`f = table[idx]` → `sprintf(buf, "%.2f", f)` → `WritePrivateProfileStringA("OpticalCamo",
"Visibility", buf, iniPath)`（字符串 VA：`"%.2f"` `0x18000426C`、`"Visibility"`
`0x180004278`、`"OpticalCamo"` `0x180004288`）。

**ini 路径与加载**：`GetModuleFileNameA(hInstance, …)` →
`strrchr('\\')` / `strrchr('/')` 取靠后者 → 截成目录 → `sprintf("%sOpticalCamo.ini")`
（`0x180004258`）→ `GetPrivateProfileStringA("OpticalCamo", "Visibility", "0.50", …)`
→ `strtof` → **取最近档位**（`|v-0|`、`|v-0.25|`、`|v-0.5|`、`|v-0.75|` 里最小者；
NaN / ±∞ / 解析失败回落 `0.50`）→ 写回索引并立刻 `SaveSetting()` 规范化。

**本地化**：随包 ini 自带 `[zh_cn]`/`[zh_cn.Optical Camo]`（"光学迷彩加强"、
"敌人视觉感知"、"状态不可用"）。本框架的菜单文本按**所属插件自己的 ini** 翻译
（`ShLangForOwned` 的查找顺序），所以插件只要保持英文标题/标签不变即可。

---

## 六、"兼容总线" `W_VisibilityBus_v2`（是什么，为什么丢掉）

`0x180001A8F` 起：`mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
PAGE_READWRITE, 0, 44, L"…W_VisibilityBus_v2")`（UTF-16 名字在 `0x1800041F0`；
`GetLastError() == 0xB7`（`ERROR_ALREADY_EXISTS`）表示**别人先建的**）→
`view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 44)`。

44 字节结构（`+` 为字节偏移）：

| 偏移 | 含义 |
|---|---|
| `+0x00` | tag `"VIS2"`（ASCII `0x32534956`）：创建方写后**读回校验**；加入方最多等 `50 × Sleep(10ms)`（`0x180001D10`） |
| `+0x04` | 创建者进程 id（加入方比对，只认同进程） |
| `+0x08` | 加入方标志 |
| `+0x0C` | 上次发布 tick（`GetTickCount`） |
| `+0x10` | available |
| `+0x14` | active |
| `+0x18` | 发布间隔：档位映射 `table[idx] * 1000 + 0.5` → `0/250/500/750`（ms） |
| `+0x1C` | 消费者存在标志 |
| `+0x20` | 消费者最后心跳 tick |
| `+0x24` / `+0x28` | 建总线时置 `1000`，本构建未再使用 |

**判据**：`linked = ([+0x1C] != 0 && GetTickCount() - [+0x20] <= 1000)`
（`0x18000121F`–`0x180001235`）。

它是一套**同进程、多插件/工具的交接协议**：创建者发布"迷彩状态 + 本档间隔"，
消费者（作者的另一插件或伴生工具）按自己的节拍干活。在本框架里：

- 没有任何框架设施发布/消费这条总线，**没有它功能一样完整**（走 Direct 分支）；
- 协议里写死了进程 id、心跳窗口、`VIS2` 版本号，属于跨版本兼容层；
- 用户已明确不需要为旧行为保留兼容。

所以**重写版不实现总线**，永远按 Direct 语义工作（自己应用因子）。

---

## 七、Linked / Direct 双通道（状态行前缀的由来）

`Pump(available, active)`（`0x180001110`，两个参数在 `ecx`/`edx`）按
`linked` 分成两半：

**Linked（`0x1800012AE`–`0x180001303`）**：只读回当前系数
（`ShGetVisibility(&f)`，`0x1800012D1`），不写任何东西；状态行用
`"Linked | Active | %.3fx"` / `"Linked | Inactive | %.3fx"` /
`"Linked | State unavailable | %.3fx"`（`0x1800043B0`/`0x1800043E0`/`0x180004360`）。

**Direct（`0x18000136E`–`0x180001438`）**：选目标值
`xmm7 = (available && active) ? table[idx] : 1.0f`（`0x180001376`–`0x180001391`）→
`ShSetVisibility(xmm7)`（`0x1800013F1`，成功则 `[0x18000614C] = 1`、缓存该值）→
再 `ShGetVisibility` 读回 → 状态行用 `"Direct | …"`（`0x1800043C8`/`0x180004400`/`0x180004388`）。

两半都由"变了才重画"保护：`|新值 - [0x180006094]| < 0.0025f` 且状态没变时，
连 `ShMenuStatus` 都不调（`0x1800012E5`–`0x180001303`）；名字打印走
`0x1800020A0`（CRT 格式化助手），最终 `ShMenuStatus(menu, text)`。

**重写取舍**：状态行去掉 `Linked`/`Direct` 前缀（那是总线有无消费者的痕迹，
不是两种读法），保留三态 + 系数。

---

## 八、线程模型与生命周期

- `DllMain` `0x180001FD0`：`DLL_PROCESS_ATTACH` → 存 `hInstance`、`DisableThreadLibraryCalls()`、
  `CreateThread(…, 0x180001520, …)` 并 `CloseHandle`；`DLL_PROCESS_DETACH` → 置停止标志
  `[0x180006264] = 1`。
- 工作线程 `0x180001520` 的顺序：算 ini 路径 → 读档位 → **解析 API（缺则 `Sleep(250)` 重试）**
  → 建菜单 + 列表 → 状态行 `"Initializing"` → `ShGetVersion() == 1`？（否则
  `"Unsupported ScriptHook version"` 后停在 `Sleep(500)` 等待循环）
  → **游戏构建门** → 解析 6 个状态 API（缺则 `"Required ScriptHook API unavailable"`）
  → 建共享内存总线（失败则 `"Compatibility bus unavailable"` 后停在等待循环）
  → 每 50 ms：`ShIsInGame` → 取玩家 → 取部件 → 逐部件读 `+0x54` → `Pump(...)`。
- **退出收尾**（`0x180001E3C`–`0x180001F42`）：清总线字段（`+0x08/+0x0C/+0x10/+0x14/+0x18`）
  → `ShSetVisibility(1.0f)` 复位（`0x180001ED3`，`xmm0 = [0x180004500]`）
  → `UnmapViewOfFile` + `CloseHandle` → `ShMenuDestroy(menu)` + 菜单句柄清零。
  **注意顺序**：先复位可见度，再拆菜单——所以插件被禁用/卸载后不会留下"永远隐身"。

### 游戏构建门（`0x18000194B`–`0x18000198E`）

`hGame = GetModuleHandleW(NULL)` → `*(u16*)hGame == 'MZ'(0x5A4D)` →
`e_lfanew = *(i32*)(hGame+0x3C)` > 0 → `*(u32*)(hGame+e_lfanew) == 'PE\0\0'(0x4550)` →
`*(u32*)(hGame+e_lfanew+8) == 0x6A7C5143`（COFF TimeDateStamp）→
`*(u32*)(hGame+e_lfanew+0x50) == 0x18B09000`（SizeOfImage）；任一不符 →
状态行 `"Unsupported game build"`（`0x180004470`，`0x180001F8A`）。
这是"认构建"的指纹（对应它当年直接写内存的做法）；本框架的
`ShSetVisibility` **自己**会校验那条 `mulss xmm8, xmm9` 的原始字节，
所以重写版不需要、也不应该再抄一份。

---

## 九、未确证 / 需游戏内复核的项

1. **`node + 0x54` 那 2 字节 bit7 的语义**：从投票方向看，**全清零 = 迷彩生效**，
   所以 bit7 应读作"该部件**未**处于迷彩/隐藏状态"（或等价的反相标志）。
   本报告只确证"读哪里、怎么投"，**不确证**引擎为何维护这一位。
   复核方式：进游戏开/关光学迷彩，看新插件的状态行与
   `logs\OpticalCamo.log` 里 `flagged/clear/failed` 的计数。
2. **Linked 模式下消费者是谁**：代码只按心跳判断"有人在读"，不关心对方身份。
   可能是作者的另一插件或伴生工具；本框架内不存在，故重写版忽略。
3. `+0x24` / `+0x28` 两个字段：建总线时置 `1000`，本构建未再读写，用途未知。
4. `ShGetEntityNodes` 返回的"部件"在引擎里对应什么（骨架/材质组？）：
   框架侧的注释是"枚举部件，让调用者隐藏任意子集"，与本次结论一致，
   但部件数随手持物/载具变化这一点**未验证**（投票要求"全部一致"，
   若列表本身会抖动，可能出现短暂"状态不可用"）。

---

## 十、重写取舍（`OpticalCamo.c`）

保留（行为等价）：

- 判定链原样搬运：`ShIsInGame` → `ShGetPlayer` → `ShGetEntityNodes(entity, …, 64)`
  → 逐部件 `ShReadBytes(node+0x54, &u16, 2)` → bit7 投票 → 三态；
- 菜单页 `Optical Camo` / 项 `Camo Visibility`（档位表本身按用户要求改过，见下）；
- ini `plugins\OpticalCamo\OpticalCamo.ini` 的 `[OpticalCamo] Visibility=`（值语义不变，
  旧设置照读）；"取最近档位 + 立刻规范化回写"的加载行为；
- 三态状态行、50 ms 轮询、退出时 `ShSetVisibility(1.0f)` 复位；
- `[zh_cn]` 本地化（随包的 ini 一并更新为新状态行模板）。

不保留（本框架里无意义的兼容层）：

- `GetModuleHandleA`/`GetProcAddress` 晚绑定（改为静态链接 `libscripthook`）；
- `W_VisibilityBus_v2` / `"VIS2"` / 心跳 / Linked 分支；
- `ShGetVersion() == 1` 版本门与 PE 指纹构建门（框架自己校验注入点）。

有意的小改动：

- **菜单重做**：原版只有一项四档 `0.0/0.25/0.50/0.75`，而且**总是**写因子（没有关闭的办法）；
  重写版按用户要求改为两项、默认 `关 + 0.5x`：
  - 项一"`Optical Camo (crouch effect)`"：`关/开` 总开关（`ShMenuToggle`，框架渲染成
    `[关]/[开]`，中文由 `[zh_cn.Optical Camo]` 提供）；
  - 项二"`Camo Visibility`"：九档 `0.1x … 0.9x`（`ShMenuList`；没有 `0.0x`，
    那是训练器的隐身开关而不是迷彩强度）；
  - **关的时候**：不下发因子、**不读游戏**（不取玩家、不遍历部件）、线程阻塞在事件上
    （开关/档位/模式变化立刻唤醒，1 秒兜底），真正做到零数值零性能影响；
  - **开的时候**：20 Hz 轮询，投票第一步是 `ShIsInGame()` —— **只在游玩状态监测**，
    主菜单 / 加载 / 地图下不碰玩家与部件；从开到关只写一次 `1.0f` 把效果收回，然后再次 park；
  - **状态行只在"本页正显示"时推送**（`ShMenuIsShowing(menu)`，本轮为此新增的公共 API）：
    该页没显示时一个字节都不写进菜单模型，期间发生的状态变化只记一个"待推送"标志，
    页面一出现立刻显示当前值。这个查询是在重写过程中发现"框架缺一条：插件的状态行
    不知道自己的页有没有在屏幕上"之后补上的 —— 语义是**精确的那一页**："菜单开着
    **且** 当前页就是本菜单"。之所以不是"在本页路径上"：`ShMenuCaptureView` 只取
    **当前页**的状态行（`scripthook_menu.c:941`），进了子菜单之后父页的状态行并不画，
    所以那时问父页返回 0、问子页返回 1；状态行放在子菜单里的插件要传子菜单的 id，
    想同时关心自己好几页的插件就逐页问。菜单关着时一律为 0；
- 状态行去掉 `Linked |` / `Direct |` 前缀，直接三态 + 系数；档位为 1.0x 时显示
  `原版 | %.3fx`（此时状态无关紧要，因为什么都没施加）；
- 状态文字用 `ShMenuStatusF` 模板（框架会按 `[zh_cn.Optical Camo]` 翻译模板）；
- 读到的部件数、三种计数进 `logs\OpticalCamo.log`，便于复核第九节第 1 条。

**不在本轮**：通用的"当前装备 / 背包 / 武器"查询 API。本次逆向已证明
这份插件既没有、也不需要装备信息；要做通用装备查询得另立一轮去逆
inventory / gadget 对象图（起点：`scripthook_ammo.c` 的 `FindInventory`、
`gadget_probe` 的 `gadget_*.log`、以及 `@defgroup entities` 的组件哈希）。

---

## 附：地址速查

| 地址 | 是什么 |
|---|---|
| `0x180001010` | 路径拼接助手（`sprintf` 包装） |
| `0x180001080` | `SaveSetting(idx)`：钳位 → `%.2f` → 写 ini |
| `0x180001110` | `Pump(available, active)`：发布总线 / Linked 或 Direct / 画状态行 |
| `0x1800014F0` | `ShMenuList` 回调：存档位 → `SaveSetting` → 置重画标志 |
| `0x180001520` | 工作线程主体（ini → 绑定 → 菜单 → 构建门 → 总线 → 轮询） |
| `0x180001770` | 第一批 API 解析块（`GetModuleHandleA("dinput8.dll")` + 5 个导出） |
| `0x18000198E` | 第二批 API 解析块（6 个状态导出） |
| `0x180001A8F` | 共享内存总线建立与 `"VIS2"` 握手 |
| `0x180001B69` | 写 tag `"VIS2"`（`mov ecx,32534956h` / `xchg [bus]`） |
| `0x180001C30` | 轮询一轮的开始（`ShIsInGame`） |
| `0x180001CD0` | 逐部件读 `+0x54` 的循环 |
| `0x180001DE4` | 三态判决 |
| `0x180001FD0` | `DllMain` |
| `0x1800020A0` | CRT 格式化助手（`sprintf`） |
| `0x180002040` | CRT `sprintf` 入口（`%.2f` 用） |
| `0x180002F29` | `memset` 包装 |
