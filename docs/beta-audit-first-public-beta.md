# 首个公测版发布前审计报告

日期：2026-09-16
审计对象：框架（仓库根 `*.c` / `*.h`，51 个文件，约 3.6 万行）+ 随包分发的 8 个插件
方法：检查表驱动的静态精读 + 二进制/历史核对；**本次未修改任何文件、未提交 git**
证据约定：每条尽量给出「静态可判定」证据（文件:行号、字节、调用点）。凡依赖运行期条件才能确认的，一律标注「需实机验证」，不做推测性断语。

---

## 一、结论摘要

| 档位 | 条数 |
| --- | --- |
| 阻断发布 | **0**（无「无条件必崩/必坏档」项；有 2 条条件性高危，见 2.1） |
| 发布前必修 | **11**（首轮审计） |
| 建议修 | **24**（首轮审计） |
| 已知限制 / 可接受 | **12** |
| **终审必修**（2026-09-17，见第十节） | **6**（建议发前修 `M1` / `M4` / `M5`；`M2` / `M3` 各需一次实机确认；`M6` 视发布时间） |
| **终审建议修** | **14**（不阻塞发布） |

> **终审判定（2026-09-17，第十节）**：**条件可发布**。无阻断项；三条建议发前修（`M1` 模式识别的过期参数回落、`M4` 翻译缓存返回共享指针、`M5` 打包脚本可能静默缺件），两条需实机确认（`M2` 模式对象持续重读、`M3` cnchat 捕获判定竞态），一条属体验面（`M6` 五个插件 `lang.ini` 键与代码不匹配）。前几轮 20 条自述经逐条回读确认为已修、3 条为部分修；两处实机日志清扫无错误行。

**是否可发布**：从代码层面看，**没有发现必须推倒重来的问题**，框架与 8 个插件在已实测的会话里行为正常（生成、反召唤、天气、FOV、隐身、弹药容量均已跑通）。但发布前有 **11 条必修**，其中 8 条是**几行内可改完**的确定性缺陷，3 条属「发布准备缺失」（打包清单、卸载说明、第三方声明）——**这三条是当前离「能发」最远的部分**，不是代码 bug，而是缺交付物。

发布准备三项结论（详见第七节）：
- **缺口 1**：没有任何打包清单/忽略规则 → 极易把 `docs/`、`tools/`、`test_plugin`、`plugins_off/` 一起打进压缩包。
- **缺口 2**：README 只有「安装」没有「卸载」；回滚依赖**默认关闭**的 `-BackupDir`。
- **缺口 3**：`third_party/` 的 MIT / BSD-2 许可文件在仓库里，但**不随包复制**，也没有 NOTICE 汇总。

---

## 二、阻断发布

### 2.1 无确认的阻断项，但有两条「条件成立即阻断」

这两条都属同一类：**在特定运行期条件下会变成 use-after-unmap / use-after-free**。它们不需要「更多分析」就能修，只需要先确认触发条件是否真实存在（各自附了验证步骤）。

---

## 三、发布前必修

### 3.1 `DllCanUnloadNow` 直接转发给真实 dinput8（条件性最危险的一条）

文件：`loader.c:327-330`

```c
SH_PROXY_EXPORT HRESULT WINAPI DllCanUnloadNow(void) {
    if (p_DllCanUnloadNow) return p_DllCanUnloadNow();
    return S_FALSE;
}
```

- 问题：若真实 dinput8 返回 `S_OK`，宿主即可 `FreeLibrary` 本模块。而此时仍然活着、且代码/数据都在本模块里的东西包括：corefix 的全部 MinHook detour（`scripthook_corefix.c:851-876` 装的那批）、dinput vtable 补丁（`scripthook_dinput.c:277-288`、`305-320`）、以及 `LoaderThread` / `StageThread` / `StateWatchThread` 三条**永不退出**的线程（`loader.c:294`、`scripthook_corefix.c:2095-2106`、`scripthook_state.c:363-369`）。卸载后下一次输入或下一次文件调用即是跳进已解除映射的内存。
- 影响面：整机级崩溃，且现场看起来与框架无关。
- 最小修复方向：**恒返回 `S_FALSE`**（本模块的生命周期本来就绑定在游戏进程上，没有任何理由允许被卸载）。
- 需实机验证：**是**。在真实 `dinput8.dll` 的 `DllCanUnloadNow` 上下断点，看返回值；无论结果如何，恒 `S_FALSE` 都是正确的选择。

### 3.2 关闭句柄时不清「待完成读」表 → 悬垂 `OVERLAPPED`

文件：`scripthook_forge_io.c:341-358`（登记）、`387-413`（扫描）、`634-646`（`CloseHandle` 分支）

```c
if (_stricmp(c->api, "CloseHandle") == 0) {
    unsigned slot = SlotOf(c->handle);
    if (g_cacheH[slot] == c->handle) { g_cacheH[slot] = NULL; g_cacheO[slot] = NULL; }
    if (g_pathH[slot] == c->handle) { g_pathH[slot] = NULL; g_pathP[slot][0] = 0; }
    return;                       /* ← 只清了两个缓存表，没动 g_pending[] */
}
```

```c
if (g_pending[i].ov == NULL ||
    g_pending[i].ov->Internal == IO_STATUS_PENDING) { ... continue; }   /* 解引用调用方的 OVERLAPPED */
```

- 问题：`CloseHandle` 分支只清理句柄缓存，**不清理 `g_pending[]` 里该句柄的待完成读**。若引擎关闭一个仍有 `ERROR_IO_PENDING` 读的句柄（取消 I/O）并释放/复用其 `OVERLAPPED`，`PendingSweep` 下一次就会解引用已释放的内存。
- 触发条件：需要「异步读挂起 → 关闭句柄/取消读」的序列；**只有装了 Forge mod（走 overlay 读路径）时该表才可能非空**，这也限制了公测默认安装下的暴露面。
- 影响面：文件层崩溃或静默打错补丁（后者更坏：数据被改写且无日志）。
- 最小修复方向：`CloseHandle` 分支同时遍历 `g_pending[]`，丢弃该句柄的条目（并递减 `g_pendingCount`）。
- 需实机验证：**是**。挂钩 `CancelIo/CancelIoEx/CloseHandle` 记录序列，确认是否存在「有 pending 条目时关闭句柄」。

### 3.3 `ShQueueTransform` 先发布后填充（注释与代码相反）

文件：`scripthook_api.c:826-839`

```c
if (InterlockedCompareExchange(&g_xq[i].ready, 1, 0))
    continue;
g_xq[i].ent = entity;
g_xq[i].pos = *pos;
...
/* Published last, so the pump never sees a half filled slot. */
InterlockedExchange(&g_xq[i].ready, 1);
```

- 问题：CAS **在填字段之前**就把 `ready` 置 1，末尾那次 `InterlockedExchange` 是冗余写。而 `ShTransformPump`（`scripthook_api.c:847-856`）判的正是 `ready` → 它可能读到半填槽：`ent` 为 0（`ShPlaceEntityRot` 直接失败，丢掉一次变换）或读到上一轮的实体却套用本轮朝向。
- 影响面：偶发瞬移/丢一次朝向设置；不崩。
- 最小修复方向：用一个独立的「认领位」做 CAS，`ready` 只在字段填完后才置 1（或改为写入后 `InterlockedExchange` 发布、消费端用 `InterlockedExchange(&ready,0)` 取回）。
- 需实机验证：否（代码可判定）；可见症状可选实测。

### 3.4 `LogFirst` 的 `once` 是「每编译单元一个」，同一文件里只有第一条能落盘

文件：`log.h:92-103`

```c
static void LogFirst(const char *logName, const char *fmt, ...) {
    static LONG once;                      /* ← 整个 TU 共用 */
    ...
    if (InterlockedExchange(&once, 1)) return;
```

- 问题：`once` 是**函数级静态**，一个 `.c` 里所有 `LogFirst` 调用点共用它 → **只有第一个调用点会真正写日志**，同一文件里后续的诊断永远静默。受影响的正是本框架的「健康日志」策略：任何在一个文件里打多条 `LogFirst` 的模块（例如 `scripthook_entity.c:941/948/955/962/972` 五处、`scripthook_npc.c` 的多处）只有第一条可见。
- 影响面：不是崩溃，而是**排障能力被悄悄削掉一半**；对一个要靠日志远程支持的公测版，代价很实在。
- 最小修复方向：按「调用点」而不是按「TU」记一次（例如宏里传入 `__LINE__` 作为键，查一张小表），或给每个诊断一个显式的一次性标志。
- 需实机验证：否。

### 3.5 `LogInit` 会截断并泄漏：重复调用即清空日志

文件：`log.h:42-48`

```c
static void LogInit(const char *name) {
    char path[MAX_PATH];
    if (!LogWanted(name)) return;
    if (LogPath(path, sizeof(path), name))
        g_logFile = fopen(path, "w");      /* ← 无「已打开则跳过」判断 */
}
```

- 问题：句柄是每 TU 一份的静态变量，`LogInit` 没有「已打开就返回」的判断。任何模块若先后两次 `LogInit` 同一文件，旧句柄泄漏、新文件被清空——**本会话此前写下的诊断全部消失**。仓库里已经出现依赖人工规避的写法（`scripthook_npc.c` 用 `if (!g_logFile) LogInit(...)` 兜着），属于「再加一个调用点就爆」。
- 影响面：日志丢失（排障灾难），不崩。
- 最小修复方向：`LogInit` 开头加 `if (g_logFile) return;`（若需支持换文件，先 `LogClose`）。
- 需实机验证：否。

### 3.6 `skipintro` 卸载路径可能操作未初始化的临界区，并在 loader lock 下调用框架 API

文件：`plugins/skipintro/skipintro.c:682-688`（卸载分支）、`:627`（临界区初始化在 InitThread 里）、`:378-383`（ReleaseAll 取锁）

```c
} else if (reason == DLL_PROCESS_DETACH) {
    if (!Released()) ReleaseAll("unloading");
}
```

- 问题：`ReleaseAll` 第一步就是 `SkipLock()` → `EnterCriticalSection(&g_lock)`，而 `g_lock` 是在 `InitThread`（**另一个线程**）里 `InitializeCriticalSection` 的。若进程在 `InitThread` 跑起来之前退出（快速加载后立即结束），对零初始化的 `CRITICAL_SECTION` 取锁属未定义行为。同时该分支在 loader lock 持有中调用 `ShFileRuleDel`（框架 API），与本仓库其它插件自述的契约相矛盾（对照 `plugins/fov_changer/fov_changer.c:202-204`、`plugins/firstperson/firstperson.c:290-292` 的注释）。
- 影响面：退出时卡死或崩溃；出现概率低但用户会当成「游戏崩了」上报。
- 最小修复方向：`g_lock` 改用静态初始化的 `SRWLOCK`；卸载路径只置标志并唤醒一个自有线程去释放，不在 DETACH 里调框架 API。
- 需实机验证：**是**。装入游戏后 120 秒内直接结束进程，观察是否卡死/崩溃、`logs\skipintro.log` 是否有 `released (unloading)`。

### 3.7 版本标识缺失且互相矛盾

文件：`loader.c:275-276`（`"GRW ScriptHook loader v0.1"`）、`loader.c:276`（`__DATE__` / `__TIME__`）、`README.md:1`（`Beta1.0`）

- 问题：构建产物里唯一可追溯的信息是编译日期时间，**没有版本号、没有提交标识**；而且 `loader.c` 自称 `v0.1`、README 自称 `Beta1.0`。公测版一旦铺开，第一条用户反馈就必须先问「你装的是哪一版」。
- 最小修复方向：在 `scripthook.h` 定义版本串（如 `SH_VERSION "0.9.0-beta1"`），`loader.c` 的启动行与崩溃报告头部都打它；README 引用同一串。
- 需实机验证：否。

### 3.8 ~ 3.10 构建开关与日志量的取舍（需你决定）

文件：`build_msvc.ps1:143`、`:219`（只有 `-Release` 才定义 `SH_RELEASE=1`）；`log.h:33-40`

