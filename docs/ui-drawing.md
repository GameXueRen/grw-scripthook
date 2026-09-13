# 绘制公共 API（插件自绘 UI）

框架把「画界面」这件事做成了一套公共 API：任何插件都能注册一个每帧回调，在自己的窗口里用一组绘图原语画自己的界面 —— 与 F4 菜单同一套字体、配色与分辨率缩放，不再只能把功能塞进框架菜单。

本文是这份 API 的说明书；可运行的最小例子是 **`draw_sample.c`**（`plugins\draw_sample\`），它演示两个 drawer（带标题栏的窗口 + 无边框角落常驻面板）和输入框。

---

## 一、声明

- 这一层**只负责画**：它不读游戏内存、不碰游戏对象、不改游戏状态。插件想画什么、什么时候画，全由插件自己决定。
- **ImGui 不进插件 ABI**。原语是一组 **C ABI** 函数（`ShDraw*`，`scripthook.h` 的 `@defgroup draw`），插件是普通 C DLL，链接 `libscripthook` 或 `GetProcAddress` 都行，不需要 C++、不需要 ImGui 头。
- **两条构建链都编得动**。原语真正的实现是 ImGui（在 overlay 里），而 overlay 是 MSVC 专属；MinGW 链里没有 overlay，这时**所有原语是安全的空操作**、`ShDrawReady()` 返回 0。同一份插件源码在两条链下都能加载，插件不需要为构建方式分叉。
- **没人注册就没有任何开销**：渲染线程每帧只多做一次「注册表里有没有人」的判断。

最短的用法就是三行：

```c
static void OnDraw(void *user) {
    ShDrawText("hello from a plugin");
}

ShDrawAdd("my_window", OnDraw, NULL);   /* 注册，名字就是窗口标题 */
/* … 某处不再需要时： */
ShDrawDel("my_window");
```

---

## 二、插件要做的三件事

### 1) 注册一个 callback

```c
SH_API int ShDrawAdd   (const char *name, ShDrawFn fn, void *user);
SH_API int ShDrawAddEx (const char *name, ShDrawFn fn, void *user,
                        const ShDrawOpts *opts);
