# NPCSpawner.asi 逆向报告（原生 NPC 生成器）

日期：2026-09-12
对象：`<gamedir>\plugins\NPCSpawner\NPCSpawner.asi`（第三方插件，36 KB，PE32+ x64 DLL）
结论：**它是运行在本项目 ScriptHook 之上的第三方 MSVC C++ 插件**，行为已完整还原，
并按本框架 API 重写为 `NPCSpawner.c`（见 `plugins\NPCSpawner\NPCSpawner.asi`）。

---

## 一、结论摘要（先读这一段）

1. 该插件**不导入 dinput8 的静态符号**，而是 `GetModuleHandleA("dinput8.dll")` +
   `GetProcAddress` 逐个绑定 17 个导出——与仓库内 `spawner.c` 完全同构的**晚绑定**写法。
   因此它只依赖本项目框架的公开 ABI，可以被本项目加载。
2. 插件**自带 4 张硬编码 archetype id 表 + 1 个单例 id**，配合框架
   `ShNpcAt()` 返回的 `kind` 做兜底，实现了 5 个阵营分组
   （`Santa Blanca / Unidad / Rebels / Civilians / Special`）。
   这是本次逆向最有价值的产物：框架的 `ShNpcArchetype` 只有 `{id, kind}`、**没有名字**，
   分组信息不可能来自 API，只可能来自插件内的数据表。
3. 「生成」是**服务端式**的：菜单回调只做校验并开一个工作线程，真正的
   `ShSpawnNpc` 在插件自有线程里串行执行；总存活上限 **50**。
4. 阵型/朝向是**先算出一组落点、再逐点生成**，最后按需 `ShQueueTransform` 转向玩家。

---

## 二、取证方法与工具

本机无 IDA / Ghidra，`objdump` / `gcc` / `dumpbin` 均不在 PATH。所用手段：

| 步骤 | 命令/手段 |
| --- | --- |
| 节表/导入 | `dumpbin /headers`、`/imports`（经 vcvars64 注入环境，或用其绝对路径） |
| 全量反汇编 | `dumpbin /disasm` 输出到临时文件，再按地址定向检索 |
| 字符串 | PowerShell 正则提 ASCII / UTF-16 串 |
| 数据表 | 直接按 VA 读原始字节（`BitConverter`），VA↔文件偏移见下 |

VA 换算（本次 `.asi` 的节布局）：

```
节        RVA     大小     文件偏移
.text   0x1000   0x3673   0x0400
.rdata  0x5000   0x4526   0x3C00     →  file = VA - 0x180001400
.data   0xA000   0x03B0   0x8200     →  file = VA - 0x180008200 + 0xA000  = VA - 0x180001E00
.pdata  0xB000   ...      0x8600
镜像基址 0x180000000，LTCG（/GL）编译
```

> vcvars64: `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat`
> dumpbin: `...\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\dumpbin.exe`

### 二进制指纹

- PDB（未剥离）：`...\Documents\Visual Studio\source\Repo\NPCSpawner\x64\Release\NPCSpawner.pdb`，
  时间戳 `6AA44027`（2026-09-12 01:53:43）。
- 运行时依赖：`MSVCP140.dll` / `VCRUNTIME140.dll` / `VCRUNTIME140_1.dll` / `api-ms-win-crt-*`。
- 用到了 `std::mutex`（编译为 `Acquire/ReleaseSRWLock{Shared,Exclusive}`）、
  `malloc/free`、`std::vector`（字符串 `vector too long`）、`sprintf`（`__stdio_common_vsprintf_s`）。
- 数学：`sinf` / `cosf` / `atan2f` —— 三个都出现，与阵型/朝向的还原结果吻合。

---

## 三、API 绑定与全局变量表

### 3.1 绑定顺序（初始化函数 `0x1800029B0`）

`GetModuleHandleA("dinput8.dll")` 存 `[0xA350]`，然后依次 `GetProcAddress`
（字符串地址 → 全局槽）：

| 导出名 | 槽 | 导出名 | 槽 |
| --- | --- | --- | --- |
| `ShGetVersion` | 0xA358 | `ShNpcCount` | 0xA2F0 |
| `ShGetGameState` | 0xA318 | `ShNpcAt` | 0xA2D8 |
| `ShGetPlayer` | 0xA348 | `ShSpawnNpc` | 0xA2E8 |
| `ShGetPlayerPosition` | 0xA2F8 | `ShDespawn` | 0xA390 |
| `ShGetEntityTransform` | 0xA388 | `ShMenuCreate` | 0xA2C8（存菜单 id） |
| `ShGetEntityKind` | 0xA338 | `ShMenuAction` | 0xA2D0 |
| `ShQueueTransform` | 0xA368 | `ShMenuList` | 0xA310 |
| `ShGetHealthEntity` | 0xA328 | `ShMenuStatus` | 0xA2E0 |

