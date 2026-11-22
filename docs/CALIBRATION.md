# 校准手册 / Calibration guide

本模组不在代码里写死任何偏移。它在**运行时**用"原版数值指纹"找游戏结构，
找不到就只写日志、不动内存。这一页讲怎么把"找不到"变成"找得到"，以及可选的
真 PN 接管怎么校准。

This mod has no hard-coded offsets. It locates the game's structures at runtime by
matching vanilla numeric fingerprints; if nothing verifies it logs and changes
nothing. This page is the calibration loop, and the optional true-PN hook.

---

## 0. 三个日志文件 / the three logs

| 文件 | 上限 | 内容 |
| --- | --- | --- |
| `ACZero\ACZPropNav.log` | 500 KiB | 主日志：启动、发现结果、写入统计、警告 |
| `ACZero\ACZPropNav.key.log` | 512 KiB（保留最近一半） | 关键日志：Probe 详情、字段偏移、实体候选、代码引用 |
| `ACZero\modloader.log` | — | 只有在你同时使用社区 loader 时才有，与本模组无关 |

### 0.1 想让日志有内容：`LogLevel`

出厂 INI 里 `[runtime] LogLevel` **留空 = 1**，这是**安静**的默认值：只写启动、
钩子状态、警告与错误，2 秒汇总和每条修正明细都不会出现。排查问题时需要：

```ini
[runtime]
LogLevel = 2        ; 2 = 详细：每 2 秒一行统计 + 每条修正明细
```

| 值 | 含义 |
| --- | --- |
| 留空 / 不写 | = 1（出厂默认，安静） |
| `0` | 不写任何日志内容（两个 `.log` 文件可能仍被创建为空文件） |
| `1` | 启动 + 钩子状态 + 警告/错误 |
| `2` | 详细：2 秒遥测汇总 + 逐条修正明细 |

### 0.2 报 bug 的正确流程

现在没有安装脚本也没有诊断脚本，所以流程是**手工**的：

1. 关闭游戏，用文本编辑器打开 `ACZero\ACZPropNav.ini`（先复制一份 `.bak`）；
2. 在 `[runtime]` 段里加/改成 `LogLevel = 2`；
   **保存时保持 UTF-8 带 BOM，不要改行尾**（这份 INI 的中文注释和解析器都依赖它）；
3. 启动游戏，复现问题，退出游戏；
4. 把 `ACZero\ACZPropNav.log` 与 `ACZero\ACZPropNav.key.log` **两个**一起附到
   GitHub issue 里；
5. 排完之后把 `LogLevel` 改回留空/`1`（`2` 只为排查用，长时间开着会写出很大的日志）。

> 不要上传游戏本体文件（`acz.exe`、`*.arc`、`*.wpb`/`*.wpt`）——日志里没有游戏数据。

---

## 1. 症状与处置对照 / symptom → action

| 日志里看到 | 含义 | 怎么办 |
| --- | --- | --- |
| `参数表搜索：完成，行 N（已验证 N）` | 正常，已写入 | 不需要校准，直接调 INI 手感 |
| `没有找到可信的导弹参数表` | 指纹没命中（游戏更新 / 数值与内置表不符） | 走第 2–3 节 |
| `写 0 行 / 0 个字段` | 找到了表但没有匹配到你配置的型号键 | 检查 INI 型号键，或看第 4 节 |
| `参数表疑似被游戏重新加载` | 换关卡/重载武器数据，模组已自动重找 | 正常，无需处理 |
| `比例引导需要一次校准` | 没有 `HookRva`/`HookSignature`，走的是"引导数据 + PN 数学"路径 | 想用真 PN 接管走第 5 节 |

---

## 2. 第一步：跑一次 Probe

1. 打开 `ACZero\ACZPropNav.ini`，在 `[runtime]` 里写：

   ```ini
   [runtime]
   Probe     = 1
   LogLevel  = 2
   DryRun    = 1        ; 先只看不改，最安全
   ```

