# 构建钉子清单（engine RVA）

本框架所有引擎入口都是 **模块基址 + 写死的 RVA**（`image.h` 的 `SH_IMG`），
所以**游戏更新一旦挪动代码，这些常量就指向别的东西**。这份清单回答两个问题：

1. 哪些钉**自己能判断**（有护栏，失效时是一行日志 + 降级，不是花屏/崩）；
2. 哪些钉**只能靠实跑**（裸钉）——它们不是"已知失效"，只是没有机器可判的护栏。

盘点时间：**2026-09-21**，对照当时的实际构建（见文末"当前构建"）。
新的一轮更新之后，先看 `logs\scripthook.log` 的 `game build:` 行，再按
"重新定址流程"逐条处理。

**2026-09-27 与源码对齐**：第二节里有 8 个值与源码不一致（`F_LOCK`、`F_UNLOCK`、
`RENDER_THUNK`、`G_UIMGR`、`VT_GAME_RESOLVER`、`SH_APPLY_DAMAGE`、`HIT_SITE`、
`HIT_ORIG_CALL`）。**源码是对的一边** —— 其中 `HIT_SITE` / `HIT_ORIG_CALL` 有实跑字节
作证（见第一节与第三节）。已按源码改齐；同时把 `RVA_RETIRE`（全仓置信度最低的一条）
补进第二节，把 `scripthook_playmode.c` 的自证手法补进第四节。

**这份文件必须与源码同改**：动常量时顺手改这里，否则它就会变成一份"看起来权威的旧
数据"，而这正是它最危险的用法 —— 游戏更新后按它重新定址。写 `tools/rva-list.txt` 的
那些行是中间产物，**以源码为准**。

---

## 一、有护栏（自检）的站点

| 模块 | 站点 | 护栏形式 | 不符时的行为 |
|---|---|---|---|
| `scripthook_physics.c` | `RAY_HOOK_SITE` `0x163F18D0` | 20 字节函数序言签名，打补丁前比对 | 拒绝打补丁，日志一行，`ShGroundHeight` / `ShTeleportPlayerToGround` / `ShQueueCall` 全部不可用 |
| `scripthook_physics.c` | `CAST_RAY_FN` `0xFBB3580`（被直接 call） | 镜像内 + 可执行页 + 6 字节签名（`kCastSig`，2026-09-21 从运行中的构建读出并钉上） | 整条物理链判不可用，日志写明原因，**不会调用错地址** |
| `scripthook_stealth.c` | `VIS_SITE` `0x13BEAC48`（`VIS_LEN 5`） | 逐字节比对 `F3 45 0F …` | 拒改，并把该处**实际字节**打进日志（便于重新定址） |
| `scripthook_spawn.c` | 管理器 getter 与两个 spawn 函数 | `CallShapeOk(...)` 逐条校验函数形状（getter 读全局即返回、两函数以压栈 rbx/rsi 开头） | 记一行 no-op，不调用 |
| `scripthook_spawn.c` | `SPEC_VTABLE`（学习式） | 先按形状走，学到的 vtable 与钉值对照打印 | 仍能找到对象，只多出误报，日志给出真值 |
| `scripthook_weather.c` | `WX_RECORD` / `ENV_VTABLE` / `TIME_MGR` | 结构判定（可读 vtable、时钟落在 `[0,24)`）+ 学到值对照钉值 | 记 `the env vtable is not the pinned one …`，并给出要重新定址的值 |
| `scripthook_reflect.c` | `FLOW_METHODS` `0x483B920` | 门 + 由 `state.c` 每 100 ms 汇报判定与真值 | 静默拒绝，但判定与真值仍会进日志 |
| `scripthook_frame.c` | `FRAME_RVA` `0x0BF42B60`（`Ai::SpawningManagerUpdate`） | 30 字节函数序言签名，打补丁前比对 | 拒绝打补丁，日志一行，`ShRegisterFrameCallback` 返回 0（**只在有人注册时才装 hook**） |
| `scripthook_fov.c` / `scripthook_camera.c` / `scripthook_blur.c` / `scripthook_havok.c` | `FOV_SITE` `0x81E0C22`(6) / `MGR_SITE` `0x81E0B7E`(5) / `BLUR_MATCH` `0x1485806C`(5+16) / `HK_ALLOC_BODY` `0x163CA8C0`(5) | 2026-09-27 逐行核对过：`fov` 比**全 6 字节**（当天实机跑过，日志 `matched (89 88 80 01 00 00)`）；`camera` 比操作码 + rel32 目标等于钉值（两处站点同款）；`blur` 比 16 字节掩码签名 + 立即数 ∈{0,1}；`havok` 比 4 字节，其中第 4 字节按**形状**判（SIB 的 base 必须是 rsp），**这一条 2026-09-27 由 3 字节升到 4 字节** | 一律拒绝改动 + 一行日志（判词里带上实际字节） |
| `scripthook_hit.c` | `HIT_SITE` `0x14703F83`、`HIT_ORIG_CALL` `0x29B4E00` | **2026-09-27 新增**：站点首字节必须是 `E8`，且 rel32 解出的目标必须等于 `HIT_ORIG_CALL`（与 `camera` 的 manager site 同款）。在此之前这里是"照打不误"—— 源码注释自述没有字节校验，靠跨会话比对 4 字节代替 | 不符即 `SH_ERR_HOOK_FAILED`，拒绝打补丁 + 一行日志（含站点 5 字节，重定址从它开始） |