未绑定：`ShMenuToggle` / `ShMenuNumber` / `ShMenuStatusF` / `ShMenuClear`…

> 注：字符串里有 `ShMenuClear`，但代码里对 `[0xA310]` 之外的菜单调用只有
> `ShMenuAction`(0xA2D0)、`ShMenuList`(0xA310)、`ShMenuStatus`(0xA2E0)。
> 「Menu rebuild failed」出现在 `0x180001E00`（NPC 阵营回调）里，是列表内容刷新失败的分支。

### 3.2 关键全局（`.data`）

| VA | 类型 | 含义 |
| --- | --- | --- |
| 0xA098 | `const char*[5]` | Formation 选项数组 |
| 0xA0C0 | int | **当前 Formation 索引**（初值 4） |
| 0xA0C4 | int | **当前 Spawn Distance 索引**（初值 2） |
| 0xA0C8 | `const char*[2]` | Facing 选项数组 |
| 0xA0D8 | int[5] | **每个阵营的「NPC 编号」选择值 `sel[5]`**（初值全 1） |
| 0xA0F0 | `const char*[3]` | 「NPC 编号」三行行的 3 个**可变标签缓冲**指针 |
| 0xA108 | `const char*[3]` | Spawn Count 选项数组 |
| 0xA120 | `const char*[5]` | NPC Group 选项数组 |
| 0xA148 | `const char*[6]` | Spawn Distance 选项数组 |
| 0xA2C8 | uint32 | 菜单 id |
| 0xA2CC | int | **当前阵营索引 `group`**（初值 0） |
| 0xA330 | int | 当前 Spawn Count 索引（初值 0） |
| 0xA334 | int | 当前 Facing 索引（初值 0） |
| 0xA320 | int | 菜单重建自旋锁 |
| 0xA324 | int | **忙碌标志**（生成/撤销期间为 1） |
| 0xA340 | SRWLOCK | 已生成实体列表锁 |
| 0xA360 | uint64 | **最近一次生成的实体**（撤销目标） |
| 0xA370/0xA378/0xA380 | ptr | 已生成实体 `std::vector<uint64_t>`（begin/end/cap） |
| 0xA298/0xA2A8/0xA2B8 | char[] | 「NPC 编号」三行标签的正文缓冲 |

---

## 四、菜单结构与选项表（逐字）

菜单标题 `Native NPC Spawner`（VA `0x180007BC8`）。行按**创建顺序**排列：

| 序 | 行标签 | 控件 | 选项 / 范围 | 初值 |
| --- | --- | --- | --- | --- |
| 1 | `NPC Number` | `ShMenuList` n=3 | **动态三行**：`sel-1` / `sel` / `sel+1` | 1（中行） |
| 2 | `Spawn Selected` | `ShMenuAction` | — | — |
| 3 | `Undo Last Spawn` | `ShMenuAction` | — | — |
| 4 | `Spawn Distance` | `ShMenuList` n=6 | `10 m`/`20 m`/`30 m`/`50 m`/`75 m`/`100 m` | 2（30 m） |
| 5 | `Spawn Count` | `ShMenuList` n=3 | `1` / `3` / `5` | 0（1） |
| 6 | `Formation` | `ShMenuList` n=5 | `Line`/`Spread`/`Semicircle`/`Circle`/`Random` | 4（Random） |
| 7 | `Facing` | `ShMenuList` n=2 | `Face Player` / `Face Forward` | 0（Face Player） |
| 8 | `NPC Group` | `ShMenuList` n=5 | `Santa Blanca`/`Unidad`/`Rebels`/`Civilians`/`Special` | 0（Santa Blanca） |

选项字符串与指针数组的 VA（`.rdata` 0x9698 起为 Formation 数组，0x9720 起为 Group）：

```
Formation  0x18000A098 → 7844 Line / 784C Spread / 7858 Semicircle / 7864 Circle / 786C Random
Facing     0x18000A0C8 → 7878 Face Player / 7888 Face Forward
NPC Group  0x18000A120 → 7898 Santa Blanca / 78A8 Unidad / 78B0 Rebels / 78B8 Civilians / 78C8 Special
Distance   0x18000A148 → 7808 10 m / 7810 20 m / 7818 30 m / 7820 50 m / 7828 75 m / 7830 100 m
Count      0x18000A108 → 7838 "1" / 783C "3" / 7840 "5"
```

数值映射表（非字符串）：

```
Spawn Distance 真值  @0x180007FD0  float[6] = {10, 20, 30, 50, 75, 100}   (m)
Spawn Count 真值     @0x180007BF0  int[3]   = {1, 3, 5}
```

