# 构建钉子清单（engine RVA）

本框架所有引擎入口都是 **模块基址 + 写死的 RVA**（`image.h` 的 `SH_IMG`），
所以**游戏更新一旦挪动代码，这些常量就指向别的东西**。这份清单回答两个问题：

1. 哪些钉**自己能判断**（有护栏，失效时是一行日志 + 降级，不是花屏/崩）；
2. 哪些钉**只能靠实跑**（裸钉）——它们不是"已知失效"，只是没有机器可判的护栏。

盘点时间：**2026-09-21**，对照当时的实际构建（见文末"当前构建"）。
新的一轮更新之后，先看 `logs\scripthook.log` 的 `game build:` 行，再按
"重新定址流程"逐条处理。

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
| `scripthook_fov.c` / `scripthook_camera.c` / `scripthook_blur.c` / `scripthook_havok.c` | `FOV_SITE` `0x81E0C22`(6) / `MGR_SITE` `0x81E0B7E`(5) / `BLUR_MATCH` `0x1485806C`(5+16) / `HK_ALLOC_BODY` `0x163CA8C0`(5) | 有长度常量，位于同类比对路径上（**本次盘点未逐行核对比较代码**，下次触碰这四个模块时确认后再写死结论） | 按各模块实现，预期为拒绝改动 + 日志 |

## 二、裸钉（无机器可判护栏）

这些常量直接按 RVA 使用。它们**不是已知失效**——行为层面由实测覆盖（fov、
相机、召唤、传送、天气等都已实机验证）——但失效时的症状是花屏/崩，而不是
日志里一行"not pinned"。所以更新之后，**这一批要优先实机过一遍**。

| 模块 | 常量（RVA） | 用途 |
|---|---|---|
| `scripthook_ui.c` | `0xE4094B0` `0x674F1A0` `0x4D78D00` `0x32F5310` `0x32F4D00` `0x16BAFD60`、标签/图像一族 `0x336xxxx`/`0x333xxxx`、`VT_*` `0x3CFxxxx`/`0x3D0xxxx`、贴图一族 `0xDF5xxxx`/`0xE04F670`/`0x14FEEB0`、`G_DEVICE 0x4D5B058` | 自绘 UI 的对象、标签、贴图与设备 |
| `scripthook_scene.c` | `0x32EE140` `0x32EE1C0` `0x16B9A730` `0x16B99DC0` `0x16B9B8C0` `0x16B99570` `0x173F9930` `0x173F9160`、`F_FREE 0xF93CA90`、`F_LOCK 0x36206D0`、`F_UNLOCK 0x3287730`、`RENDER_THUNK 0x32EEFB0`、`G_UIMGR 0x4D58560`、`VT_GAME_RESOLVER 0x3A05AA0` | 场景生命周期与渲染接管 |
| `scripthook_api.c` | `SH_PLAYER_GLOBAL 0x4BC3470`；`SH_VT_ENTITY 0x39C6DF8` / `SH_VT_SKELETON 0x3ACBB58`（**学习式**：运行时从玩家实体取得，`ShEntityVtable()`） | 玩家对象与实体类型判定 |
| `scripthook_camera.c` | `CAM_THUNK 0x13796D0`、`CAM_IMPL 0xD67FFA0` | 相机取值与接管 |
| `scripthook_fpx.c` | `0xA074190` `0x188CE00` `0x120ADE51` `0x1209EA35` `0x1209C0CC` `0x113A0875` `0x147FF653` `0x147FF669` `0x14897FBA` `0x2A185A0` `0x1489A365` `0x13A1255B` `0x149C3E7A` `0x2A257C0` `0x4B90638` | 第一人称/藏头的一批字节站点与两个引擎调用 |
| `scripthook_input.c` | `IAT_ASYNCKEY 0x182B2B90`、`IAT_CURSORPOS 0x182B2BA0` | 输入 IAT |
| `scripthook_havok.c` | `RB_VT_*` `0x3AD55D8` `0x3AD7020` `0x39E7620` `0x38C88B0` `0x39979C8` | 刚体类型判定（跨类通用的一条由 `ShEntityVtable()` 承担） |
| `scripthook_health.c` | `SH_APPLY_DAMAGE 0x261D1A0` | 伤害应用 |
| `scripthook_hit.c` | `HIT_SITE 0x14D387E3`、`HIT_ORIG_CALL 0x2B241F0` | 命中回调站点与其原调用 |
| `scripthook_head.c` | `SKEL_VT 0x3ACBB58`、`BONE_LOOKUP 0xB506270` | 骨骼与头部骨骼查找 |
| `scripthook_resource.c` | `RES_GLOBAL 0x4B98E80`、`VALOFF_TBL 0x3AA1D09`、`SKILL_GLOBAL 0x4B98FA0` | 资源与技能点 |
| `scripthook_npc.c` | `RVA_NULL_BLOCK 0x4D88FE8`、`NPC_SPEC_VTABLE 0x394A4E0` | NPC 规格 |
| `scripthook_entity.c` | `CTRL_VT 0x3BCB3B8` | 控制器类型判定 |
| `scripthook_state.c` | `GAMEFLOW_HOLDER 0x4B879F8`、`SH_INPUT_ROOT 0x4D84F18` | 游戏状态与输入根 |
| `scripthook_weather.c` | `OBJ_FACTORY 0xE536F10`、`CTW_OP_DESC 0x49E2B70`、`CTW_DATA_DESC 0x49E2AD0`、`CTW_START 0x13D122D0` | 时间与天气对象工厂 |

## 三、当前构建

| | COFF TimeDateStamp | SizeOfImage |
|---|---|---|
| 框架定址时那一版 | `6A7C5143` | `18B09000` |
| 当前实测（2026-09-21） | `6A99768A` | `185BA000` |

**结论**：当前商店版**不是**框架定址时那一版（镜像小了约 5.3 MB），即上面第二节的
裸钉从来没有在这版上被机器核对过。2026-09-21 逐字节确认过的只有物理链两处：

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
   `REFUSED` / `the engine calls do not have the pinned shape`）。**报错的那条就是
   要重定址的那条**，日志里通常已经带上了该处的真实值或字节。
3. 裸钉那批没有判词：按功能实机过一遍（自绘 UI、场景接管、相机、藏头、输入、
   天气、资源/技能、NPC、实体判定），坏了再定位。
4. 定址手法沿用现有两套：**签名扫描**（在镜像里搜一段已知字节，反解 rel32 得到
   目标，再与钉值比对，见 `docs/ammocapacity-reverse.md`）与**形状判定**（读几
   条指令确认函数形态）。定到新值后：改常量 → 有护栏的补上新签名/长度 → 更新
   本文件与 `kKnownBuilds`。
