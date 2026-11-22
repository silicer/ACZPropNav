# ACZPropNav — 皇牌空战零：贝尔卡战争 · 比例引导导弹

> 把《ACE COMBAT ZERO: THE BELKAN WAR (Steam)》里导弹"追着机尾跑"的追踪方式，
> 换成现实中的**比例引导（Proportional Navigation）**。
> 配置方式与 AC8 的 `AC8PropNav` 完全一致 —— 同一套 INI 语法、同一组属性名、同样的
> "留空 = 原版"规则，老配置可以直接改个文件名继续用。

[English version](README.en.md) · [交接文档](docs/HANDOFF.md) · [校准手册](docs/CALIBRATION.md) · [原版数值表](docs/VANILLA.md) · [架构说明](docs/ARCHITECTURE.md)

---

## 一、它做了什么

| 层 | 内容 | 生效时机 |
| --- | --- | --- |
| **普通 MSL 比例引导**（默认开启） | 钩住 `FUN_14013ECA0`（每帧一次的世界级武器管理器），在游戏算完之后直接改写每枚导弹的位置与姿态，把飞行方向换成按 `λ̇` 算出的 PN 指令，受 `MaxRotationAngle` 限速、导引头视场限制 | 进关卡后，导弹在飞的每一帧 |
| **特殊武器比例引导**（默认开启） | 钩住共享转向积分器 `exe+0x90510`，同一套 PN 数学，作用于 QAAM/SAAM/XLAA/XMAA/XAGM 等 | 同上 |
| **静态参数**（可选） | 在内存里按"原版数值指纹"找导弹参数记录并改写（射程、导引头角、锁定距离） | 每次武器数据重新加载后 |

原版导弹的制导是"朝目标**当前**位置转"（纯追踪）：转弯率一卡，侧向大机动就脱靶。
比例引导不追目标现在的位置，而是把**视线角速度**（LOS rate，`λ̇`）压到 0 —— 也就是
飞向拦截点：

```
λ̇ = (R × Vr) / |R|²          R = 目标位置 − 导弹位置，Vr = 目标速度 − 导弹速度
ω = N · λ̇                    N = 4（PnGain 可调）
每帧转角 = min(|ω|, MaxRotationAngle × dt)
```

关键点是**全程按这套律飞，末端不切回原版**：近距的 `λ̇` 奇点由
`rmin = max(2 帧行程, 25 m)` 软钳位兜住（详见
[docs/MSL_PN.md](docs/MSL_PN.md) 与 `src/pn/msl_steer.h`）。

`HomingForesightAmount` 是**纯追踪 ↔ 真 PN 的混合系数**：0 = 纯追踪（追尾，但仍受
`MaxRotationAngle` 限速），1 = 完整 PN，中间值把两个需求按各自份额放进同一个每帧转角
预算里合成。数学推导、198 组交战几何的仿真（交叉目标命中率 **38.9% → 57.4%**）在
[docs/PN.md](docs/PN.md)。

---

## 二、安装

**要求**：Windows 10/11 x64、Steam 版 ACZ:TBW。