```

- `name` 既是**窗口标题**，也是注销时的键；最长 47 字节。
- `fn` 每帧被调一次，跑在**渲染线程**上，包在一个以 `name` 为标题的 ImGui 窗口里。
- 用同名再注册一次是**替换回调**，不会占两个槽位（插件重载自己的回调时不会漏槽）。
- 最多 **16 个** drawer；满了一个会**记日志并返回 0**，不会静默退化。
- `opts` 决定窗口形态（`ShDrawOpts`）：

| flags | 含义 |
| --- | --- |
| `SH_DRAW_FRAMELESS` | 无标题栏、无背景、随内容自适应大小：**HUD 式常驻面板**与框架自己的聊天框都是这个形态 |
| `SH_DRAW_NO_MOVE` | 保留标题栏，但位置钉死 |
| `SH_DRAW_NO_RESIZE` | 保留标题栏，但尺寸钉死 |

不带 flag 就是普通窗口：可拖动、可折叠、可关闭，位置与尺寸**本次会话内**被 ImGui 记住（框架刻意不落 `imgui.ini` 到磁盘）。

### 2) 在回调里画

回调里只能调原语；原语画在**当前光标处**，与 ImGui 的习惯一致（`ShDrawSpacing` / `ShDrawSameLine` / `ShDrawSeparator` 控制排布）。完整清单见第四节。

### 3) 不需要时注销

```c
SH_API int ShDrawDel(const char *name);            /* 连窗口一起消失 */
SH_API int ShDrawShow(const char *name, int on);   /* 只是隐藏/显示，不注销 */
SH_API int ShDrawShown(const char *name);
```

用户点窗口自己的关闭按钮 = **隐藏**（`ShDrawShown` 变为 0，回调不再被调用），用 `ShDrawShow(name, 1)` 就能叫回来；真正不再需要时用 `ShDrawDel`。注销可以在回调内部调用，登记的 drawer 从下一帧开始。

---

## 三、线程、生命周期与边界

| 事项 | 约定 |
| --- | --- |
| 回调线程 | **渲染线程**，每帧一次，在游戏 `Present` 路径上 |
| 回调里**不能**做的事 | 阻塞（sleep、等别的线程、文件 I/O、弹窗）—— 卡住它就是卡住游戏帧；能做的只有画 |
| 注册 / 注销 / 查询 / `ShDrawInput*` | **任意线程**都可以调用 |
| overlay 没起来时 | 回调一次都不会被调用；原语是空操作 |
| 在回调外调原语 | 忽略 + 日志里记一行（`scripthook_ovl.log`），不会崩 |
| 位置与尺寸 | 本次会话内记住；换分辨率时尺寸按同一缩放比重新计算 |
| 绘图缩放 | `ShDrawScale()` 返回与菜单相同的缩放（`[Settings] MenuScale`，0 = 按 1080p 自动），插件自己算像素时用它 |
| 字体 | `ShDrawPushFont(SH_DRAW_FONT_DEFAULT / SH_DRAW_FONT_BOLD)`，框架已按缩放给好字号，插件不用管字号 |
| 颜色 | `ShDrawTextColored` / `ShDrawPanel` / `ShDrawRect` 用 `0xRRGGBB` + alpha；命名常量 `SH_DRAW_COL_TEXT / DIM / HI / WARN / GOOD / PANEL` 就是菜单那套配色 |
| 崩溃隔离 | **没有**。回调跑在渲染线程上，插件在回调里越界写内存就是游戏崩。这一层给的是能力和约定，不是沙箱 |
| 鼠标 | 有 drawer 时，窗口的鼠标消息会被喂给 ImGui，插件的按钮/开关/滑块点得动；鼠标消息**不会**被吞掉（游戏读的是 DirectInput，不依赖窗口鼠标消息） |

---

## 四、原语清单

| 原语 | 说明 |
| --- | --- |
| `ShDrawText` / `ShDrawTextColored` / `ShDrawTextWrapped` | 一行 / 带色 / 自动换行的文本 |
| `ShDrawHint` | 小一号的灰绿色提示行（与菜单的提示行同款） |
| `ShDrawSpacing` / `ShDrawSameLine` / `ShDrawSeparator` | 空行 / 与上一个控件同行 / 分隔线 |
| `ShDrawPanel` / `ShDrawRect` | 指定尺寸的圆角面板 / 直角矩形（会占位，光标越过它） |
| `ShDrawButton` | 按钮，点击那一帧返回 1 |
| `ShDrawToggle` | 复选框，直接绑定 `int *`（非 0 为开），翻转那一帧返回 1 |
| `ShDrawNumber` | 整数输入框，带 ± 步进并夹在 `[mn, mx]` |
| `ShDrawSlider` | 浮点滑块，夹在 `[mn, mx]` |
| `ShDrawList` | 下拉列表（`const char *const items[]` + 数量），选择变化返回 1 |
| `ShDrawPushFont` / `ShDrawPopFont` | 默认字体 / 加粗 CJK 字体；回调结束时框架会把没配平的 push 收干净 |
| `ShDrawScale` | 当前 UI 缩放 |
| **输入框** | 见下一节 |

---

## 五、输入框：谁负责什么

这是全篇最需要读清楚的一节，因为职责是**故意**这样切的，而且由游戏的输入模型决定：

游戏自己的窗口是系统输入法**唯一**能把成字送进来的地方（文本以 `WM_CHAR` / `WM_IME_CHAR` 到达），而中文输入法还要有人管会话（组合串、候选列表、候选窗定位）。所以：

| 谁 | 负责 |
| --- | --- |
| **框架** | 输入框的外观（面板 / 文本 / 光标 / 组合串 / 候选列表 / 提示行）、焦点、分辨率缩放、**输入法会话**（启用、组合串、候选列表、候选窗与系统光标的锚点 —— 按**被聚焦输入框画出来的矩形**定位）、**收集字符**（把窗口收到的 `WM_CHAR`/`WM_IME_CHAR` 排进队列）、窗口消息的吞键（防止游戏自己的聊天框看到你输的字） |
| **插件** | 文本缓冲本身：调 `ShDrawInputTake()` 取字并追加、退格、粘贴、热键、回车/取消**提交什么**，以及自己的配置页 |

一句话：**框架从不编辑一个字节，也从不解释一个命令键** —— 因此一个字节只有一个写者。

### 相关 API

```c
/* 开一个输入会话：从这一刻起，游戏窗口收到的字符归这个盒子，
 * 输入法会话也归它。同时只有一个会话，开新的会顶掉旧的。 */
