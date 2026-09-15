# 逆向：AmmoCapacity.asi（无源码插件）→ 重写规格

对象：`plugins\AmmoCapacity\AmmoCapacity.asi`（15,872 字节，2026-09-04），来源
<https://www.nexusmods.com/ghostreconwildlands/mods/123>（同目录 `作者.txt`）。

本文是**静态逆向**（`dumpbin /disasm` + 原始字节）的结论，也是本项目重写它的规格。
所有 RVA 都是**相对 GRW.exe 模块基址**，即 `image.h` 的 `SH_IMG(rva)`。

## 0. 一句话结论

它钩住游戏里一个**返回"弹匣容量（eax 低 16 位）"的函数**，在钩子跳板里**先执行原函数、
再把自己的返回值按比例缩放**：

```
新容量 = min(原容量 × 分子[档] ÷ 分母[档], 0xFFFF)
```

**容量不是内存字段**，是那次调用的返回值 —— 这正是"读字段 / 写断点"两条探查路线
（见 `plugins\CapProbe`）注定找不到它的根本原因：它根本不存在于内存里。

## 1. 钩点

| 项 | 值 |
|---|---|
被钩地址 | `RVA 0x614CB0`（`SH_IMG(0x614CB0)`）|
该处内容 | `E9 AB 75 0D 08` + 其后 `CC` 填充（Denuvo 风格**跳板表**：每 `0x180` 一个 `E9 rel32`，见 `0x614CB0` 与 `0x614E30`）|
定位方式 | 扫描一个 5 字节签名 `E8 rel32`，用 rel32 反解出**调用目标**，再与 `base + 0x614CB0` 比对 |
校验失败 | 状态行 `"Unsupported game build"`；补丁失败为 `"Hook validation failed"` |

反汇编证据（`.asi` 的 `0x1800013A5`–`0x180001431`）：

```asm
lea  r8,[0x180003430h]        ; 5 字节签名
    ; 逐字节比较缓冲与签名（跳过首字节，即跳过 E8 操作码）
mov  rdx,[0x1800053C0h]       ; exe 模块基址
movsxd rcx,dword [rsp+29h]    ; 签名处 E8 的 rel32（有符号）
add  rdx,5
add  rdx,rcx                  ; → 调用目标
mov  rax,[0x180005170h]
add  rax,614CB0h              ; 期望：base + 0x614CB0
cmp  rdx,rax
jne  失败                      ; "Unsupported game build"
```

## 2. 跳板（它构造并写入目标入口的代码）

它 `VirtualAlloc` 一个 `0x1000` 缓冲、逐条写入下列指令、`VirtualProtect(..., PAGE_EXECUTE_READ)`
后把目标入口改写为跳到该缓冲。反汇编证据 `0x180001264`–`0x1800012B5`（`C7 01 48 83 EC 28`
这类"把指令当数据写"的序列）：

```asm
sub  rsp, 28h
mov  rax, <原函数 = SH_IMG(0x614CB0)>
call rax                 ; ① 先跑原函数
mov  ecx, eax            ; ② 它的返回值（低 16 位）作为参数
mov  rax, <缩放函数 = base + 0x1000>
call rax                 ; ③ 再调自己的缩放函数
add  rsp, 28h
ret                      ; ④ 缩放函数的返回值 = 原函数的返回值
```

## 3. 缩放函数（`base + 0x1000`，`.text` 第一段，完整还原）

```asm
; int scale(uint16_t value)   value = 原函数返回值 cx
movzx edx, cx                 ; edx = 容量
test  edx, edx ; je → return 0
movsxd rax,[0x180005078h]     ; g_index：用户选的档位
test  eax,eax ; js  → return value      ; 负数 → 原样
cmp   eax,6   ; jge → return value      ; ≥6   → 原样
cmp   eax,2   ; je  → return value      ; ==2  → 原样（1.00x 直通档）
mov   r8d,[0x1800033D0h + rax*4]        ; 分子表
imul  rax,r8                            ; 容量 × 分子
mov   ecx,[0x1800033E8h + rax*4]        ; 分母表
div   rax,rcx                           ; ÷ 分母（64/32 无符号）
cmp   eax,0FFFFh ; cmova eax,0FFFFh     ; 截断到 65535
ret
```

即：

```c
static int Scale(uint16_t value) {
    if (!value) return 0;
    if (g_index < 0 || g_index >= 6 || g_index == 2) return value;
    if (!g_den[g_index]) return value;                 /* 防御，原版没有 */
    uint64_t v = (uint64_t)value * g_num[g_index] / g_den[g_index];
    return v > 0xFFFFu ? 0xFFFF : (int)v;
}
```

## 4. 档位表（`.rdata`，RVA `0x33D0` / `0x33E8`，各 6 个 int32）

| idx | num | den | 倍率 | 菜单文字 |
|---|---|---|---|---|
| 0 | 1 | 2 | 0.50 | `0.50` |
| 1 | 3 | 4 | 0.75 | `0.75` |
| 2 | 1 | 1 | 1.00 | `1.00`（"1.00x  Vanilla"，直通）|
| 3 | 5 | 4 | 1.25 | `1.25` |
| 4 | 3 | 2 | 1.50 | `1.50` |
| 5 | 2 | 1 | 2.00 | `2.00` |

`.rdata` 里紧邻的字符串证实了这六行（`0.50`…`2.00`），其 `lang.ini` 里
`"1.00x  Vanilla"` 是第 3 项的显示文字。