## 二、裸钉（无机器可判护栏）

这些常量直接按 RVA 使用。它们**不是已知失效**——行为层面由实测覆盖（fov、
相机、召唤、传送、天气等都已实机验证）——但失效时的症状是花屏/崩，而不是
日志里一行"not pinned"。所以更新之后，**这一批要优先实机过一遍**。

| 模块 | 常量（RVA） | 用途 |
|---|---|---|
| `scripthook_ui.c` | 分配与挂载：`F_ALLOC_CTX` `0xE410100`、`F_ALLOC` `0x674F1A0`、`G_POOL` `0x4D78D80`、`F_ATTACH` `0x32F4230`、`F_CONT_CTOR` `0x32F5310`、`F_DIRTY` `0x16BAFD60`、`F_WIDGET_COLOUR` `0x32F3310`；标签：`F_LINST_CTOR` `0x336E440`、`F_LABEL_CREATE` `0x336EB60`、`F_LABEL_APPLY` `0x336EB20`、`F_LABEL_TEXT` `0x3336260`、`F_LABEL_SIZE` `0x3336310`、`F_LABEL_UPDATE` `0x16C30910`、`F_LABEL_REGIST` `0x3334E20`、`F_LABEL_FIXW` `0x3335AD0`、`F_LABEL_FIXH` `0x3336570`；图像与贴图：`F_IINST_CTOR` `0x336DB60`、`F_IMAGE_CREATE` `0x336E010`、`F_IMAGE_APPLY` `0x336DFC0`、`F_TEX_CTOR` `0xDF5C480`、`F_TEX_CREATE` `0xE04F670`、`F_TEX_MAP` `0xDF7FEB0`、`F_TEX_PUSH` `0x14FFE70`、`F_IMG_UV0` `0x32FD280`、`F_IMG_UV1` `0x32FD420`；设备：`G_DEVICE` `0x4D5B0D8`；vtable：`VT_LABEL` `0x3CF8930`、`VT_CONTAINER` `0x3CF0920`、`VT_CONT_PRIV` `0x3CF0958`、`VT_IMAGE` `0x3CF15C0`、`VT_LABEL_INST` `0x3D05228`、`VT_IMAGE_INST` `0x3D04E00` | 引擎 HUD 树里的原生控件（容器 / 标签 / 图像 / 贴图）。**2026-09-27 按源码逐条写名**。**使用者**：框架自己的 HUD 板与 toast（`scripthook_hud.c`，120 ms tick）、公测插件 `AmmoControl`（建自己的场景、读游戏 HUD 的弹药标签）、开发件 `test_plugin` / `ui_sample` / `ModeProbe`。**与 ImGui 菜单无关** —— 菜单画在 `scripthook_ovl.cpp`，它只从 `scripthook_hud.c` 取 toast 快照 |
| `scripthook_scene.c` | `F_SCENE_CTOR` `0x32EE140`、`F_SCENE_DTOR` `0x32EE1C0`、`F_SCENE_TICK` `0x16B9BB20`、`F_SCENE_FLIP` `0x16B9B440`、`F_SCENE_RENDER` `0x16B99A30`、`F_SCENE_RESIZE` `0x16B98D80`、`F_SCENE_SETCTX` `0x16B9A730`、`F_SCENE_SETRES` `0x16B99DC0`、`F_FREE` `0xE4F8110`、`F_LOCK` `0x3621550`、`F_UNLOCK` `0x3286FF0`、`RENDER_THUNK` `0x32EE4A0`、`G_UIMGR` `0x4D585E0`、`VT_GAME_RESOLVER` `0x3A05A00` | 场景生命周期与渲染接管。**2026-09-27 按源码逐条写名**（原来那 8 个匿名值里 4 个是 2026-09 更新前的地址、4 个是 `rva-list.txt` 的试算草稿，见下） |
| `scripthook_api.c` | `SH_PLAYER_GLOBAL 0x4BC3470`；`SH_VT_ENTITY 0x39C6DF8` / `SH_VT_SKELETON 0x3ACBB58`（**学习式**：运行时从玩家实体取得，`ShEntityVtable()`） | 玩家对象与实体类型判定 |
| `scripthook_camera.c` | `CAM_THUNK 0x13796D0`、`CAM_IMPL 0xD67FFA0` | 相机取值与接管 |
| `scripthook_fpx.c` | `0xA074190` `0x188CE00` `0x120ADE51` `0x1209EA35` `0x1209C0CC` `0x113A0875` `0x147FF653` `0x147FF669` `0x14897FBA` `0x2A185A0` `0x1489A365` `0x13A1255B` `0x149C3E7A` `0x2A257C0` `0x4B90638`（**全为匿名值，2026-09-27 未逐条复核**） | 第一人称/藏头的一批字节站点与两个引擎调用 |
| `scripthook_input.c` | `IAT_ASYNCKEY 0x182B2B90`、`IAT_CURSORPOS 0x182B2BA0` | 输入 IAT |
| `scripthook_havok.c` | `RB_VT_BASE 0x3AD54F8`、`RB_VT_VEHICLE 0x3AD6F48`、`RB_VT_C 0x39E74E0`、`RB_VT_D 0x38C8820`、`RB_VT_E 0x39977B8` | 刚体类型判定（跨类通用的一条由 `ShEntityVtable()` 承担） |
| `scripthook_health.c` | `SH_APPLY_DAMAGE 0x27cc0c0` | 伤害应用（源码记着实跑验证：`gcall 1427CBEA0 <comp> 1E 1 0` 把 100 打到 70） |
| `scripthook_head.c` | `SKEL_VT 0x3ACBB58`、`BONE_LOOKUP 0xB506270` | 骨骼与头部骨骼查找 |
| `scripthook_resource.c` | `RES_GLOBAL 0x4B98E80`、`VALOFF_TBL 0x3AA1D09`、`SKILL_GLOBAL 0x4B98FA0` | 资源与技能点 |
| `scripthook_npc.c` | `RVA_NULL_BLOCK 0x4D89068`、`NPC_SPEC_VTABLE 0x394A4E0`、**`RVA_RETIRE 0x99FDBB0`（置信度最低，见下）** | NPC 规格与退场 |
| `scripthook_entity.c` | `CTRL_VT 0x3BCB2A8` | 控制器类型判定 |
| `scripthook_state.c` | `GAMEFLOW_HOLDER 0x4B879F8`、`SH_INPUT_ROOT 0x4D84F18` | 游戏状态与输入根 |
| `scripthook_weather.c` | `OBJ_FACTORY 0xE536F10`、`CTW_OP_DESC 0x49E2B70`、`CTW_DATA_DESC 0x49E2AD0`、`CTW_START 0x13D122D0` | 时间与天气对象工厂 |