### 「NPC Number」为什么是 3 项列表

框架的 `ShMenuList` **借用**选项字符串指针（`scripthook_menu.c`: `it->opts[i] = opts[i]`），
每次捕获时用 `it->opts[value]` 显示。插件因此把 3 个指针指向**自己的可变缓冲**
（`0xA298/0xA2A8/0xA2B8`），每次刷新 `sprintf(buf, "%d", v)`（格式串 `"%d"` 在 `0x18000790C`，
长度 2、被 ≥4 字符的字符串扫描漏掉）写入 `sel-1 / sel / sel+1`，
把「列表」当成一个**三档数字转盘**用：

- 用户把选择挪到第 0 项 → 回调收到 `value == 0` → `sel--`；
- 挪到第 2 项 → `value == 2` → `sel++`；
- 选到第 1 项（中行）→ 不动。
- `sel` 在 `[1, 该阵营 archetype 数]` 之间回绕。

### 随包 ini 的翻译表错误（第三方遗留，已修正）

`NPCSpawner.ini` 随第三方包发布时，**漏写了 `"撤销上次生成"` 一行**，导致其后的中文值
整体上移一位，于是：

| 行（真实身份） | 第三方 ini 给出的显示 | 应为 |
| --- | --- | --- |
| `Undo Last Spawn` | `生成距离` | `撤销上次生成` |
| `Spawn Distance` | `生成数量` | `生成距离` |
| `Spawn Count` | **无此键** → 直接显示英文 `Spawn Count` | `生成数量` |

这不是插件的行序问题：上面第 4 节的行序来自原件 `0x1800011BB9`→`0x180001D92` 的**创建顺序**，
与 ini 的键顺序完全一致；原件配这份 ini 会显示成一样。判定依据还有一条：
`Spawn Distance` 行的值显示为 `< 30 m >`，正是距离的默认索引 2（30 m），
而 `Spawn Count` 行显示的是数量值——两行的**值**都落在正确的行上。

`plugins\NPCSpawner\NPCSpawner.ini` 已修正；文案另存
`plugins\NPCSpawner\lang.ini` 的 `[zh-CN]` 段（行标签与状态行都在这里，
键仍是英文原文的**字面量** —— 本插件尚未改用稳定 ID）。

> 译文已与插件自身的 `NPCSpawner.ini` 分离：那份只有插件配置，文案在 `lang.ini`
> （框架**只读、永不回写**）。加载是"每个 owner 每份文件一次"，但**切换语言会
> 清表重扫**，所以改完 `lang.ini` 切一次语言即可看到改动，不必重启。

---

## 五、阵营分组：分类函数 `0x1800011C0`（核心）

> **实现位置已变更（2026-09-12）**：这 4 张 id 表与分类器已从插件搬进**框架**
> （`scripthook_npc.c`），对外暴露为 `ShNpcGroupOfArchetype` /
> `ShNpcGroupOf` / `ShNpcGroupSize` / `ShNpcAtInGroup` / `ShNpcGroupName`；
> `NPCSpawner.c` 改为调用它们，仓库里只保留一份数据。阵型几何与朝向同样移入框架
> （`ShNpcPlanFormation` / `ShNpcSpawnFormation`）。下文的规则即框架现在的实现，
> 换个插件也能直接用。

签名（推断）：`bool NpcInGroup(int group /*ecx*/, const ShNpcArchetype *a /*rdx*/)`

```
if (a == NULL) return false;
id   = a->id;              // [rdx]
kind = a->kind;            // [rdx+8]

if (id ∈ 黑名单表[44] @0x180007C00)        return false;   // 永不入任何组
if (id ∈ {0xF645ED5E6D,0x154BBBC8ADA})     return group == 0;  // @0x180007FC0 (2)
if (id == 0x1A987A7937C)                   return group == 1;  // Unidad 单例
if (id ∈ 表[3] @0x180007FE8)               return group == 3;  // Civilians
if (id ∈ 表[76] @0x180007D60)              return group == 4;  // Special

switch (group) {                       // 兜底：只看引擎 kind
  case 0: return kind == 3;            // Santa Blanca
  case 1: return kind == 5;            // Unidad
  case 2: return kind == 6 || 7;       // Rebels
  case 3: return kind <= 1;            // Civilians
  case 4: return kind == 4;            // Special
  default: return false;
}
```

### 5.1 黑名单（44 项，永不生成）@0x180007C00

