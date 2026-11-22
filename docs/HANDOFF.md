# 交接文档 / HANDOFF

> 目标读者：接手这个项目的下一个人（或下一个 session 的我）。
> 读完这一页 + `docs/MSL_PN.md`，就能在 10 分钟内回到现场。
>
> 维护状态：v1.0.1 已发布（公开仓库，两条制导路径都经过真机验证）。
> 本页只保留**当前仍然成立、且不写下来就会重做**的东西；历史推演过程已随
> 15 份逆向笔记一起删除（见 `docs/INDEX.md` 末尾的说明）。

---

## 0. 一句话现状

| 项 | 状态 |
|---|---|
| AC8 兼容的 INI（`[player]`/`[enemy]` + 五个参数 + 逐型号块） | ✅ 完成，`initest` 覆盖 |
| 加载器 / 安装 / 卸载 / 日志 | ✅ 完成（安装=手工复制 DLL，见 §2） |
| **特殊武器（QAAM/SAAM/XLAA/XMAA/XAGM）比例引导** | ✅ 真机验证（`exe+0x90510` 积分器钩子；QAAM 6 发全命中） |
| **普通标准导弹（MSL）比例引导** | ✅ 真机验证（`FUN_14013ECA0` 后置钩子） |
| 离线测试 | ✅ 全绿：`pnsim` 69 条断言（本机可跑）+ `initest`/`memtest`/`hooktest`/`hwwatchtest`/`smoke`（Windows PE） |
| 朝向接管（让弹体模型跟着 PN 方向转） | ⚠️ 代码在、**默认全程关闭**：旋转矢量幅值约定未定（§7） |

历史上的"普通 MSL 未完成"早已解决；当年**如何**找到 `0x13ECA0` 的过程只留在
git 历史里，结论在 §3 与 `docs/MSL_PN.md`。

---

## 1. INI 约定（**不能破坏**）

`ACZPropNav.ini` 的语法与语义必须与 AC8 的 `AC8PropNav.ini` 保持一致 —— 这是本项目的
产品契约，`initest` 拿真实 AC8 INI 逐项对照。

```ini
[player]
msl = { HomingForesightAmount = 0.7      ; 0 = 纯追踪, 1 = 满额 PN
        MaxHomingAngle        =          ; 导引头视场半角（度）
        MaxRotationAngle      =          ; 每帧转角上限（度/秒）
        LifeTime              =          ; 秒
        LockonRange           =          ; 米 }
[enemy]
msl = { HomingForesightAmount = 0.3 }
```

规则（改动任何一条都是 breaking change）：

* 留空 / 不写 = 该型号保持原版；`msl = 0.8` 旧写法 = 只设 `HomingForesightAmount`；
* 型号键大小写不敏感、**最长键优先**（`4aam` 不会被 `aam` 抢走）；
* 属性名固定五个，未知属性只警告、忽略；
* 文件是 **UTF-8 带 BOM**、注释为中文；解析器 BOM 感知、CRLF/LF 都吃，
  但**多一个 BOM 会产生 `INI: line 1: text outside of any section` 警告**。

`[runtime]` 是本模组的扩展段（`LogLevel` / `DryRun` / `Probe` / `HwWatch` /
`RingSurvey` / `GuidanceOverride` / `PnGain` / `TerminalZone` / `LeadScale` /
`LeadMode` / `HookRva` / `HookSignature` …），出厂 INI 把整段**注释掉**，而代码
默认值就是"正常游玩"的配置。逐项说明在 INI 文件自己的中文注释里。

---

## 2. 交付物与安装

发布包是**一个 zip，内容直接解压进 `ACZero\`**（`acz.exe` 所在目录）：

```text
ACZero\
  ACZPropNav.dll      主 DLL（导出 AczCoreRun 与社区 AczAddonInit）
  XINPUT1_4.dll       代理加载器（转发系统 XInput 的全部导出）
  ACZPropNav.ini      配置（UTF-8 + BOM，中文注释）
  README.md  README.en.md  LICENSE  RELEASE_NOTES.md