SH_API int  ShDrawInputOpen(const char *id);
SH_API void ShDrawInputClose(void);          /* 结束（安全，可重复调用） */
SH_API int  ShDrawInputIsOpen(void);         /* 会话还在吗？ */

/* 把自上次调用以来收到的字符取走（UTF-8，NUL 结尾，最多 cap 字节）。
 * 放不下的字符留在队列里，下一帧再取 —— 不会丢字。 */
SH_API int  ShDrawInputTake(char *out, int cap);

SH_API void ShDrawInputSetMode(int mode);    /* 0 自绘候选（默认）/ 1 输入法自己的候选窗 */
SH_API int  ShDrawInputGetMode(void);
SH_API int  ShDrawInputComposing(void);      /* 组合中：回车/Esc/退格让给输入法，任意线程可调 */

/* 画这个盒子；id 必须与 Open 时同名，out 回填焦点/组合状态/候选模式 */
SH_API int  ShDrawInputBox(const char *id, const char *text,
                           const char *hint, ShDrawInput *out);
```

### 一个能跑的形状（`draw_sample.c` 就是照这个写的）

```c
static char g_text[256];      /* 我们的缓冲，只有我们写 */

/* 菜单里打开开关时 */
ShDrawInputOpen("my_box");
ShDrawInputSetMode(0);

static void OnDraw(void *user) {
    ShDrawInput in;
    ShDrawInputBox("my_box", g_text, "在这里输入", &in);

    for (;;) {                       /* 取字：这一帧收到的都拿走 */
        char got[128];
        int n = ShDrawInputTake(got, (int)sizeof(got));
        if (n <= 0) break;
        strncat(g_text, got, sizeof(g_text) - strlen(g_text) - 1);
    }

    if (in.focused && !in.composing) {
        /* 命令键由插件自己轮询：这里是退格/回车/Esc 的地方。
         * 组合中（in.composing）必须让给输入法，否则会把已上屏的字删掉。 */
    }
}

