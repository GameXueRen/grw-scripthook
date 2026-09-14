# 插件模式黑名单（plugin mode blacklist）

给 `plugins\` 下的插件用的一套统一 API：**在源码里声明自己不允许运行的模式**；本局进入其中某个模式时，框架把该插件的菜单入口藏起来，并通知插件把自己停下来。

> **一句话边界**：这是**协作式**机制。框架能做的是"**隐藏菜单 + 通知状态**"；**停止插件代码是插件自己的事** —— 插件是各自独立的 DLL，各自开线程、各自带一份 MinHook，框架没有可以挂起它们的东西。文档、头注释与日志都按这条边界写。

---

## 一、声明（插件源码里一次调用）

```c
ShPluginBlacklist(SH_MODE_BLACKLIST_GHOST_WAR | SH_MODE_BLACKLIST_MERCENARIES);
```

| 声明方式 | 效果 |
| --- | --- |
| 给出位掩码（如上） | 只在**列出的条件**被禁 |
| `ShPluginBlacklist(SH_MODE_BLACKLIST_NONE)`（值 0） | **任何模式下都不限制** —— 这是唯一的"放行"写法 |
| **从未调用过** | **默认在 Ghost War 与雇佣兵下被禁**（所有第三方插件都落在这一档） |

可用的位（一个位 = 一个"条件"）：

| 位 | 条件 | 说明 |
| --- | --- | --- |
| `SH_MODE_BLACKLIST_GHOST_WAR` | Ghost War | 4v4 PvP |
| `SH_MODE_BLACKLIST_MERCENARIES` | 雇佣兵 | 8 人 PvPvE |
| `SH_MODE_BLACKLIST_CAMPAIGN` | 战役 | 主线，以及跑在它里面的 Narco Road / Fallen Ghosts / 最后的仪式 |
| `SH_MODE_BLACKLIST_GHOST_MODE` | 幽灵模式（魅影） | 永久死亡战役 |
| `SH_MODE_BLACKLIST_GUERRILLA` | 游击战 | 守营地波次 |
| `SH_MODE_BLACKLIST_ANY` | 全部 | 上面五个的并集 |

**框架内置模块与内置页不受影响**（它们的菜单 owner 为空）："ScriptHook 设置"、Forge 等在任何模式下都照常可用。默认禁用只针对 `plugins\` 下的 `.asi` 插件 —— 中文聊天输入自 2026-09 起也是插件（`plugins\cnchat\`），因此它现在同样受这套规则约束（未声明即默认禁 Ghost War 与雇佣兵）。

### 为什么没有"主菜单"这一档（实测，已放弃）

**条件只有模式，共五种**。2026-09-13 曾经加过一档"主菜单"（前端还没选任何东西），判据是 **GameFlow 机器的第 17 个子对象（slot 16）为空**，当时实测：

| 时刻（同一局，pid 1296） | 状态 | slot 16 | 在哪儿 |
| --- | --- | --- | --- |
| 10:24:09 – 10:24:14 | `MenuOrLobby`（不变） | `0` | 主菜单 |
| 10:24:15 起 | `MenuOrLobby`（不变） | `D638A0D9` | 已进入模式界面（选存档/大厅） |
| 10:24:30 | — | — | `SetCurrentGameMode(0)` 才发生 |

**它在游戏内实测被判为"不覆盖"**：同一局里从战役退回主菜单后，插件又全部显示出来了。日志（`logs\scripthook_blacklist.log`）显示，退回主菜单那一刻**没有任何条件变化行** —— 也就是说 slot 16 是**锁存**的：打开过一次模式界面后它不再变空，于是"主菜单"只在进程刚启动、还没进过任何模式界面时成立一次。

而框架没有第二个判据可退：游戏状态把"主菜单"和"各种大厅"合成一个桶（`MenuOrLobby`），模式本身在前端也还没设定。**时对时错的条件比没有条件更糟**（插件会"时隐时现"，且没有可归咎的原因），所以这一档被**移除**，只保留五个模式。若将来找到一个**真正实时**的信号（例如前端 UI 树里当前页的指纹），可以按同样的取证流程再加回来。

---

## 二、插件要做的三件事

1. **声明**：`ShPluginBlacklist(...)`，放在自己的初始化里；
2. **订阅**：`ShPluginOnBlocked(OnBlocked, NULL)`；
3. **每次干活前问一句**：`ShPluginAllowed()`。

```c
static void OnBlocked(int allowed, int mode, void *user) {
    (void)mode; (void)user;
    if (!allowed) StopMyWork();      /* 拆钩子、停线程、收起 HUD */
    else          StartMyWork();
}