```
5325BD7A52 78FADA79FF 5325BD7A4D 78FADA4D05 5325BD7A48 31B65512F7
10F2A192C3D 10F2A192C35 3D66E0ABDF 2309B3C694 5325BD7627 2FD455483E
237A1CBFBA 237A1CBFB9 10F2A192C39 4738A95B71 51021FED77 4FEB645A8D
4FEB645A8E 4AC59FEDD9 4FEB647516 4FEB6459B2 4AC59FEDD7 4FEB6459EB
4D8AB38F5A 4FEB6459EC 4AC59FEDD6 4FEB647517 4AC59FEDD8 4D8AB38F5B
4AC59FEDDA 4FEB6459B1 7C33CC49CA 3456303A13 5B708516B0 3F73BD8D99
3F4892BDFD 8AFE25F47F 6E164F05B3 3F4892BCF7 3456303D78 2EC3CD6993
2DF374C80C BBE631D833
```

### 5.2 Special 显式表（76 项）@0x180007D60

```
1A987A8752D 1AFB8794FE6 1B155F83D72 1AFB8751E8E 1A987A5256F 1B155F6628C
1AFB8794FEA 1A987A5FFD7 183A1B02363 185B81997B0 18173A2EB26 18173A5399A
187334C4DB7 185B81B4750 187E427E5C5 18173A30AC6 18173A2FCA4 18173A3102F
197A932C062 18F3D2B2FA2 1A16C520991 18D7BD5327E 1994707A751 8A9482DAC2
7C0B092643 8A9482DACC 14397E627AE 154BBBC37D0 154BBB495E1 537991063F
4AC59FCA66 5379910630 537991063E 4AC59FCA65 5379910631 4AC59FCA64
68EB25F118 433A9B6E26 68EB25EB16 7D662A1D27 78C9B348CF 45F1E58279
792C60E200 18B72EE403D 198B997684C 71CBB77732 71CBB77725 71CBB77721
71CBB77722 71CBB7772D 71CBB77720 71CBB77739 7929219EF8 45F1E58223
71CBB77729 71CBB77724 71CBB77727 71CBB77736 147CD1A13C3 82AF1233BC
71CBB77735 71CBB77731 71CBB7772C 71CBB77728 71CBB7772B 71CBB77726
71CBB77734 71CBB7772A 71CBB77730 71CBB7773A 71CBB77738 7926908976
71CBB77737 CA9DCD4408 71CBB77723 7C33CCA452
```

### 5.3 Santa Blanca 显式表（2）@0x180007FC0 / Civilians（3）@0x180007FE8

```
Santa Blanca : F645ED5E6D  154BBBC8ADA
Civilians    : 1AFB8765956 7DECAB61CD 7DECAB1E1D
Unidad       : 1A987A7937C     （单例）
```

> 观察：`71CBB777xx` 一族密集出现在 Special 表里，明显是同一次批量导出的结果；
> `x88` 与 `Unidad` 单例 `1A987A7937C` 仅低位差 1，说明这些 id 有内部结构。
> 这些表都是**静态数据**，可用「游戏内逐项召唤 + 观察外观/阵营」复核。

---

## 六、生成流程

### 6.1 `Spawn Selected` 回调 `0x180002630`

```
if (原子锁 0xA324 已被占)     → 状态行 "Busy" (0x7A3C); 返回
reqCount   = countTable[[0xA330]]                 // 1/3/5
aliveCount = vector(0xA370).size()
if (aliveCount + reqCount > 50) → 状态行 "Limit | Total %d/%d" (0x7A48, 参数 alive,50); 释放; 返回
// 在「当前阵营」里数到第 sel[group] 个 archetype
i = 0; hit = 0
for (k = 0; k < ShNpcCount(); k++)
    if (NpcInGroup(group, ShNpcAt(k))) { i++; if (i == sel[group]) { arch = ShNpcAt(k); idx = k; break; } }
if (!arch)                     → 状态行 "Selection unavailable" (0x78E0); 释放; 返回
req = new(0x18)
req[0]  = arch->id                 // (u64)
req[8]  = distanceTable[[0xA0C4]]  // (float, m)
req[12] = reqCount                 // (int)
req[16] = [0xA0C0]                 // formation
req[20] = [0xA334]                 // facing
h = CreateThread(NULL,0, 0x180002070 /* SpawnWorker */, req, 0, NULL)
if (!h) { free(req); 状态行 "Could not start worker" (0x7A78) }
// 锁 0xA324 由 worker 释放（菜单不阻塞）
```

### 6.2 生成工作线程 `0x180002070`

