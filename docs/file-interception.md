# 文件 API 拦截层（file interception layer）

框架里**唯一一套**文件 API 钩子，以及给它登记规则的那些模块。

> **一句话边界**：这层负责"**把调用拦下来并按规则回答**"，不负责"**该不该拦**"——那是登记者的事。它不提供目录级虚拟化、不提供全局开关、不给插件真函数指针，也不替任何模块决定一条路径的语义。

## 一、为什么会有这一层

在这层之前，仓库里有**五套**各自为政的文件拦截：

| 模块 | 做法 | 它必须绕开的坑 |
| --- | --- | --- |
| `skipintro.c` | 手写 PE 解析，改 GRW.exe 导入表的四个槽位 | 只能影响主模块；还原时要检查槽位是否还是自己的 |
| `GhostNoWipe.c` | MinHook 挂 `MoveFileExW`/`MoveFileW`/`CreateFileW` | 与 `skipintro`、`forge_io` 抢 `CreateFileW` |
| `GhostWipeProbe.c` | MinHook 挂 12 个文件 API（只读取证） | 与 `GhostNoWipe` 抢 `MoveFileExW`，两者不能同时启用 |
| `scripthook_forge_io.c` | MinHook 挂 `ReadFile` 与完成路径 | 故意**不挂** `CreateFileW`，只能用 `GetFinalPathNameByHandleW` 事后反查句柄 |
| `scripthook_forgeprobe.c` | MinHook 挂 13 个文件 API（只读取证） | 与上一条争同一批目标 |

原因是一条硬约束：**MinHook 对同一个目标只允许一个钩子**（且状态按 DLL 分开）。谁先到谁拥有，其余模块只能改设计绕开——`forge_io` 的句柄反查、`skipintro` 的导入表补丁、两个探针的"二选一"都是这条约束的产物。

这一层把目标所有权**上收到框架 DLL 一处**：钩子只有一份，其余模块改成**登记规则**。

## 二、三种参与方式

```c
#include "scripthook.h"      /* 插件直接包含；框架模块先 #define SH_BUILD 1 */

static void OnAfter(ShFileCall *call, void *user) { /* 看，或改结果 */ }

ShFileRuleDesc d = {0};
d.name   = L"Nvidia.bk2";                 /* 文件名部分，大小写不敏感 */
d.group  = SH_FILE_ATTR | SH_FILE_OPEN;   /* 关心哪几类调用 */
d.action = SH_FILE_HIDE;                  /* 命中就回答"文件不存在" */
d.after  = OnAfter;                       /* 可选 */
d.user   = myState;
ShFileRule *r = ShFileRuleAdd(&d);        /* 0 = 被拒，原因在层自己的日志里 */
```

| 动作 | 命中的调用会怎样 |
| --- | --- |
| `SH_FILE_HIDE` | **不调用真函数**，直接回答"文件不存在"（`INVALID_FILE_ATTRIBUTES` / `INVALID_HANDLE_VALUE` / `FALSE` + `ERROR_FILE_NOT_FOUND`）。只能用于**带文件名**的组（open / attr / move / delete / find） |
| `SH_FILE_REDIRECT` | 用 `desc->to` 替换主路径后调用真函数 |
| `SH_FILE_DECIDE` | 把调用交给 `desc->before`：它**自己作答**（`call->result` / `call->error` + 返回 1），或返回 0 让层执行真实调用 |

**只读观察**就是一条只有 `after`、没有 `before` 的 `DECIDE` 规则：它看得到每一次调用（`call->api` 是 `"MoveFileExW"` 这样的真实名字），不改任何结果。若还想在结果出来后改（例如改写 `ReadFile` 收到的数据），也在 `after` 里做。

组的位（可或起来）：

```
SH_FILE_OPEN    CreateFileA/W
SH_FILE_ATTR    GetFileAttributesA/W/ExA/ExW
SH_FILE_MOVE    MoveFileA/W、MoveFileExA/W
SH_FILE_DELETE  DeleteFileA/W、RemoveDirectoryA/W
SH_FILE_FIND    FindFirstFileA/W/ExA/ExW、FindNextFileA/W
SH_FILE_READ    ReadFile、SetFilePointer(Ex)、GetFileSize(Ex)、
                CreateFileMappingA/W、MapViewOfFile、UnmapViewOfFile
SH_FILE_WAIT    完成路径：WaitFor*、GetOverlappedResult(Ex)、CloseHandle
SH_FILE_INFO    SetFileInformationByHandle、CopyFileA/W
SH_FILE_ANY     以上全部
```

## 三、多条规则命中同一次调用时

**隐藏 > 重定向 > 决定**，同一级别按**登记顺序**（先登记者优先）。

- 隐藏排第一是刻意的：**"文件不存在"是唯一不会把错内容交给调用者的答案**；
- **决定不是"一个赢家"**：多条 `DECIDE` 命中时，层会按登记顺序**依次询问每个带 `before` 的决定者**，第一个作答者结束这次判定。所以一个只观察的规则（没有 `before`）**挡不住**另一个模块的决定逻辑——这条顺序在 `GhostNoWipe` 这类模块上是功能性的，不是风格问题；
- 无论谁答的，所有带 `after` 的匹配规则都会在**结果出来之后**跑一遍（包括被隐藏的调用：`after` 能看到它，但隐藏的答案不会被改写）。

## 四、生命周期