- 事实：`-Beta` **不定义** `SH_RELEASE`。因此公测包（若用 `-Beta` 构建）会**照常生成全部模块日志**（`scripthook_menu.log`、`scripthook_camera.log`、`scripthook_physics.log`…约 30 个），并且 `log.h:70` 每写一行就 `fflush` 一次（同步落盘）。
- 两面性：这**正是**远程支持需要的现场；但玩家的 `logs\` 会有一二十个文件、且每个文件每次启动被截断（`fopen(...,"w")`），同时 `scripthook_crash.log` 是**追加、从不截断**（`scripthook_crash.c:23`、`:56-58`），跨会话无限增长，**全仓库无轮转机制**。
- 需你决定：公测包带 `SH_RELEASE`（只留 2 个日志）还是带全量诊断？若带全量，建议同时给 `scripthook_crash.log` 加启动截断或大小上限。
- 需实机验证：**是**。清空 `logs\` → 启动一次 → 数文件个数与总大小；连续启动多次 → 看崩溃日志是否持续变大。

### 3.11 开发件会被误打包（`plugins_off/` 尤其危险）

文件：`build_msvc.ps1:417-427`（把非公测插件 `Move-Item` 到 `<gamedir>\plugins_off\<yyMMdd_HHmmss>\`）

- 问题：`-Beta` 把集合外插件**移走而非删除**（设计正确，可恢复），但**移到了游戏目录里面**。任何「把游戏目录里的东西打包」的做法都会把 `plugins_off\<stamp>\` 一起带上——里面是 `test_plugin`（本地 TCP REPL，可任意读写进程内存）、`ModeCallProbe`（按硬编码 RVA 挂钩）、`chaos`（加载即改数值 + 全局键鼠钩子）等开发/探针插件。
- 另：仓库根还有 `tools/`（含硬编码本机绝对路径）、`docs/api/`（Doxygen 生成物）、`reflection_classes.tsv`(350 KB)、`plugins/test_plugin/test_plugin.c`(219 KB)，以及**没有任何打包清单/忽略规则**（全仓库搜不到 `Compress-Archive` / manifest）。
- 最小修复方向：新增一个打包脚本或清单，**显式白名单**：`dinput8.dll`、`scripthook.ini`、`plugins\<8 个>\`（+ 可选 `lang.example.ini`、`LICENSE`、第三方声明）；打包前确认 `plugins_off/`、`logs/`、`mods/` 不在其中。
- 需实机验证：否（但打包后建议人工列一次目录）。

---

## 四、建议修（24 条）

### 4.1 框架：配置与解析

| # | 位置 | 问题 | 最小修复方向 |
| --- | --- | --- | --- |
| 1 | `scripthook_config.c:505-524` + `529-559` | `IsLangCodeLike` 把**裸的 2–3 字母段名**当语言码，`[ui]`/`[mod]`/`[npc]` 这类段落的行会被整段丢弃（只写 `TextLog`，而 `-Release` 下连这个都不落盘 → 真正静默）。**自带 `scripthook.ini` 的段名都是长名（`[loader] [plugins] [Settings] [MenuOrder] [playmode] [forgemod]`），所以现状不受影响；风险在第三方插件** | 收窄判据：只有「出现在 lang.ini 里的段」或「带 `.` 的语言表段」才丢弃，其余一律当设置段 |
| 2 | `scripthook_config.c:222-234` | `NextLine` 对超长行在 `cap-1` 处停下但不推进指针，**同一行的剩余部分被当成新行解析**；`PeekLanguages` 用 `line[256]`、`ParseConfig` 用 `line[512]`，同一文件两处行宽不一致 | 截断时跳过该行剩余字节；统一行宽常量 |
| 3 | `scripthook_config.c:1808-1834` | 两次 `fopen` 都失败时 `g_configReady` 保持 0 → 此后每次查询都重新 `WriteDefaultConfig`（反复写盘） | 失败时也置 ready（用内存默认值）并只报一次 |
| 4 | `scripthook_config.c:1942-1964` | 条目表满（`ENTRIES_MAX=256`）时磁盘已写、内存未更新，函数仍返回 1 | 表满时先失败并报错 |
| 5 | `scripthook_config.c:1881-1884` | `strtol(v, &end, 0)`：`cpu_cores=08` 被当八进制解析成 0，静默取默认 | 用基数 10（或显式拒绝前导零） |
| 6 | `scripthook_config.c:46-60`、`1967-1971` | `snprintf` 截断不报错：安装目录很长时 `plugins\<name>\<name>.ini` 被截成不存在的路径，插件读不到配置且无提示 | 检查返回值 ≥ 缓冲区大小时报错 |

### 4.2 框架：日志与可观测性

| # | 位置 | 问题 | 最小修复方向 |
| --- | --- | --- | --- |
| 7 | `log.h:60-71` | 每行一次 `fflush`（同步落盘）。框架自身热路径已刻意不打日志，但落在 4 ms/40 ms/120 ms 轮询线程上的日志会被放大 | 行缓冲；或只在错误级别/退出时 flush |
| 8 | `scripthook_hud.c:172-175` | UI 起不来时 `SyncSlot` 会随 HUD 刷新（rev/gen 变化）**反复**写该行并 flush | 改一次性（`LogFirst` 语义）或节流 |
| 9 | `scripthook_crash.c:377-384` | `ShCrashStartup` 忽略 `LogPath` 返回值 → 失败时回退到相对名 `"scripthook_crash.log"`，随 CWD 落到不确定目录，**崩溃现场丢失** | 失败时显式报错并保留可用路径 |
| 10 | `scripthook_crash.c:51-66` | `WriteFile` 返回值未检查 → 磁盘满/写入失败时报告静默丢失 | 检查并（尽力）写到备用位置 |
| 11 | `scripthook_corefix.c:1576-1608` | 进程树日志会打印**完整 exe 路径**；若游戏装在 `C:\Users\<用户名>\…`，用户名进日志（崩溃报告本身已确认不含隐私，这一处是唯一残留） | 只打基名或做路径脱敏 |
| 12 | `scripthook_dinput.c:34-69` | `g_dikVkReady` 既非 `volatile` 也无发布屏障：另一线程可能读到「就绪但数组未填完」 | 用 `InterlockedExchange` 发布就绪标志 |

### 4.3 框架：性能（按「每帧执行 × 单次成本」排序）

| # | 位置 | 问题 | 最小修复方向 |
| --- | --- | --- | --- |
| 13 | `scripthook_menu.c:863-895` | 菜单打开时**每帧**做 O(行数) 的可见性判断与翻译（每帧约 29 次翻译 × 每次数百次字符串比较，`config.c:652/672` 线性扫两张表） | 翻译结果缓存进行结构，语言切换时失效 |
| 14 | `scripthook_hud.c:250-255` | 同一槽文本被 `SplitLines` **解析三遍**（`SlotHeight`/`SlotWidth`/`SyncSlot` 各一次） | `SyncAll` 解析一次并透传 |
| 15 | `scripthook_ui.c:1513-1521`、`816-820`、`825-834` | 子节点枚举/级联/销毁对每个节点都扫全部 256 槽（O(k×256)） | 维护父→子索引 |
| 16 | `scripthook_havok.c:207-275` | 体表重建两遍 + `MapAdd` 线性查重（O(体数²)）+ 未命中再重建一遍；映射表**全程无临界区** | 未命中不立即重建；映射表加锁或用哈希 |
| 17 | `scripthook_physics.c:601-604`、`693-696` | 忙等 `Sleep(1)` 最长 3 秒（`ShGroundHeight` 每个落点最多两次 = 6 秒），把调用线程冻住 | 改事件驱动/可取消等待，降低单点超时 |
| 18 | `scripthook_ui.c:1060-1082` | `RunJob` 同步排队 + `Sleep(1)` 等结果；HUD 重建逐控件排队（十余次） | 走已有 `ShUiBegin/Commit` 批处理通路 |
| 19 | `scripthook_entity.c:992-1008` | `radius <= 0` 时退化为 1e9（**无裁剪**），每个候选实体至少 2 次内核读 + 1 次 `VirtualQuery`；框架自身调用点带半径 120，风险在插件 API | 把「半径 0」定义为有限默认；缓存可读区间减少 `VirtualQuery` |
| 20 | `scripthook_uiinput.c:75-88` | 有场景取得焦点时，每 8 ms 对 1..255 全部 `GetAsyncKeyState`（约 3.2 万次/秒） | 只轮询已注册热键 |
| 21 | `scripthook_input.c:215` | `ShBlockKey` 每次 O(256) 重扫求 `g_anyKeyBlock`；菜单开/关时连续调用 7 次 | 维护置位计数 |
| 22 | `scripthook_tick.c:91` | `GetTickCount()`（48.9 天回绕）与其它处的 `GetTickCount64` 混用 | 统一 `GetTickCount64` |

### 4.4 插件（公测 8 个）

| # | 位置 | 问题 | 最小修复方向 |
| --- | --- | --- | --- |
| 23 | `plugins/spawner/spawner.c:53-54`（`AHEAD 6.0f` / `LIFT 1.0f` @ `:12-13`） | `pos.x += AHEAD` 把「前方 6 m」加到**东向**（`ShVec3` 为 x 东 / y 北 / z 上），与朝向无关；宏名与行为不符（也可能是刻意的侧向落点） | 按玩家朝向分解，或改名承认是侧向；**先实机确认设计意图** |
| 24 | `plugins/firstperson/firstperson.c:948-959` + `1365-1372` | 缺 `ShGameFocused` 时把 `g_diagOn` **永久置 1** → tick 线程之后每秒写一行 beat（含 flush），整局持续落盘，与「diag 默认关」的初衷相反 | 该提示只写一次，不打开持续 beat |
| 25 | `plugins/firstperson/firstperson.c:636`、`:658` → `SaveIni` `:1216-1256` | 每次滑块变化整体重写约 36 个键（每个 tick 一次整文件读写） | 内存更新 + 去抖落盘 |
| 26 | `plugins/firstperson/firstperson.c:366-407` | 头注释称「仅 tick 线程写」，实际 Tick/Hotkey/Bind **三线程并发**；首开 `g_diag` 无锁 → 可能双 fopen 泄漏句柄 | 用一次性标志或 `SRWLOCK` 保护首开 |
| 27 | `plugins/firstperson/firstperson.c:713-734` | 缺 `ShMenuStatusF` 时把**键名**（`"@fp.status.on"`）当 printf 模板用 → 状态行显示原始键名 | 回退路径先取文案再格式化 |
| 28 | `plugins/spawner/spawner.c:142`、`plugins/firstperson/firstperson.c:1374-1375`、`plugins/ammo_capacity/ammo_capacity.c:228`+`:196` | 菜单创建失败未拦截 → 后续对 `menu==0` 操作（`ammo_capacity` 还会对 0 写状态行） | `if (!g_menu) return;` |
| 29 | `plugins/spawner/spawner.c:133-136` | **整个插件没有任何日志**，任一导出缺失即静默不工作 | 至少记一行「缺哪个符号」 |
| 30 | `plugins/fov_changer/fov_changer.c:219-221` | 黑名单声明与 `ShPluginOnBlocked` 注册的返回值都未检查（其余插件会检查并记日志） | 检查并记录 `ShLastError()` |
| 31 | `plugins/cnchat/cnchat.c:910`+`:921-924` | `Bind()` 失败时已注册的 drawer 不回滚（对照 `OpticalCamo.c:436-441` 会回滚菜单） | 失败分支先 `ShDrawDel` |
| 32 | `plugins/OpticalCamo/OpticalCamo.c:434-435`、`:331` | `ShMenuToggle` 返回值未检查（相邻的 `ShMenuList` 有检查并回滚）；`ShGetVisibility` 返回值被丢弃 → 失败时显示 1.0f 与实际不符 | 检查返回值；失败保留上次值 |
| 33 | `plugins/TimeWeatherControl/TimeWeatherControl.ini:10-11`、`:17-18` | 随包 ini 的注释仍在描述**已删除的 blending 行为**，且未提 `dusk_speed`/`dawn_speed`（代码已是四个离散窗口 + 四把键） | 更新注释与键表 |
| 34 | `plugins/TimeWeatherControl/TimeWeatherControl.c:341-360` + `:474-511` | 每个菜单事件整体重写 ini（约 7 个键），连按时每 tick 一次 | 去抖落盘 |
| 35 | `plugins/skipintro/skipintro.c:613-616` | 路径过长时回退分支把日志写到**游戏根目录**而非 `logs\` | 回退也落到 `logs`（或走框架统一出口） |
| 36 | `plugins/skipintro/skipintro.c:534-535` | `WritePrivateProfileStringA` 返回值未检查，写盘失败完全静默 | 检查并记一行 |
| 37 | `plugins/ammo_capacity/` | 目录内**无 `lang.ini`**（其余 7 个都有）。已核实：文案 `kEn/kZh` 已编译进代码并 `ShLangDeclare`（`ammo_capacity.c:64-91`），**功能与双语显示正常**，仅少一个「玩家就地覆盖文案」的模板文件 | 补一个同格式的注释型 `lang.ini` |
| 38 | `plugins/ammo_capacity/ammo_capacity.c:285-290` | 黑名单声明在 `BuildMenu()`（`:280`）之后（框架 250 ms 轮询判定，无实际窗口期，仅顺序不一致） | 声明前移 |
| 39 | `plugins/ammo_capacity/ammo_capacity.c:78` | 中文含未译英文词「Hook校验失败」；且 `docs/text-cn-review.ini` 无 `[ammo_capacity]` 段（只有旧第三方插件的 `[AmmoCapacity]`） | 改「挂钩校验失败」并补校对段 |
| 40 | `docs/text-cn-review.ini` | 缺 `[TimeWeatherControl]` 段（该插件 24 个中文键无法进校对流程） | 补段 |

### 4.5 其它（低）

| # | 位置 | 问题 | 最小修复方向 |
| --- | --- | --- | --- |
| 41 | `scripthook_dinput.c:277-288`、`305-320` | 补丁失败时 `g_devVt[g_nDev]`/`g_diVt[g_nDi]` 已写但计数未推进（slot10 失败更会导致 `g_origData[i]` 为 NULL 而被调用） | 失败即回滚表项并返回 |
| 42 | `scripthook_forge_io.c:707-726` | 三条文件规则部分注册失败无回滚（功能半生效且静默） | 失败即撤销已注册规则 |
| 43 | `scripthook_forge.c:920` | `g_ovl = calloc(...)` 未检查 → OOM 时空指针写 | 检查并置 `dry_run` |
| 44 | `loader.c:294-298` | 线程创建失败被静默吞掉：插件全不加载，玩家只看到「什么都没生效」 | 失败写一行日志 |
| 45 | `scripthook_api.c:170-180` | `ReadProcessMemory` 传 `NULL` 取字节数：部分读取也返回成功，`out` 尾部是未初始化数据 | 接收字节数并校验 |
| 46 | `build_msvc.ps1:13` | 用法注释写 `only the nine shipped plugins`，实际是 8 个 | 改成 8（或改为引用 `$betaSet`） |

---

## 五、已知限制 / 可接受（12 条）

1. **三条常驻线程永不退出、从不 join**（`loader.c:295`、`scripthook_corefix.c:2095-2106`、`scripthook_state.c:363-369`、`scripthook_forge.c:962`）：句柄都关了、不泄漏，安全性完全建立在「进程存活期间 DLL 不卸载」之上——**这就是 3.1 之所以是必修的原因**。
2. **`ShFindEntities` 半径 0 即无裁剪 + 逐实体内核读**：框架自身调用都带半径（`spawn.c:638/716` 用 120），成本主要落在插件 API 使用者身上；`entity.c` 顶部注释与 `docs/` 已声明代价。
3. **头部控制器扫描**（`scripthook_entity.c:426/512`）：全地址空间扫描，但已按 250 ms 预算切片并有未命中记忆（`HEAD_MISS_MS=600000`）——属**正确的自限设计**。
4. **`ShUiCommitAsync` 的堆分配**（`scripthook_ui.c:1311`）：每次异步提交一块，`CommitThread` 结束时释放，成对且不在每帧路径。
5. **公测 8 插件均不做版本号校验**（只有 `GetProcAddress` 缺失即放弃；`OpticalCamo.c:70-72` 注释明确是**有意移除**版本门）：`SH_API_VERSION` 未变的前提下可接受。
6. **`SH_RELEASE` 实为「运行期抑制」而非「编译掉」**（`log.h:33-40`）：诊断代码仍在二进制里，只是不落盘；崩溃报告不受它影响。文档措辞可修，行为可接受。
7. **`firstperson` / `fov_changer` / `OpticalCamo` / `TimeWeatherControl` 默认进幽灵战+雇佣兵黑名单**（框架默认行为）：对这类插件是恰当的。
8. **`cnchat` 默认关闭且需重启生效**（`cnchat.c:527`、`:812-817`）：产品设定，`@chat.hint` 已说明。
9. **`cnchat` 与 `draw_sample` 共用同一 IME/输入会话**：两者互斥使用，不冲突（`draw_sample` 非公测件）。
10. **`spawner/lang.ini` 4483 B**：已核实为**正常**——承载框架 65 条载具目录名的中文翻译（`scripthook_spawn.c:134-204` 与 `lang.ini:10-74` 逐条一一对应），且这些名字不在框架内嵌表里，只能由该文件提供。
11. **全部 ini/源码 UTF-8 无 BOM**（`docs/i18n-refactor.md:249/391`、`scripthook_config.c:1727-1733` 显式跳 BOM）：编码一致，无 GBK 混装。
12. **崩溃报告不含隐私**（`scripthook_crash.c:71-88` 只打「模块名+偏移」，`:93-100` 只打基址）：唯一隐私残留是 4.2 第 11 条的进程路径日志。

---

## 六、实机验证清单（攒着一次验）

| # | 目的 | 步骤 | 预期/判据 |
| --- | --- | --- | --- |
| 1 | 确认 `DllCanUnloadNow` 返回值（3.1） | 附加调试器，在系统 `dinput8.dll` 的 `DllCanUnloadNow` 断点 | 无论结果，恒 `S_FALSE` 是正解 |
| 2 | `skipintro` 卸载路径（3.6） | 启动后 120 秒内用任务管理器结束进程 | 不卡死/不崩；`logs\skipintro.log` 有 `released (unloading)` |
| 3 | Forge 异步读 + 句柄关闭（3.2） | 装一个 Forge mod，正常游玩若干分钟（含读档、切地图） | 无崩溃；`logs\forge_probe.log` 无异常序列 |
| 4 | 日志规模与开关（3.8-3.10） | 清空 `logs\` → 启动一次 → 数文件；连续启动三次看崩溃日志大小 | 得到「全量诊断」下的文件数与体积；确认崩溃日志是否持续增长 |
| 5 | 队列变换（3.3） | 用会调 `ShQueueTransform` 的功能（如 NPCSpawner 的朝向玩家，本体暂不在公测集，可用开发插件） | 目标朝向正确、无偶发丢帧式错位 |
| 6 | `spawner` 落点语义（4.4-23） | 面朝北召唤一辆，再转 90° 召唤一辆 | 判断落点是否恒在东侧——若不是设计意图，则按朝向修正 |
| 7 | `firstperson` 诊断开关（4.4-24） | `firstperson.ini` 设 `diag=1`，连按热键多次 | 日志不每秒增长；三线程首开不重复创建文件 |
| 8 | 菜单创建失败的降级（4.4-28/29） | 正常进游戏逐个打开 8 个插件页 | 无插件显示「不可用/无回调」的空行；`spawner` 页面正常 |
| 9 | 语言回退链 | 删除 `<gamedir>\lang.ini` 与所有插件 `lang.ini` → 看菜单是否仍中英双语 | 回落编译内基线；再单放 `plugins\spawner\lang.ini` → 载具名显示中文 |
| 10 | 打包内容核对（3.11） | 按最终方式打包后列目录 | 只含 `dinput8.dll`、`scripthook.ini`、8 个插件目录（+ 许可/声明）；**无** `plugins_off/`、`logs/`、`docs/`、`tools/` |
| 11 | 卸载与回滚 | 删 `dinput8.dll` + `plugins\` + `scripthook.ini` 后启动游戏 | 游戏正常；确认残留仅 `logs\`、`mods\`；记录回滚步骤 |
| 12 | 长路径安装（4.1-6） | 若可能，把游戏放在接近 `MAX_PATH` 的路径下 | 观察 `snprintf` 截断导致的配置读写失败 |

---

## 七、发布准备检查结果

| 项 | 结论 | 风险 |
| --- | --- | --- |
| 版本与构建开关 | `SH_API_VERSION=1` 已导出；`-Release` 只额外定义 `SH_RELEASE=1`（`build_msvc.ps1:143/219`），`-Beta` 不定义；产物仅有 `__DATE__/__TIME__`，**无版本号/提交号**，且 `loader.c:275` 的 `v0.1` 与 README 的 `Beta1.0` 不一致 | 必修（3.7） |
| 公测集干净度 | `$betaSet` 为 8 项；集合外插件被**移到** `<gamedir>\plugins_off\<stamp>\`（不删除、可手动恢复，但脚本**没有还原命令**）；仓库有 `tools/`、`docs/api/`、`test_plugin`、`reflection_classes.tsv` 等开发件；**无任何打包清单/忽略规则** | 必修（3.11） |
| 语言包与文案 | 四层回退（插件 `lang.ini` → `<gamedir>\lang.ini` → 编译内当前语言 → en-US，`scripthook_config.c:398-408`）；7 个插件有覆盖模板、`ammo_capacity` 无（**功能正常**）；`spawner/lang.ini` 4483 B 正常；全部 UTF-8 无 BOM | 可接受 + 2 处建议修 |
| 日志与崩溃转储 | 目录固定 `<gamedir>\logs`；普通日志每次启动截断；崩溃日志追加但**有 512 KB 上限并轮转**（`scripthook_crash.c:29`）；**`[Settings] LogLevel` 已实现**（2026-09-17，见 3.8–3.10 行）：一处决定「哪些日志文件存在」与「每档写多细」，工作版默认 `info`、发布版默认 `warn`（只留 `scripthook.log` 与崩溃报告两个文件），临时改 `debug` 可取全量；崩溃报告不含用户名/机器名/完整路径 | **已解决**（日志开关策略见 3.8–3.10 行） |
| 安装 / 卸载 / 回滚 | README 有安装（`README.md:101-114`）、**无卸载章节**；回滚依赖**默认关闭**的 `-BackupDir`（`build_msvc.ps1:98-111`）；卸载后残留 `logs\`、`mods\`、`plugins_off\<stamp>\` | 必修 |
| 第三方依赖与许可 | `third_party/imgui`（MIT，`LICENSE.txt` 1083 B）与 `third_party/minhook`（BSD-2 + HDE BSD-2，`LICENSE.txt` 4446 B）文件都在；仓库本身 GPL-3.0；但**构建不复制任何许可进包，也没有 NOTICE 汇总**（另 `docs/plugins.md:172-186` 的 Credits 含第三方插件来源，若派生数据随包需按各自条款处理） | 必修 |

---

## 八、附录：审计覆盖度

**精读**
- 框架（51 个 `.c`/`.h`）：本次全部打开；其中加载/生命周期/并发/配置/日志/崩溃（`loader.c`、`guard.c`、`scripthook_dinput.c`、`scripthook_corefix.c`、`scripthook_crash.c`、`scripthook_config.c`、`scripthook_state.c`、`scripthook_api.c`、`log.h`、`forge*`）与每帧链（`ui/menu/draw/text/uiprop/hud/uiinput/fpx/tick`）及实体物理（`entity/scene/physics/havok/hit`）逐文件过。
- 公测 8 插件：8 个全部逐文件过（含各自 `lang.ini` 与 ini 与实际用键的比对）。
- 主结论复核：报告中的高危条目均由**主代理重新读源核对**（`loader.c:285-339`、`scripthook_config.c:495-564`、`log.h` 全文、`scripthook_api.c:805-864`、`scripthook_forge_io.c:330-424`/`605-664`、`plugins/skipintro/skipintro.c:615-694`、`plugins/spawner/spawner.c:1-70`、`plugins/ammo_capacity/ammo_capacity.c:55-104`、`scripthook_hud.c:150-189`、`scripthook_physics.c:590-704`），并据此**下调或修正**了 2 条子结论（`IsLangCodeLike` 的实际影响、`ammo_capacity` 缺 `lang.ini` 的性质）。

**粗筛**：其余 24 个插件（误装后果、接口一致性、与公测集共存、一眼可见缺陷）。要点：`test_plugin`（本地 REPL + 任意内存改写）、`chaos`（加载即改数值 + 全局键鼠钩子）、`GhostNoWipe`（拦截存档改名，默认开启）、`ModeCallProbe`（按硬编码 RVA 挂钩）、`freecam`（与 `firstperson`/`fov_changer` 抢同一相机字段）——**它们都不在公测集内，只需确保不随包**。另有 5 个目录只有 `lang.ini` 无源码（`trainer`、`DayNightVisibility`、`EqualizeEnemyHealth`、`GunShotDetection`），放入 `plugins/` 也不会加载。

**未覆盖（明确声明）**
- `third_party/imgui`、`third_party/minhook` 的**内部实现**未审计（只查了许可与分发条件）。
- `docs/api/`（Doxygen 生成物）、`tools/grw-relocate.ps1`、`docs/*.ps1` 脚本未做逻辑审计。
- `scripthook_ovl.cpp`（渲染钩子）只按调用频率核对，未逐行审计。
- 未做动态测试（无 AddressSanitizer / 无长时间连续运行压测）。

**本次审计阶段未修改任何文件。** 报告定稿后按用户指示执行了必修修复，状态见第九节；报告如需移入会话或改路径，告知即可。

---

## 九、修复状态（2026-09-16 晚，审计之后）

按用户决定「先做必修项」执行，**未提交 git**（改动待审阅，`git status` 可查）。

| 报告条目 | 位置 | 处理 |
| --- | --- | --- |
| 3.1 `DllCanUnloadNow` | `loader.c:327-336` | 恒返回 `S_FALSE`，并写明理由（detour、vtable 补丁、三条常驻线程都在本镜像内） |
| 3.2 forge_io 悬垂 `OVERLAPPED` | `scripthook_forge_io.c:634-660` | `CloseHandle` 分支同时清理该句柄的待完成读条目（原先只清两个句柄缓存） |
| 3.3 `ShQueueTransform` | `scripthook_api.c:812-862` | 认领位与就绪位分离：字段填完才发布，泵读完再释放 |
| 3.4 `LogFirst` 每编译单元一次 | `log.h:92-126` | 改为**按调用点一次**（宏内块级静态）；`entity.c` 五条、`havok.c` 四条、`stealth/input/fov` 各三条等原本被吞掉的诊断全部恢复可写 |
| 3.5 `LogInit` 截断 | `log.h:42-74` | 同名幂等；换名时先关旧文件（`scripthook_npc.c` 的人工规避不再是必需） |
| 3.6 skipintro 卸载路径 | `skipintro.c:171-181`、`:627`、`:682-690` | 加「锁就绪」标志，早到的 DETACH 不再触碰未初始化临界区；保留「必须交还规则」的取舍说明与理由 |
| 3.7 版本标识 | `scripthook.h:31-37`、`loader.c:275`、`scripthook_crash.c:96`、`README.md:1` | 新增 `SH_VERSION` 单点定义，四处引用同一串（当前值 `1.0-beta1`） |
| 3.8–3.10 日志策略 | `scripthook_crash.c:20-28` + `Emit`；`log.h`；`scripthook_config.c` 的 `ShLogLevel` | **已决定并实现（2026-09-17）**：把预留的 `[Settings] LogLevel`（`none/error/warn/info/debug`）做成唯一的日志旋钮，一个值同时决定「哪些日志文件存在」与「每档写多细」——`info`/`debug` 建全量**框架模块**日志，`warn` 及以下不建这些（`logs\scripthook.log` 与 `logs\scripthook_crash.log` 是**地板**：任何级别都写，报障要的正是它们），`none` 在地板之上不再写行（启动两行用 `LOG_ALWAYS` 例外保留）。工作版默认 `info`、发布版（`SH_RELEASE`）默认 `warn`（`log.h` 的 `LOG_DEFAULT`），现场排查时 `debug` 可临时拿回全量；级别由 `SH_API int ShLogLevel()` 单点解析（带递归守卫，因为第一次读取可能发生在配置自身装载过程中），`log.h` 每编译单元向 DLL 问一次并缓存，**插件也照同一答案写日志**。崩溃日志另有 512 KB 上限与轮转（同小节上一行）。**插件侧**：`skipintro` / `firstperson` / `TimeWeatherControl` / `ammo_capacity` / `spawner` 各自在日志出口接了级别（懒绑定的可选调用 `ShLogLevel()`，拿不到就不设门 = 旧行为；`fov_changer` 本就不写日志，无需接），走 `log.h` 的 `cnchat` / `OpticalCamo` 用 `LogInitAlways`。**口径当场修正（2026-09-17）**：这套门最初按「文件轴」统一挂在 `>= info` 上，等于把插件自己的报错也一起吞掉 —— 而插件日志恰恰是报障时最该留下的东西（文件小、事件驱动、最要紧的一行往往就是「哪里没成」）。现在**只有框架自己的模块日志受级别管**，**插件自己的日志除 `none` 外一律照写**（`log.h` 新增 `LogInitAlways`：不受级别门控、行也不按级别过滤，因为插件那些行本来就没有级别；5 个自写日志的插件门阈值改为 `> SH_LOG_NONE`）；`none` 仍会关掉它们 —— `none` 的字面意思就是什么都不要。**包的形态**：玩家包改用 `-Release` 构建（默认 `warn`，只留两个文件），测试套件由打包脚本写入 `[Settings] LogLevel=debug`，玩家包里那一行一律删除。**保留上一场**（2026-09-17）：`LogInit` 打开 `scripthook.log` 前先把它改名为 `scripthook.log.prev`（`MOVEFILE_REPLACE_EXISTING`，单槽，下次启动覆盖；只对这一个名字做 —— 每个模块/插件都给 `.prev` 会把已经很长的 `logs\` 翻倍），于是重启游戏不再丢掉刚结束那一场的记录；重命名跑在 `DllMain`，此时文件拦截层尚无任何规则（钩子随首条规则装上），不会被自己的规则拦到 |
| 3.11 打包清单 | `tools/package-beta.ps1`（新增） | 白名单打包、生成 `THIRD-PARTY-NOTICES.txt`、列出被跳过项；产物落 `out/`（git 已忽略） |
| 七节 安装/卸载/回滚 | `README.md` | 新增「版本与日志策略」「卸载」「回滚到上一版」「打包公测版」四节 |
| **新增** `lang.ini` 只存在于游戏目录 | `plugins/*/lang.ini` | 5 份比仓库更完整的**实测译文**同步回仓库（`skipintro`/`spawner`/`firstperson`/`fov_changer`/`OpticalCamo`）；只同步 `lang.ini`，插件 `.ini` 里的玩家设置不动 |

> 一条**不是产品缺陷**但值得记下的事：某些宿主 shell 在自己的作用域里保留了带类型的 `$Zip` 变量，而 PowerShell 变量名大小写不敏感，于是脚本里给 `$zip` 赋值会报「String → SwitchParameter」。`tools/package-beta.ps1` 的局部变量因此叫 `$archive`，注释里写明了原因，避免被「顺手改回 `$zip`」。

### 第二批：建议修里的高优先级项（同日稍晚）

| 报告条目 | 位置 | 处理 |
| --- | --- | --- |
| 4.1-1 短段名被当语言表丢掉 | `scripthook_config.c` `IsLangCodeLike` | 只有带地区/文字后缀的标签才算语言表，裸 `[ui]`/`[mod]`/`[npc]` 回归设置段。代价方向是安全的：残留的 `[zh]` 被当成设置读，而不是设置被静默丢掉 |
| 4.1-2 超长行被拆成新行 | 同文件 `NextLine` | 整行消费完再前进，截断时留一行说明；原先剩余部分会被当成新的 `key=value` |
| 4.1-3 读不到配置时反复重写 | 同文件 `LoadConfig` | 读失败也置就绪并记一行；原先每次查询都会再写一次默认配置（`WriteDefaultConfig` 是 `"w"`） |
| 4.1-4 表满时内存与磁盘不一致 | 同文件 `SetEntry` | 表满时一次性说明「该键已落盘、未进内存」 |
| 4.1-5 前导零被当八进制 | 同文件 `ShConfigGetInt` | `strtol` 基数改 10（`cpu_cores=08` 原先读成 0 且不报错） |
| 4.1-6 路径截断静默 | 同文件三个路径函数 | `snprintf` 返回值 ≥ 缓冲区大小即报错返回 |
| 4.3-13 菜单每帧重复翻译 | 同文件，文本层新增缓存 | 按 `(owner,key)` 记忆翻译，复用 `TextLock`，语言切换时整体失效；调用点无需改动 |
| 4.3-16 Havok 表无锁 / 二次重建 / O(n²) | `scripthook_havok.c` | 双缓冲：重建在锁外、发布在锁内；读取走共享锁。未命中不再二次重建；查重是死代码（两遍扫描不可能产生同一对）已删 |
| 4.3-17 三秒忙等（实际更久） | `scripthook_physics.c` | 改为回调发信号的等待事件。`Sleep(1)` 会被调度器抬到约 15.6 ms 的节拍，3000 次循环最长可阻塞近一分钟；单次探测另设 500 ms 上限 |

> 第四节的建议修还剩 **约 20 条**未动（各插件菜单与回调的小疏漏、`ui.c` 的 O(n²) 子节点枚举、`uiinput` 的 256 键轮询、`log.h` 每行 `fflush` 等）。

### 第三批：性能与加固（同日更晚）

| 报告条目 | 位置 | 处理 |
| --- | --- | --- |
| 4.1 其余低风险项 | 多文件 | dinput 键码表用原子发布就绪标志；`WrapDevice`/`ShWrapDirectInput` 只在补丁真的装好后才推进计数（slot 10 失败会让 `g_origData[i]` 为 NULL 且可被调用）；forge 文件层三条规则改为全有或全无并回滚；崩溃日志路径解析失败有记录、写入失败回退到工作目录；`forge.c` 的 overlay 表检查分配并转入试运行；loader 线程创建失败留一行；`ShReadFast` 校验实际读到的字节数 |
| 4.2-8 HUD 面板缺失日志 | `scripthook_hud.c` `SyncSlot` | 改为只写一次（原先每次 HUD 刷新都写并 flush 一次） |
| 4.2-11 进程路径写进日志 | `scripthook_corefix.c` | 只写可执行文件基名，避免 `C:\Users\<用户名>\` 进日志（崩溃报告本身早已不含隐私） |
| 4.2-12 键码表就绪标志 | `scripthook_dinput.c` | 与上表第一行同处，见其说明 |
| 4.3-14 HUD 文本解析三遍 | `scripthook_hud.c` | `SyncAll` 一次解析并把结果透传给 `SyncSlot`；随之删掉两个不再使用的测量辅助函数（每槽每次刷新省两遍解析与两次加锁往返） |
| 4.3-21 `ShBlockKey` 重扫 255 项 | `scripthook_input.c` | 改为计数维护；菜单开/关时连调 7 次不再各扫一遍 |
| 4.3-19 `ShFindEntities` 半径 0 | `scripthook_entity.c` | **刻意不改行为**：把 0 收窄成有限半径会改变今天调用它的插件语义。改为就地说明代价与「近处请传半径」 |
| 4.3-22 tick 时钟混用 | `scripthook_tick.c` | **审查后确认无需修改**：该模块整体是 32 位毫秒且用无符号差值比较，本身抗回绕；已写注释说明，避免被「顺手改成 64 位」破坏 |
| 4.4-33 随包 ini 注释过时 | `plugins/TimeWeatherControl/TimeWeatherControl.ini` | 注释仍在描述**已删除的渐变混合**且只列两把速度键；改为四窗口说明并补齐 `dusk_speed`/`dawn_speed` |

**刻意留作后续的三条**（都属于 UI/HUD 层的结构性改动，值得单独一轮并在游戏内验证，而不是顺手改）：

- 4.3-15 `ui.c` 的子节点枚举/级联/销毁是 O(k×256)：需要维护父→子索引，改的是 UI 层核心数据结构。
- 4.3-18 HUD 重建逐控件同步排队（`ShUiPanel`+十余个 `ShUiLabel`，每控件一次 `Sleep(1)` 等待）：应改走已有的 `ShUiBegin/Commit` 批处理通路。
- 4.3-20 `uiinput` 每 8 ms 轮询 1..255 全部 `GetAsyncKeyState`：需要一张「已注册热键」表，涉及输入层与热键注册的接线。

**一条工具链动作**（不是代码缺陷）：4.4-40 的 `docs/text-cn-review.ini` 缺 `[TimeWeatherControl]`/`[ammo_capacity]` 段 —— 该文件由 `docs/export-text-review.ps1` **生成**，手改会被下次导出覆盖，应当在下次翻译评审前重跑该脚本。

### 第四批：换机测试前的准备（同日晚）

| 项 | 位置 | 处理 |
| --- | --- | --- |
| **产物依赖 VC++ 可再发行组件** | `build_msvc.ps1` 的 `$c` 与 `$cpp` | 加 `/MT`。原先走默认 `/MD`，`dinput8.dll` 与 8 个插件都依赖 `VCRUNTIME140.dll` / `MSVCP140.dll` —— 它们**不是 Windows 自带**（`ucrtbase.dll` 才是）—— 而干净机器上缺了它们的表现是 `LoadLibrary` 失败：游戏能开、菜单没有、**日志一个字都没有**（DLL 根本没运行）。静态链接后已验证三个产物对 `VCRUNTIME140` / `MSVCP140` / `api-ms-win-crt` 的名称全部为 False。跨模块堆也已核查：`free()` 的调用点全是各模块自己 `malloc` 的指针 |
| 包内 `scripthook.ini` 启用了未随包的插件 | `tools/package-beta.ps1` + 游戏目录的 ini | 打包脚本按实际随包集合**重写** `[plugins]` 段（原先沿用的是游戏目录那份，里面还有 `LastRites_dlcfix=1` / `NPCSpawner=1`，会让框架在干净机器上记两条 `no X.asi, skipping`）；游戏目录的 ini 同步清成 8 行并去掉 `@lr.page` |
| 包内没有说明文件 | `tools/package-beta.ps1` | `README.md` 随包：安装、菜单键、日志位置、卸载步骤都在里面 |
| 日志目录不可写时完全无声 | `log.h` + `scripthook_crash.c` | `logs\` 建不出或写不了时，日志落到**游戏目录**（崩溃日志同）并在首行写明原因。此前是一整场无日志、且没有任何地方说为什么 —— 换机时这类环境（Program Files、只读盘、扫描器占用）恰恰最常见 |

**换机测试前的三项非代码准备**（给测试者）：

1. 那台机器的**游戏版本必须与本机一致** —— 框架里所有常量都是按这份 `GRW.exe` 钉的。判据现成：`logs\scripthook.log` 的健康日志出现 `MISMATCH` / `is not readable` / `holds ... wanted ...` 即为版本不符。
2. **杀软先给游戏目录加白名单** —— `dinput8.dll` 是代理 DLL 且做 hook，被隔离的概率不低，而它的现象与「运行库缺失」完全一样（游戏能开、mod 全无）。本批 `/MT` 之后运行库这条已排除，**剩下的就只剩杀软这一条**。
3. **别在联机模式带插件跑**（单机没问题；插件已声明幽灵战 / 雇佣兵黑名单）。

**分辨率与 DPI 无需准备**：`scripthook_ovl.cpp` 的 `MaybeRescale` 按 backbuffer 高度自动缩放并记一行 `ui scale … (res WxH)`，`[Settings] MenuScale` 可手动微调；UI 走 backbuffer 像素，系统 DPI 不影响。

### 第五批：换机反馈与第四节的收尾（2026-09-17）

这一批并排记两件事：换机测试那 6 条反馈加随后补报的指针一项（现场与判据见 `docs/beta-test-plan.md` 的 5.1 节），以及第四节剩下的两条。

| 项 | 位置 | 处理 |
| --- | --- | --- |
| 反馈 1 全新安装的游戏首次启动闪退 | `scripthook_ovl.cpp` | 探针设备那一段改为分步日志 + 稳定等待 + 重试（`ecf2e1d`）。崩溃落在「覆盖层线程拿到窗口之后、`hooks installed` 之前」的 0.35 秒里，现在这段每一步都留字，失败可退。只在全新安装首启出现，根因仍未坐实：下次新装时看 `logs\scripthook_ovl.log` 停在哪一步即可定位 |
| 反馈 2·3·4 菜单文本互相串字（提示反复上下闪、语言行显示成页面名、数值乱、标题与页脚乱） | `scripthook_config.c` 文本层 | 根因是翻译缓存里存的是**轮转缓冲区的指针**而非副本：下一轮翻译把缓冲区覆盖之后，各行读到的就是别人的字（`3fef8f9`，缓存改为自持文本）。为定位加的「菜单模型 vs 视图」对拍诊断随后按计划撤掉（`82a8f36`） |
| 反馈 6 光学迷彩设了数值没效果 | `plugins/OpticalCamo/OpticalCamo.c` | 日志显示因子在 `0.5x(active)` 与 `1.000x(inactive)` 之间每秒来回跳：激活判定改为**锁存**一次读到的激活态，并把每次翻转的投票依据写进日志（`80be335`）。语义按决定保持「仅在迷彩激活时生效」 |
| 反馈 5 第一人称开镜过渡玩久了消失 | `plugins/firstperson/firstperson.c`、`scripthook_menu.c` | 只加了瞄准闸门的翻转日志（`1c9ba5f`），随后与对拍诊断一并撤掉（`82a8f36`）：**行为未改**，此项待实机复现后再定性 |
| 4.4-23 spawner 的「前方」落到东向 | `plugins/spawner/spawner.c` | `AHEAD` 沿玩家朝向分解，只有拿不到相机时才退回旧的东向偏移（`95dfa14`） |
| 4.4-35 skipintro 日志回退落到游戏根目录 | `plugins/skipintro/skipintro.c` `OpenLog` | 改走框架的 `ShLogPath`（就是审计建议的「框架统一出口」）：`<gamedir>\logs\skipintro.log`，目录也由它创建。手写的那段路径在安装目录长到文件名放不下时会把日志丢在 `GRW.exe` 旁边 —— 正好落在玩家被要求打包的 `logs\` 之外 |
| 4.4-40 校对表缺两个段 | `docs/text-cn-review.ini` | 重跑 `docs/export-text-review.ps1`：`[ammo_capacity]`（7 行）与 `[TimeWeatherControl]`（28 行）补齐，全表 464 行、**空中文 0 条**；顺带清掉已不在树里的旧第三方 `[AmmoCapacity]` 段 |
| 包的形态（与代码无关，但换机要用） | `tools/package-beta.ps1` | `mods\` **恒定随包**、逐字节原样（`95dfa14`）；`[forgemod] enabled` 测试套件写 1、玩家版写 0；随包的每份文档同时给 `.md` 与 `.txt`（`f8a2bb1`） |
| 反馈 7 菜单一打开就冒出系统鼠标指针 | `scripthook_ovl.cpp` 的 ImGui 初始化 | 从来不是游戏、也不是绘制代码在显指针，而是 **ImGui 的 Win32 后端**：它按默认逻辑 `SetCursor(IDC_ARROW)` 显示 OS 箭头，而且**替我们回答了 `WM_SETCURSOR`**（后端返回 1 → 窗口子类 `return 1` → 游戏自己的窗口过程再没机会把指针藏回去，所以「只有开菜单才出现」）。按决定加 `io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange`：框架**一次也不碰**系统指针，`WM_SETCURSOR` 交回游戏 —— 指针显隐完全由游戏决定（游玩时隐藏，游戏自己的界面里照旧）。菜单本来就是键盘操作，不需要指针 |
| 反馈 8 幽灵战 / 雇佣兵里**所有插件照样能用** | `scripthook_playmode.c` 的监听线程 | 黑名单与插件声明都没问题（启动日志里 7 个插件都声明了 `Ghost War+MERCENARIES`，`cnchat` 声明不限模式）。断的是**模式信号**：监听线程要求先看到 `SetCurrentGameMode` 的调用才肯去看模式对象（`if (t < 0) continue;`），而 2026-09-17 在这台机器上实测到整整一场（从前端进幽灵战、进雇佣兵）**它一次都没被调用** —— 于是对象从没被读过，模式始终 `NONE`，按「读不到的条件不贡献任何位」什么也没禁。改为：**对象自己就能决定模式**（`ResolveMode` 本来就是这么设计的：只有 `t < 0 && !fp` 才无解），并且**对象一变就写一行**（未识别的对象正是需要补行的情形，两个未知对象连着出现原本会完全静默），另加一条一次性诊断，用来区分「`CreateGameMode` 从未被调用」与「调用了但描述读不出来」。**随后实测到根因**：两次独立会话（arg 0 的战役、arg 2 的雇佣兵）读到的对象都比表里的条目**正好低 `0x90`** —— 描述指向的是条目往前那一格。比对因此改为「条目本身、或条目往前 `MODE_DESC_BACK`（`0x90`）」两个写法都接受（条目彼此至少隔 `0x1F8`，不会串到别人身上）。修之前那两次会话都读成 `unrecognised mode object`、模式停在 `NONE`、什么都没禁 —— 与反馈完全一致。**收尾（同日 08:47）**：逐次调用日志拿到 Ghost War 的读数 —— 对象 `03908CB8`、`arg 3`、**在进模式界面那一刻就调用**（早于对局，正是期望的时机），它比表里那行 Ghost War 低 `0xE0`（Ghost War 的条目不在另外四个共用的 `0x38DDxxx` 表区，间距本就不同）。对象进表后，**MERCENARIES 与 Ghost War 均已实机确认「一进模式界面就禁」** ✓。同时补了一条规则：对象未知、但参数只会指一个模式时（`3` → Ghost War、`0` → 战役）不再丢弃该参数（`2` 仍不猜 —— 幽灵模式 / 雇佣兵 / 游击战 三者共用，只有对象能分开）|
| 反馈 9 自绘输入框：发送后**短时间**按 T/Y/U 不立即弹框 | `plugins/cnchat/cnchat.c` 的轮询线程 | 发送期间轮询被整段跳过（`if (g_sending) … continue;`），而一次发送 = 逐字注入 + 60 ms + **Enter 按住 800 ms**（≥0.86 s，消息越长越久）—— 这期间按聊天键不但不开框，连按过都没记录，玩家的感受就是「得再按一次」。改为：发送期间**仍然盯着聊天键**，按下即记成待开框（若键盘当时还在我们手里 → 连按键一起补发，因为游戏根本没看见那一下，它的聊天框没开、下一条消息会无处可落），注入一落地立刻开框；`SendThread` 收尾改为**先放键盘再清标志**（否则新框刚拿到的键盘会被收尾那一下放掉）。**已实机确认** ✓ |

**第四节逐条回读核对**（不只信本表；重点回读了仍有疑虑的十余处 —— dinput 补丁计数、崩溃日志的两道退路、进程路径脱敏、forge 三条规则全有或全无、overlay 分配、loader 线程失败、skipintro 写盘失败、TWC 与 firstperson 的落盘去抖）：**除「仍未处理」下列出的以外，没有遗留项。**

> 一处**不是缺陷**的可读性小事，记在这里而不改：`scripthook_text.c` 的英文侧把箭头写成 `\xE2\x86\x91` 这类转义，导出脚本照抄进 `; en:` 注释，于是那两行的英文原文显示为转义串。中文侧是真箭头，校对不受影响；要让英文侧也好看，得在导出脚本里解码 —— 与本轮无关。

### 仍未处理

- 4.2-7 `log.h` 每行 `fflush`：**保留**（理由同前：崩溃现场的可诊断性优先于那点吞吐；落在热路径上的日志点已移除）。
- 4.3-15 / 4.3-18 / 4.3-20 三条 UI 与输入层的结构性改动：**刻意留后**（见上「刻意留作后续」）—— 动的是 `ui.c` 的子节点索引、HUD 控件的批处理通路、`uiinput` 的热键表，都要进游戏验证，值得单独一轮。
- 反馈 5（第一人称开镜过渡）：**行为未改**，待实机复现后再定性。
- 第五节 12 条已知限制按定义不修。
- 第六节**实机验证清单 12 条尚未执行**（需进游戏）。

### 覆盖度声明更正

第八节结尾原写「本次未修改任何文件」——那是**审计阶段**的事实。定稿后按指示执行了上表的必修修复；完整改动清单以 `git status` 为准，未提交。

---

## 十、发布前终审（2026-09-17）

首轮审计（一~八节）与五批修复（九节）之后，对**当前代码**做的最后一次全面复审。

**方法**：三路静态复审（① 本轮改动区逐行；② 框架其余模块 + 逐条回读三批自述；③ 8 个随包插件）+ 两处实机日志（开发机 `logs\`、Beta 机 `logs\`）全量清扫。**本轮只审不改**，每条附「修 / 不修」建议。

### 10.1 结论

| 档位 | 条数 | 说明 |
| --- | --- | --- |
| 阻断发布 | **0** | 无「无条件必崩 / 必坏档」项 |
| 必修（建议发前处理） | **6** | 见 10.2；改动多为数行，其中 2 条需一次实机确认才能定论 |
| 建议修（不阻塞发布） | **14** | 见 10.3 |
| 已知 / 可接受 / 刻意保留 | **9** | 见 10.4 |
| 前几轮「已修」自述回读 | **20 条已修 / 3 条部分修** | 见 10.5 |

**发布判定：条件可发布。** 代码层面没有新的结构性风险；本轮改动区实测（MERCENARIES / Ghost War 一进模式界面即禁、菜单无系统指针、发送后立刻按聊天键弹框）均已实机确认。但六条必修里 **M1、M4、M5** 与「发布件本身」直接相关（模式识别残留风险、翻译缓存指针、打包脚本可能静默缺件），建议先修这三条再发；**M2** 已修（2026-09-17，描述只在创建后的新鲜期内读、读到即存）；实机验证（**判据同日修正**）：**在战役会话里不重启切一次模式**，看第二次 `CreateGameMode` 有没有被跟上、黑名单有没有翻转即可（几秒 ✓，不必待 30–60 分钟）、**M3** 已实机确认（`0fbe49a`，发送后立刻按聊天键那一条）；**M6** 属体验与支持面，可视发布时间决定。**M1、M4、M5 已于 2026-09-17 修完（见 10.2 后的表）；M3 已由 `0fbe49a` 修完；M2、M6 仍未动。**

### 10.2 必修（建议先修；逐条给判据与最小改法）

| # | 位置 | 问题 | 判据 / 证据 | 最小修复方向 | 建议 |
| --- | --- | --- | --- | --- | --- |
| M1 | `scripthook_playmode.c:348-369`（`ResolveMode` 的「相信参数」分支） | 参数与对象来自**两个不同的钩子**，可错配：`g_gmType` 只在 `SetCurrentGameMode` 被调用时改写且**永不失效**，`fp` 只在 `CreateGameMode` 时更新。于是「过期的参数 0 + 一个未知的 PvP 对象」会被判成**战役** → PvP 里插件照常生效（正是反馈 8 的形态，只是换了个入口）；反向「过期的 3 + 未知对象」会在单机里**误禁** | 代码自证：注释里已记录「整个会话 `SetCurrentGameMode` 一次都没被调用」确实发生过；`ResolveMode` 在对象未识别时直接用参数结算 | **只保留 `3 → Ghost War`（保守方向），去掉 `0 → 战役` 的回落** —— 战役本来就由已实测的对象行覆盖（`38DC760` 命中容错）；去掉后那类未知对象回到 `NONE`（= 什么都不禁，与战役等价、无害） | **修**（数行） |
| M2 | `scripthook_playmode.c:303-314`、`:393`（`FingerprintOf`） | `g_gmDesc` 被**永久保存**并每 200 ms 重读。若游戏释放/复用那段内存，`fp` 会读出垃圾并可能落在表或容错范围内 → 会话中途模式被改写，表现为「同一个场景有时封有时不封」 | 读到的 RVA 只要落在 `[0x1000,0x40000000)` 即被当作有效（`FingerprintOf` 只挡这两种越界） | 记录 `g_gmDesc` 所属的 `CreateGameMode` 调用号，只在对象**新近创建**时读；或同一 desc 只认第一次读到的 `fp` | **已修**（2026-09-17）：描述只在 `CmDetour` 开出的**新鲜期**（30 个监听周期 ≈ 6 s）内被读，**第一个有效读数即存下**（`FreshFingerprint`），之后不再碰那段内存；新的 `CreateGameMode` 会重开窗口并**清掉存下的读数**，所以「同一会话里换模式」那种情形（战役 ⇄ 其它，**不需要重启** ✓）每次都按新描述重读。实机验证（**判据同日修正**）：**在战役会话里不重启切一次模式** —— `playmode.log` 要有第二次 `CreateGameMode call N` 且模式行跟上、`blacklist.log` 的模式要翻转（几秒即可 ✓）；幽灵战/雇佣兵换模式必重启（一场一模式 ✓），**不必待 30–60 分钟** ✗。**另一条修法已否掉** ✗：以指针值为锚（同一 desc 只认第一次）在「新描述复用同一段地址」时会抱着旧模式不放 —— 那正是本行的症状本身 |
| M3 | `plugins/cnchat/cnchat.c:625-626` | `g_pendingOpen = g_ownsKeys ? 2 : 1` 用的是一次**瞬时**读取，与 `SendThread` 的 `ReleaseKeys()` 存在竞态：判成 1 而实际仍持有 capture → 不补发键 → 游戏聊天框没开，后面的注入无处可落；判成 2 而 capture 已放 → 多打一个字符 | 两处非原子；`ReleaseKeys()`（`cnchat.c:447/465`）与 poll 线程的 15 ms 轮询交错 | 由发送线程在清 `g_sending` **之前**置一个「capture 已结束」标志，poll 线程按「按键发生在捕获期间 / 之后」判定 1/2，而不是读瞬时 `g_ownsKeys` | **修**（小改）+ 实机验证：发送后 0–2 秒内连按聊天键，看是否出现多余字符 |
| M4 | `scripthook_config.c:1054-1056`、`1079-1082`（`ShLangText` 命中/未命中返回路径）、`:1014/1078`（`LCA_TEXT=160`） | 缓存虽然改为**自持文本**，但**返回给调用方的仍是共享槽内的指针**：`TextUnlock()` 之后，另一线程用同一槽（1/256）的键翻译时会覆盖它 → 与 09-17 那次「菜单串字」同一根因，只是窗口更窄。另外 `LCA_TEXT=160` 使 >159 字节的译文**命中时被截断、首读时完整**，同一键两次结果不同 | 返回指针指向静态槽，锁已释放；`CopyN(slot->text, sizeof(slot->text), v)` 只按 key 判定是否入缓存 | 缓存改为**按唯一键持有各自字符串**（键总数约 500 个，代价可忽略），指针终生有效；超长值不入缓存 | **修**（一处数据结构，收益是堵死同一类现场） |
| M5 | `tools/package-beta.ps1:301-305`、`192-198`、`200-217` | ① `$missing` 非空时只 `Write-Warning`，**仍出包** → 可能发出静默缺件的发布包；② `[forgemod]` 重写只写 `enabled`，同节其余键（`dry_run` / `strict` / `report_copies` / `probe` / `log_reads`）被丢弃；③ `[plugins]` 只改已有节、不创建节 —— 源 ini 若没有该节，包内就没有，框架按「无行=关闭」把所有插件关掉 | 三处都能在脚本里直接读到（`$ErrorActionPreference='Stop'` 对 `$missing` 的累加不生效） | ① 玩家包在 `$missing` 非空时 `throw`；② 保留 `[forgemod]` 其余行，只改 `enabled`；③ 末尾 `-not $wrotePlugins` 时补写 `[plugins]` 与八行 | **修**（各 1–3 行） |
| M6 | M6 | `plugins/{firstperson,fov_changer,OpticalCamo,skipintro}/lang.ini`（`spawner` 的 `@sp.*` 行同） | 这些 `lang.ini` 用**英文原文**作键，而代码查的是 `@ID`（`ShLangText` → `RowFind` 是精确 `strcmp`）→ 这些文件**从不生效**：玩家就地改文案无效、新增语言覆盖不到插件自身行（内置 zh-CN/en-US 由编译内基线兜底，所以当前显示正常，问题在「覆盖能力名不副实」） | 对照 `TimeWeatherControl/lang.ini` 用 `@tw.*` 是**正确**写法；`spawner` 的载具名行（英文键，无源码插件约定）是有效的 | 把这几个 `lang.ini` 的键改成与代码一致的 `@ID`（顺带修掉两行连字面都不匹配的死行） | **修**（机械替换，但要逐条核对）；至少要在文档里写明「带源码插件必须用 `@ID`」 |

#### M1 / M4 / M5：已修（2026-09-17）

三条都在同一天里改完并复核。改的是「输入的有效期」「缓存的存储形态」「脚本的拒绝条件」，没有一处动到别的东西。

| # | 改了什么 | 怎么判它对 |
| --- | --- | --- |
| M1 | `ResolveMode` 里两个「0 → 战役」分支**去掉**，只留「3 → 幽灵战」；监听线程裁决定一次后，用 `InterlockedCompareExchange` 把参数**消费掉**（值仍是刚用过的那个才清，钩子在另一个线程写它） | 参数只在「刚设置后的第一次裁决」有效 → 过期参数不可能再落成任何模式；未知对象 + 无参数回到 `NONE`（按设计不贡献任何位，与战役等价、无害）。战役与雇佣兵仍由对象命中：`38DC760` / `38DD0E8` 都在容错范围（`MODE_DESC_BACK 0x90`）内 |
| M4 | 翻译缓存从「256 个定长共享槽」改为**按唯一键各持一份堆字符串**：开放寻址索引（2048 槽 / 1024 条，读多写少时通常一次命中）、命中直接返回该条自己的串、语言切换（`g_lcGen` 变了）时整体释放重建、表满则「照常翻译但不缓存」并留一行字 | 返回的指针在语言切换前**终生有效**（不再有 1/256 被别的键改写）；`LCA_TEXT=160` 的截断随定长字段一起消失（>159 字节的行不再「首读完整、命中截断」）；键长不再有限制（`LCA_KEY=128` 曾把长句直接排除在缓存外，那也是一条串字通道） |
| M5 | 打包脚本：① `$missing` 非空时 `throw` 并**不写 zip**；② `[forgemod]` 只替换自己的 `enabled=` 行，同节其余键原样保留；③ 源 ini 缺 `[plugins]` 时按本包插件集**补写**该节 | 用**合成游戏目录**实测（不动部署）：源 ini 无 `[plugins]`、`[forgemod]` 带 `dry_run/strict/report_copies/probe/log_reads` → 产物里八行插件开关齐、五个额外键**全部保留**、`[Settings]`/`[Other]` 未被触碰；再删掉一个 `.asi` → 脚本报 `this package is incomplete (1 file(s) missing) - no archive written`，**未产生 zip** |

> 三条的构建与打包也随之复核：新 `dinput8.dll` 在两个 zip 里与部署版**逐字节一致**；测试套件 `docs\` 4 篇（`.md` 与 `.txt` 各一份）、`mods\` 4 个、8 个 `.asi`；玩家版无 `docs\`、`[forgemod] enabled=0`。

### 10.3 建议修（不阻塞发布）

| 位置 | 问题 | 判据 | 最小修复方向 |
| --- | --- | --- | --- |
| `scripthook_domino.c:189-203` | 帧任务队列槽位「查后即置」非原子，两个生产者可选中同一槽（对照 `scripthook_api.c` 的 `g_xq` 用 CAS） | `if (g_dq[i].ready) continue; … g_dq[i].ready = 1;`，`ready` 仅 `volatile int` | 增加独立 claim 位 CAS（同 `xq` 模式） |
| `scripthook_files.c:1595-1602` | 全框架最热路径（文件调用，数万次/s）每次 2 次 `QueryPerformanceCounter` | `FilesDecide` 调 `ShTickNow()` + `ShDecideFeed(at)` 各一次 | 复用一次 QPC，把值传入 feed |
| `scripthook_config.c:81`、`:107`、`:112`、`:1829` | 路径 `snprintf` 只判 `< 0`，截断仍返回成功（与已修的三个路径函数同类） | 对照 `ShPluginIniPath:135` 的 `n >= size` 检查 | 同款检查并置 `SH_ERR_BAD_ARG` |
| `scripthook_ui.c:1060-1092`（`JOB_WAIT_MS=3000`）、`ui.c:1506-1524/806-844`、`scripthook_uiinput.c:75-88` | 三条**已知待改**：HUD 重建逐控件同步排队；子节点枚举 O(k×256)；有场景取得焦点时每 8 ms 轮询 1..255 全键（≈3.19 万次/s） | 与九节「刻意留作后续」一致，代码里均有据 | 单独一轮改（HUD 走 `ShUiBegin/Commit`、维护父→子索引、只轮询已注册热键） |
| `scripthook_menu.c:358-375` | 回调 worker 无任务时 `Sleep(5)` 空转（≈200 次/s 取锁+比较） | `CallThread` 循环 | 空闲时等事件（`CallPush` 触发） |
| `scripthook_ammocap.c:196-200` | 安装竞争时 `Sleep(1)` 自旋上限 5000（≈5 s）阻塞调用线程 | 自旋计数 | 事件或短超时 |
| `scripthook_dinput.c:289` | slot 10 的 `Patch` 返回值未检查，失败仍 `g_nDev++` → 半包装设备（`HookGetData` 不可达，故不会空指针调用） | `Patch(vt,10,…)` 未判返回值 | 校验失败即回退 slot 9 并 `return` |
| `scripthook_fov.c:136-139` | `ShFovSet` 越界直接 `return 0`，不置 `ShLastError` | 无 `ShSetError` | 置 `SH_ERR_BAD_ARG` |
| `plugins/TimeWeatherControl/TimeWeatherControl.c:329-330` ↔ `:349-368` | `hour` / `minute` **只读不写**：`OnHour` / `OnMinute` 置脏却无对应写盘 | 读键 ↔ 写键不对称 | `SaveConfig` 补写两项，或去掉这两个回调的 `SaveConfigSoon()` |
| `plugins/firstperson/firstperson.c:595-596/607-608`、`:795-801` | 两条 toast 与 away 状态行的 `why`（"menu"/"drone"/"engine view"）硬编码中文、绕过语言层 | `SayStatusWhy` 只译模板不译实参；`ShToastEx` 原样存文本 | 加 `@fp.*` ID 后再传 |
| `plugins/OpticalCamo/OpticalCamo.c:287-296` | 投票翻转时每次变化写一行日志（现场约 1–2 行/s 的磁盘写） | `LatchCamoState` 在 `raw != lastRaw` 即 `Log` | 按时间窗限流 |
| `plugins/{fov_changer,TimeWeatherControl,skipintro,ammo_capacity}` 菜单注册处 | 菜单行注册返回值多数未检查（`skipintro` 连 `ShMenuCreate` 的 0 也未判）→ 失败会留半成品页或缺行，无日志 | 对照 `OpticalCamo.c:512-524` 的正例 | 至少判 `ShMenuCreate==0` 与关键行，失败记状态/回滚 |
| `plugins/{firstperson,spawner,skipintro}` | 未显式 `ShPluginBlacklist` / 未订阅 `ShPluginOnBlocked`（吃框架默认 GW+MERC）；`spawner` / `skipintro` 也没有 `ShPluginAllowed()` 检查点 | 框架默认行为与之一致（黑名单篇已声明「未声明即默认禁」），故**行为正确**，但少了「模式中途翻转时停下来」的通道 | 显式声明 + 订阅 + 干活前查一次（可选） |
| `scripthook_scene.c:376`（`RenderHook` → `RenderOurs:199`、`TickAll:227`） | 每次场景渲染（一帧多 pass）都做一遍 O(16²) 选择排序与 16 次 tick 派发 | 注释 `:244` 明说「一帧多次 pass」 | 按帧缓存排序结果（只在场景集合变化时重排） |

#### 性能八项：已修（2026-09-17）

本表的第 4（三条结构改动）、6、7、8、9 行都在同一天改完。改法一律是「把重复的计算或空转去掉」，没有一处改语义；唯一新增的对外接口是 `ShUiInputWatch`。

| 项 | 改了什么 | 怎么判它对 / 收益说明 |
| --- | --- | --- |
| 最热路径时钟采样 | `ShDecideFeed` 不再**每调用**做 64 位除法：累计原始计数，`ShDecideTake` 每帧转换一次 | 两次 `QueryPerformanceCounter` 采样**保留**——它们量的是区间，不是浪费；本项真实收益很小（每次数十周期），所以只做「把除法搬走」这类无损改动，没有为了凑条目去动诊断语义 |
| 场景每 pass 排序 | 可见场景表**按帧缓存**（`g_ordGen`），只在场景创建/显示/隐藏/排序/世界失效时重建 | 一帧多 pass 的每次场景渲染不再重跑 O(16²) 选择排序；失效点覆盖 `Create` / `DestroyJob` / `ShSceneInvalidate` / `ShSceneSetOrder` / `ShSceneShow` |
| 菜单回调线程空转 | 空闲改为**等事件**（`CallPush` 置位；10 ms 超时只为保住 ping 节奏） | 不再每 5 ms 取一次锁（≈200 次/s）；`SH_TICK_MENUCALL` 的 ping 节奏与语义不变（ping 仍在循环顶，晚帧行照旧） |
| 帧任务队列槽位 | 加**独立 claim 位**（CAS，与 `ShQueueTransform` 的 `g_xq` 同型）；泵也持认领后再执行 | 「查后即置」曾允许两个生产者挑中同一槽并静默丢一个任务；现在认领是原子的，泵在处理期间也不会被改写 |
| 安装竞争长自旋 | 改**等事件**（`InstalledWait/Signal`），5 秒只作上界；事件建不起来时退回有界睡眠 | 起装线程不再被阻塞 5 秒；顺带修掉 `prev == 2`（别人刚装好）被当成失败返回 0 的那一支 |
| `uiinput` 全键轮询 | 新增 `ShUiInputWatch(vk, on)`：**声明过**就只扫「已声明 ∪ 正在按住」，**没有任何声明仍扫全键** | 对不调用它的消费方**零行为变化**。实测更正：真正经 `ShUiSetInput` 回调的只有开发样例 `ui_sample`（↑↓/Enter + F7），菜单与 8 个随包插件各自轮询或用钩子，**不走这条路径**——所以「3.19 万次/s」只在「有场景取得焦点且消费方做了声明」时才会出现 |
| HUD 逐控件排队 | `SyncSlot` 的编辑改走 `ShUiBegin`/`ShUiCommit` **一次提交**（在 `EnsureView` 之后开始，避免早退留下未提交的批次；提交失败丢视图重建并留字） | 一次刷新从「几十个同步 job（每个都等渲染侧）」变成**一个**；单槽操作数（≤ ~40）远低于 `MAX_BOPS 256`，按槽分块即可 |
| 父子枚举 O(k×256) | 新增**父→子链**（惰性构建），`Children` / `CascadeAlpha` / `DestroySubtree` 不再各自扫全表 | 失效只挂在**两处** `Widget.parent` 写入点（`Create`、`OP_REPARENT`）；死亡不需要重建——遍历本身校验 `alive`/`parent`，漏一次 bump 只会晚重建，绝不会走进已回收的控件 |

> 结构改动的验收在 `docs/beta-test-plan.md` 第 17–19 项（HUD/面板照旧、菜单回调即时生效、热键与聊天键照旧），需要一次实机确认。

### 10.4 已知 / 可接受 / 刻意保留（9 条）

1. `log.h:115` 每行 `fflush`：**刻意保留**（崩溃现场的可诊断性优先）。
2. `ui.c` 子节点 O(k×256)、HUD 重建未走批处理、`uiinput` 全键轮询：**刻意留后**（结构性改动，需进游戏验证）。→ **2026-09-17 已做**（父→子链、HUD 走批处理、`ShUiInputWatch` 声明式收窄），见 10.3 后的表；实机验收见测试清单第 17–19 项。
3. `ShFindEntities` 半径 0 = 不裁剪、`tick.c` 32 位毫秒回绕：**刻意不改**（代码注释有据）。
4. `skipintro.c:695` 在 `DLL_PROCESS_DETACH` 里调用框架 API（`ShFileRuleDel`）：与插件契约相悖，但属**有意权衡**（「必须把规则交还」）；风险是退出时与持层锁线程互等，概率低。
5. `scripthook_state.c:296-308`：`busy` 期间丢弃的新状态由下一 tick 补处理。
6. `scripthook_physics.c:631/726`：事件等待仍带 1 ms 超时轮转，量级远小于旧忙等。
7. `image.h:15-32`：镜像基址惰性初始化无同步，多线程各算一次、值相同。
8. `scripthook_domino.c` / `ShQueueTransform` 的可见性等问题属「需实机验证」，沿用第六节清单。
9. 反馈 5（第一人称开镜过渡）：**行为未改**，待复现再定性。

### 10.5 前几轮「已修」自述回读（不只信文档）

**已修（20 条）**：配置解析 6 项 + 文本层缓存与语言切换失效 ✓、Havok 双缓冲与未命中不重建 ✓、physics 事件等待与 500 ms 上限 ✓、dinput 键码表原子发布 ✓、崩溃日志路径回退/写入回退/512 KB 轮转/去隐私 ✓、forge_io 三条规则全有或全无 + `CloseHandle` 清待完成读 ✓、forge overlay 分配检查 ✓、loader 线程失败留字 ✓、`ShReadFast` 字节校验 + `ShQueueTransform` 认领/就绪分离 ✓、HUD 日志一次 + 解析一次 ✓、`ShBlockKey` 计数 ✓、corefix 路径脱敏 + 全默认早退 ✓、`LogFirst` 按调用点 + `LogInit` 幂等 ✓、两条「刻意不改」注释有据 ✓；插件侧：firstperson 五项 ✓、spawner 三项 ✓、fov 返回值 ✓、OpticalCamo 四项 ✓、TWC 去抖与随包 ini ✓、skipintro 日志出口与写盘检查 ✓、ammo 四项 ✓。

**部分修（3 条，已在 10.2 / 10.3 列出）**：
- 路径 `snprintf` 截断只修了三个函数，`ShPluginsDir` / `ShLogPath` / 框架 `lang.ini` 路径仍是「静默截断成功」（10.3）。
- 翻译缓存「自持文本」了，但**返回指针仍共享**，且 `LCA_TEXT=160` 截断（M4）。→ **2026-09-17 已补完**：改为按唯一键各持堆字符串，指针终生有效、截断与键长限制一并消失（见 10.2 后的表）。
- `WrapDevice` 仍不检查 slot 10（10.3）。

### 10.6 实机日志证据（两处 `logs\` 全量清扫）

- **两处共 93 个日志文件（0.60 MB + 0.18 MB）**，按 `FAILED|refused|error|denied|MISMATCH|is not readable|unrecognised|ANOMALY|gave up` 全量扫：**唯一命中**是 Beta 机 `scripthook_playmode.log` 里那条**已修**的 `unrecognised mode object` —— 当前会话无错误行、无被拒调用。
- **崩溃日志 3 份**：开发机 1 份（`2026-09-16 00:26:39`，first-chance `C0000005`，`GRW.exe+0x1061FD54`，`read of 0x20`，`rsi=0` / `rcx=0`）—— **未被后续任何修复覆盖，仅这一次、无复现**，列入遗留；Beta 机两份内容相同（`2026-09-16 19:23` 的已知首启崩溃，已加固 + 分步日志，等下次新装验证）。
- **日志体积**：默认（全量诊断）一场约 30+ 文件 / 0.6 MB；`firstperson.log` 在开着 `diag=1` 的一场约 580 KB（默认 `diag=0`，无此量）。

### 10.7 性能结论（当前代码）

**每帧成本前三**：① `scripthook_scene.c:376 RenderHook`（每次场景渲染都跑，一帧多 pass：O(16²) 选择排序 + 16 次 tick 派发）；② `scripthook_camera.c:~288` 的泵链（`ShVisibilityPump → ShTransformPump → ShDominoPump → ShHeadPump`，每帧）；③ `scripthook_draw.c:252 ShDrawFrame`（最多 16 个 drawer × begin/end）。

**每秒成本前三**：① `scripthook_uiinput.c:75-88`（焦点场景下 8 ms × 255 键 ≈ **3.19 万次/s**，全框架最高）；② `scripthook_files.c:1595-1602`（每次文件调用 2×QPC + 至多 32 条规则，属最热路径）；③ `scripthook_menu.c` 三个常驻线程（40 ms 键轮询 / `CallThread` 5 ms 空转 / `HitPump` 4 ms）。

**结论**：本轮改动**没有新增热路径退化**（playmode 每 200 ms 读数、菜单开一次一行、`SetModeLine` 仅变化时写）；上表前三项与文档「刻意留后」一致，可在后续版本单独处理。

**2026-09-17 更新**：上表各项已在同日处理完（见 10.3 后的「性能八项」表）。其中两条需要**实测更正**：

- 每秒成本第 ① 名的 `uiinput` 全键轮询只在**有场景取得焦点**时发生（`InputThread` 先看 `g_focus`），而真正经 `ShUiSetInput` 注册回调的消费方只有开发样例 `ui_sample`；菜单与 8 个随包插件各自轮询或用钩子。所以这一项的「3.19 万次/s」是有条件的量级，不是常驻开销。
- 每帧成本第 ① 名的场景排序，`TickAll` 其实**每帧只跑一次**（在 `NewFrame` 分支里），真正每 pass 重跑的是 `RenderOurs`——本轮缓存的是它。
- 载具派遣的「首场十几秒」（2026-09-17 现场）：**不是本节的性能项**，是框架首次进世界时在后台扫全地址空间、找 vehicle spec（`logs\scripthook_spawn.log`，实测 14.7~19.3 s，跑在 `WarmThread` 上，游戏本身不卡）。已做两件：① 派遣不再自己跑扫描，改为等预热线程、有上界（30 s）并留一行字（`SpecScanWait`；实测那次只等了扫描的尾巴，随后的 12 次派遣各 **20 ms**）；② 进度经 `ShSpawnWarmProgress` 暴露，派遣页状态行显示「预热中 N/65」。**另一条「把 spec 地址按 RVA 记住」已实测不可行** ✗：写不进任何东西 —— 那些对象不在映像里（地址落在模块基址之下的游戏自有区域），RVA 描述不了它们，会话间变的是**堆布局**而不是映像 ASLR；此结论已同时记在 `scripthook_spawn.c` 的 `g_specCache` 注释里，免得再试一遍。

### 10.8 遗留清单（可以不修，但要知道）

1. `scripthook_playmode.c` 的两条必修（M1、M2）—— M1 建议发前修；M2 需一场 30–60 分钟的 PvP 观察。
2. 开发机 `2026-09-16 00:26` 那次崩溃：位置在 `GRW.exe+0x1061FD54`，与 ovl 首启那条不是同一处；仅一次、无复现，若再出现请保留 `logs\` 与 `scripthook_crash.log`。
3. 第五节 12 条已知限制、第六节实机验证清单 12 条（部分已由本轮实机覆盖）、10.4 的 9 条。
4. 反馈 5（第一人称开镜过渡）待复现。

---

## 十一、公测后第一份现场报障与修复（2026-09-17 夜）

**报障**：首个公测版发布当晚，一名 **Steam 版**玩家反馈"启动游戏后黑屏、进不去游戏"，共四轮日志留档（`F:\UbisoftGames\Beta1.0\BUG\`）：20:42（warn 级）、22:32 与 22:58（debug 级，两轮 A/B）、23:33（修复验证）。

### 11.1 第一轮（warn 级）为什么读不出东西

`logs\` 里只有三行：版本、构建时间、`log level: warn - module logs off, plugin logs on`，加五份插件日志（内容与开发机健康会话逐字节同长）。没有崩溃报告（崩溃报告不受级别影响，所以**进程里没发生过致命异常**；玩家是把卡死的进程结束掉的）。

问题不在玩家：`warn` 是**整文件级**的门（`log.h` 的 `LogWanted`），框架各模块自己的日志文件根本不创建，连 `scripthook.log` 里非 ALWAYS 的行也被过滤 —— 于是 `loader.c` 自己的 `FATAL: could not load real dinput8.dll`（info 级）、插件扫描计数、覆盖层分步日志、判断游戏版本是否匹配的健康日志，**全部无声**。"真实 dinput8 加载失败""覆盖层装到错窗口""版本不符"这三种情况在这份日志里完全同形。**这一条直接催生了 11.6 的"地板里程碑"。**

### 11.2 第二轮（debug 级）把范围收窄到"框架自身"

| 事实 | 证据 |
| --- | --- |
| 插件可以排除 | 20:42/21:55 两场插件全开也黑；22:23/22:58 两场 `plugin loading disabled` 也黑 |
| 文件拦截层可以排除 | `[forgemod] ledger=0` 后该层一个钩子都不装（它只在有规则时才挂钩），仍黑 |
| playmode 的两个 MinHook 可以排除 | 再加 `[playmode] enabled=0`，仍黑；且那两场 `CreateGameMode`/`SetCurrentGameMode` 一次都没被调用 |
| 没有崩溃 | 无 `logs\scripthook_crash.log`（VEH 默认就位，只报不治） |
| 前端从未起来 | corefix 认出主窗口（`ScimitarEngineWindowClass`）创建，但**始终没有** `the main menu is up (state MenuOrLobby)`；`scripthook_dinput.log` 里 `game reads GetDeviceState` 一整场都没出现（游戏从未进入前端/输入轮询） |
| 游戏版本相符 | Steam 版 exe 在 `SetCurrentGameMode`/`CreateGameMode` 两个站点通过字节验证（`verified and hooked (try 1)`），reflect 也报方法表 `pinned ... ok` |
| 游戏是被阻塞 | 玩家实测"卡黑屏时 CPU 几乎为 0" |

### 11.3 一条被修正的判断（值得记：这是本次最容易读错的地方）

起初我把"主窗口出现后 10–21 秒还没变成可见大窗口"读成"引擎卡住"。修复后的成功会话证明**这台机器上前端本来就要 ~55 秒**（主窗口 `23:25:34` 创建 → `the main menu is up` `23:26:29`）。

于是失败会话要重读：主窗口出现 → 引擎在前端加载中（几十秒）→ **旧代码"60 秒预算用尽就取它看到的最大窗口"正好落在引擎建渲染器的窗口期里**：子类化了 466×310 的**启动画面窗**，并在引擎建设备的同时建了**第二个 D3D11 设备**。这与 2026-09-16 19:23 那场"新装首启 0.35 秒闪退"（探针设备刚建完就崩）是同一族，也就是 `scripthook_ovl.cpp` 注释里自认"这个竞态赢不了"的那一条。开发机之所以从没复现：它前端 ~6 秒就起来，60 秒的扫描总能先抓到真窗口，探针也就落在游戏设备已建好之后。

### 11.4 三处修改

| # | 文件 | 改了什么 | 为什么 |
| --- | --- | --- | --- |
| 1 | `proxy.def`、`loader.c` | 补上 `GetdfDIJoystick`，代理的 6 个导出此后与系统 `dinput8.dll` **逐序对齐**（ordinal 0–5 相同）。转发不猜原型：四参透传、按指针宽度返回，对"无参返回指针"和"出参 + HRESULT"两种形状都正确 | 系统 dinput8 有 6 个导出，我们只有 5 个。用标准摇杆数据格式（`c_dfDIJoystick`）的代码按名字解析它，装我们的代理就必然解析失败 —— 调用方要么整模块加载失败，要么拿到 NULL 走自己的错误路径。核对手法固化在 `tools/list-pe-exports.ps1` |
| 2 | `loader.c` | `LoadLibraryA(system32\dinput8.dll)` 与 `CreateThread(LoaderThread)` 移出 `DllMain`，改为**首次调用本 DLL 任意导出**时由 `ShFrameworkStart()` 执行（三态 once 守卫，并发调用者等待而不是转发空指针） | 在宿主的导入解析期间（loader 锁里）做嵌套 `LoadLibrary` / 建线程是 MSDN 明确警告区，可能与其他线程的模块初始化互等。这类 loader 锁死锁的表现正是"卡住、CPU 近 0、日志里什么都没有"。Steam 启动时注入的 overlay、驱动 shim 都在这时间窗里初始化 |
| 3 | `scripthook_ovl.cpp` | 挂载重做：**route 1 先捕获游戏自己的 swapchain**（自建一个 DXGI 工厂并给其 vtable 的 `CreateSwapChain`/`CreateSwapChainForHwnd` 打补丁，游戏自建 swapchain 时接住并挂 `Present`/`ResizeBuffers`，**此路径不创建任何设备**）；窗口选择三级：捕获到的窗口 → `ScimitarEngineWindowClass` 类匹配 → 只在 **180 秒**预算用尽后才退回"取最大可见窗口"。探针设备降级为**兜底**，且只在渲染窗口出现后再等 20 秒才走；两条路都落日志 | 关掉"两个设备抢驱动"这一族（09-16 闪退），也不再在错误时机挂错窗口（默认安装片头 47 秒 + 前端 55 秒，60 秒预算必然踩在中间） |

### 11.5 验证（玩家第四轮，23:33）

```
23:24:37.249 overlay: factory capture installed (2 vtable(s)) - the game's own swapchain is the target
23:26:07.579 hooks installed (CreateSwapChain): origPresent=… ok=1
23:26:07.579 swapchain captured (CreateSwapChain): hwnd=2011082 buffer 1920x1055 windowed=1
23:26:07.921 init thread: render window 2011082 is the captured swapchain's (1920x1055)
23:26:07.921 init thread: subclass installed (…) (captured swapchain)
23:26:07.921 overlay: the captured swapchain carries the hooks - no probe device is created at all
23:26:09.574 imgui ready: hwnd=2011082 device=…
23:26:20.357 menu OPEN … menu closed …（玩家按 F4 用了菜单）
23:26:29.402 corefix: the main menu is up (state MenuOrLobby)
```

`loaded real dinput8.dll` 出现在 `23:25:09.879`（游戏第一次调用导出的那一刻，而不是 DllMain）—— 第 2 处也按设计生效。全量扫描（`FAILED|refused|error|MISMATCH|unrecognised|…`）**无真错误行**；playmode 正确识别 `campaign`；我们自己的每帧开销可忽略（`ours 3µs`、`threads: none`）。**黑屏消失，菜单可用。**

### 11.6 诊断地板与 `[loader] overlay=0`

- **地板里程碑**：`log.h` 新增 `g_logFloor` 路由 —— 某个编译单元自己的日志被级别丢掉时，它的 `LOG_ALWAYS` 行改为**追加写入 `logs\scripthook.log`**（多单元共享同一文件，故一行一次 `fwrite`，避免交错）；同时把该写的行提升为 `LOG_ALWAYS`：loader 的 `FATAL`/真实 dinput8 加载/`DirectInput8Create hr`/配置加载/插件扫描计数、覆盖层的路线与失败、playmode 挂载结果、文件层挂载计数与失败、forge 扫描与 io 安装、dinput 接口/键盘设备包装与首次读键、corefix 档位与"全默认未动任何东西"、blacklist 注册与生效。**从此发布包（warn）也能回答"谁、到哪一步、有没有失败"。**
- **`[loader] overlay=0/1`（默认 1）**：覆盖层此前**没有任何开关**，玩家和我们都没法把它单独摘出来做对照 —— 这是本次排查唯一卡住的地方。关掉后：不扫窗口、不捕获/挂钩 Present、不子类化窗口。随包 `scripthook.ini` 里已带这一行与说明。

### 11.7 重发

`build_msvc.ps1 -Release` + `tools/package-beta.ps1 -Zip` → `out\GRW-ScriptHook-1.0-beta1.zip`（23:42，替换线上资产）。`SH_RELEASE` 全树**只用在 `log.h:85`**（默认日志级别），所以正式包与玩家验证过的 `-Beta` 包行为一致，差别仅在默认级别为 `warn`。

### 11.8 版本号与第二次打包（同日 23:46）

`SH_VERSION` 提到 **`1.0-beta2`**（`scripthook.h:37`，单点定义；`README.md` 首行同步）。理由：这一版带的是"黑屏"的承重修法，与线上已有的 `1.0-beta1` 资产只是同名不同内容，日志首行若还写 beta1，玩家与我们都分不清手里是哪一份。随后：

- `build_msvc.ps1 -Release` 重新构建并**部署到当前游戏目录**（`dinput8.dll` 23:46:23，1,281,536 B；已核对产物内含 `1.0-beta2`、不含 `1.0-beta1`）；
- `tools/package-beta.ps1 -Zip` → **`out\GRW-ScriptHook-1.0-beta2.zip`**（1,587,684 B，23:46:36），线上资产用这个替换；
- 随包 `scripthook.ini` 带 `overlay=1`（已文档化），无 `LogLevel` 行（默认 `warn`），插件 8 个。
- 尚未同步的文档：`docs/beta-test-plan.md`（换机测试说明）与 `docs/release-post-1.0-beta1.md` 里仍写着 `1.0-beta1`、旧 zip 名与旧 tag —— 下一轮测试说明按 beta2 重写时一并更新。

### 11.9 遗留

1. **三处修复里"哪一处是致命一击"无法从日志单独判定**。机理与时间线上最可能是第 3 处（与 09-16 闪退同族、且它的日志证据最直接：route 1 之后游戏顺利完成前端加载），但第 1 处是客观缺陷、第 2 处是对"CPU≈0 阻塞"这一大类的加固，两条都保留。要彻底分清需要一次"只带其一"的对照实验，**不建议**为此再占用一轮玩家。
2. `build_msvc.ps1` 的 `-Release` 帮助文字写"diagnostics are compiled out"，与事实不符（`SH_RELEASE` 只决定默认级别）—— 顺手改。
3. 九节那条"换机判据：健康日志出现 MISMATCH…"在 11.6 之后才真正成立；此前那些行都在模块日志里，`warn` 下并不存在。**11.12 已补齐**：`LogFirst` 在地板上改为 `LOG_ALWAYS`，这九类模块"没匹配上/没读到"的行现在 warn 档也落在 `logs\scripthook.log`。
4. 本节的修复**未提交 git**（改动见 `git status`：`loader.c`、`proxy.def`、`scripthook_ovl.cpp`、`log.h`、`scripthook.ini`、`scripthook_playmode.c`、`scripthook_files.c`、`scripthook_forge.c`、`scripthook_forge_io.c`、`scripthook_dinput.c`、`scripthook_corefix.c`、`scripthook_blacklist.c`，新增 `tools/list-pe-exports.ps1`）。

### 11.10 本机复现（23:51）与真正的根因（同日 23:58 修）

**现象**：本机（`graphicstatedump.txt` 自报 GPU 为 AMD Radeon RX 5700）用 23:46 部署的那一版启动 —— 走到主窗口加载阶段黑屏、进程有明显占用、约 17 秒后游戏自己走崩溃上报退出。

**当时的旁证**：

| 证据 | 说明 |
| --- | --- |
| `graphicstatedump.txt`（游戏根目录，23:51:54） | **引擎自己的**渲染器状态转储，11 个设备上下文列表（Main + Deferred 0–10）**全空** = 渲染器根本没建起来。它写在覆盖层最后一行日志的**同一秒** |
| `logs\client_crash_reporter.txt`（0 B，23:52:11）、`dxdiag.txt`（103 KB，23:52:20） | 游戏侧的崩溃上报流程跑了一遍 |
| 系统事件日志 | **无** Application Error / AppHang，**无** TDR 事件 → 不是驱动重置，也不是未处理异常，是游戏自己判定渲染器失败后退出（与 `logs\` 里没有 `scripthook_crash.log` 一致） |
| `logs\scripthook.log` 末行（23:51:53.939） | `overlay: no swapchain captured yet - waiting 20000 ms before the probe device (fallback)` —— 之后再无覆盖层行；探针兜底还在睡觉，游戏就已经倒了 |

**根因：route 1 的两条补丁打在同一张 vtable 上，而钩子查找取到了没有原函数的那条记录。**

- 实测（离线小程序，CreateDXGIFactory1 取 vtable 后比对，用完即删）：`IDXGIFactory`、`IDXGIFactory1`、`IDXGIFactory2` 三个接口返回**同一个** vtable 指针；本机 23:24 那次自己的日志也写着 `factory capture installed (2 vtable(s))` —— 一个数组、两条记录。
- 旧代码"一次调用一条记录"，`HookForVtable` 返回**第一条**匹配 vtable 的记录：槽 10 那条（`origForHwnd = NULL`）排在前面 → **任何 `CreateSwapChainForHwnd` 调用都走进没有原函数的记录，直接返回 `E_FAIL`，从不调用真正的实现**；而失败路径**完全静默**（只有成功才写日志）。同一份小程序还证明 `D3D11CreateDeviceAndSwapChain` 与 `IDXGIFactory2::CreateSwapChainForHwnd` 两条路**都会**进到我们的钩子里 —— 即 route 1 这条路本身是通的，坏的只是查找与失败处理。
- 现代 D3D11 游戏建主交换链走的正是 `CreateSwapChainForHwnd`（flip model 只有这一条），于是引擎拿不到交换链 → 上下文全空的 `graphicstatedump` → 黑屏、空转、然后退出。23:24 那次为什么没事：那次游戏的主交换链走的是槽 10（日志 `swapchain captured (CreateSwapChain)`），槽 10 的记录是对的。
- **第二条独立缺陷**，证据就在同一份日志里：`scripthook.log` 第 8 行只剩片段 `d while it is up`，正是 `overlay: factory capture installed (...) - ... no probe device is created while it is up` 的**尾巴**。原因是 loader 用 `"w"` 打开 `scripthook.log`（私有写位置），而别的单元按追加写同一文件 —— loader 下一次写入把那几行追加的内容**盖掉了**。当次最关键的一行（走了哪条捕获路线）就是这样变成碎片的。

**修复与验证**：

- `scripthook_ovl.cpp`：工厂记录改为**一张 vtable 一条**、每个槽只打一次补丁；钩子查找要求"这条记录能交出我要调的那个原函数"，交不出时经 `FactoryCallRefused()` **大声写日志**而不是静默 `E_FAIL`；`CreateSwapChainForHwnd` 的捕获行升为地板级并带上尺寸/格式/缓冲数；表满、vtable 不可写也各有日志。
- `log.h`：`scripthook.log` 改为"先用 `"w"` 截断、随即以 `"a"` 打开"（`LogOpen()`），所有写者统一为追加语义 → 不再互相覆盖。
- 重新构建 `-Release` 并部署（`dinput8.dll` 23:58，1,285,632 B，含 `1.0-beta2`），`tools/package-beta.ps1 -Zip` 重打 **`out\GRW-ScriptHook-1.0-beta2.zip`**（1,589,342 B，00:00:49）。**23:46 那一版 beta2（部署的与打进 zip 的）都带这个缺陷，不能发给玩家；线上资产要用 00:00 这一版替换。**
- **本节结论当晚就被推翻，见 11.11**：工厂记录的缺陷是**真的**、也确实该修（它就摆在那里，任一台机器只要走 `CreateSwapChainForHwnd` 就会被拒），但它**不是这次黑屏的原因** —— 修完之后同一台机器四次运行仍然黑屏，覆盖层那一层也被 `overlay=0` 排除。真正的根因是 11.4 第 2 处的启动时机，见下。

### 11.11 根因：框架启动时机（当日 00:29 修，00:28 运行验证）

**根因一句话**：11.4 第 2 处把框架启动从 `DllMain` 挪到了"宿主第一次调用我们导出函数"的那一刻。本机宿主第一次 `DirectInput8Create` 在 DLL 加载后**约 6 秒**，那时引擎的渲染器和它自己的线程**已经在跑**；于是 18 个文件钩子、工厂 vtable 补丁、playmode/fpx 对游戏代码的补丁、8 个插件 DLL 全都落在**一个活着的进程**上，而 18:52 构建（在 attach 路径装完）在同一台机器、同一游戏构建、同一配置下正常。

**四轮对照**（本机，AMD RX 5700，游戏 `GRW.exe` 为 2026-09-15 的 TU25）：

| 轮次 | 构建 | 结果 |
| --- | --- | --- |
| 1 | 23:58（beta2 + 11.10 的修复） | 黑屏（三次运行同一形状） |
| 2 | 同上 + `[loader] overlay=0` | **仍黑屏** → 覆盖层（route 1 / 选窗 / 子类化）洗清 |
| 3 | 18:52:53（今晚改动之前的构建，取自 `out\GRW-ScriptHook-1.0-beta1-testkit.zip`，核对过 DLL 内构建字符串） | **正常进游戏**，F4 菜单可用 |
| 4 | 23:58 的代码 + 启动时机回到 attach 路径 | **正常进游戏** |

**旁证（失败轮 vs 成功轮的差别）**：

- 失败轮 `config loaded` 在 DLL 加载后 6 秒、splash 窗口**已可见之后**；18:52 轮与第 4 轮都在 `DllMain` 的同一毫秒。
- 失败轮里游戏在根目录写下 `graphicstatedump.txt`（11 个设备上下文列表全空）与 `dxdiag.txt`（晚约 15 秒）；**这两份文件在跑通的轮次里根本没有被写** —— 它们确实是引擎渲染器初始化失败时留下的记录，不是常规启动产物（11.10 里对它们的读法是对的，但**触发原因**判断错了）。
- 系统事件日志四轮都没有 Application Error / AppHang / TDR，也没有 `scripthook_crash.log`：不是崩溃，是渲染器起不来之后游戏自己走上报并退出。

**修复（"两全"）**：安装必须**早**（回到 attach 路径），但**加载不能在 `DllMain` 里做**（嵌套 `LoadLibrary` 正是 11.4 要规避的死锁成因）。分工：

- `DllMain`：`ShCrashStartup()` / `ShCoreFixStartup()` / **`ShFrameworkStartEarly()`** —— 只创建框架线程、**不等待**（该线程的第一件事就是 `LoadLibrary`，在我们自己的 `DllMain` 返回前拿不到 loader 锁，等它等于和自己死锁）；
- 框架线程 `FrameworkThread()`：`LoadRealDinput8()` → 发布"就绪" → 执行 `LoaderThread()` 正文（配置、状态、文件层、playmode/fpx、插件、黑名单）；
- 每个导出：`ShFrameworkStart()` 先确保线程已起（导出被先调用时兜底），再等"就绪"（30 秒上限，超时写一行）—— 宿主线程先到也不会被塞空指针。

**目击验证**（本机 00:28:55–00:29:20，`1.0-beta2` / built `Sep 18 2026 00:27:27`）：

```
00:28:55.576 GRW ScriptHook 1.0-beta2
00:28:55.579 loaded real dinput8.dll from C:\Windows\system32\dinput8.dll   ← 与版本行同一瞬间
00:28:55.579 config loaded from scripthook.ini
00:28:56.255 plugin scan done: 8 loaded, 0 skipped
00:28:56.657 overlay: factory capture installed (1 vtable(s))
00:29:12.770 hooks installed (CreateSwapChain): origPresent=7ffeef8787e0 ok=1
00:29:12.770 swapchain captured (CreateSwapChain): hwnd=8a02c6 buffer 1920x1080 windowed=1   ← route 1 抓到游戏自己的交换链，未建探针设备
00:29:13.346 imgui ready: hwnd=8a02c6
00:29:20.110 corefix: the main menu is up (state MenuOrLobby)
```

早装之后 route 1 **第一次真正生效**（此前每一轮都是 `no swapchain captured yet` → 只能退探针）：我们的工厂补丁比游戏创建交换链早约 16 秒落地，于是无需第二个 D3D11 设备就挂上了 `Present`。这也说明"探针设备竞态"这类推测在本机并不成立 —— 真正的问题一直是**安装时机**。

**收尾**：`out\GRW-ScriptHook-1.0-beta2.zip`（1,589,440 B，00:29:58）已用这一版重打；随包 `scripthook.ini` 为 `overlay=1`、`[forgemod] enabled=0`、无 `LogLevel`（= `warn`）。

### 11.12 最后一处盲区：`LogFirst` 在地板上提升为 `LOG_ALWAYS`（当日 00:36）

**问题**：`LogFirst` 是每个模块"我要说的第一件事" —— 站点校验通过、或者**字节签名与游戏版本不符而拒绝挂载**（`entity`、`havok`、`stealth`、`input`、`fov`、`blur`、`hit`、`npc`、`reflect` 九个模块都用它）。它走的却是 `LOG_INFO`，而 11.6 的地板只保留 `LOG_ALWAYS` —— 于是**发布包（warn）里"模块因为版本不符没挂上"和"根本没人问过"仍然无法区分**。九节写的换机判据（"看健康日志里的 MISMATCH"）在这条修掉之前，对玩家手上的包并不成立。

**改法**（`log.h`）：`LogFirstNow()` 改走 `LogAt(g_logFloor ? LOG_ALWAYS : LOG_INFO, …)` —— 模块自己的日志存在时（info/debug）行为一字不变；被级别丢掉、整个单元挂在地板上时，那行就去 `logs\scripthook.log`。每个调用点本来就是"一次一处"（11.6 已把它做成 per-call-site 的 static 守卫），所以地板最多多出十几行，代价可以忽略。

**验证**：本机 ini 已处于 `LogLevel=warn`（随包默认，也是玩家手上那一档），下一次启动就是玩家视角验收 —— 预期在 `logs\scripthook.log` 里能看到这九个模块各自的那一行（通过或拒绝都会写），而不是只有加载器的三行。

**已重打**：`out\GRW-ScriptHook-1.0-beta2.zip`（1,589,551 B，00:36:44），部署与包内 `dinput8.dll` 均为 `built Sep 18 2026 00:36:20`。

## 十二、第二份现场日志：另一台全新机器 4.5 小时（2026-09-18 夜 → 09-19 读）

**来源**：另一台全新安装游戏的机器（游戏在 `G:\`），一场 `19:14:46 → 23:45:49`（约 4.5 小时）的会话，`LogLevel=debug`，8 个插件全部加载。目录 32 个文件逐个看过。

**健康项**（这四项不用再动）：

| 项 | 证据 |
| --- | --- |
| **零崩溃** | 目录里**没有** `scripthook_crash.log` —— 09-18 傍晚那次「进房闪退」的修复在这台机器上站住了 |
| **模块日志零错误行** | 全目录按 `FAILED/refused/MISMATCH/failed/not found/BAD/cannot/missing/error` 扫，**一处都没有**（`resolve: no player position` 与 `headptr:` 两类是已知状态行，不是失败） |
| **每帧开销** | `scripthook_ovl.log`：`ours` 最大 **3 µs**；4.5 小时 24 次 >100 ms 卡顿全是游戏侧（`file` 计数高、`decide` <2 ms） |
| **载具派遣** | `warm: done in 1000 ms`；9 次派遣全部 **20–40 ms** —— 「越召唤越慢」未复现（§4.4-23 之后的形态） |
| **Forge 侧载** | 3 mod **3 applied / 0 rejected**；索引 301,945 个 id / 23 归档 |
| **上一场留档** | `scripthook.log.prev` 在（§11 之后加的那条机制生效） |

**两条真问题**：一条是**新政策放大出来的**（插件日志现在按 `LogInitAlways` 照写，插件内部一处不当日志就此变成磁盘量），另一条是**同一症状的根**。

### 12.1 `TimeWeatherControl` 日志风暴：64,498 行 / 2.5 MB

**现象**：`plugins\TimeWeatherControl\TimeWeatherControl.log` 2.5 MB / 64,503 行，其中 **64,498 行是同一句** `handed back (switch off)`，尾行间隔正好 250 ms。

**根因**（一行位置错）：tick 线程里那句 `gaveBack = 0;` 被放在**开关判断之前**，于是 250 ms 一轮的循环每轮都把「已经交还过」这个记忆抹掉，下面 `if (!gaveBack)` 的「只做一次」守卫永远为真 —— 会话里 `enabled=0`（功能没开）就每 250 ms 写一行。算术印证：4.5 h ÷ 250 ms = 64,800 ≈ 实测 64,498。

**为什么必须发版前修**：插件日志现在是「除 `none` 外一律照写」，**玩家包里也会建这个文件** —— 每个加载了该插件却没用它的玩家，一场游戏白写 2.5 MB，挂机时持续增长。

**改法**：删掉那句提前的清零（保留驱动路径里那一处，那里才等于「又有东西要交还了」）。改后：功能没开过就一行都不写；真的从「在驱动」翻到「关掉」时仍只写一行。

### 12.2 第一人称的眨眼：环的戳、世代与粘滞保持

**现象**：`scripthook_fpx.log` 里 **97 次「选中值变化」**，第 1 次（19:15:40）成功（距离 0.00 m），此后**每一次被记录到的变化都是掉成 `0`**；奇数次的「回来」落在 250 ms 节流窗内没被打印 —— 也就是**约 48 次「掉一下又回来」**。`scripthook_api.log` 里 `resolve: no player position` **60+ 次**，时间点与这些抖动一一对应。

**这是同一症状的根**：旧版（12 m 半径、无粘滞）在**同一时刻**会把这个空窗交给「最近的捕获」，也就是**队友的头** —— 现场报的「闪现队友视角」；半径收到 4 m 之后不再落到队友身上，但空窗本身还在，表现从「闪到队友」变成「闪一下引擎镜头」。

**三条错在同一处：把「环里这一帧有没有」当成了「能不能用」。**

| # | 位置 | 问题 | 改法 |
| --- | --- | --- | --- |
| 1 | `RING_FRESH = 120`（帧数） | 环是**引擎自己的每角色调用**填的，不是我们按帧填的；戳却按帧计。于是一个引擎两秒前写入、完全可用的条目被判过期 → 掉出选取，而且**永不恢复**（现场日志里 19:15 之后再没选中过任何东西） | 戳改成**毫秒**（`GetTickCount64()`，剪到低 40 位），窗口放到 `RING_FRESH_MS = 5000`；stub 只读一个词、原样存，引擎路径一字未改 |
| 2 | `Fp2Forget` 清空 256 个槽 | 清空后要等引擎下一次跑该角色的头调用才填回来，那段时间就是空窗 = 眨眼 | 改为**换世代**：戳的高位是世代号，`Fp2Forget` 只把世代 +1，旧条目一次性全部作废（不选它），槽留给引擎下一次写入。**跨世界禁用旧捕获的语义一字不变**，改的只是执行方式 |
| 3 | 空窗直接 `arg = 0` | 一帧没有条目就等于把镜头还给引擎 | 加**粘滞保持** `g_pickHold`（成功选中就记住，含 a2/a3 与毫秒戳）：空窗帧若「同一世代 + ≤1500 ms + `HeadTransform` 仍解析」就继续用它。三道闸同时成立才用，所以仍不可能跨越世界更换；每次使用计数并节流写一行 `pick: ring had nothing, kept the last capture (N time(s) so far)` —— **这一行就是下一份现场日志里判断本次修复是否真的消掉了眨眼的判据** |

顺带把 `RingTrace` 的每条目改成写「为什么没选中」（`retired` / `stale(Nms)` / 坐标 / `-`），下次不必再从零推。

**遗留（本次未动，非同一原因）**：`ShGetPlayerPosition` 那一类空窗（现场 60+ 次）走的是另一条分支 —— 位置读不到时**立即 `Fp2Forget` 并放弃本帧**，这是 §11 那次崩溃的防护，本次刻意不动它。**所以下一次现场日志里如果还有眨眼，看两类行分辨**：有 `kept the last capture` 说明是环的空窗（本次已修）；只有 `pick: 0 (me UNKNOWN …)` 说明是位置读取空窗（另一条设计题，可选做法是「保持上一帧的摆位矩阵几百毫秒、不做任何引擎调用」，风险为零，但属新决定）。

### 12.3 第一版修法被联机复测推翻（2026-09-19 白天）

**复测**（本机，03:47:35 → 03:55:56，约 8 分钟联机，`LogLevel=debug`）：

| 项 | 结果 |
| --- | --- |
| `TimeWeatherControl.log` | **5 行 / 313 B** ✓ —— 12.1 的修复成立（此前同一会话长度会写 64,498 行）✓ |
| 崩溃日志 | 最后写入 **09-18 15:28** ✓ —— 本场没有新崩溃 ✓ |
| `fpx` | **7 次选中值变化** ✗，且 **5 次环快照全部是 `retired`** ✗ |

**成因（12.2 表格第 2、3 条的做法错了）**：`Fp2Forget()` 被挂在「**世界不在 live**」上，而 live 为假包括**暂停、地图、加载**——这是联机里最常发生的事 ✗。每帧都作废一次世代，于是：

- 环里的条目**只要引擎写过就立刻被作废** ✗；
- 而引擎写一个槽只在「它为某个角色跑一次头调用」时发生（**每角色一次，不是每帧**）✗ —— 现场日志里 03:52:43 之后**两分半没有新写入** ✗；
- 结果是环长期全 `retired` ✗，选取失败，镜头交还引擎**并以分钟计** ✗（`pick:` 只在变化时写行，所以「一直是 0」在两行之间是不出声的 ✗）。

也就是说：**我修的是「作废的方式」，没修「作废得太频繁」** ✗ —— 而频次才是根 ✓。

**第二版改法（三条，均按 09-19 现场数据定的阈值）**：

| # | 改成什么 | 依据 |
| --- | --- | --- |
| 1 | 「不在 live」只调 **`Fp2DropRemembered()`**（丢记住的指针），**不碰环** ✓ | 暂停与地图占了「不在 live」的绝大多数，而环是稀缺资源（引擎每角色写一次）✗→✓ |
| 2 | 只有**加载屏**（`ShGetUiState() & SH_UI_LOADING`）才算「世界被更换」✓：在出去时置 `g_worldSuspect`，**回到 live 的第一帧**才 `Fp2Forget()` ✓ | 暂停/地图不动世界 ✓，加载屏才是世界被替换 ✓；这是一条比「不在 live」精确得多的信号 ✓ |
| 3 | 位置读不到：**短抖用粘滞保持兜住**（`NO_POS_GRACE_MS = 500` ✓，写一行 `pick: no position this frame, kept the last capture (N)` ✓），**长于 500 ms 才 `Fp2Forget()`** ✓（写一行「世界正在被替换，忘记本场」✓）| 本场 8 分钟内 5 次位置空窗、其中 4 次「下一次 trace 就好了」✓ = 短抖 ✗；而 §11 那次崩溃是空窗**两秒后**才发出调用 ✓ —— 500 ms 覆盖抖动、只到崩溃窗口的四分之一 ✓ |

顺带两条：粘滞保持现在**不再刷新自己的时间戳** ✗→✓（否则反复短抖会让同一个捕获永生 ✓，而它必须从**最后一次真实选中**起算 ✓）；`RingTrace` 的 `retired` 也带上毫秒（`retired(1234ms)` ✓），下次一眼能看出被作废的条目到底多新 ✓。

**下一场联机要看的判据**（写进测试清单第 20 条）：
1. `TimeWeatherControl.log` 仍是几行 ✓；
2. `pick:` 变化行应**极少** ❏（本场 8 分钟 7 次 ✗，目标：一场下来个位数 ✓）；
3. `ring p:` 里**不应再整行 `retired`** ❏，应能看到坐标 ✓；
4. 出现 `no position this frame, kept the last capture (N)` = **短抖被兜住**（玩家看不到 ✓，N 是它救下来的次数 ✓）；
5. 出现「no player position for N ms - the world is being replaced」= 真的换了世界 ✓（一场几次属正常 ✓）；
6. `scripthook_crash.log` 仍**不该**有新条目 ✓。

### 12.4 真根因：戳写得太晚，开关又作废一次（2026-09-19 第二次联机复测）

**复测**（本机，04:03:54 → 04:22:25，约 19 分钟联机，跑的是 `built 04:00:31` 那版 ✓，部署 dll 与 `out\` 产物同哈希 ✓）：

| 项 | 结果 |
| --- | --- |
| `TimeWeatherControl` 这次**真用了**（开关、天气都点过 ✓） | **76 行 / 3,183 B** ✓ —— 12.1 的修复在「真用」路径下也成立 ✓（此前同类会话会到 2.5 MB） |
| 崩溃 ✓ | 崩溃日志最后写入仍是 **09-18 15:28** ✓，本场零条目 ✓ |
| 12.3 的两条新机制 ✓ | `pick: no position this frame, kept the last capture` **出现 2 次** ✓（短抖确实被兜住 ✓）；`pick` 变化 **6 次 / 19 分钟** ✓（上一场 7 次 / 8 分钟 ✓） |
| 但环仍不可用 ✗ | `ring p:` 四个槽**全 `retired`** ✗，且年龄是 **10,177,656 ms** 与 **10,953,578 ms** ✗ —— 两次相差 **775,922 ms**，正好等于两次 trace 的间隔 775.9 s ✓ |

**决定性证据（都在日志里，不必猜）**：

1. `args stub@048: … 4D 8B 1B 4D 89 9C D2 00 18 00 00 …` ✓ —— 写 `t` 的那条 `mov [r10+rdx*8+0x1800], r11` **就在 stub 里** ✓；两个 imm64（`&g_ring` 与 `&g_ringStamp`）相距正好 **0x2000** ✓ = 结构体大小 ✓ → 指令、偏移、地址全对 ✓；
2. 那么「retired 的年龄恰好等于开机时长」只剩一个解释 ✓：**`t` 全是 0** ✗ —— 这些槽**从来没人写过** ✓（进程刚起时数组是零 ✓，而 `a0` 有值 ✓ 是 stub 写的 ✓）；
3. 为什么 `t` 没被写 ✓：**`g_ringStamp` 第一次被写是在第一帧相机帧** ✗，而**引擎跑那个捕获点比它早** ✓ —— 世界构建期、玩家还没进世界的时候 ✓。于是引擎写的每一条都带 **stamp 0** ✓，而 0 的世代位是 0 ≠ 当前世代 ✓ → **整个环一整场都是「上一世代」** ✗；
4. 第二重打击 ✓：`ShFp2Enable(1)` 的上升沿（玩家一开第一人称 ✓）**又 `Fp2Forget()` 一次** ✓ —— 把刚能用的条目再作废一遍 ✓，而引擎**不会重写**（一个世界只写一次 ✓）✗。

**第三版改法（四条，全部以这份日志为据）**：

| # | 改成什么 | 依据 |
| --- | --- | --- |
| 1 | **`ShFp2Install` 里先 `RingStampNow()`** ✓（在任何 stub 能跑之前 ✓） | 引擎的写发生在相机帧之前 ✓；这样它写的每条都带**本场**的戳 ✓，`t` 不再可能是 0 ✓ |
| 2 | **世界检查提到 `!g_fp.want` 门之前** ✓（加载标记 ✓、世界不在 live ✓、以及开关关着时的漏检 ✓） | 开关关着时 placement 直接返回 ✗，而「开关关着时换了会话」过去正是靠 `ShFp2Enable` 那次作废兜的 ✗ |
| 3 | **`ShFp2Enable` 不再作废环** ✓（只丢记住的指针与粘滞值 ✓） | 环是「一个世界一次」的稀缺资源 ✓；真正的世界更换交给第 2 条 ✓ |
| 4 | **环条目不再按年龄判** ✓（`RING_FRESH_MS` 整个删掉 ✓），只按世代 ✓ | 引擎按「每角色一次」写 ✓，一小时的条目仍是本场的 ✓；**按年龄判正是 09-18 与 09-19 两次「眼睛再也不工作」的成因** ✗ |

`RingTrace` 的每条目现在都带真实年龄（`x,y,z@1234ms` ✓ / `retired(1234ms)` ✓），下一份日志能直接读出「这条是哪一秒写的」✓。

**下一场要看的**（测试清单第 20 条已同步）：
1. `ring p:` 里应看到**坐标 + 年龄** ❏，不再是整行 `retired` ✓；
2. `retired` 只在**真换世界之后**短暂出现 ❏（加载 / 长位置空窗 ✓）；
3. `pick` 变化行：十几分钟应**个位数** ✓；
4. 两类粘滞行按需出现 ✓（`ring had nothing` ✓ / `no position this frame` ✓）；
5. `scripthook_crash.log` 仍**无新条目** ✓。

### 12.5 提示条跟随菜单语言（2026-09-19）

**症状**（实机反馈）：第一人称开关的提示条**始终中文** ✗ —— 菜单语言切成 English 也不变 ✓。

**根因**：`firstperson` 的这两句是**硬编码中文** ✗：

```c
Say(SAY_FP_ON, "第一人称已开启（头部若未隐藏，请按右键瞄准或重切一次）", …);
Say(SAY_TP,    "第三人称已开启", …);
```

它们是该插件**最后两条不走自己文案表的字符串** ✗ —— 菜单行、状态行、`@fp.hint` 全都走 `kEn`/`kZh` ✓，只有提示条漏了 ✓。

**改法**：两句改成键 `@fp.say.on` / `@fp.say.off` ✓，写进 `kEn`/`kZh` ✓ 与随包的 `plugins\firstperson\lang.ini`（两节各两行 ✓，玩家可就地覆盖 ✓）；调用点走本插件已有的 `SetText()` ✓（可选绑定 `ShLangText` ✓，解析不到就回落到键名 ✓，与状态行同一套行为 ✓）。

**验收**：把菜单语言切成 English ✓，切一次第一人称 ✓ —— `logs\scripthook_hud.log` 那一行写的是**渲染后**的文本 ✓ → 应读作 `toast 1 "Third person on"` / `toast 1 "First person on (aim or toggle again if the head shows)"` ✓（不必截图就能验 ✓）。

> 英文长度（同日反馈）：提示条是窄条 ✓，英文原句 98 字符比中文（26 字 ≈ 52 列）长出近一倍 ✗ → 压到 **55 字符** ✓（`aim or toggle again if the head shows` ✓），与中文显示宽度同量级 ✓。`@fp.hint`（菜单页里那条长提示 ✓）保持完整句 ✗→✓ —— 菜单有整行可放 ✓，提示条没有 ✓。

**顺带记下（未动）**：`plugins/*/lang.ini` 里仍有一批**旧键** ✗（纯英文原文当键 ✓，如 `"Airplane" = "Airplane"` ✓）—— 即终审 `M6` 那条 ✓；它们不匹配任何代码键 ✓、只是白占行 ✓，要清就用 `docs/export-plugin-lang.ps1` 重生成 ✓。