```
读 req 并立刻 free(req)
player     = ShGetPlayer()            // 失败 → "Player unavailable" (0x79A0)
playerPos  = ShGetPlayerPosition()    // 失败 → 同上
yaw        = ShGetEntityTransform(player.entity, &tmp, &yaw,&pitch,&roll)   // 只用 yaw
PlanFormation(formation, dist, count, playerPos, yaw) → positions[count]

spawned = 0; failed = 0; last = 0
for (i = 0; i < count; i++) {
    if (ShGetGameState() == 2 /*LOADING*/ || == 5 /*RELOADING*/) break;
    e = ShSpawnNpc(req.id, &positions[i]);
    if (!e) { failed++; continue; }        // 实际是 continue 到循环尾
    if (ShGetGameState() 是 2 或 5) break;
    track.push_back(e);                    // 加锁 0xA340
    last = e;                              // → [0xA360]
    spawned++;
    if (facing == 0 /* Face Player */) {
        dx = playerPos.x - e.pos.x;
        dy = playerPos.y - e.pos.y;
        ShQueueTransform(e, &positions[i], atan2f(dy, dx), 0.0f, 0.0f);
    }
}
释放锁 0xA324
if (ShGetGameState() 是 2 或 5) 返回
if (spawned == 0)                状态行 "Spawn failed" (0x79B8)
else if (spawned == count && failed == 0)  刷新状态行(0x1340)
else 状态行 "Spawn %d/%d | F %d | Total %d/%d" (0x79C8, spawned,count,failed,alive,50)
```

要点：

- **落点 z 直接用玩家 z**，不做地面吸附（与本仓库 `reinf_boost.c` 的 `ShGroundHeight` 不同）。
- `Face Forward`（facing==1）**不做任何处理**：因为这些 NPC 由 `ShSpawnNpc` 内部
  `ShSpawnBuildMatrix()` 用玩家根矩阵生成，已经继承玩家朝向。
- `Face Player`（facing==0）用 `atan2f(playerY-posY, playerX-posX)` 朝向玩家（弧度）。

### 6.3 阵型几何

`PlanFormation` = 规划函数 `0x180001770` + 每点生成器 `0x180001530`。

规划函数（`0x1770`）：

```
cx = playerPos.x + dist*cos(yaw)                     // yaw = 玩家 yaw，来自 ShGetEntityTransform
cy = playerPos.y + dist*sin(yaw)
seed = GetTickCount() ^ (uint32)(vector 指针)         // 每次生成不同
for i in 0..count-1:
    (ox, oy) = F(formation, i, count, &seed)
    pos.x = cx + ox*sin(yaw) + oy*cos(yaw)
    pos.y = cy - ox*cos(yaw) + oy*sin(yaw)
    pos.z = playerPos.z
```

> 即：整个阵型先沿玩家朝向前进 `dist` 米，再按 `yaw` 旋转。「Spawn Distance」
> 因此是「阵型中心在玩家前方多远」，不是「每个 NPC 离玩家多远」。
>
> `count <= 1` 时 `(ox,oy) = (0,0)`，即单点生成正好在「前 `dist` 米」。

每点生成器 `F`（`0x1530`，按 formation 分派；常量见下）：

| formation | 公式 |
| --- | --- |
| 0 `Line` | `ox = (i - (count-1)*0.5) * 3.0`，`oy = 0` |
| 1 `Spread` | `q = i/3`，`r = i%3`；`ox = (r - (count-3q-1)*0.5) * 3.5`，`oy = (q - (((count+2)/3)-1)*0.5) * 3.5`（3 列网格，行内居中） |
| 2 `Semicircle` | `t = i/(count-1)`，`a = π*t - π/2`；`ox = 5*cos(a)`，`oy = 5*sin(a)`（半径 5 m 的半圆） |
| 3 `Circle` | `a = 2π*i/count`；`ox = 4*cos(a)`，`oy = 4*sin(a)`（半径 4 m 的整圆） |
| 4 `Random` | 见下 |

`Random` 用一个 24 位哈希 PRNG（同一 seed 派生两个值，seed 随之更新）：

```
h1   = (seed*0x19660D    + 0x3C6EF35F) & 0xFFFFFF
seed = (seed*0x17385CA9  + 0x47502932)            // 未掩码，写回
h2   = seed & 0xFFFFFF
r1   = h1 * 2^-24          // ∈ [0,1)
r2   = h2 * 2^-24
a    = (2π/count) * (i + r1)
rad  = 2.5 + 4.5*r2        // ∈ [2.5, 7.0)
ox   = rad*cos(a); oy = rad*sin(a)
```

浮点常量表（`.rdata`）：

```
0x8008 = 2^-24 (5.9604645e-8)   0x800C = 0.5      0x8010 = π/2    0x8014 = 2.5
0x8018 = 3.0                    0x801C = π        0x8020 = 3.5    0x8024 = 4.0
0x8028 = 4.5                    0x802C = 5.0      0x8030 = 2π     0x8040 = -0.0f(符号位)
```