/* 菜单里关掉开关时 */
ShDrawInputClose();
```

`Out` 结构（`ShDrawInput`）：

| 字段 | 含义 |
| --- | --- |
| `focused` | 这个盒子当前持有会话（`id` 与 `ShDrawInputOpen` 的一致） |
| `composing` | 输入法组合中（等价于 `ShDrawInputComposing()`） |
| `mode` | 当前候选窗模式 |

**为什么要插件自己轮询命令键**：游戏的键盘读取走 DirectInput，鼠标/键盘的**命令键**在窗口消息里并不可靠（这也是框架自己的中文聊天用轮询的原因），而字符本身有 `WM_CHAR`/`WM_IME_CHAR` 这条可靠通道。所以框架只抢「字符 + 输入法会话」，命令键留给插件 —— 插件最清楚自己想要的交互（热键、退格、回车提交、Esc 取消）。

### 框架替你兜底的两件事（都请读一遍）

输入的语义是「**会话开着 = 键盘归你**」，所以框架必须保证任何一个写坏了的会话都不会把游戏变成不可玩。两条规则：

1. **盒子必须每帧被画**。会话开着时，框架每帧都记一次「这个盒子被画了」（`ShDrawInputBox` 被调用即算）。如果超过 **2 秒**没被画，框架就**不再吞键**（键盘还给游戏），超过 **5 秒**则**直接结束这个会话**并还原输入法栈，日志里会写明原因。
   含义：不要"隐藏窗口但留着会话"——要让盒子消失就 `ShDrawInputClose()`；不要在一个会阻塞的回调里开盒子（框架自己的聊天是每帧画的）；如果你在会话期间让画面停住（长加载、断点），会话会被框架收走，你会从 `ShDrawInputIsOpen()` 看到 0。
2. **会话没了就是没了**。既然框架可能结束一个会话，插件就必须把「`ShDrawInputIsOpen()` 为 0」当作自己的盒子结束 —— 二选一：**收掉自己的盒子**（`cnchat` 的做法：轮询线程发现会话没了就关框），或者**重新开一个**（`draw_sample` 的做法：开关还开着、自己又在画，就重新 `ShDrawInputOpen`）。什么都不做会留下一个"画着但打不进字"的空壳盒子。
   更细一点：如果**别人的**盒子抢走了会话，你的盒子会读到 `ShDrawInput` 的 `focused == 0`（框架同一时刻只认一个会话，最后 `ShDrawInputOpen` 的赢），此时同样要收掉或重开自己的盒子。

另外两条与输入法有关、由框架处理的规则，**插件自己的热键与提交逻辑都不该依赖它们**，但要知道它们在那里：

- **组合陈旧**：组合串 / 候选列表连续 **10 秒没有任何变化**时会被判为"已结束"（活着的组合每敲一键都会更新），这样即使输入法不再发关闭消息，`ShDrawInputComposing()` 也不会永远为真、把插件的回车/Esc 一直让给输入法。
- **会话结束时的输入法拆除**：会话结束时框架会取消组合、把输入法开关状态还原、解除对外部 IME 窗口的子类化。但如果结束时**输入法还持有组合**，框架会**保留窗口的输入上下文**（不摘、不销毁）—— 在那一刻摘掉上下文会让 TSF（`textinputframework.dll`）在下一个输入事件上空指针崩溃（2026-09-13 实测），日志里会写明 `the IME still had a composition, so the window keeps its input context`。代价是那个窗口留着输入上下文，换来的是不崩。

---

## 六、自测（用示例插件）

`draw_sample` 默认开启（`scripthook.ini` 的 `[plugins] draw_sample=1`），进游戏后：

1. 屏幕上出现 **Drawing sample** 窗口（文字、提示、按钮、开关、数字、滑块、下拉），它在**菜单关着的时候也在**；
2. F4 打开菜单 → **Drawing sample** 页：
   - `Corner readout (frameless)` 打开 → 左上角出现一行随分辨率缩放的常驻读数，并且**帧号在涨**（说明回调每帧都在跑）；
   - `Text box` 打开 → 窗口里出现输入框，用你惯用的输入法在游戏内打字（中文、候选列表都该正常），字符会出现在框里；
   - 关闭这三个开关，窗口/读数/输入框都该消失，且 `logs\scripthook_draw.log` 里能看到 `drawer 'x' registered` / `gone` / `hidden`。
3. `logs\scripthook_draw.log` 在 overlay 就绪时会写一行 `renderer attached: the drawing primitives are live`；`logs\scripthook_ovl.log` 里则会出现任何「在回调外调用原语」的告警。

---

## 七、排错

| 现象 | 看哪里 |
| --- | --- |
| 窗口没出现 | `logs\scripthook_draw.log`：有没有 `drawer 'x' registered`；有没有 `refused: all 16 slots are taken` / `the name is longer` |
| 注册了但不画 | `logs\scripthook_ovl.log` 里有没有 `imgui ready`；`ShDrawReady()` 是不是 0（overlay 没起来，MinGW 构建也永远是 0） |
| 画了一次就没了 | 是不是用户点了窗口的关闭按钮（`drawer 'x' hidden`）——`ShDrawShow(name, 1)` 可以叫回来 |
| 输入框收不到字 | `ShDrawInputOpen` 的返回值（0 = id 太长/为空）；`ShDrawInputTake` 有没有被调用（不取会被日志警告 `input queue is full`）；`in.focused` 是不是 0（会话被别的盒子抢走了） |
| 盒子忽然没了 / 键盘忽然还给游戏了 | 第 5 节的两条兜底：盒子超过 2 秒没被画（不再吞键）、超过 5 秒没被画（会话被结束）；`logs\scripthook_draw.log` 里有对应的一行 |
| 组合中误删字 | 忘了看 `ShDrawInputComposing()` / `in.composing` |
| 输入法状态卡住不结束 | 框架 10 秒无变化的陈旧规则会兜底，`logs\scripthook_ovl.log` 里会有 `ime: the composition state has not moved for ...` |
| 卡帧 | 回调里做了阻塞的事。回调必须只画 |
| 关掉插件后窗口还在 | 插件要在卸载路径里 `ShDrawDel`；`draw_sample` 用菜单开关直接注销，可作参考 |

---

## 八、明确不做的事

- **不是沙箱**：不隔离插件崩溃，不限制插件能调什么。
- **不提供 ImGui 头/符号**：API 是 C ABI 的一组原语，不做「把 ImGui 暴露给插件」。
- **不做布局引擎**：没有绝对坐标定位、没有自动排版；要精确排布就自己用 `ShDrawSameLine` / `ShDrawPanel` / `ShDrawScale()` 组合。
- **不落盘窗口位置**：会话内记忆，跨会话不记忆（框架不给游戏目录添 `imgui.ini`）。
- **不接管键盘**：输入框只收字符与输入法会话；热键、取键捕获（`ShCaptureKeys`）是插件自己的事。