### 全仓置信度最低的一条：`RVA_RETIRE 0x99FDBB0`

`scripthook_npc.c:60`。`tools/rva-list.txt:53` 对它的记录是"only .pdata candidate
(80%); live (undo)" —— **没有调用点可以投票**，是从 `.pdata` 里挑出来的唯一候选。
实跑结果是：**这个调用被接受，实体不走**（`scripthook_npc.c:54-58` 的模块头写着这句）。

它没有站点校验（是**直接 call**，不是打补丁，所以没有"字节不符就拒绝"这种东西），
护栏在后端：`ShDespawn` 调用后**读回核实**实体是否还在生成系统里
（`EntityStillSpawned`，`:795-804`），还在就返回 0 + `SH_ERR_NO_EFFECT`，并在
`scripthook_npc.log` 记一行 `the retire call did not take: the entity is still in the
spawn system (RETIRE is the weakest pin in this build)`。

也就是说，它的失效症状是**"收不掉"（干净地失败并报出来）**，不是花屏也不是崩 ——
但重新定址时它要排在前面：判据用 Domino 的 `UnspawnFromEntity` 调用点，
不要再用 `.pdata` 候选。

### `tools/rva-list.txt` 的用法（2026-09-27 补）

那份文件是**重新定址当时的过程记录**：两列分别是"更新前的旧值"和"试算的新值"。它的
`new` 列**不等于**源码现值 —— 2026-09-27 按名字比过约 10 处（`F_SCENE_TICK` /
`F_SCENE_FLIP` / `F_SCENE_RENDER` / `F_SCENE_RESIZE`、`F_LOCK`、`F_TEX_PUSH`、`G_POOL`、
`G_DEVICE`、`G_UIMGR`、`RENDER_THUNK`），每一处都与源码不同；里面还夹着"body 70%"、
"only .pdata candidate (80%)"这类**当时**的置信度，那是判断过程的痕迹，不是结论。

本文件第二节原先那一行匿名值就是从它抄的：4 个抄了 `old`（更新前的地址），4 个抄了
`new`（试算草稿），于是 8 个全是错的（2026-09-27 已按源码改齐）。