2. 启动游戏，**进入一个有关卡内有敌机的任务**，飞 30–60 秒（触发几次导弹发射）。
3. 退出游戏，打开 `ACZero\ACZPropNav.key.log`，会看到类似：

   ```
   ================ PROBE ================
   module ...\ACZero\acz.exe base=0x140000000 size=0x389b000
   writable regions: 3121, total 0x1c3a9000 bytes
   discovery: MSL: 0 anchors (lead=40 tol=1, scope=1600 tol=32, range=3000 tol=60)
   discovery: 没有布局假设达到 minScore
   rows found=0 verified=0
   ================ /PROBE ================
   ```

   `0 anchors` = 连单个数值都没匹配上 ⇒ 内置原版值不对，走下一步。

---

## 3. 第二步：修正 `[vanilla]`（原版数值）

扫描器靠"**原版**数值"定位，所以只要把这版游戏的真实原版值填进去就行。

**原版值从哪来：**

* 游戏数据在 `ACZero\*.arc` 里（LZ4 打包）。社区工具 `aczarc.py`
  （见 [README](../README.md) 致谢）可以解包；
* 解包后看 `data/mis/mpl/*.wpt` —— 这些是**明文的数据结构 + 数值**，例如：

  ```
  f32  scope : 1600.000000      ; 锁定距离
  f32  range : 3000.000000      ; 射程
  f32  lead  : 40.000000        ; 前置量（帧）
  s16  speed : 1000             ; 速度
  u8   startAng : 90            ; 导引头起始角
  ```

  配套的 `*.wpb` 是同一条记录的二进制形式；
* 把数值按下面的映射填进 INI 的 `[vanilla]`：

  | INI 属性（AC8 名） | ACZ 字段 |
  | --- | --- |
  | `HomingForesightAmount` | `lead` |
  | `MaxHomingAngle` | `startAng` |
  | `LifeTime` | `range` |
  | `LockonRange` | `scope` |
  | `MaxRotationAngle` | *（数据里没有，见第 5 节）* |

  ```ini
  [vanilla]
  msl = {
      HomingForesightAmount = 40
      MaxHomingAngle        = 90
      LifeTime              = 3000
      LockonRange           = 1600
  }
  ; 再给另一种型号（用来交叉验证偏移，必须与 msl 数值不同）
  qaam = {
      HomingForesightAmount = 80
      LifeTime              = 2000
  }
  ```

> `[vanilla]` 只需要**能区分型号**就够：每个型号至少两项、且不同型号之间要有差异，
> 交叉验证才能把"偏移布局"钉死。给得越全，定位越稳。

改完重启游戏，主日志应出现 `参数表搜索：完成，行 N（已验证 N）`。

---

## 4. 第三步：确认型号键匹配

发现成功后日志会逐行列出：

```
行 / row 0x1E5C2A40 [msl] score=12 verified=1  HomingForesightAmount+0x2C=40.00 LifeTime+0x18=3000.00 ...
```

`[msl]` 是这个行被哪个 **`[vanilla]` 签名**匹配到的标签。模组再拿这个标签去
`[player]`/`[enemy]` 里按"最长键优先"找配置（和 AC8 的规则一样）。所以：

* 想让某个型号被改，就在 `[vanilla]` 里给它一个签名，**并在 `[player]` 里用同一个键**
  （或将键写成该标签的子串）；
* 日志里 `写 0 行` 通常就是"表找到了，但 `[player]` 里没有匹配的键"。

---

## 5. 可选：真 PN 接管（`MaxRotationAngle` 唯一生效的路径）

ACZ 的数据记录里没有"转向速率"字段（它硬编码在引擎里），所以想用
`MaxRotationAngle` 限速、并让制导律完全由本模组接管，需要一次钩子校准：

**已有可直接使用的逆向结果（详见 [RE.md](RE.md) §3.7 / §10，已验证于 acz.exe
4,046,008 B / SHA-256 `3802B0DC…`）：**

| 目标 | RVA | 说明 |
| --- | --- | --- |
| 导弹攻击调度器（推荐钩这里） | `exe+0x09DBB0` | 参数仅 `this`（RCX），所以 `HookArgIndex=1` |
| 单枚导弹更新函数（需保留 RDX/R8/XMM3） | `exe+0x091DC0` | ABI `(this, int slotIndex, MissileSlot*, float const 2.0f)`，**不能用本模组现在的两参 detour** |
| 导弹实例数组 | `this+0x80`，步长 `0x68`，16 槽 | 存活掩码 `*(u32*)(this+0x5C)`，第 i 枚 = `this+0x80+i*0x68` |