```

* 安装 = 解压进 `ACZero\`；卸载 = 删掉两个 DLL（INI 与日志是用户数据，留着）。
* **没有安装脚本**。原来的 13 个 `.cmd`/`.ps1`（`install.cmd`、`enable_pn.*`、
  `probe_*.*` …）已随 1.0 发布全部删除；它们做的两件事（复制 DLL、改 INI）
  现在由用户手工完成。
* `VERSION.dll`（社区 modloader 用的备用代理）仍可从 `loader/` 构建，但不随包发布。
* 发布的构建与打包由 `.github/workflows/` 与 `tools/package_release.sh` 负责，
  见 `docs/RELEASING.md`。

---

## 3. 已验证的技术事实

版本：`ACZero\acz.exe` 4,046,008 B，SHA-256 `3802B0DC…`，image base `0x140000000`。
**所有地址都是 RVA（= VA − 0x140000000）**。Steam 更新会移动一切，届时钩子会因入口
字节校验失败而**安全放弃**（导弹退回原版行为，不崩游戏）。

### 3.1 两个钩子目标

| | 普通 MSL | 特殊武器（QAAM/SAAM/…） |
|---|---|---|
| 钩子 | `FUN_14013ECA0` @ `RVA 0x13ECA0` | 共享转向积分器 @ `RVA 0x90510` |
| 修改内容 | 导弹**位置**（后置）；姿态写入默认关闭（§7） | 导弹**速度矢量**（后置，保持速率大小） |
| 入口字节 | 15 B `48 8B C4 \| 48 89 58 08 \| 48 89 70 18 \| 48 89 78 20`（边界 `0x13ECAF`） | 8 B `48 8B C4 C5 FA 11 58 20`；唯一化用 12 B 形式（后缀 `C5 FA 11 50`） |
| 权威文档 | `docs/MSL_PN.md` | `docs/MSL_PN.md` §10 + 本节 |

### 3.2 普通 MSL：`FUN_14013ECA0` 的调用约定

`ctrl`（RCX）= **世界级武器管理器** = `world+0x6A130`，**每帧一次**（不是每架飞机一次）。
世界指针 `world = *(u64*)(base+0x3409F8)`；玩家索引 `*(u8*)(base+0x3409F2)`。

| 位置 | 内容 |
|---|---|
| `ctrl+0x6E8` | `u32` 活弹位图（bit i = 槽 i） |
| `ctrl+0x6EC` | `u32` 引擎帧率常量（本版本恒为 **60**） |
| 槽 | `ctrl+0x158 + i*0x58`，`i = 0..15` |
| `slot+0x00` | 位置 vec4（`+0x0C` = 1.0f）；**积分器把每帧位移加到这** |
| `slot+0x10` | 姿态（三段欧拉角；见 §7 的分歧） |
| `slot+0x20` | 每帧位移 **输出缓存**（游戏每帧重算 → 只写它不改变轨迹） |
| `slot+0x30` | `qword` → `P`（`.wpb` 记录指针） |
| `slot+0x38` / `+0x3C` | 飞行距离 / 转速累加 |
| `slot+0x40` | `u16` 尾迹句柄 |
| `slot+0x42`/`+0x43` | 目标 (obj, part) 索引，`0xFF` = 无 |
| `slot+0x44`/`+0x45` | 发射者 (obj, part) 索引 |
| `slot+0x46`/`+0x47` | 标志位 |

目标位置解析：
`objArray[tObj] -> +0x110`（部件数组，步长 `0x520`）`+ 0x520*tPart + 0x258`。
`objArray` 优先取游戏自己的 ctrl 相对路径 `*(u64*)(*(u64*)(ctrl+0x08))`（读点 `0x13ED70`），
回退 `*(u64*)(world+0x697B8)`；对象数 `world+0xA87E4`。
**目标速度不存在于任何结构里** —— 游戏自己也是逐帧差分。

### 3.3 武器参数记录 `P`（`.wpb`）

`P = *(u64*)(slot+0x30)`，`P = .wpb + 0x10`，校验 `*(u32*)(P-0x10) == 0x10`。

| `P+` | 含义 |
|---|---|
| `0x14` | `range`（最大飞行距离，m）；射程 kill 用 `range*5.12` |
| `0x20` | `scope`（锁定距离，m） |
| `0x2C` | **`lead` = 每帧最大转角，单位 度/秒**（见 §3.4） |
| `0x44` | `u16` speed（m/s，发射用） |
| `0x48` | `u8` type（2 = 导弹） |
| `0x4A` | `startAng` 导引头视场**半角**（度，90 ⇒ 180° 全视场） |

### 3.4 关键单位 / 常量（踩过坑，别再猜）

* **位置单位不是米**：1 m = **5.12** 游戏单位（射程 kill 处常量 `0x1402D46B0` = 5.1199999
  证实）。实机日志 `|v| ≈ 60.4` 单位/帧 ≈ 11.8 m/帧 ≈ 707 m/s。
  ⇒ 帧时间只能取引擎的 `1/60`（`ctrl+0x6EC`），**绝不能用 `|v|/speed` 反推**（旧代码
  这么做把每帧转角预算放大了 3 倍：填 90 实际得到 270）。
* `P+0x2C`（`lead`）的语义是 `maxTurnPerFrame = lead·π/180 / fps`，即**度/秒的转速上限**，
  不是"提前量"。原版数值：玩家 MSL 40、僚机 80、`mis_old` 10 —— 和 AC8 的
  `MaxRotationAngle` 原版值（MSL 40 °/s）完全对得上。
* 特殊武器槽：`ctrl+0x80 + i*0x68`（16 槽），活弹位图 `ctrl+0x5C`（u32）；
  `slot+0x54` = 秒龄，`slot+0x5C` = 自身对象句柄（u16，`&0x3FF`，`0xFFFF` = 无）。
  **LASM 的槽步长是 `0x78`**（派发器 `exe+0x9D550`），不要用 `0x68`。
* 特殊武器的共享积分器有 7 个调用者：`0x9165D 0x99B50 0x9AFB0 0x9D168 0x9E030 0x9E990 0x9F230`；
  每发导弹更新 `exe+0x91DC0`，派发器 `exe+0x9DBB0`（vtable 槽 9）；
  导引头视场门限 `exe+0x90A60`，`tGo` 估计 `exe+0x90950`（`k = 0.5·tGo`），
  原版转角钳位 `±1.42222 rad/s ÷ fps`。
* 特殊武器 vtable：QAAM `0x2AF740`、SAAM `0x2AFAE8`、XMAA `0x2AF308`、XLAA `0x2AF560`、
  XAGM `0x2AF980`、`WeaponExFireCtrl` `0x2AF470`、`WeaponModel` `0x2AE790`。
  继承链 `WeaponEx*Ctrl → WeaponExCtrlBase → WeaponRef`，**只有 vtable 槽 7/8 被各类重写**，
  槽 8 = 导引律。
* 所有钩子都在运行时**逐字节校验**目标入口，对不上就放弃安装、只记日志。

### 3.5 静态参数路径（`.wpb` 扫描）**默认关闭**

游戏会把 `.wpb` 记录**重新序列化成引擎结构**（每个字段独立带头、成员槽粒度 `0x18`），
所以"按磁盘 96 字节布局扫内存"在这个版本**必定不命中**，而且很贵。
因此静态参数路径只在 INI 里出现 `[vanilla]` 段时才启用（`Patcher::Start`），
否则一行日志说明后完全跳过。这是故意的设计，不是没写完。

---

## 4. 环境事实（踩过的坑，别再踩）

1. **`acz.exe` 被 SteamStub 3.1 包裹**：`.bind` 在导入解析完之后才解密 `.text`。
   我们的 DLL 在导入解析期间就已驻留，所以**任何代码字节校验都必须延后重试**
   （`Patcher::Tick` 每 200 ms 重试、最多 60 s；RingSurvey 每 500 ms、最多 120 s）。
   这是"integrator bytes mismatch"的唯一原因。
2. **静态分析用解包副本**：`work/acz_copy.exe.unpacked.exe`（3,811,952 B，
   image base `0x140000000`，section RVA 与活进程一致）。
3. **沙箱不能写 `ACZero\`**（workspace-write 只允许工作区）。改游戏目录、复制 DLL、
   改 INI 都必须由用户手工操作。
4. **INI 是 UTF-8 + BOM，只能按字节安全地改**：BOM-less 另存会被按 GBK 解码成乱码
   （曾经写坏过）。规矩是：用能保留 BOM 的编辑器存成 UTF-8（带 BOM），
   不要整体改行尾，改前先复制一份 `.bak`。
5. **`llvm-objdump` 不接受 `-b binary`**：验证手写机器码要用 `clang -c` 汇编等价指令再
   逐字节比对（`hooktest` 就是这么钉死两个 prologue 的）。
6. **Ghidra/REA 时好时坏**：有的 session 每次调用都超时，所以逆向主要走 `tools/re/*.py`
   （单次查询约 0.5 s，不需要 Ghidra）。
7. **两次真实崩溃，都是探测太激进**，教训已成规矩：
   * 追指针式"按 vtable 值找实例"→ `ucrtbase` 0xc0000005（假阳性 → 任意页面）；
   * `PAGE_GUARD` 写监视 → unhandled guard fault（`0x80000001`，ntdll）。
   **现在的规矩：只读、有界、单层指针、验证过再读，失败就放弃。** `ArmPageGuard` 的
   挂载逻辑已删除；数据观察一律用硬件读断点（one-shot，VEH 里清 `Dr0..3/Dr6/Dr7` 再
   `CONTINUE_EXECUTION`，约 100 ms 后重新 arm；逐线程 `SetThreadContext`）。
8. 日志里的 `caller` 是**返回地址**；判断"是不是特武"用区间 `0x90000..0xA1FFF`
   （7 个 WeaponEx 更新函数都在里面）。
9. **`LogLevel` 语义**：空白/不写 = **1**（安静：启动、钩子状态、警告与错误）；
   `0` = 完全不写日志内容；`2` = 详细（2 秒汇总 + 逐条修正明细）。
   默认是 1，所以"日志里什么都没有"通常是正常的。

---

## 5. 已排除的路线（含为什么，别重复）

| 路线 | 结论 |
|---|---|
| 直接改 `.wpb` 参数表（AC8 的做法） | ❌ 运行时记录被重新序列化（§3.5）；且 `lead` 是转速上限，**改它不会产生提前量** |
| 在投射物池节点上写速度 | ❌ 池节点是**发射快照**，没有每帧写入者（穷举证明）。单例 `exe+0x3219A0`，`0x70` 字节节点，三条侵入式链表 `S+0x20`/`S+0x98`/`S+0x290`，状态字节 `+0x6B`，6 个分配点 `0x1254A2 0x12B631 0x13B09B 0x13D665 0x1412B4 0x1C7080` |
| 尾迹环 `EFF_MIS_POSDIR` 反推制导 | ❌ 环 vtable `0x2ACC68`，对象 = `CMissionControl+0x240`，条目 `mc+0x248 + idx*0x20 = {pos vec4, dir×3 + phase}`，写入者 vtable 槽 2/3 = `0x3B300`/`0x3B370`。这些全是**尾迹特效**的 update：位置只是 `rcx` 传进来的拷贝，`entry[3]` 是尾迹相位，既没有速度也没有目标 ⇒ 只能回答"导弹在哪"，不能回答"怎么转向" |
| 按世界/对象模型找导弹飞行实体 | ❌ 普通 MSL 是 `ctrl+0x158+i*0x58` 槽式、四元数/欧拉积分的弹丸，**绕过** `0x1B9A80` 武器调度器（帧计数 `0x33ED94` 全程为 0），也不在世界对象数组里 |
| 钩 `0x91DC0`（每发导弹更新） | ❌ 只覆盖 5 个 `WeaponEx*` 类，普通 MSL 不经过；且它是**终端命中/销毁**那一半，不写速度 |
| `PAGE_GUARD` 写监视 / 追堆指针 | ❌ 崩过两次（§4.7），不要再提 |

---

## 6. 诊断开关（都在 `[runtime]`，正常游玩不需要碰）

| INI | 作用 | 是否只读 |
|---|---|---|
| `Probe = 1` | "Run A" 探针：三个观测钩子（池 Allocate `exe+0x201460`、池节点弹出 `exe+0x1B2C8`、每帧 world tick `exe+0x1B7B00`）+ Tick 采样器，写 `PROBE-A:` 行 | 只读（只装代码钩子） |
| `HwWatch = 1` + `HwWatchSeconds` | "Run B" 探针：对活记录的 `P+0x2C/0x48/0x14/0x44` 下 one-shot 硬件**读**断点，报出读取指令的 `rip=exe+0x…`。**`0x13ECA0` 就是它找出来的** | 只读 |
| `RingSurvey = 1` | 观察 `EFF_MIS_POSDIR` 尾迹环的写入者（已判死路，仅留作 detour thunk 的活体验证） | 只读 |
| `DryRun = 1` | 算但不写 | 只读 |
| `GuidanceOverride = 0` | 总开关：完全不接管制导 | — |
| `TerminalZone = 0` | 0 = 全程 PN；正数 = 末端该半径内交还原版（仅供 A/B 对比） | — |
| `PnGain` | 导航常数 `N`（默认 4） | — |
| `LeadScale` / `LeadMode` | **旧静态参数路径**的遗留旋钮，只在 `[vanilla]` 段存在、静态记录真的被找到时才起作用 | — |

`Probe` 与 `HwWatch` 都会抢占分配器钩子，所以两者不建议同时开（Run A 优先）。

---

## 7. 仍未接入 / 仍开放的（明确说清楚）

* **朝向接管（Step 1B）默认全程关闭。** 位置修正是对的、弹道是对的；把槽的姿态按 PN
  方向旋转这一步，卡在 `FUN_1402036D0`（RVA `0x2036D0`，rotvec → 欧拉角）的**幅值约定**
  上：命令转 0.575°，四个变体却转出 9.9/11.0/9.9/11.0°（比值 17–19），说明我们塞进去的
  弧度制轴角不是它期望的东西。运行时自动标定因此 `ok=0` 并**静默关闭**朝向写入
  （只影响**弹体视觉朝向**，不影响弹道）。日志里会打
  `MSLPN: STEP1B orientation calibration: … ok=<0|1>` 与 command/variants 角度比值；
  比值若稳定为常数，就是只差一个缩放。
* **`slot+0x10` 到底是三段欧拉角还是四元数，历史文档互相矛盾。** 代码按**欧拉角**实现
  且 `ok=0`，所以这条分歧目前**不影响**已验证可用的配置。判定方法同上：`ok=1` 说明欧拉角
  理解正确；`ok=0` 就需要重新查。附带分歧：积分是 `+= speed*forward` 还是
  `delta = -speed*axis`，标定用 `|dot|` 匹配把符号问题掩盖了。
* `LifeTime` / `LockonRange` 在 PN 路径上**没有接线**：射程/寿命 kill 和 HUD 锁定圈仍由
  原版负责（PN 只改位置与姿态，不改 `P` 记录）。特殊武器路径同理。
* `MaxRotationAngle` 生效于两条 PN 路径的限速，但**没有真正写回 `P+0x2C`**；
  若要"改数据就生效"，那是另一个方向（需在加载点重写 `P`）。
* 特殊武器的 PN 仍然每帧受原版 `±1.42222 rad/s` 钳位影响（我们只改速度方向、不改幅值）。

---

## 8. 外部代码评审的核实记录（2026-11-22，别重做）

收到一份外部审核报告（12 项）。逐条核实后：**6 项真并已修，3 项已被更早的修复覆盖或本就安全，
3 项为建议（部分采纳）**。

### 8.1 确认并修复的真 bug

| # | 位置 | 问题 → 修法 |
|---|---|---|
| ① | `src/core/mod.cpp` | **线程句柄二次关闭 + 卸载期死锁**：`StartOnce()` 关闭 `g_thread` 却不置空，`DLL_PROCESS_DETACH` 又对同一句柄 `WaitForSingleObject`+`CloseHandle`（句柄可能已被复用）；且进程终止时（`lpvReserved != nullptr`）持 loader lock 等待工作线程是经典死锁。修：`StartOnce` 不关句柄；DETACH 只在 `lpvReserved == nullptr`（真正 `FreeLibrary`）时等待+关闭 |
| ② | `src/pn/msl_steer.h::MatrixAxis` | 行候选步长写成 3（应为 **4**）：`m[3*r]` 混进第 4 列并完全漏掉真正的第 2 行。列候选本来就用步长 4，所以朝向标定通常选列候选、从未触发。修：改 `m[4*r]`，新增 `pnsim` (j7b)/(j7c) 用"每个元素值不同"的矩阵钉死步长 |
| ③ | `src/pn/msl_pn.cpp::CacheFind` | 64 槽 `TargetCacheEntry` 满后直接 `nullptr` ⇒ 大型关卡（`tObj` 实测已达 17–64）之后新锁定的目标全拿不到速度估计，退化成无提前量拦截。修：按 `lastFrame` **LRU 淘汰**；同时"位置瞬移"不仅钳位，还**丢弃 EMA 历史**，避免一次瞬移连续污染多帧 |
| ④ | `src/common/acz_common.h::RotateTowards` + `ComputePn` 纯追踪项 | 两向量恰好反向时 `Cross` 为零、`Normalize` 失败、旧代码返回原方向 ⇒ **导弹直飞不转弯**。修：此时任取一个垂直轴按限速转一步。回归断言 `pnsim` (k14a–c) |

### 8.2 评审提出但已覆盖 / 本就安全

| # | 项 | 结论 |
|---|---|---|
| ⑤ | `WaitTramp()` 竞态与 trampoline 发布顺序 | 已在更早的修复里解决：`*trampoline = trampMem` 在 `WriteJumpLocked` **之前**，失败路径逐个回滚置空 |
| ⑥ | `AczAddonInit` 硬编码 ABI 越界 | 本就安全：全部走 `mem::IsReadable` + `mem::Read`（内核中介的 ReadProcessMemory，非法地址返回 false 而不抛异常）+ `wchar_t buf[512]` 有界拷贝，不是裸解引用。（"加版本号/大小校验"的长期建议记在 §8.4） |
| ⑦ | `FlushInstructionCache` 缺失 | 本就调用，共 6 处（relay 写入后、跳板写入后、prologue 打补丁后、卸载还原后） |
| ⑪ | `loader/proxy_common.h` DLL 搜索劫持 / loader-lock | 本就正确：真实系统 DLL 用 `GetSystemDirectoryW` 拼绝对路径再 `LoadLibraryW`，不依赖搜索顺序；`DllMain` 内不 `LoadLibrary`（ATTACH 只做 `GET_MODULE_HANDLE_EX_FLAG_PIN` + 建 event + `CreateThread`），真正的加载在工作线程 |

### 8.3 部分采纳

| # | 项 | 处置 |
|---|---|---|
| ⑧ | `ComputePn` 的 `gameFps` 非 60 漂移 | 评审假定调用处传死 60。实际每帧从 `*(u32*)(ctrl+0x6EC)` 读引擎自己的帧率常量（`g.gameFps`），无需改动 |
| ⑨ | `msl_steer.h::Compute` 与 `ComputePn` 冗余 | 确认：`Compute` 是 Step-1 前置点公式，**只被 `pnsim` (i1)–(i9) 当参考模型**，生产走 `ComputePn`。已在 `Compute` 上方加 **DEPRECATED** 注释，说明它是测试参考、不要改 |
| ⑩ | 没有调试符号可供分析崩溃 | 采纳：`./build.sh debug` 产出未 strip 的 `-O1 -g` 到 `build/debug/`（含 debug 段），发行版仍 strip |

### 8.4 暂不采纳（backlog）

* 给 addon ABI 加版本号/大小校验 —— 合理，但宿主 ABI 是逆向出来的既定事实，加字段要社区
  loader 配合，收益低于风险；已有 fault-safe 读层保护。
* README 标注已验证的 `acz.exe` SHA-256 —— 每次 Steam 更新都会变，而入口字节校验本就会在
  版本不匹配时安全放弃。可后续加。
* `hook.cpp` 中段解码器（REX/VEX/EVEX、RIP 相对判定）**仍缺一次完整审计**：`hooktest` 覆盖了
  安装/卸载/拒绝不安全序言与两种代理转发，但没有对全部 x86-64 前缀组合做穷举。
  风险可控 —— 解码器是**保守**策略，任何不认识的编码都让 `Decode` 返回 0、
  `Install` 拒绝安装并记录原因，后果是"放弃 Hook（导弹退回原版）"而不是打错补丁。
  值得补一组 fuzz/穷举测试。

---

## 9. 逆向工具（Ghidra 不可用时的替代品）

| 工具 | 作用 |
|---|---|
| `tools/re/xref.py 0x90510` | 快速交叉引用：直接 call/jmp、RIP 相对操作数、数据区绝对指针，按 `.pdata` 归属到函数。首次 ~0.5 s，结果缓存在 `work/xref_cache.json` |
| `tools/re/vtable_diff.py bases/dump/diff` | 走 RTTI：COL → CHD → 基类链，逐槽 dump，多类并排 diff |
| `tools/re/rtti_scan.py` | 静态 MSVC RTTI 扫描：`.data` 里的 RTTI 是**明文**的，可恢复 `name → vftable RVA`（本版本 387 个类 / 404 个 vftable）。`acz.exe` 的 `.text` 被加密，但这条链不需要解包 |
| `tools/re/{dump_vtable,owners,xref_consts,pe_report,iat,find_ptrs,build_func_index,find_node_writers}.py` | vtable dump / 所有者 / 常量引用 / PE 报告 / IAT / 指针 / 函数索引 / "谁在 +off 上写 float" |
| `tools/arc/arc_tool.py`, `wpb_dump.py` | `.arc` 解包与记录解码（见 `docs/ARC.md`、`docs/VANILLA.md`） |
| `tools/gen_vanilla_header.py` | 从解包档案生成 `src/core/vanilla_records.h` |

这些工具都依赖 `work/` 里的解包副本（`work/acz_copy.exe.unpacked.exe`、
`work/acz_rtti.tsv`）。`work/` 是 gitignored 的本地草稿，**不会出现在 clone 里**，
需要用 `tools/arc/arc_tool.py` + Steamless 自己重建。

---

## 10. 接手后前 10 分钟

1. `./build.sh test` → 必须全绿（Linux 无 Wine 时它只会编译并跑原生 `pnsim --selftest`，
   并明确说其余测试没跑；那不是全绿，只是部分验证）。
2. 读 `docs/MSL_PN.md`（两条路径的机制、公式、日志字段）和本页 §3。
3. 确认 `ACZero\ACZPropNav.ini` 的 `[runtime]` 是"正常游玩"状态
   （`GuidanceOverride=1`、`Probe=0`、`RingSurvey=0`、`HwWatch=0`、`DryRun=0`）。
4. 看最近一次日志：`ACZero\ACZPropNav.log`（主）与 `ACZero\ACZPropNav.key.log`
   （`MSLPN:` 行；注意默认 `LogLevel=1` 时后者几乎没有内容）。
5. 要改行为就改 `docs/MSL_PN.md` 指向的 `src/pn/msl_steer.h`（纯数学，`pnsim` 可测），
   **不要在 detour 里写数学**，detour 里也不要打日志。
6. 任何新的探测都遵守 §4.7 的规矩。两次崩溃的代价已经付过了。

### 16.6 内存读层：`longjmp` 出 VEH 处理器在 x64 上会崩（CI 首次暴露）

**症状**：Windows CI 上 `memtest` 段错误，且**没有任何输出**（连 banner 都没有）。
先加 `setvbuf(stdout, nullptr, _IONBF, 0)` 让崩溃前的输出不丢，再加 `MEMTEST_TRACE=1`
逐调用打印，才定位到：

```text
- fault-safe ReadBytes
      [trace] ReadBytes(0, buf, 4)
      [trace] ReadBytes(1, buf, 4)  <-- first real fault
Segmentation fault
```

也就是**第一次故意制造非法访问就死了** —— 保护机制本身没生效。

**根因**：原设计是"裸 `memcpy` + 进程级 VEH 处理器 + `setjmp/longjmp` 恢复"，
`ReadProcessMemory/WriteProcessMemory` 只是 `EnsureGuard()` 失败时的兜底。
两个问题：

1. **从 VEH 处理器里 `longjmp` 不是受支持的 x64 展开路径**。它跳出处理器帧，
   却没有走异常分发器自己的记账，所以进程常常直接死而不是"恢复"。
2. 即便侥幸能用，也意味着模组在**游戏线程上每秒触发并吞掉几十次真实硬件异常**
   —— 在别人的进程里这不该是常规操作。

另外 `t_frame` 原本是**非 volatile** 的普通指针：`-O2` 下编译器可以把它的存储
下沉到 `memcpy` 之后（memcpy 是它看得懂的内建函数），此时处理器读到空指针就
直接放行 AV。这条在游戏里之所以没暴露，纯粹是因为三次实机测试里模组读到的指针
**恰好都是有效的**，热路径从未真正触发过。

**修法**（三层，按可靠性排序）：

1. **主路径改为内核中介**：`CopyFromTarget/CopyToTarget` 先走
   `ReadProcessMemory/WriteProcessMemory` —— 内核会做范围校验，非法地址**返回 FALSE
   而不在我们线程里抛异常**。
2. `g_isSelf && EnsureGuard()` 的 VEH 路径降级为**只处理"部分可读范围"**的兜底
   （内核会因为范围里有一页不可读而整体拒绝，这时再用 guard 取回合法字节，
   保持旧行为）。→ **这一步后来被证明是错的，已在第二轮评审后彻底删除。**
3. `t_frame` 改 `thread_local GuardFrame* volatile`，并在两次 `memcpy` 前后加
   `__sync_synchronize()`，保证帧在故障发生前已真实写入内存。

> **后续（2026-11-22 评审）+ 修正**：上面第 2 步的"部分可读范围兜底"**不能成立**。
> `GuardedRead` 一旦真的触发 fault 就 `return false`，而 `ReadProcessMemory` 对**同一批
> 地址**也是 `return false` —— 两者结果**完全等价**，兜底没换来任何行为差异，只是把
> "在游戏线程里抛一个真实硬件异常"重新放回了路径上。所以整个 VEH / `setjmp` /
> `longjmp` 机制连同 `EnsureGuard`、`t_frame`、`GuardFrame`、`g_isSelf`、`ArmWindow`、
> `AczGuardHandler`、`<csetjmp>`、`ACZ_NOINLINE`、`kGuardPageViolation` 已**全部删除**。
> `CopyFromTarget/CopyToTarget` 现在就是两行内核调用，没有任何回退分支。
> 这就是"部分可读范围"的真正处理方式：**整体拒绝**。对本模组是安全的 —— 所有调用方
> 都用返回值判断，没有一个依赖"部分字节可用"。

**代价核算**（按真实会话的计数，不是猜的）：13707 次 `Apply()` / ~505 s，
10755 次活槽遍历，每槽约 10 次读 ⇒ **约 213 次内核拷贝/秒**。按 1–2 µs 计
= **单核的 0.02–0.04%**。正确性远比一微秒重要。

**顺带**：`docs/ARCHITECTURE.md` 原先写"**Everything SEH-guarded.** `__try/__except`
around every dereference" —— 这**是假的**：该工具链根本不生成 MS 异常处理器
（无 `__C_specific_handler`、无 `.xdata` funclet，实测 AV 不被捕获）。
已改为如实描述"内核中介 + VEH 仅作兜底"，并把 `SEH-safe` 字样从 `src/mem/*`、
`src/core/probe*`、`src/core/mod.cpp` 和 `memtest` 的用例名里一并去掉。

**教训**：一个在实机三次都没崩的保护层，未必是好的 —— 它可能只是从未被走到。
CI 的价值就在这里：Windows PE 测试是唯一能跑 `memtest`/`initest`/`hooktest` 的地方，
而 Linux 本机只能跑 `pnsim`。

### 16.7 CI 从红到绿：四个真实问题的修法

CI 第一次真正跑起 Windows PE 测试，连续暴露了四个问题。按发现顺序：

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | `build.sh: unzip: command not found`（line 176） | `ci.yml` 的 MSYS2 包列表装了 `zip`，漏了 `unzip`（两者是**独立包**；`release.yml` 本来就有） | 补 `unzip`；并在 `package_release.sh` 开头 preflight 检查 `zip`/`unzip` 并提示该装哪个包 —— 原来是在**归档已经生成之后**才炸，读起来像打包 bug 而不是缺工具 |
| 2 | `initest` 2 条失败：`unknown [runtime] key 'TerminalZone'` | `ApplyRuntime()` 有分支，但 `IsKnownRuntimeKey()` 白名单没加 —— 值被**静默丢弃**，设了也不生效且无提示 | 白名单补 `terminalzone` / `terminalhandback` / `handback` |
| 3 | `initest` 1 条失败：`runtime: no warnings` | 上一条修好后，解析器给合法选项 `TerminalZone` 打了 `Warn()`，被"无警告"断言判为配置错误 | 删掉那条 `Warn()` —— 合法选项不该和真正的配置错误走同一通道；值已由 MSL_PN 启动行回显 |
| 4 | `memtest` **段错误，零输出** | 完整根因见 §16.6（`longjmp` 出 VEH 处理器） | 改内核中介拷贝为主路径 |
| 5 | `memtest` 91 项里 1 条：`page-crossing write landed correct bytes` | **我的测试写错了**：12 字节写从 `page-8` 起，float0 在 `[page-8,page-4)`、float1 在 `[page-4,page)`、float2 在 `[page,page+4)`；我在 `page-4` 读却断言 `3.5f`（实际是 `2.5f`） | 改成两半都验（顺带成为更强的断言：证明写确实跨进了第二页） |
| 6 | `smoke`：`log contains the version banner` | 硬编码 `"ACZPropNav 1.0.0"`，1.0.1 升版后失效；**一直被 memtest 段错误掩盖**（`build.sh` 在第一个失败二进制处停止） | 改为匹配 **banner 形状** `ACZPropNav <数字>.`，升版不再破坏它 |

**排障方法论（值得复用）**：`memtest` 崩得**连 banner 都没有**，因为 stdout 缓冲在进程死时丢光。
两步解决：
1. `std::setvbuf(stdout, nullptr, _IONBF, 0)` 放 `main` 第一句 —— 崩溃前的输出不再丢；
2. 加 `MEMTEST_TRACE=1` 逐调用打印（`Trace("ReadBytes(1, buf, 4)")`），
   `build.sh` 对 memtest 默认开启。

一次 CI 运行就把崩溃点收敛到**单个函数调用**。猜是没用的，让程序自己说出来。

**另一个教训**：`build.sh` 在第一个失败的测试二进制处停止，所以 `smoke` 的陈旧断言
被 `memtest` 的崩溃掩埋了好几轮。修好前一个 bug 后，下一个才浮出来 ——
看到"只差一个"时，别假设它是最后一个。

### 16.8 发布产物本身是错的：zip 带了一层包装目录

**这是最"丢人"的一个 bug，因为它让前两个版本的安装说明完全失效。**

`tools/package_release.sh` 把文件放进 `$STAGE/ACZPropNav/` 再 `zip -r ACZPropNav`，
所以 zip 成员是 `ACZPropNav/ACZPropNav.dll`。但 README.md / README.en.md /
RELEASE_NOTES.md / docs/RELEASING.md **全部**写的是"把发布包解压到 `ACZero\`"。
照做会得到：

```text
ACZero\ACZPropNav\XINPUT1_4.dll      ← 游戏永远找不到这个代理 DLL
ACZero\ACZPropNav\ACZPropNav.dll     ← 也就永远不会加载
```

结果：**模组静默不生效**。用户会以为装好了、游戏没变化。

**实测确认**：直接下载已发布的 `v1.0.1-win64.zip`，`unzip -Z1` 显示确实带
`ACZPropNav/` 顶层目录。v1.0.0 同理 —— 两个版本的"解压即用"从未真正成立。

**修法**：
* `DEST=$STAGE`（不再多一层），归档时按**文件名列表**打而不是按目录：
  `zip -q -X "$ZIP" "${PAYLOAD_FILES[@]}"`；
* 归档后**硬断言**任何成员含 `/` 就失败：
  `unzip -Z1 "$ZIP" | grep -q '/'` → `die`；
* `build.sh test` 新增 `do_package_check`：真打一个包并断言平铺，
  本地 `./build.sh test` 就能抓到，不必等发版。

**教训**：打包脚本"跑成功"不等于"产物可用"。原来的校验只对比了成员**文件名集合**
（用 `sed 's#^ACZPropNav/##'` 把前缀剥掉再比），**恰好把包装目录这一层掩盖了** ——
校验逻辑自己把要检的东西规范化掉了。现在的断言直接看原始 `unzip -Z1` 输出。

### 16.9 第二轮评审：五条意见逐条核实的结果

用户转来一份针对 `dev` 的审核报告（5 条）。**逐条验证后：4 条成立并已修，1 条前提有误但
方向正确、且暴露了一个更严重的问题。**

#### ① 高优先级：`CopyFromTarget`/`CopyToTarget` 仍保留 VEH+`longjmp` 回退 —— **成立，已彻底删除**

评审说的没错，但**实际比评审描述的更严重**：那条"部分可读范围兜底"**根本不可能有收益**。

```cpp
// 旧代码
if (g_isSelf && EnsureGuard()) return GuardedRead(dst, src, n);
// GuardedRead 一旦真的 fault 就 return false
// ReadProcessMemory 对同一批地址也是 return false
// => 两者结果完全等价
```

也就是说兜底没换来任何行为差异，只是把"在游戏线程里抛一个真实硬件异常"重新放回路径上。
`ReadProcessMemory` 返回 `FALSE` + `ERROR_PARTIAL_COPY` 时**本来就能拿到有效前缀字节数**
（`lpNumberOfBytesRead`），完全不需要 `memcpy` 去撞异常。

**处置**：整个 VEH / `setjmp` / `longjmp` 机制**全部删除** —— `EnsureGuard`、`t_frame`、
`GuardFrame`、`ArmWindow`、`AczGuardHandler`、`g_isSelf`、`g_guardState`、`<csetjmp>`、
`ACZ_NOINLINE`、`kGuardPageViolation` 一个不留。现在就是两行内核调用：

```cpp
bool CopyFromTarget(void* dst, uintptr_t src, size_t n) {
    EnsureProcess();
    SIZE_T got = 0;
    return ReadProcessMemory(g_hProcess, reinterpret_cast<LPCVOID>(src), dst, n, &got) != FALSE &&
           got == n;
}
```

"部分可读范围"的正确处理就是**整体拒绝** —— 对本模组安全，因为所有调用方都用返回值判断，
没有一个依赖"部分字节可用"。`memtest` 的用例名也从"VEH guard declines…"改成
"a foreign fault is never absorbed by the memory layer"（现在证明的是更强的性质：
mem:: 根本没注册处理器）。

顺带发现 `src/core/patcher.cpp` 里还有**另一处 VEH**（`AczGuardVeh`，page-guard 写监视），
`ArmPageGuard()` **没有调用者**，所以它从未被安装过 —— 是死代码。注释已改为如实说明。

#### ② 中优先级：同版本号重复触发 Release 覆盖 —— **成立，已修**

这正是我在升 1.0.2 时手动躲过的那个隐患：当时 `kVersion` 还是 1.0.1、tag `v1.0.1` 已存在，
合并 main 会走 `upload --clobber` **原地覆盖已发布资产**。我靠升版本号绕开了，但**触发机制还在**。

**修法**：`release.yml` 新增 `Decide whether this version should be published` 步骤（`id: gate`）：

```bash
if git rev-parse "$TAG" >/dev/null 2>&1; then
  if [ "${{ github.event_name }}" = "workflow_dispatch" ]; then publish=true   # 手动=故意重发
  else publish=false                                                          # 普通合并=只构建不发布
  fi
else publish=true
fi
```

`Create tag` 与 `Publish` 两步的 `if` 都加上 `steps.gate.outputs.publish == 'true'`。
摘要表也会写明"未发布（tag 已存在，请升 kVersion）"。这样：
**普通合并 + 没升版本 = 构建测试但不发布；确实要重发就得手动 `workflow_dispatch`。**
`docs/RELEASING.md` 同步改写。

#### ③ 中优先级：smoke 硬编码主版本号 —— **成立，已修**

上一轮我把 `"ACZPropNav 1.0.0"` 改成枚举 `0.`/`1.`/`2.`/`3.`，评审指出 4.x 又会失效。
现在改成**结构性匹配**：找到 `"ACZPropNav "` 前缀后要求紧跟"至少一位数字 + `.`"，
没有任何版本清单要维护。

#### ④ 低优先级：`docs/RELEASING.md` 重复标题 —— **成立，已修**

第 113/115 行确实有两个 `### Where the binary comes from`，删掉一个。

#### ⑤ 低优先级：目标瞬移当帧 `Vt` 归零而非截断 —— **方向对，但前提错了，且牵出一个真 bug**

评审建议"把 `vtLen > kMaxTargetStep` 时的 `Vt` 直接归零而非截断到 20"，理由是这样会
"产生虚假速度尖峰"。**结论我采纳了**（归零比截断更诚实：跳变的方向不携带目标去向信息），
但**评审的数值前提是错的** —— 它接受了代码注释里的 `20 m/frame == 1200 m/s`。

**位置单位不是米。** 我用真实日志交叉验证过：

```text
日志 partPos=(-35789.3 2466.3 -17576.6)  targetPos=(-39020.3 2319.3 -23449.7)
两点原始距离            = 6704.8
同帧报告的 |R|          = 6705
比值                    = 1.0000   ← 全程没有任何 5.12 换算
```

所以 `20.f` 是 **20 位置单位/帧 ≈ 234 m/s**，不是 1200 m/s。真实目标（600–900 m/s）的
每帧位移是 51–77 单位，**每一帧都超阈值** ⇒ 速度被砍到 234 m/s（方向保留、大小严重低估），
而且 `velSmoothValid` 每帧被清 ⇒ **EMA 平滑形同虚设**。

**修法**：新增 `msl_steer::SanitizeTargetStep(step, maxUnitsPerFrame, out)`（纯函数、可测），
边界按诚实单位计算 `1200 m/s / 60 * 5.12 ≈ 102.4 units/frame`；超范围或非有限值 ⇒
`out` 置零并返回 false，调用方同时清 EMA 历史。回归断言 (k15a)–(k15f)，
其中 (k15b) 直接钉住"600 m/s 的位移 51.2 单位 > 旧的 20 单位边界"这个事实。

顺带把 `rmin` 的注释也校准了：`25.0f` 是 25 单位 ≈ 4.9 m 而非 25 m，但对任何真实导弹
`2*|v|`（≈121 单位 ≈ 23.6 m）都占主导，所以只是注释错误，不是行为 bug。

**教训**：注释里的单位也是断言。`20 m/frame == 1200 m/s` 这句注释从写下那天就是错的，
而它让代码看起来"合理"，于是连续两轮评审都没质疑它，直到有人去核对日志里的原始坐标。