**用法**：拿它当"当时试过什么、哪一条还没在游戏里验证过"的线索；**取值一律以源码为准**，
改完源码再回来改本文件。

## 三、当前构建

| | COFF TimeDateStamp | SizeOfImage |
|---|---|---|
| 框架定址时那一版 | `6A7C5143` | `18B09000` |
| 当前实测（2026-09-21） | `6A99768A` | `185BA000` |

**结论**：当前商店版**不是**框架定址时那一版（镜像小了约 5.3 MB），即上面第二节的
裸钉从来没有在这版上被机器核对过（`loader.c` 的 `kKnownBuilds` note 仍只记着
2026-09-21 那一次 —— 改它属于源码改动，本轮没做）。

到 **2026-09-27** 为止，在这版构建上被机器核对过的站点：

| 站点 | 怎么核对的 |
|---|---|
| 物理链两处（`ray hook` 站点与它 call 的 `cast`） | 2026-09-21 逐字节，日志原文见下 |
| `FOV_SITE` `0x81E0C22` | 2026-09-27 实跑：`fov site 7FF717170C22 matched (89 88 80 01 00 00)` —— 当时新校验比的就是这 6 字节 |
| `HIT_SITE` `0x14703F83` / `HIT_ORIG_CALL` `0x29B4E00` | 2026-09-26 的实跑字节 `hit site 7FF74EB03F83 holds E8 78 0E 2B` 与两个钉值算出的 rel32 低三字节**逐字节吻合**（2026-09-27 复核） |
| `RAY_HOOK_SITE`、`SPEC_VTABLE` 一族 | 每次会话各自的自检判词（`ray hook: … hooked`、`spawn: spec vtable learned`） |

2026-09-21 逐字节确认过的物理链两处：

```text
cast fn: 7FF67D483580 opens 40 55 57 41 54 41
ray hook: 7FF683CC18D0 hooked - the pump, the ground queries and every queued engine call run from this callback
```

已知构建表在 `loader.c` 的 `kKnownBuilds`：每加一条，要写清**在这版上验证过什么**，
不写"已适配"这种泛泛结论。报告方式：启动日志一行 + 菜单 About 页一行
（`@about.build.ok` / `@about.build.new`，`SH_VERSION` 之下的那行）。

## 四、重新定址流程

1. 跑一次游戏，读 `logs\scripthook.log` 的 `game build:` 行——不匹配就把它加进
   `kKnownBuilds`（note 写清范围）。
2. 逐个看模块日志里"有护栏"那批的判词（`not patched` / `not the pinned` /
   `REFUSED` / `the engine calls do not have the pinned shape` / `the ray callback
   is NOT installed`）。**报错的那条就是要重定址的那条**，日志里通常已经带上了该处
   的真实值或字节。最后那条是一句汇总：物理链一断，场景 tick、召唤泵、NPC 泵、地面
   查询与排队引擎调用**全停** —— 看到它就不必再去解释那四个各自超时的现象了。
3. 裸钉那批没有判词：按功能实机过一遍（自绘 UI、场景接管、相机、藏头、输入、
   天气、资源/技能、NPC、实体判定），坏了再定位。
4. 定址手法有三套，按强度排：

   **（一）自证** —— 最强的一套，`scripthook_playmode.c`。不去比对会随构建变化的字节，
   而是找一段**函数自己引用的常量**：`GameModeManager::SetCurrentGameMode` 与
   `CreateGameMode` 各自带一行日志字符串，函数开头会用 `lea reg,[rip+disp32]`
   （`48/4C 8D 05`）取它的地址。于是"在函数开头一段窗口里搜到这条指令、把 disp32 反解
   出来等于那条字符串的地址"就证明它就是它自己 —— `RefVerified`（`:220-235`），
   站点与证明地址在同文件 `:103-106`。窗口要够大：0x80 曾漏掉 `CreateGameMode` 那条
   （它在 +0x92 处），现在是 `GM_PROBE 0x180`。搜不到就不挂钩（`ArmOne`，`:268-273`，
   失败还会 `MH_RemoveHook` 回滚）。这一套既不需要预先知道字节，也不需要调用点投票。

   **（二）签名扫描** —— 在镜像里搜一段已知字节、反解 rel32 得到目标，再与钉值比对
   （见 `docs/ammocapacity-reverse.md`）。

   **（三）形状判定** —— 读几条指令确认函数形态（`CallShapeOk` 一类；`hit` 那种
   "站点首字节 + 目标等于钉值"也属于这一档，它不需要预先知道位移字节）。

   定到新值之后：改常量 → 有护栏的补上新签名/长度 → **更新本文件与 `kKnownBuilds`**，
   并写清"在这版上验证过什么"。