1. **关闭游戏。**
2. 把发布包里的文件**解压到 `ACZero\`**（就是 `acz.exe` 所在的那个文件夹）：

   ```
   ...\steamapps\common\ACE COMBAT ZERO THE BELKAN WAR (Steam Version)\ACZero\
   ├── acz.exe
   ├── XINPUT1_4.dll      ← 加载器（转发系统 XInput，再加载下面的 DLL）
   ├── ACZPropNav.dll     ← 模组本体
   └── ACZPropNav.ini     ← 配置
   ```

   就这三件事，没有安装程序、不需要管理员权限。

3. 从 Steam 正常启动游戏。

**卸载**：删掉 `XINPUT1_4.dll` 和 `ACZPropNav.dll` 即可（`ACZPropNav.ini` 和日志
可以留着）。**全程不改动 `acz.exe`、任何 `*.arc`、存档。**

> 关于 `XINPUT1_4.dll`：这是 Windows 手柄 DLL 的**转发代理**，只是借它的加载时机
> 把我们的 DLL 送进游戏进程；所有手柄调用都原样转发回系统 DLL
> （`System32\XInput1_4.dll`），不改变任何手柄行为。

**和社区 loader 一起用（可选）**：如果你已经装了社区的 `ACZero\version.dll`
预加载器（`ACZero\mods\` 体系），可以不用我们的代理，直接把
`ACZPropNav.dll` 放进 `ACZero\mods\`、`ACZPropNav.ini` 也放 `ACZero\mods\`
即可 —— 本 DLL 同时导出社区 addon 入口 `AczAddonInit(void* hostCtx)`，
会自动从宿主拿游戏目录并读 `mods\ACZPropNav.ini`。

### 2.1 确认装好了（可选，30 秒）

默认配置**几乎不写日志**。想确认一次，把 `ACZero\ACZPropNav.ini` 末尾的
`[runtime]` 段取消注释并设 `LogLevel = 1`（或直接看日志是否存在），
启动后 `ACZero\ACZPropNav.log` 里应出现：

```
[CORE] ACZPropNav 1.0.0
[CORE] MSLPN: HOOK exe+0x13ECA0 installed (...)
[CORE] MSLPN: full PN for the whole flight: no terminal hand-back (TerminalZone=0)
```

第二、三行说明普通 MSL 的比例引导已经接管，而且是**全程 PN**。

## 三、配置

打开 `ACZero\ACZPropNav.ini`，**只改等号后面的数字，留空 = 保持原版**：

```ini
[player]
msl = {
    HomingForesightAmount = 0.7     ; 提前量，0 = 原版追尾，1.0 = 拉满
    LockonRange           =         ; 锁定距离（米）
    MaxRotationAngle      =         ; 转向速率（度/秒）
    MaxHomingAngle        =         ; 导引头视场（度）
    LifeTime              =         ; 存活时间（秒）
}