### 6.4 `Undo Last Spawn` 动作 `0x180002910` + 工作线程 `0x180002490`

```
if (锁 0xA324 被占) → "Busy"；返回
CreateThread(NULL,0, 0x180002490 /* UndoWorker */, NULL, 0, NULL)   // 失败 → "Could not start worker"
```

工作线程：

```
e = [0xA360]; [0xA360] = 0
if (e == 0)                    → "Nothing to undo" (0x79F0)
else if (游戏状态 2/5)          → 静默返回
else if (ShGetEntityKind(e)==0)→ 从列表移除 e; "Undo target unavailable" (0x7A00)
else if (!ShGetHealthEntity(e,&cur,&max)) → 从列表移除 e; "Undo target unavailable"
else if (cur == 0)             → "Last spawn is dead" (0x7A18)
else {
    ok = ShDespawn(e);
    if (ok) 从列表移除 e;
    释放锁；
    if (ok) 刷新状态行(0x1340); else "Undo failed" (0x7A30);
}
```

### 6.5 状态行刷新 `0x180001340`

```
group = [0xA2CC]; n = ShNpcCount()
inGroup = count{ k : NpcInGroup(group, ShNpcAt(k)) }
if (inGroup <= 0) → "No entries" (0x78D0); 返回
clamp sel[group] 到 [1, inGroup]
找到该组第 sel[group] 个 archetype（记整体下标 idx）
if (没找到) → "Selection unavailable" (0x78E0)
else        → "%d/%d | Total %d/%d" (0x78F8, sel, inGroup, idx, 50)
```

### 6.6 全部状态/提示字符串（逐字，VA）

| 字符串 | VA |
| --- | --- |
| `Selection unavailable` | 78E0 |
| `%d/%d \| Total %d/%d` | 78F8 |
| `Menu rebuild failed` | 7988 |
| `Player unavailable` | 79A0 |
| `Spawn failed` | 79B8 |
| `Spawn %d/%d \| F %d \| Total %d/%d` | 79C8 |
| `Nothing to undo` | 79F0 |
| `Undo target unavailable` | 7A00 |
| `Last spawn is dead` | 7A18 |
| `Undo failed` | 7A30 |
| `Busy` | 7A3C |
| `Limit \| Total %d/%d` | 7A48 |
| `Spawn request failed` | 7A60 |
| `Could not start worker` | 7A78 |
| `No entries` | 78D0 |

> `Spawn request failed`(0x7A60) 在 `0x180002630` 的「分配请求结构失败」分支上使用。

---

## 七、线程模型与并发

- **菜单回调线程**（框架提供）：只做校验与 `CreateThread`，不阻塞。持锁 `0xA324`。
- **插件自建工作线程**：每个「生成」/「撤销」各开一个线程，做完释放 `0xA324`。
  这解释了为什么插件里没有常驻 tick 线程，也解释了 `Busy` 这个状态串。
- 已生成实体列表用 `SRWLOCK(0xA340)` 保护，与 `vector too long`（std::vector）配套。
- 菜单重建用 `0xA320` 作自旋锁（`lock cmpxchg` / `xchg`）。
- 所有 `[0xA324]` 的获取/释放都是 `lock cmpxchg` + `xchg` 的**原子标志**语义。

---

## 八、与原插件的行为差异（重写时的有意取舍）

重写件 `NPCSpawner.c` 保持上述机制与文案，仅有以下**有意**差异，逐条列出：

1. **撤销列表会剪枝**。原插件只在「撤销」时从列表移除一个实体，死亡的 NPC 会永久占用
   「50 总量」额度，玩久了就再也生成不了。重写件在生成前用 `ShGetHealthEntity` 剔除
   已失效句柄（原插件的 `PruneTrack` 缺失属于缺陷，非设计意图）。
2. **「NPC 编号」用 `ShMenuSetValue` 回归中行**，不再清空重建整个菜单。
   框架的 `ShMenuList` 借用字符串指针，改写 3 个标签缓冲后界面即刷新；
   把选择位同步回中行用 `ShMenuSetValue(menu, "NPC Number", 1)`（不发回调）。
   效果与原插件一致（转盘），且省掉 `ShMenuClear` + 重加全部行。
3. **状态行改用 `ShMenuStatusF`** 直接 printf 模板（框架会先按菜单 scope 翻译再格式化），
   原插件是自建 `sprintf` 缓冲 + `ShMenuStatus`。显示结果相同，英文模板未在
   `NPCSpawner.ini` 中给出翻译，因此回退为原文。
4. 重写件为 **C17**（本项目插件层规范），不引入 MSVC C++/STL 依赖；
   原件的 `std::vector` / `std::mutex` 换成固定数组 + `CRITICAL_SECTION`。
