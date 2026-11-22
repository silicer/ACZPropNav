# 验证记录 / Verification record

本次交付在本机（无游戏运行环境）完成的全部验证，以及**尚未**能验证的部分。
All checks below were run on the delivery machine; the game itself was not launched.

环境 / environment: Windows x64, LLVM-MinGW (clang 23.1.4, target
`x86_64-w64-windows-gnu`), 构建脚本 `build.sh`（GNU make 的 jobserver 管道在本机沙箱中
不可用，因此提供同功能的脚本；两者编译/链接参数一致）。

---

## 1. 构建 / build

```
./build.sh
```

产出（`build/`）：

| 文件 | 大小 | 说明 |
| --- | --- | --- |
| `ACZPropNav.dll` | 626 KB | 模组本体，导出 `AczCoreRun` + 社区 addon 入口 `AczAddonInit` |
| `XINPUT1_4.dll` | 11.7 KB | 加载器代理（转发 15 个导出，含序号 2/3） |
| `VERSION.dll` | 12.3 KB | 备选代理（转发全部 17 个导出） |
| `initest.exe` `pnsim.exe` `memtest.exe` `hooktest.exe` `smoke.exe` | — | 离线测试 |

## 2. 测试结果 / test results

| 测试 | 断言数 | 结果 |
| --- | ---: | --- |
| `initest.exe` — INI 解析（含真实 `AC8PropNav.ini` 逐项比对） | 268 | **PASS 268 / FAIL 0** |
| `pnsim.exe --selftest` — PN 数学（λ̇ / tGo / 混合 / 限速 / 视场 / 退化输入） | 15 | **PASS 15 / 0** |
| `memtest.exe` — 内存层 + 数值相关发现（合成镜像、SEH、诱饵表） | 85 | **PASS 85 / 0** |
| `hooktest.exe` — 内联钩子 + 代理转发（含 128 次装卸循环） | 70 | **PASS 70 / 0** |
| `smoke.exe` — 真实 DLL 端到端加载 | 8 | **PASS** |
| **合计** | **446** | **全绿 / all green** |

`./build.sh test` 一次跑完以上全部。

### 2.1 PN 数学证据（pnsim `--report`）

198 组交战几何 × 3 档前置量 × 2 种导引头配置：

| 场景族 | 纯追踪 lead=0 | 混合 lead=0.7 | 全 PN lead=1.0 |
| --- | ---: | ---: | ---: |
| 全部（视场 360°，隔离导引律） | 39.9 % | 56.1 % | **60.1 %** |
| 其中交叉/横越 | 38.9 % | 52.8 % | **57.4 %**（+18.5 pp） |
| 全部（原版 75° 视场） | 35.4 % | — | **53.0 %** |
| 其中交叉/横越 | 32.4 % | — | **48.1 %** |

定型案例：交叉 90°、距离 2400 m、目标 450 m/s、15°/s 转弯 —— 纯追踪 9 s 后脱靶
190.9 m；全 PN 在 4.19 s 命中。详见 [PN.md](PN.md)。

### 2.2 模组端到端（smoke）

把真实 `ACZPropNav.dll` 载入一个合成进程，断言：`LoadLibrary` 成功、`AczCoreRun` 导出
存在、15 s 内写出 `ACZPropNav.log`、日志含版本横幅与配置路径、走到发现阶段、给出
"未命中"结论且不写内存、tick 循环继续运行不崩溃。

这一步同时暴露并修复了一个真实缺陷：**模组自己的静态表里就含搜索用的浮点指纹**，
不加保护会"发现"并改写自己镜像内的数据。现在 `patcher.cpp` 会把本模组镜像
（`GetModuleInformation` 得到的 `[base, base+SizeOfImage)`）从所有扫描与结果中排除，
`smoke` 与 `memtest` 都覆盖该行为。

### 2.3 代理转发（hooktest #5）

`XINPUT1_4.dll` / `VERSION.dll` 的导出表用脚本与系统 DLL 逐项比对（名字 + 序号）一致；
`XInputGetState(0)` 转发返回 1167（无手柄），`GetFileVersionInfoSizeW(kernel32.dll)`
转发返回 1836；`FreeLibrary` 干净。

