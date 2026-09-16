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
| 发布前必修 | **11** |
| 建议修 | **24** |
| 已知限制 / 可接受 | **12** |

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
| 日志与崩溃转储 | 目录固定 `<gamedir>\logs`；普通日志每次启动截断；**崩溃日志追加且从不截断、全仓无轮转**；崩溃报告不含用户名/机器名/完整路径 | 必修（决定日志开关策略） |
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
| 3.7 版本标识 | `scripthook.h:31-37`、`loader.c:275`、`scripthook_crash.c:96`、`README.md:1` | 新增 `SH_VERSION "1.0.0-beta1"` 单点定义，四处引用同一串 |
| 3.8–3.10 日志策略 | `scripthook_crash.c:20-28` + `Emit` | 按决定保留全量诊断；崩溃日志加 512 KB 上限，超限轮转并在新文件写下构建信息 |
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

### 仍未处理

- 第五节 12 条已知限制按定义不修。
- 第六节**实机验证清单 12 条尚未执行**（需进游戏）。

### 覆盖度声明更正

第八节结尾原写「本次未修改任何文件」——那是**审计阶段**的事实。定稿后按指示执行了上表的必修修复；完整改动清单以 `git status` 为准，未提交。