5. **状态行 `F` 字段改为「本批未出现的人数」**。原件这里统计的是
   `ShQueueTransform` 的失败次数（几乎恒为 0）；召唤改由框架
   `ShNpcSpawnFormation` 完成后插件拿不到那个计数，于是改为 `count - 生成数`。
   信息量更大，且格式串 `Spawn %d/%d | F %d | Total %d/%d` 不变，
   只有 `NPCSpawner.ini` 里的中文措辞跟着调整。
6. **分组表、阵型几何与生成循环已移入框架**（见第五节开头的说明），
   插件只保留菜单、转盘、撤销列表与上限——因此本插件现在**依赖**
   `ShNpcGroupSize` / `ShNpcAtInGroup` / `ShNpcSpawnFormation`：
   绑不到就记日志并放弃注册菜单，而不是给出一个点不动的菜单。

---

## 九、未确证 / 需游戏内复核的项

| 项 | 状态 | 复核方法 |
| --- | --- | --- |
| `NPC Number` 列表的 `initial` 取值 | 从 `r15d` 传入，反汇编链路较长未完全定死；重写件取 1（中行） | 进游戏看该行默认显示的是 `sel-1/sel/sel+1` 哪一项 |
| 4 张 id 表的**语义**（哪些是 Santa Blanca 等） | 表已精确导出，但「为何这些 id 属于该组」只有作者知道；本报告按表内容与兜底 kind 规则记录 | 逐项召唤，观察外观/阵营/是否敌对 |
| `Spread` 的居中细节 | 公式已按指令逐条还原，但 `count-3q-1` 这一项在不足 3 列的行上含义略显可疑 | 生成 Count=5 的 Spread，观察 3+2 的排布间距 |
| 黑名单的取舍理由 | 表已导出（44 项永不生成） | 若某项确实需要，把它从表中删掉再试 |
| `Spawn Distance` 的语义 | 已确证是「阵型中心沿玩家朝向前进的距离」（yaw 来自 `ShGetEntityTransform`） | 原地转 180° 再生成，落点应随朝向翻转 |

> **2026-09-16 复核进度**：分组过滤（`ShNpcGroupSize` / `ShNpcAtInGroup`）已随公测集实测 ——
> Santa Blanca 组计数 **83**；单个 / 3 个 / 5 个生成与反召唤全部成功（见第十一节）。
> 上表其余各项仍未逐项复核。

---

## 十、对本项目「敌人增强」的意义

- 现在可以直接**按阵营 + 编号指定要生成的敌人类型**，不必像
  `reinf_boost.c` 那样「先打中一个敌人、再探测它的 archetype id」来猜类型
  （见 `F:\UbisoftGames\GameXueRen\reinf_boost\reinf_boost.c` 的 `LockOnto` / `ProbeTick`）。
- 分组表本身就是一份**精选的敌人 archetype 清单**（尤其是 `Unidad` 单例与
  `Special` 的 76 项），可直接复用于「按阵营刷兵」。
- 多选生成 + 阵型 + 朝向的整套代码在 `NPCSpawner.c` 里，可作为波次刷兵的底座。

---

## 十一、框架侧的引导链：常数是怎么钉的（2026-09-16）

插件本身只绑定框架的 `ShNpcCount` / `ShNpcAt`（见第三节），**目录这条链只有框架实现过**，
落在 `scripthook_npc.c`。它的一组引导常数在 2026-09 更新里全部失效，下面是重新钉住它们
的方法，以及一条踩出来的教训。

### 11.1 三个必须一起对的量

```
REGISTRY 槽 ──┐
              ├─→ 采集器 COLLECT(reg, ARCH_DESC, &hdr) → hdr{ptr,cnt} → 档案块数组
ARCH_DESC 槽 ─┘                                                    └→ KIND(obj) → {id, kind}
```

- `REGISTRY`：一个**指针槽**（读出来才是目录对象的地址），不是结构体本身；
- `ARCH_DESC`：交给采集器的**类型描述符**，形态为 `{指针, 0, 0, 共有 id 0x1438BE156, 尾部 16 字节}`；
- `KIND`：一个函数，输入档案对象、返回种类号，分类器用它兜底（`kind == 3` 即 Santa Blanca 等）。

### 11.2 数据槽不能靠 dump / reloc 推

工具的 `dump` 在**数据节**里不可靠：同一地址在旧版标成 `.rsrc`、新版标成 `.data`，
读出来的字节和内存里不是一回事；`reloc` 对这批槽返回 `old refs: 0`，**并不是**「没人引用」，
而是引用它的指令本身不在扫得到的范围。仅凭 dump 字节下结论（「这些槽不是指针」）是错的 ——
本轮为此绕了一大圈。