## 3. 配置兼容性（initest 摘要）

* 真实 `AC8PropNav.ini`（219 行）→ 11 个玩家型号 + 1 个敌方型号，`HomingForesightAmount`
  逐型号精确匹配，其余四项全部 `nullopt`，**0 warning**；
* 本项目 `ACZPropNav.ini` → 10 条玩家 / 7 条敌方，0 warning；
* `msl = 0.8` 旧写法、单行块、跨行块、注释/尾注释、CRLF、BOM、大小写/下划线、重复键合并、
  `Resolve` 最长键优先（`4AAM_X` → `4aam`）、`Sanitize` 幂等 —— 均有断言覆盖。

## 4. 真机第一次运行（2026-10-25 16:04–16:08）/ first live run

用户在自己的机器上安装并实际启动了一局（`ACZero\ACZPropNav.log` + `.key.log`，各 155/114 行）。

**成功的部分**
* 代理加载、DLL 注入、INI 读取（`[player] 10 条`）全部正常，**游戏没有崩**；
* 内存扫描/日志/回读校验链路按设计工作，未命中时**没有写任何内存**；
* 用户把 `[enemy]` 整段注释掉后，模组按文档行为处理（这也是日志里敌方条目为 0 的原因）。

**暴露的问题（已修复，见下节）**
1. **`.wpb` 96 字节记录不在可写内存里按原样存在**：全量扫描 1.6 GB 只找到 16 个 8 字节
   指纹假阳性，逐条校验全部被拒 —— 说明引擎把 `.wpb` 解析成了自己的运行时结构
   （或留在只读映射/低地址区，而当时只扫了可写区）。
2. **数值相关回退太慢**：一次全量相关扫描约 16 s，且每 6/12/24/48/60 s 重试一次，
   会拖累游戏（4 分钟里跑了 7 次）。
3. 日志 `[enemy]` 条数打印用错了字段（固定等于 `[player]` 条数）。

### 4.1 针对这次真机结果做的修正

| 修正 | 内容 |
| --- | --- |
| 扫描范围 | 字节指纹扫描改为扫**所有可读区域**（含只读映射/镜像数据），仍然排除本模组自身镜像 |
| 性能保护 | 连续 3 次未命中后转入**轻量扫描**（只做字节扫描，120 s 一次），不再做昂贵的相关扫描；锚点上限 8192→2048 |
| 运行时探测 | Probe 模式新增 **RTTI 类解析**（`WeaponModel` / `WeaponEx*Ctrl` / `ObjPlane` 的 vtable，先读回类型名验证，再找活对象并 dump 浮点字段）与**数值邻域转储**（1600/3000/40/80 附近 ±0x30 的浮点布局） |
| Probe 时机 | 改为启动后 10/30/60/90/120/180/240/300 s 多次采样（武器数据要等关卡加载后才在内存里） |
| 日志 | `[player]`/`[enemy]` 条目数分别统计 |

下一次运行只需在 INI 里加（注意 `Probe` 的输出走 `Key()` 通道，**必须同时开
`LogLevel = 2`**，否则日志里只有启动/钩子/警告几行）：

```ini
[runtime]
Probe    = 1
LogLevel = 2
```

进任务飞 1 分钟、退出后把 `ACZero\ACZPropNav.key.log` 发回，即可据 RTTI/邻域转储
把**运行时字段偏移**钉死（`docs/RUNTIME_PARAMS.md` 同步记录逆向结论），随后
v1.1 就能直接改写运行时参数（`lead` 等）而不再依赖磁盘记录布局。

---

## 5. 尚未验证 / not verified here

1. **游戏内效果**。`acz.exe` 被 SteamStub 3.1 包裹，本机没有可运行的游戏会话（需要 Steam
   登录 + GPU）。因此"参数表在实际进程中被找到并改写""导弹手感变化"属于**待你真机确认**：
   启动一次，检查 `ACZero\ACZPropNav.log` 是否出现 `记录字节扫描成功`；若未命中，按
   [CALIBRATION.md](CALIBRATION.md) 跑一次 `Probe=1` 并回传 `ACZPropNav.key.log`。