## 5. 其余还原到的事实

- 配置：自己的 ini `[AmmoCapacity] Multiplier=1.00`（`wcstod` 解析为 double），
  写到 `GetPrivateProfileStringW` / `WritePrivateProfileStringW`；
  ini 里同时并存它的 `[zh_cn]` 文本段（本项目 i18n 迁移后的形态）。
- 文本：`Ammo Capacity` 页 + 行 `Capacity Multiplier`；状态
  `Changes apply after visiting an ammo crate.`；错误 `Unsupported game build` /
  `Hook validation failed` / `INI save failed`。
- 它**本来就是按本框架的插件 API 写的**（晚绑定）：`.rdata` 里有
  `ddinput8.dll`、`ShGetVersion`、`ShReadBytes`、`ShMenuCreate`、`ShMenuStatus`、
  `ShMenuDestroy` 等符号名，运行时 `LoadLibrary` + `GetProcAddress` 取用。
  即：作者有 `scripthook.h`，只是没给源码。
- 线程：`CreateThread` + `Sleep`，用于菜单轮询 / 等待游戏就绪。
- 完整性：MSVC 静态 CRT（`__security_cookie`、CPUID 分支、`__C_specific_handler`）。

## 6. 为什么"字段法"必然失败（写给未来的自己）

本轮先有一支只读探针（`plugins\CapProbe`，**已随本轮清理删除**）依次尝试了：
扫堆找库存对象 → 打印字段网格 → 对比补给前后的字段 → DR0 硬件写断点。
前两步证明 `+0x180` 是**当前弹药**，后两步没来得及给出答案，
而静态逆向一步到位：**容量是函数返回值**，既不在库存对象里，也不在任何可写的字段上。

同时被清理的还有 `ShGetAmmo` / `ShSetAmmo` 这对"当前弹药"API：它全树只有一个调用者
（chaos 的"弹药清空 / 装满"两个效果，一并移除），而它的查找器在冷缓存时会同步扫完
游戏全部可读写页（实测游戏内 ~23 秒），与它提供的价值不相称。容量这条链**不依赖它**
（`ShSetAmmoScale` 走的是引擎自己的返回值），因此不受影响。

结论：**遇到"旧插件能改、我们读不到"的量，先反汇编旧插件**。
它的 `.pdata`/`.text` 只有 7 KB，`dumpbin /disasm` 一次就能读完全貌，
比在游戏里跑任何探针都快。

## 7. 重写方案（本项目的形态）——**已实现**

1. **框架侧** `scripthook_ammocap.c`（唯一写引擎内存的人）：
   - MinHook 钩 `SH_IMG(0x614CB0)`（该处本就是 5 字节 `E9 rel32`，MinHook 正常改写；
     不必像旧插件那样手工 `VirtualAlloc` 跳板 + `FlushInstructionCache`）；
   - detour 即 `return Scale(orig(...))`，`Scale` 是第 3 节的整数有理数缩放；
   - API：`ShSetAmmoScale(num, den)` / `ShGetAmmoScale(&num, &den)` /
     `ShAmmoScaleActive()`，缺省 `1/1`（等于旧插件的直通档）；
   - **缺省不装钩**：只有调用方要了非 `1/1` 才安装 —— 与本框架"不用就不碰引擎"
     的一贯做法一致；`1/1` 时 detour 是纯直通；
   - `num`/`den` 打包进一个 32 位字（Interlocked），读到的永远是完整的一对；
   - 失败一律 `ShLastError = SH_ERR_HOOK_FAILED` 并写 `logs\scripthook_ammocap.log`，不静默；
   - **互斥已实现**：安装前检查 `GetModuleHandleA("AmmoCapacity.asi")`，加载了就拒绝安装
     并写明原因（两把内联钩子钩同一入口 = 竞争，没必要输）。
2. **插件侧** `plugins\ammo_capacity\`：按本项目规范（`ShLangDeclare` +
   `ShMenuList` + `ShMenuStatus` + 自己的 ini），复刻旧插件的界面 ——
   一行 `@ac.mult`（六档 `0.50x`…`2.00x`）、提示与状态 `@ac.note`（补给后生效）、
   错误文案 `@ac.err.build` / `@ac.err.hook` / `@ac.err.save`（与原插件逐字对应）。
   ini 沿用旧的键名与小数写法 `[AmmoCapacity] Multiplier=1.00`，值按"最接近的档位"
   匹配，所以从旧插件带过来的 ini 读出来结果一致。
3. **部署**：旧 `AmmoCapacity=0`、`ammo_capacity=1`（已写入游戏目录的 scripthook.ini）。
4. **验证**：进游戏 → F4 →「弹药容量」→ 选 `2.00x` → 到弹药箱补给 → 弹匣应为原版两倍；
   选回 `1.00x` 补给后恢复原版；`1.00x` 时框架不装钩（日志里没有 hook 行）。


## 8. 仍未确认（重写时用运行时日志补齐）

- 被钩函数（`RVA 0x614CB0`）的**参数语义**（rcx 是武器还是槽位）与
  "返回值恒为 16 位容量"这一点：旧插件只看 `cx`，说明它至少**取低 16 位是安全的**；
- 该跳板真正指向的函数体（`E9 0D75AB08` → `RVA 0x86EC260`，落在保护壳的虚拟区），
  只有需要直接调用它读容量时才要弄清。