`0x09DBB0` 的字节签名（唯一命中）：

```
48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 C5 F8 29 74 24 30 C5 FA 10 35 ?? ?? ?? ?? 48 8B E9 48 8D B1 80 00 00 00
```

所以校准顺序是：

```ini
[runtime]
HookSignature = 48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 C5 F8 29 74 24 30 C5 FA 10 35 ?? ?? ?? ?? 48 8B E9 48 8D B1 80 00 00 00
HookArgIndex  = 1
DryRun        = 1
Probe         = 1
```

重启后看 `ACZPropNav.key.log`：应出现 `制导钩子已安装`，以及每个 tick 的
`实体布局未识别 / entity layout not recognised: ...`（**这是预期**：v1.0 的实体相关器
还不认识 `WeaponCtrl` 的槽结构，它只会拒绝而不会乱写）。要让 PN 真正生效，下一步需要
在 `patcher.cpp` 里为 `WeaponCtrl` 增加一个"数组遍历"适配器：

```text
每 tick（或每次原函数返回后）：
  mask = *(u32*)(this + 0x5C)
  for i in 0..15:
      if !(mask & (1 << i)) continue
      slot = this + 0x80 + i * 0x68
      if *(u8*)(slot + 0x50) == 0: continue     // 未激活
      // 读 slot 内的位置/朝向（RE.md §3.7.2 给出 disasm 与偏移），
      // 用 pn.h 计算 desiredDir，受 MaxRotationAngle / MaxHomingAngle 约束后写回
```

这就是"一次会话"的工作量：`pn.h` 的数学、钩子引擎、日志与配置都已经就绪，
只差把 `WeaponCtrl` 的槽字段映射补上（用 `docs/RE.md §3.7.2` 的 x64dbg 检查法确认
`slot+0x00` 位置向量、`slot+0x10/0x14` 朝向标量、`slot+0x50` 存活字节）。

> 如果你不想现在做这一步：v1.0 的"数据 + 前置量"路径已经能改变普通 MSL / 僚机弹的
> 追踪手感（`HomingForesightAmount` → `lead`），这是可用的主体功能。

---

## 6. 调手感：`LeadScale` / `LeadMode`

`HomingForesightAmount` 是 0…1 的归一化强度，引擎 `lead` 是"帧"（60 Hz）。默认：

```ini
LeadMode  = add      ; lead = 原版 + amount * LeadScale
LeadScale = 260      ; amount = 1.0 时额外 260 帧 ≈ 4.3 s 前置
```

* 原版 MSL 的 `lead = 40`（≈0.67 s）⇒ `amount=0` 即完全原版；
* `amount=0.7`（出厂默认）⇒ 40 + 182 = 222 帧 ≈ 3.7 s 前置，明显"提前量打"；
* 觉得太强就降 `LeadScale`（120 已经很凶），想更接近"必中"就调到 400+；
* `LeadMode=blend` 适合想保留原版差异（型号越强的原版 lead 越高，混合后仍保留差异）；
  `absolute` 则所有型号手感一致，不推荐日常使用。

---

## 7. 安全开关

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `DryRun` | 0 | 1 = 只检测、只写日志，绝不写内存 |
| `RequireVerifiedOffsets` | 1 | 1 = 结构没通过交叉验证就不写 |
| `ForceOffsets` | 0 | 1 = 信任手工偏移（配合 `Entity*Offset`），仅在明确知道自己在做什么时用 |
| `LogLevel` | 1（留空） | 0 = 完全不创建日志文件 / 1 = 启动+钩子状态+警告错误（默认，安静）/ 2 = 详细（2 秒汇总 + 逐条明细） |

所有内存读写都有异常保护；写入前后都做读回校验；结构一旦被游戏重新加载，
模组会主动丢弃旧指针并重新发现（日志会有 `参数表疑似被游戏重新加载`）。