qaam = { HomingForesightAmount = 1.0 }   ; 单行写法也行
2aam = 0.9                               ; 旧写法：只设提前量
```

### 3.1 五个属性

| 属性 | 单位 | 作用 | 在 ACZ 里改的是 |
| --- | --- | --- | --- |
| `HomingForesightAmount` | 0 – 1 | **主参数**：前置量（比例引导强度） | 导弹记录的 `lead` 字段（+0x3C） |
| `MaxHomingAngle` | 度 | 导引头视场/起始角，超出即丢失制导 | `startAng`（+0x5A） |
| `MaxRotationAngle` | 度/秒 | 转向速率 | 引擎里为硬编码常量，仅在"真 PN 接管"时作为限速使用 |
| `LifeTime` | 米 | 射程（≈存活时间 × 速度） | `range`（+0x24）；`timeWait`（+0x30）为制导时限 |
| `LockonRange` | 米 | 锁定圈距离（只对玩家） | `scope`（+0x20） |

> `lead` 的出厂值是 40（玩家 MSL）/ 80（僚机）/ 28（僚机 2）。它看起来是"帧"
> （60 Hz，40 ≈ 0.67 s 前置）；模组用 `LeadScale` 把它换算成 0…1 的
> `HomingForesightAmount`，见 3.3 节。字段偏移与证据见
> [docs/VANILLA.md](docs/VANILLA.md)（已从正式版 `a.arc` 解包逐字节验证）。

> ACZ 的数据记录有 `lead / startAng / scope / range / speed / timeWait / timeStep …`
> 这些字段（详见 [docs/VANILLA.md](docs/VANILLA.md)），而**没有**显式的转向速率字段；
> 这也是"五属性里四个能落数据、转向速率只能靠 PN 接管限速"的原因。INI 语法保持
> 与 AC8 完全一致，所以写起来没有区别；如果你只关心效果，把 `HomingForesightAmount`
> 当成唯一旋钮就够了。

### 3.2 规则（与 AC8 一致）

* 只改**填了数字**的项，留空 = 保持原版；
* 型号键按**长度降序**匹配 ⇒ `4aam` 不会被 `aam` 抢走；
* 旧写法 `msl = 0.8` 仍然有效（只设 `HomingForesightAmount`）；
* 型号键大小写不敏感，可自由增删 —— 键会被当作"武器标识里的子串"来匹配游戏数据；
* `[enemy]` 段用于敌方武器；ACZ 里每种武器通常只有一份参数记录（敌我共用），
  只有在游戏里发现**第二份**参数记录时 `[enemy]` 才会单独生效，日志会明确说明。

### 3.3 ACZPropNav 扩展段（可选）

```ini
[runtime]
LogLevel        =        ; 留空 = 1（几乎不写日志）；0 = 不写；2 = 详细（排查问题用）
GuidanceOverride= 1      ; 总开关：0 = 完全关掉比例引导
PnGain          = 4      ; 导航常数 N（经典 PN 取 4）；调大更早转弯、调小更平滑
TerminalZone    = 0      ; 0 = 全程比例引导（默认）；>0 = 末端该半径内交还原版
MaxRotationAngle 见上     ; 每帧转角上限，真的按"度/秒"生效
```

其余 `[runtime]` 项（`DryRun` / `Probe` / `RingSurvey` / `HwWatch` / `HookRva` /
`TickHz` …）是**逆向与诊断用的开发者选项**，正常游玩不需要碰；INI 文件里有逐项
中文注释，想深究再看 [docs/RELEASING.md](docs/RELEASING.md) 和
[docs/CALIBRATION.md](docs/CALIBRATION.md)。

> **真正需要调手感的旋钮是 `HomingForesightAmount`**（默认 `[player] msl = 0.7`）
> 和 `PnGain`（N，默认 4；调大更早转弯、更激进，调小更平滑）。
>
> `LeadScale` / `LeadMode` 是**旧静态参数路径**的遗留旋钮，只有在内存里找到
> `.wpb` 参数表并改写引擎 `lead` 字段时才起作用；当前正式版把记录重新序列化了，
> 这条路默认不生效（日志会说明），所以日常不用管它们。

---

## 四、怎么做到的（为什么是注入 DLL）

* `ACZero\acz.exe` 被 Steam DRM 包裹：`.bind` 段在运行时才解密 `.text`，
  离线看不到也改不了游戏代码 —— 所以没有硬编码偏移，全部在运行时按特征字节验证。
* `ACZero\*.arc` 是 LZ4 压缩的资产包，里面是**数据**不是制导逻辑；改它做不出
  可实时调参的效果，而且要重打包游戏文件，卸载不干净。
* 所以本模组走的是和 AC8 的 `AC8PropNav` 一样的路：**注入 DLL → 读 INI →
  在运行时定位制导函数 → 每帧改写制导结果**。好处是改 INI 立刻生效、
  不碰游戏文件、卸载干净。

具体两条路径：

| 目标 | 做法 |
| --- | --- |
| 普通 MSL | 钩住每帧一次的武器管理器 `FUN_14013ECA0`，在游戏算完后按 PN 改写导弹的**位置与姿态** |
| 特殊武器（QAAM/SAAM/XLAA…） | 钩住共享转向积分器 `exe+0x90510`，同一套 PN 数学，改写速度方向 |

所有钩子都在运行时**逐字节校验目标函数的入口**，对不上就放弃安装、只记日志，
绝不盲目打补丁。细节见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) 与
[docs/MSL_PN.md](docs/MSL_PN.md)。

---

## 五、常见问题

**Q: 怎么确认它真的生效了？**
A: 默认配置几乎不写日志。想看证据：在 `ACZero\ACZPropNav.ini` 的 `[runtime]` 里设
`LogLevel = 2`，打几发横穿目标的导弹，然后看 `ACZero\ACZPropNav.log` 里的
`MSLPN: steer ...` 明细和 `applied=` 计数。手感上最直观的是：横穿/侧向大机动的
目标明显更容易命中。

**Q: 日志在哪？会不会越写越大？**
A: 在 `ACZero\` 下，`ACZPropNav.log`（上限 500 KiB）和 `ACZPropNav.key.log`
（上限 250 KiB，只保留最近一半）。默认 `LogLevel=1` 时几乎不增长，可以放着不管。

**Q: 会不会被 Steam 判定为作弊 / 影响联机？**
A: 本模组只改本机内存里单机模式的导弹制导，不改可执行文件、不联网、不注入网络流量。
ACZ:TBW 是单机游戏；仍建议只在单人模式使用。

**Q: 会不会崩游戏？**
A: 所有内存访问都做了异常保护（VEH + 边界检查），只对通过校验的导弹槽写入，
每帧的数学输入都有 NaN/inf 拦截。实际多局运行 0 崩溃。**真出问题请提 issue，
并附上日志（见下）。**

**Q: 和社区 modloader 冲突吗？**
A: 不冲突：本模组自带 `XINPUT1_4.dll` 代理，不需要社区 loader。若你已经在用
社区的 `version.dll` + `ACZero\mods\` 体系，`ACZPropNav.dll` 可以只放进 `mods\`，
本 DLL 同时导出社区 addon 入口 `AczAddonInit`。

**Q: 想调手感 / 觉得太强？**
A: 改 `[player] msl` 里的 `HomingForesightAmount`（0 = 纯追踪，1 = 全力 PN）。
嫌敌方导弹太强就改 `[enemy] msl`，或者把整个 `[enemy]` 段删掉。

---

## 五之二、遇到问题怎么办（Issues）

**发现 bug、崩游戏、或者手感不对，欢迎提 issue；但请理解这是个人项目，
随缘更新，不保证响应时间。**

提之前先做这一步（**否则基本没法定位**）：

1. 打开 `ACZero\ACZPropNav.ini`，在 `[runtime]` 段设 **`LogLevel = 2`**（详细日志）。
   没有 `[runtime]` 段就自己加一个。
2. 启动游戏，**复现问题**（崩游戏就跑到崩掉为止）。
3. 把 `ACZero\ACZPropNav.log` **和** `ACZero\ACZPropNav.key.log` 一起附在 issue 里。
4. 说明：游戏版本（Steam 版）、你装了什么别的 mod、崩溃前在做什么
   （哪一关、什么动作）、以及 `ACZPropNav.ini` 里你改过哪些数字。

issue 列表：见本仓库的 **Issues** 页。
**请不要在 issue 里附上游戏本体文件或任何 `*.arc` 内容** —— 那属于游戏资产。

---

## 六、从源码构建

需要 [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw) 或任意 x86_64-w64-mingw32 工具链：

```bash
./build.sh          # 构建全部产物到 build/（strip 过的发行版）
./build.sh test     # 构建 + 跑全部离线测试
./build.sh debug    # 未 strip 的调试版（-O1 -g）到 build/debug/，用于分析崩溃
./build.sh clean
```

`Makefile` 提供同样的目标（`make all` / `make test`），`build.sh` 是为那些
GNU make 的 jobserver 管道跑不起来的受限环境准备的同功能脚本。

离线测试（全部可在无游戏环境运行）：

| 测试 | 覆盖内容 |
| --- | --- |
| `build/initest.exe` | INI 解析：与真实 `AC8PropNav.ini` 逐项比对（225 项断言） |
| `build/pnsim.exe --selftest` | PN 数学：λ̇、混合律、限速与单位、rmin 奇点保护、末端一致性、退化输入（**64 项**） |
| `build/memtest.exe` | 内存层：合成内存镜像、跨页写保护恢复、SEH 安全读写正负例 |
| `build/hooktest.exe` | 内联钩子：安装/卸载/拒绝不安全序言、两种代理转发 |
| `build/smoke.exe` | 端到端：把真实 `ACZPropNav.dll` 载入一个合成进程，验证导出与日志 |
| `build/pnsim_native` | 仅主机为 Linux（无 Wine）时由 `build.sh test` 自动编译并运行，验证同一套数学 |

---

## 七、状态与边界（很直白地说）

* ✅ **两条路径都已真机验证命中**：普通 MSL（`FUN_14013ECA0` 后置钩子）与特殊武器
  （`0x90510` 积分器，实测 QAAM `class=?AVWeaponExQaamCtrl@@`，6 发全命中）。
  多局运行 0 崩溃，`terminal=0` 证明末端没有切回原版。
* ✅ 离线测试全绿；DLL/代理可构建、可加载；配置与 AC8 语义逐项对齐；
  PN 数学有量化证据（198 组交战仿真 + 自测断言）。
* 🧪 **0.7 这类中间值此前是被低估的**：旧的混合实现把"纯追踪"和"PN"两个**旋转矢量**
  直接线性加权，二者方向经常不共线（中位夹角约 45°，约 5% 的几何超过 120°），
  于是中途会互相抵消 —— 实测在某个几何下 `amount=0.5` 只转 0.012°/帧，而
  `amount=0` 和 `amount=1` 都转 3.0°/帧，也就是**中间值反而最不转弯**。
  现在改成先各自算出**两个结果方向**（都按每帧转角预算钳位），再沿二者大圆插值；
  端点精确、中间值对 `amount` 单调，回归断言见 `pnsim` (k11b1)–(k11b3)。
  （在两个律确实相反的几何里，中间值的净转角本来就该比端点小 —— 那是真实折中。）
* ⚠️ **末端不再切回原版制导**。曾经有过一个 30 m 的"末端安静区"（进 30 m 就交还原版），
  当时是为了规避"双弹齐射时一枚过冲绕圈"；那其实是**槽缓存按 owner 撞键**导致的
  （已按槽行号重新做键修复）。972 组几何的闭环仿真显示：去掉这段交接后**
  命中率与最近距离完全不变**，而 `rmin`（33.3 m）本来就比 30 m 更早介入。
  所以默认 `TerminalZone = 0`（全程 PN）；想自己对比可设成正数。
* ⚠️ `MaxRotationAngle` 在 ACZ 数据里没有对应字段，它只在比例引导接管时作为
  "每帧最大转角"生效；纯静态参数模式下这一项不起作用（日志会提示）。
* ⚠️ 特殊武器（QAAM/LAAM/4AAM…）的数据记录里没有独立参数，只能走代码路径。

---

## 八、致谢 / 参考

* [AC8PropNav](https://www.nexusmods.com/acecombat8wingsoftheve/mods/14) —— INI 语法与
  "引导一次 + 廉价维持"的工程思路都来自它（本项目的配置格式即其超集）。
* ACZ Enhanced Gunplay / `aczarc.py`（Volcano Water 等）—— `.arc` 解包工具与
  `data/mis/mpl/*.wpt` 数据结构信息、社区 modloader 体系。
* [docs/RE.md](docs/RE.md) —— 对 `acz.exe` 加壳、AC8PropNav 字段偏移、addon ABI 的逆向记录。
* **[docs/HANDOFF.md](docs/HANDOFF.md)** —— 交接文档：现状、已验证的 RVA、已排除的路线、
  环境坑、以及仍未接入的部分（`LifeTime`/`LockonRange` 等）与恢复现场清单。
* **[docs/INDEX.md](docs/INDEX.md)** —— 全部文档的索引：先读哪三份、哪些是历史证据。

本项目不分发任何游戏原始数据或修改过的游戏资产。