2. **真 PN 接管（`HookRva`/`HookSignature`）**。钩子引擎本身已测（70 项），但导弹更新
   函数的地址需要真机/进一步逆向校准。[RE.md](RE.md) 已经给出候选：武器控制器 vtable
   家族（slot 8 = 每型号导引头、slot 9 = 导弹实例数组 `this+0x80`，步长 0x68，16 个槽位）
   与通用制导辅助函数（0x090510 等），并附可直接用于 `HookSignature` 的字节序列。
   由于 ACZ 的普通 MSL 没有 `WeaponEx*Ctrl` 类，普通弹的飞行循环属于基类/非 RTTI 类，
   这是下一步要做的一件事。
3. **特殊武器（QAAM/LAAM/HVAA/4AAM…）**。已从正式版 `a.arc` 证明它们**没有数据记录**
   （`datapack_jp.bin` 对 `mis.wpb` 引用 199 次 = 每架飞机一次；特殊武器全在代码里）。
   因此 5 个属性里 `HomingForesightAmount` 只能作用于普通 MSL / 僚机弹（数据路径），
   特殊武器必须等第 2 项的代码路径启用。

## 6. 游戏安装未被修改 / game install untouched

* 全程只读 `ACZero\`（`acz.exe`、`*.arc`、`settings\` 的修改时间均保持原样，见下）；
* 反编译/解包实验都在 `ACZPropNav\work\` 的副本上进行；
* `install.cmd`/`uninstall.cmd` 只在 `loader-hook` 的临时布局中测试过，没有实际写入游戏目录
  （由你决定何时安装）。*（这两个脚本此后已删除；现在的"安装"就是手工把两个 DLL 复制进
  `ACZero\`，"卸载"是删掉它们。）*

```
ACZero\acz.exe      4,046,008  2026-10-24 22:22   (未变)
ACZero\a.arc      502,706,872  2026-10-07 19:12   (未变)
ACZero\b.arc    1,736,730,816  2026-10-24 22:22   (未变)
ACZero\settings\setting.ini        390  2026-10-24 22:35   (未变)
```

## 7. 真机结果的结构性结论 / structural finding from the live run

对脱壳镜像的静态分析（[RUNTIME_PARAMS.md](RUNTIME_PARAMS.md)，56 KB，逐条标注
VERIFIED/INFERRED/UNKNOWN）给出了**决定性结论**：游戏不会把 `.wpb` 记录（或它的载荷）
原样保存在内存里。它走一个**类型描述符序列化器**——每个字段都成为独立的、带头的成员
（成员头 `{qword hdr, f32@+0x08, meta@+0x0C}`，槽位粒度 `0x18`，相邻字段之间至少隔
`0x18` 字节）。这正好解释了第一次真机日志"1.6 GB 扫描只找到 16 个假阳性"：
字段在内存里根本不相邻。

**因此"按磁盘布局改写参数"在当前版本不可行**（模组会一直保持"未命中即不写"的安全行为），
但同一份分析定位了比例引导真正需要的对象——**活的导弹实例**：

| 结构 | 偏移 | 依据 |
| --- | --- | --- |
| `WeaponEx*Ctrl` 存活掩码 | `ctrl+0x5C`（u32，bit0..15） | VERIFIED |
| 导弹实例槽数组 | `ctrl+0x80 + i*0x68`，16 槽 | VERIFIED |
| ↳ 位置（世界坐标 f32×3） | `slot+0x00` | VERIFIED |
| ↳ 朝向（弧度） | `slot+0x10` / `slot+0x14` | VERIFIED |
| ↳ 每帧重写的 vec4 | `slot+0x20..0x2C` | VERIFIED |
| ↳ 存活/状态字节 | `slot+0x50` | VERIFIED |
| 从任一 `WeaponModel` 反查 ctrl | `manager=obj-0x38`，`ctrl[i]=*(manager+0x1F540+i*8)` | INFERRED（运行时需校验） |

这就是 v1.1 的 PN 接入点：钩住导弹调度器（[RE.md](RE.md) 的 `exe+0x9DBB0`，签名已在
`ACZPropNav.ini` 的 `[runtime]` 注释里给出，`HookArgIndex=1`），在原函数返回后遍历槽数组，
用 `pn.h` 计算期望朝向并写回（受 `MaxRotationAngle` / `MaxHomingAngle` 约束）——
**完全不需要静态参数表**。

### 7.1 为这条路径新增的探针（当年靠 `enable_probe.cmd` 一键开启，**脚本已删除**；现在手工写 INI 的 `Probe=1` + `LogLevel=2`）

`Probe=1` 现在向 `ACZPropNav.key.log` 写入三类以前没有的诊断：

1. `RTTI probe`：按 vtable 解析 `WeaponModel` / `WeaponEx*Ctrl` / `ObjPlane`，先读回
   类型描述符名验证，再在**所有可读区域**找活对象并 dump 浮点字段；
2. `missile slot probe`：按上表配方定位 ctrl 与 16 个槽，打印位置/朝向/存活以及
   `+0x00..0x67` 的完整浮点布局 —— 这一步会直接回答"`slot+0x20` 是不是瞄准点、
   目标向量在哪"；
3. `value neighbourhoods`：1600/3000/40/80 附近 ±0x30 的浮点布局。

这些探针已在合成进程里跑通（`smoke --wait`，无崩溃）；真机上跑一次即可把 PN 适配器的
最后一块补上。

## 8. 真机第二次运行：闪退分析（16:58）与修复 / live run #2 crash

用户第二次启动（`Probe=1`）时游戏**闪退**，且导弹尚未发射。证据链：

```
[16:58:17] ================ PROBE ================
[16:58:45] ---- missile slot probe (WeaponModel vtable 0x7FF6B573E790) ----
[16:58:45]   WeaponModel instances: 11        ← 日志到此为止
Windows 事件日志（Application/1000）:
  出错应用程序 acz.exe 2.0.0.1
  出错模块      ucrtbase.dll 10.0.26100.9711
  异常代码      0xc0000005 (access violation) 偏移 0xee3dd