- **第一条规则登记时**才装钩子（只装被需要的那些目标，逐目标 `MH_CreateHook` + `MH_EnableHook`）；后续规则登记只补装自己需要的新目标；
- **注销规则**：`ShFileRuleDel(r)`；**最后一条**注销后，层把全部目标 `MH_DisableHook` + `MH_RemoveHook`，回到"零钩子、零 trampoline"，日志记一行；
- 所以一个**什么规则都没登记**的会话，文件调用一个都不经过这层；这也让一次性用户能守约：`skipintro` 拦满四个片名（或进过主菜单）后注销自己的四条规则，层随即把自己摘掉；
- 规则表**定长 32 条**，槽位**永不释放**（只复用）：登记时可以放心把路径字符串的地址交给层——它拷一份；注销后另一条规则可能占用同一槽位，这是唯一的"陈旧值"风险（代价远小于在文件调用里释放内存）。

## 五、回调契约（写代码前请读完）

- 回调在**调用者线程**上、**在那次文件调用里面**执行，脚下的栈是调用者的：**别阻塞、别等另一个需要同线程做文件调用的东西**；
- 回调里**可以调用任何文件 API**（层认得出自己的线程，直接放行到真函数）：`GhostNoWipe` 就是靠这个在回调里 `CreateFileW` 打开改道后的目标。但**不要指望自己的规则作用于这些调用**——它们不会，这是刻意的，否则"从规则里看一眼磁盘的真实状态"就不可能；
- 需要把整段工作标记成"这是我自己的 I/O、谁都别看"（例如加载器读归档来服务读取）：`ShFileOwn(1)` / `ShFileOwn(0)`，**逐线程、不嵌套**；
- 回调里**不要写日志到同一行**（会刷屏）：层的日志只在登记、注销、装钩/卸钩，以及每条规则的**前三次命中**记录。

## 六、它代答了谁

钩子是**全进程内联**的：游戏自己的调用、其它插件的调用、框架自身的调用，全都经过同一次判定——**包括通过 `GetProcAddress` 拿到的函数指针**（`skipintro` 旧的导入表补丁做不到这一点）。

代价与对策：

- 框架自己的 I/O（`log.h` 的日志、`scripthook_config.c` 的 ini、`scripthook_crash.c` 的崩溃写盘、`forge.c` 的归档读取）也会走一遍：它们不匹配任何规则，扫描后原样透传；`ShFileOwn` 用于"我自己的读取不许被自己的规则看见"；
- 不装规则时热路径只有"线程局部读 + 两个计数器 + 一次位测试"；
- 层内**绝不**用 `MH_EnableHook(MH_ALL_HOOKS)`：那会把别的模块刻意创建但尚未启用的钩子一并打开。

## 七、现状：谁在登记

| 模块 | 规则 | 说明 |
| --- | --- | --- |
| `scripthook_forge_io.c` | 3 条 `DECIDE`（open / read / wait），只观察 | open 记句柄→路径（替代 `GetFinalPathNameByHandleW` 事后反查，后者保留为兜底）；read 在数据到齐后改写被 mod 覆盖的字节；wait 收尾异步读 |
| `scripthook_forgeprobe.c` | 1 条观察者（open / read / find） | `[forgemod] probe=1` 时才登记 |
| `skipintro.asi` | 4 条 `HIDE`（每个片名一条），带 `after` 计数 | 开关实时增删规则；拦满启用集合或进过主菜单即注销全部 |
| `GhostNoWipe.asi` | 1 条 `DECIDE`（move / open），before + after | 死亡时的"改名"被丢弃并回答成功；`.tmp` 换内容后再让真实调用通过 |
| `GhostWipeProbe.asi` | 1 条观察者（6 个组） | 只记录存档目录上的操作，含引擎状态与调用栈 |

## 八、自测

1. 构建部署（`pwsh ./build_msvc.ps1`）；
2. 进一次游戏，看 `logs\scripthook_files.log`：应当有 `--- file interception layer ---`、每个参与者的 `rule N (<owner>) ...`、装钩数量变化的 `N of 39 target(s) hooked`，以及命中行 `rule N (skipintro) hid ... - hit 1`；
3. 一次性用户的释放：`rule N (...) is out after ...` 逐条出现，落点数量随之下降；
4. **没有任何规则**时（把插件与开关全关）该日志根本不会生成——那才是"零钩子"的证据。

## 九、排错

| 现象 | 先看哪儿 |
| --- | --- |
| 游戏加载期卡住/闪退 | 把 `plugins\` 改名跑一次：没有插件=没有规则=零钩子。能进说明是某个迁移后的插件，再看层日志的最后一行 |
| 某条规则没生效 | 层日志里有没有 `rule N (...)`：没有就是被拒（原因也记在那里：组为空、重定向没写目标、决定既没 before 也没 after、名单超长、槽位满 32） |
| 隐藏"时灵时不灵" | 规则表按**登记顺序**裁决同级；检查是否有更早登记的 `DECIDE` 规则在你之前作答（它不是赢家，而是被**先问**） |
| 某次调用看不到 | `SH_FILE_*` 组没覆盖该 API，或该调用发生在 `ShFileOwn(1)` 的线程上，或由回调自己发出（这两类一律直通） |
| 卸载不彻底 | 层只在自己是最后一条规则时才整套下线：`ShFileMatchCount()` 应为 0，`ShFileInstalled()` 应为 0 |

## 十、明确不做的事

- **没有优先级字段**：三个动作 + 登记顺序就是全部规则；
- **没有目录虚拟化**：规则只回答单次调用，不重写视图；
- **不把真函数给插件**：回调只能"作答"或"放行"，拿不到 trampoline；
- **不提供全局开关**：一条规则就是一次登记，两个写者抢一个答案正是这层要消灭的东西；
- **不做"只对主模块生效"的模式**：钩子是全进程的（这也是它相对导入表补丁的全部价值）。