### 11.3 唯一可靠的方法：解码引擎自己的调用点

采集器经**跳板**被调用，所以先找跳板、再找调用点，然后看调用前那几条指令：

```
jumps  -Rva C1F5BA0 -File new      # 采集器 → 跳板 F0DE60
calls  -Rva F0DE60  -File new      # → 107 个调用点
dump   -Rva <调用点> -Before 48    # 看 rcx / rdx 从哪儿来
```

调用点里 `mov rcx,[rip+disp32]` 的 disp32 是**可算的**：

```
目标 = (指令地址 + 7) + disp32
NEW 3FC4F0: 48 8B 0D 81 53 7C 04 → 3FC4F7 + 047C5381 = 4BC1878
NEW 235B541: 48 8B 0D 30 63 86 02 → 235B548 + 02866330 = 4BC1878   ← 两处独立调用点同值 ✓
旧版同一处解码 → 4BC17F8，正是仓库里原有的旧值 ✓ 互为印证
```

**判据**：新值至少要**两个独立调用点**一致；再用**旧版同一处**解码，看是否给出仓库里的旧值 ——
给得出，说明方法对、旧值本身就是槽地址，只差一个位移。

本轮钉出的两个槽（这一族数据位移为 `+0x80`）：

| 常数 | 旧（09-15 有效） | 新 | 依据 |
| --- | --- | --- | --- |
| `RVA_REGISTRY` | `4BC17F8` | **`4BC1878`** | 两个调用点解码一致 ✓ |
| `RVA_ARCH_DESC` | `42C2560` | **`42C2570`** | 共有 id 与尾部 16 字节逐字节相同 ✓ |

### 11.4 教训：没验证过的槽，绝不能交给引擎

本轮那次崩溃（`scripthook_crash.log` 里 `GRW.exe+0x8AE8EFB`，即注册函数内部）
就是**把陈旧槽读出的值直接当参数**传进引擎：那不是管理器，注册函数往里写就崩了。
现在三个写入点都有形态检查：

- `mgr`（`MGR_GETTER` 的返回值）必须可读；
- `spec`（`SPAWN` 的返回值）必须可读**且虚表等于 `NPC_SPEC_VTABLE`**，否则不写它的任何字段；
- 种群管理器改用**引擎自己三个调用点解码出的候选**（`4BACFA8` / `4B957A8` / `4B99BA8`），
  逐个做「可读 + 首字是镜像内虚表」检查，全不通过就**跳过并记账**，绝不再崩。

### 11.5 顺带去掉的两个依赖

生成路径原先把 `id` 反查回档案块（`POOL + 0x100` → 池查找 → 块），这两个槽同样无法离线验证。
但**目录扫描本来就拿着每个档案的块**，于是把它与目录同序存下来：

```
旧的生成路径：id → 池槽(不可验证) → 池查找(不可验证) → 块
新的生成路径：id → 目录块表（扫描时就有的）→ 块
```

`RVA_POOL` / `RVA_POOL_FIND` 不再在关键路径上，`RVA_POPMGR` 退役。

### 11.6 实测（2026-09-16，公测集 + NPCSpawner）

```
npc: catalogue 527 of 527 collected        目录 527 条 ✓
npcspawner: spawn … n=1 … got=1             单个 ✓
npcspawner: undo ent=… ok=1                 反召唤 ✓
npcspawner: spawn … n=3 … got=3             批次 3 ✓
npcspawner: spawn … n=5 … got=5             批次 5 ✓
```

---

## 附：函数地址速查

| 地址 | 作用 |
| --- | --- |
| `0x1800029B0` | 初始化：`GetModuleHandleA` + 17 个 `GetProcAddress` |
| `0x1800011C0` | **`NpcInGroup(group, archetype)` 分类器** |
| `0x1800012C0` | 从已生成列表移除一个实体 |
| `0x180001340` | **刷新状态行**（`%d/%d \| Total %d/%d`） |
| `0x180001530` | **每点阵型生成器** `F(formation,i,count,&seed,&ox,&oy)` |
| `0x180001770` | **阵型规划**：产出 `positions[]` |
| `0x180001A70` | 重建「NPC 编号」三行标签 |
| `0x180001E00` | NPC Group 列表回调 |
| `0x180001EF0` | NPC Number 列表回调 |
| `0x180001FF0 / 2010 / 2030 / 2050` | Distance / Count / Formation / Facing 列表回调 |
| `0x180002070` | **生成工作线程** |
| `0x180002490` | 撤销工作线程 |
| `0x180002630` | **Spawn Selected 回调** |
| `0x180002910` | Undo Last Spawn 回调 |