```

**结论：是模组（探针）导致的，不是游戏本身的问题。** 上一版 Probe 会做两件危险的事：

1. 用 `WeaponModel` 的 vtable 地址在**所有可读内存**里找"实例"——这是**值匹配**，
   必然有假阳性（日志里 `instances: 11` 里就混着假的，后面的浮点 dump 已经出现
   15,365,243,353,956,352 这种明显不是坐标的数就是证据）；
2. 再顺着这些"实例"去 `obj-0x38 → +0x1F540` 追指针、读 `ctrl`。
   假阳性对象会让偏移落到任意页面，最终在 CRT 的拷贝路径上触发访问违例，
   故障落在 `ucrtbase.dll` 的偏移上，异常保护也来不及把它当"可恢复的探测读"处理。

**已修复（本版）**
* 上述两类探针**全部删除**（不再按值/按 vtable 找对象，不再追指针）；
* `Probe=1` 改为**只读观察钩子**：用唯一签名钩住游戏自己的导弹调度器
  （`exe+0x9DBB0`，签名命中校验、只保留一个），在回调里**只读游戏自己传进来的那个
  `this` 对象**（`mask @+0x5C`、`curIdx @+0x60`、`slot = this+0x80+i*0x68`），
  dump 20 次后**自动卸载钩子**；全程只读、不写、不追指针；
* Probe 只触发**一次**（启动 15 s 后），不再在 10/30/60/…/300 s 反复安装；
* **静态参数扫描改为 opt-in**：没有 `[vanilla]` 段就完全不扫描（该版本结构上不可能命中），
  不再有每轮十几秒、1.6 GB 的内存扫描，游戏保持流畅；
* 全部离线测试仍绿：`initest` 268、`pnsim` 15、`memtest` 85、`hooktest` 70、
  `smoke`（含 Probe 路径、`--wait` 触发）PASS。

**怎么避免再闪退**：如果不想再冒险，把 INI 里 `Probe` 改成 `0`（或整段删掉
`[runtime]`），模组就退化为"加载后什么都不做、只写日志"的完全被动状态。
需要继续推进 PN 时再打开 `Probe=1` 跑一次即可。

## 9. 第三次真机运行：抓到活导弹槽位 → PN 已实现（2026-10-25 18:36）

探针（安全版）在真机上成功捕获了**带活导弹的帧**，数据与离线反汇编
（[GUIDANCE_ASM.md](GUIDANCE_ASM.md)）逐字段吻合：

```
probe frame 1: ctrl=0x16A46092098 mask=0x0001 curIdx=0
  slot 0 @0x16A46092118
    +0x00: -15570.8291  7140.4907  77320.3594  1.0000   ← 位置 (m)
    +0x10:     0.9843   -0.4388      0.0000  0.0000   ← 朝向 {elev, azim} (rad)
    +0x20:     3.5943    11.6735     -7.2661  0.0000   ← 速度 (m/帧, |v|≈14.2 → ≈850 m/s)
    +0x30: -14519.4395  8474.5889  76438.4688  1.0000   ← 目标位置 (m, 距离≈2.1 km)