static DWORD WINAPI Tick(LPVOID p) {
    for (;;) {
        Sleep(200);
        if (!ShPluginAllowed()) continue;   /* 睡着这段时间模式也可能变了 */
        DoMyWork();
    }
}
```

- 回调在**框架线程**上派发，每次"允许 ↔ 禁止"真正翻转只叫一次（250 ms 轮询）；
- 回调里**没有持有框架的锁**，所以从回调里再调框架 API 是安全的；但别在里面做重活，只做状态翻转；
- 第一帧的查询就是准的：如果插件启动时已经在黑名单模式里，`ShPluginAllowed()` 直接返回 0（不必等回调）。

其它可用的查询：`ShPluginBlockedBy()`（挡住我的是哪些**位**，0 = 没被挡）、`ShPluginBlacklistModes()`（对我生效的掩码，未声明者返回默认的 Ghost War + 雇佣兵）。显示用 `ShBlacklistName(bit)`（单个位 → 名字）或 `ShBlockedText(bits, buf, cap)`（多位 → `Ghost War+MERCENARIES`）。

---

## 三、判定与可见性

- 判定依据是 `ShSelectedPlayMode()`（框架钩住游戏自己的模式管理器读出来的）；
- **模式未知时谁都不禁**：`SH_PLAYMODE_NONE`（还在前端、游戏尚未设置模式，或这个 build 读不到模式）→ 全部允许 —— 所以**主菜单/前端里插件照常显示**，这是设计（见上一节为什么没有"主菜单"档）；
- 被禁插件在 **F4 模组菜单根页整行消失**，其子页不可进入；若翻转发生时正停在该插件页面上，会自动退回上级页面；恢复后入口重新出现；
- **"ScriptHook 设置 → 各插件开关"** 页多一行提示，写明当前模式禁掉了哪些插件，例如：

  ```
  这些改动需重启游戏生效。
  Off now (Ghost War): firstperson, freecam (+3)
  ```

  中文环境下第二行会显示为「当前不可用（幽灵战争）：…」（`[zh_cn]` 里 `"Off now"` 的译文）。

---

## 四、自测（用示例插件）

`plugins\blacklist_sample\blacklist_sample.asi` 就是最小可用示例，它声明 **"幽灵模式（魅影）下不跑"**（单人可以进，不需要联机），HUD 左上角打一个计数，菜单里有一个开关和一行状态。

验证四件事互相一致：

| 看哪儿 | 应该看到 |
| --- | --- |
| F4 模组菜单根页 | 进入幽灵模式后**没有** `Blacklist sample` 这一行；回战役后回来 |
| 屏幕左上角 HUD | 进入幽灵模式后计数**停住**、该行收起 |
| `logs\scripthook_blacklist_sample.log` | `sample: blocked (blocked by Ghost Mode)` / `sample: allowed again (blocked by nothing)` |
| `logs\scripthook_blacklist.log` | 框架侧对应行：`blacklist: blacklist_sample is off now (Ghost Mode)`，标签页提示同源 |

想验证"未声明即默认禁"：`plugins\test_plugin\`（或其他没声明过的插件）在 **Ghost War / 雇佣兵**下都会从菜单里消失 —— 而示例插件声明过黑名单，所以它在其它模式里**仍然可见**（"声明过就不再吃默认"）。

关掉示例：在 `scripthook.ini` 的 `[plugins]` 段加 `blacklist_sample=0`（或删掉 `plugins\blacklist_sample\`）。

---

## 五、排错

| 现象 | 先看哪儿 |
| --- | --- |
| 菜单毫无变化 | `logs\scripthook_playmode.log` 里有没有 `the game set GameModeType ...`。没读到模式时框架不会禁任何插件（"无模式可归咎"），这是设计而非故障 |
| 插件被禁但没收到回调 | 回调只在翻转时触发；先看 `ShPluginAllowed()` 是不是本来就返回 0 |
| `ShPluginBlacklist` 返回 0 | `ShLastError()`：`SH_ERR_BAD_ARG` = 调用者不是插件（例如从框架代码里调）；`SH_ERR_REGISTRY_FULL` = 登记表满（64 个） |
| 想知道框架眼中谁被禁 | `ShPluginBlacklistCount()` / `ShPluginBlacklistAt(i, ...)`，或直接看 `ShPluginBlacklistNotice()` 生成的那行 |