```

由此确定 `MissileSlot`（步长 `0x68`，`ctrl+0x80+i*0x68`，16 槽）的制导字段：
**`+0x00` 位置、`+0x10` 朝向（弧度）、`+0x20` 速度（米/帧）、`+0x30` 目标位置（每帧刷新）、
`+0x52/+0x53` 目标有效性字节**。目标速度引擎不存，按帧间差分估计（游戏自己也是这样做的）。

### 9.1 由此落地的 PN 实现

* 钩子目标改为**共享转向积分器 `exe+0x90510`**（`docs/GUIDANCE_ASM.md` §10）：
  这是整个 homing 家族唯一提交"速度"的地方（写 `slot+0x20/0x24/0x28`），
  普通 MSL 与 `WeaponEx*` 特殊武器都走它，因此**不需要区分弹种**。
* 用一个**运行期生成的 x64 thunk**（`sub rsp,0x58` 帧，保存 `rdx`=slot，
  重放第 5 个栈参数，先 `call trampoline` 跑完游戏原逻辑，再 `call` 我们的 C 处理器），
  参数寄存器完全保真 —— 这是它能安全挂钩的关键。
* C 处理器 `OnMissileSlotUpdated()`：读 slot → 用 `pn.h` 的 `ComputeGuidance()`
  算拦截点方向（`HomingForesightAmount` 0..1 控制前置量、`MaxRotationAngle` 限速、
  `MaxHomingAngle` 限视场）→ **保持速度模长不变、只改方向**，写回 `slot+0x20..0x2C`。
* 安全边界：只写速度三连（+0x2C 清 0），不碰位置/状态/碰撞；每帧转角受
  `MaxRotationAngle × dt` 限制（默认 45°/s ⇒ 0.75°/帧）；`DryRun=1` 只记录不写；
  `GuidanceOverride=0` 完全不挂钩。

### 9.2 验收方式

```
复制新 DLL 到 ACZero\ + INI 手工设为 Probe=0 / GuidanceOverride=1 / LogLevel=2
                       （当年是跑 enable_pn.cmd，该脚本已删除）
进任务发射 MSL         → ACZero\ACZPropNav.log 应出现：
  PN 积分器钩子已安装 / PN integrator hooked at exe+0x90510 (stole 8 bytes, thunk 0x…)
  PN applied: frames=… range=… m los=… deg turn=… deg speed=… m/s tGo=…
```

## 10. 真机第七轮：MSL 路径取证（2026-10-25 22:5x）

**已验证可用**：`WeaponEx*` 家族（QAAM/SAAM/XLAA/XMAA/XAGM）的比例引导。现场证据：
勾住 `exe+0x90510` 后，QAAM 飞行中每帧的 slot 数据（位置/速度/目标）都在变化，
`PN applied` 连续输出，`frame=16.67 ms`、`integratorCalls/applied` 同步增长。

**已证明 MSL 不在该路径**：MSL-only 会话里 `integratorCalls` 保持不变（1148 → 1148），
而 QAAM 会话里它从 0 涨到 586。

**已定位 MSL 的运行时容器**（`docs/MSL_GUIDANCE.md`）：单例 `exe+0x3219A0`，
0x70 字节节点，三条侵入式链表（`+0x20` / `+0x98` / `+0x290`），
状态字节 `+0x6B`（发射=2，飞行/拖尾=6）。现场 dump 到活的节点：

```
LIVE node 0x… list+0x98 state=2 slot=0 ids=0100 raw:
  +0x30: 0.000  -34783.137  1983.061  -19741.242    ← 位置（+0x34..+0x3C）
  +0x40: 0.156    2.807     -0.444    43.759        ← 方向/速度三连 + 标量
```

**仍未闭环**：MSL 节点每帧推进它的**写入者**（以及目标向量来自哪里）。
需要一次带写断点的调试（x64dbg `ba w4` 于 node+0x34 / node+0x40）或更长的采样，
才能拿到写入指令的 RVA 并据此做 PN 接管。在此之前 MSL 保持原版行为。

**探针噪音问题已修**：调度器实时槽 dump 之前会淹没 key.log（当时的 250 KiB 上限导致轮转，
现在上限是 512 KiB），
现在限速到 1 Hz；`disp: LIVE slot` 只有在真的存在活弹时才打印。

## 11. 真机第八轮：弹丸池节点是"发射记录"，不是飞行实体（2026-10-25 23:1x）

现场观察（用户亲测）：每一发 MSL 对应**不同的** `MSL node 0x…` 地址，而且**发射后该地址的内容不再变化**
（`MOVE` 行只有 `f0` 这种指针字段变化）。

结论：`exe+0x3219A0` 池里 `state=2` 的 0x70 节点是**发射时的快照/请求记录**（位置、初始方向、剩余寿命被写入一次），
**不是每帧推进的飞行实体**。飞行中的导弹由另一个对象表示（更可能是 world 对象/效果系统里的一份拷贝）。

因此"在池节点上打写断点"找不到每帧写入者 —— 这也解释了为什么 Cheat Engine 里值冻结不动。

下一步（两条路，任选）：
1. **自动路线**：钩住 `EFF_MIS_POSDIR` 环的写入点（`exe+0x3B370` / `0x3B300`，队友已给唯一签名），
   记录**返回地址** → 那个 return address 就是"每帧把导弹 pos/dir 推给渲染"的调用者，
   顺着它的调用者链就能找到飞行实体的更新函数（对所有弹种成立，含 MSL）。
2. **手动路线**：在飞行中的导弹 `pos` 上打硬件写断点（需要先知道飞行实体地址，
   可由路线 1 的环条目反查）。

在任一条路走通之前，普通 MSL 保持原版行为；`WeaponEx*` 家族（QAAM/SAAM/XLAA/XMAA/XAGM）的 PN 已可用。

## 12. PAGE_GUARD 写断点尝试：**失败并导致游戏崩溃**（2026-10-26 00:32）

为自动找出"每帧推进 MSL 节点"的指令，我实现了 PAGE_GUARD + VEH 的写监视
（替代 x64dbg 的写断点），并在 `Probe=1` 时对环探针发现的"变化位置"地址装 guard。

**结果**：
```
Windows 事件 1000 / 2026-10-26 00:32:28
  出错模块   ntdll.dll
  异常代码   0x80000001  (STATUS_GUARD_PAGE_VIOLATION)   ← 未处理的 guard 页异常
```
即：有一个 guard 页违例**没有被我们的 VEH 接住**，最终把进程打崩。
第二局（00:33）guard 装上了但 **0 次命中**（所监视的地址其实从未被写），也没有再崩溃。

**处置**：已**彻底移除** guard 的武装逻辑（`ArmPageGuard` 保留但不再调用），
Probe 模式恢复为纯只读；普通模式不受影响。离线测试全绿。

**结论**：PAGE_GUARD 这条路在当前实现下不安全，不再使用。MSL 的"每帧推进函数"
仍是唯一缺口，后续若继续，只能走外部调试器（x64dbg/CE）在**飞行中的节点**上打写断点，
或从渲染侧 `EFF_MIS_POSDIR` 的写入点顺调用者链向上找。
