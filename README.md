# ACZPropNav — 皇牌空战零 · 比例引导导弹

> 把《ACE COMBAT ZERO: THE BELKAN WAR (Steam)》里导弹"追着机尾跑"的追踪方式，
> 换成现实中的**比例引导（Proportional Navigation）**。
> 配置语法与 AC8 的 `AC8PropNav` 完全一致 —— 同一套 INI、同一组属性名、同样
> 的"留空 = 原版"规则，老配置改个文件名就能继续用。

[English](README.en.md) · [配置详解](docs/CONFIG.md) · [问题排查](docs/CALIBRATION.md) · [架构](docs/ARCHITECTURE.md) · [技术细节](docs/MSL_PN.md) · [文档索引](docs/INDEX.md)

---

## 一、它做了什么

原版导弹每帧朝目标**当前**位置转（纯追踪）：转弯率一卡，侧向大机动就脱靶。
比例引导不追目标现在的位置，而是把**视线角速度**（`λ̇`）压到 0 —— 也就是飞向
拦截点：

```
ω = N · λ̇          N = 4
每帧转角 = min(|ω|, MaxRotationAngle × dt)
```

| 目标 | 做法 |
| --- | --- |
| 普通 MSL（每架飞机都打的） | 钩住每帧一次的武器管理器 `FUN_14013ECA0`，在游戏算完后按 PN 改写导弹的位置与姿态 |
| 特殊武器（QAAM / SAAM / XLAA / XMAA / XAGM） | 钩住共享转向积分器 `exe+0x90510`，同一套 PN 数学 |

**全程比例引导，末端不切回原版。** 近距的 `λ̇` 奇点由
`rmin = max(2 帧行程, 25 m)` 软钳位兜住。

`HomingForesightAmount` 是**纯追踪 ↔ 真 PN 的混合系数**：`0` = 纯追踪（仍受
`MaxRotationAngle` 限速），`1` = 完整 PN，中间值在两个结果方向之间插值。

数学推导与 198 组交战几何的仿真（交叉目标命中率 **38.9% → 57.4%**）见
[docs/PN.md](docs/PN.md)。

---

## 二、安装

**要求**：Windows 10/11 x64、Steam 版 ACZ:TBW。

1. **关闭游戏。**
2. 把发布包里的文件**解压到 `ACZero\`**（`acz.exe` 所在目录）：

   ```
   ...\steamapps\common\ACE COMBAT ZERO THE BELKAN WAR (Steam Version)\ACZero\
   ├── acz.exe
   ├── XINPUT1_4.dll      ← 加载器（转发系统 XInput，再加载下面的 DLL）
   ├── ACZPropNav.dll     ← 模组本体
   └── ACZPropNav.ini     ← 配置
   ```

   就这一步。没有安装程序、不需要管理员权限。

3. 从 Steam 正常启动游戏。

**卸载**：删掉 `XINPUT1_4.dll` 和 `ACZPropNav.dll` 即可（INI 和日志可以留着）。
**全程不改动 `acz.exe`、任何 `*.arc`、存档。**

> `XINPUT1_4.dll` 是手柄 DLL 的**转发代理**，所有手柄调用原样转发回
> `System32\XInput1_4.dll`，不改变任何手柄行为。
>
> 如果你已经在用社区的 `version.dll` loader（`ACZero\mods\` 体系），可以不用我们的
> 代理，直接把 `ACZPropNav.dll` + `ACZPropNav.ini` 放进 `ACZero\mods\` —— 本 DLL
> 同时导出社区 addon 入口 `AczAddonInit`。

---

## 三、配置

编辑 `ACZero\ACZPropNav.ini`，**只改等号后面的数字，留空 = 保持原版**：

```ini
[player]
msl = {
    HomingForesightAmount = 0.7     ; ★ 主参数：0 = 纯追踪，1 = 全力比例引导
    MaxRotationAngle      =         ; 转向速率（度/秒），留空 = 180
    MaxHomingAngle        =         ; 导引头视场（度），留空 = 用游戏原值
}
```

最常调的就三个：

| 属性 | 作用 | 建议 |
| --- | --- | --- |
| `HomingForesightAmount` | 制导律混合强度 | 0.7 起手；嫌弱往 1 靠，想要原版手感就调低 |
| `MaxRotationAngle` | 转向速率（度/秒） | 留空 180；嫌钝可调低到 90，超过 360 基本等于瞬间转向 |
| `MaxHomingAngle` | 导引头视场（半角，度） | 调大会明显变强 —— `360` 意味着几乎无法甩脱 |

`[enemy]` 段用于敌方/僚机武器。**想只让玩家受益就把整个 `[enemy]` 段删掉。**

完整的属性说明、型号键匹配规则、`[runtime]` 扩展项（含 `LogLevel`、`PnGain`、
`TerminalZone`）见 **[docs/CONFIG.md](docs/CONFIG.md)**。
INI 文件本身也带逐项中文注释。

---

## 四、确认它生效了

默认配置**几乎不写日志**（这是故意的，正常游玩不需要）。想验证一次：

1. 在 `ACZero\ACZPropNav.ini` 的 `[runtime]` 段设 `LogLevel = 2`
   （没有该段就自己加）。
2. 打几发横穿目标的导弹。
3. 看 `ACZero\ACZPropNav.log`，应出现：

```
[CORE] ACZPropNav 1.0.1
[CORE] MSLPN: HOOK exe+0x13ECA0 installed (...)
[CORE] MSLPN: full PN for the whole flight: no terminal hand-back (TerminalZone=0)
```

`MSLPN: steer ...` 明细行和持续增长的 `applied=` 就是它正在改写的证据。

---

## 五、遇到问题 / Issues

**发现 bug、崩游戏、或者手感不对，欢迎提 issue；但请理解这是个人项目，
随缘更新，不保证响应时间。**

提之前请先做这一步（否则基本没法定位）：

1. INI 的 `[runtime]` 段设 **`LogLevel = 2`**。
2. 启动游戏，**复现问题**（崩游戏就跑到崩掉为止）。
3. 把 `ACZero\ACZPropNav.log` **和** `ACZero\ACZPropNav.key.log` 一起附上。
4. 说明游戏版本、装了什么别的 mod、崩溃前在做什么、改过 INI 里哪些数字。

**请不要附游戏本体文件或任何 `*.arc` 内容** —— 那属于游戏资产。

---

## 六、状态与边界

* ✅ **两条路径都已真机验证命中**：普通 MSL 命中；特殊武器实测 QAAM **6 发全命中**。
  多局运行 0 崩溃。
* ✅ 离线测试 69 项断言全绿（`pnsim`）+ 5 个 Windows 测试程序。
* ⚠️ **弹体视觉朝向不做同步**：弹道正确，但导弹模型朝向可能比真实轨迹略微滞后。
  运行时自检失败会自动关闭该功能，不影响弹道。
* ⚠️ `LifeTime` / `LockonRange` 在普通 MSL 的 PN 路径上未接线，仍由原版负责
  （射程/寿命判定、HUD 锁定圈）。
* ⚠️ 特殊武器的参数按 `[player] msl` 的配置驱动 —— 引擎不告诉模组槽位属于哪个型号。

---

## 七、从源码构建

需要 MinGW-w64（任意 `x86_64-w64-mingw32-g++`）：

```sh
./build.sh          # 构建到 build/（strip 过的发行版）
./build.sh test     # 构建 + 跑全部离线测试
./build.sh debug    # 未 strip 的调试版到 build/debug/（分析崩溃用）
./build.sh clean
```

测试与发布流程见 [AGENTS.md](AGENTS.md)；打包用
`tools/package_release.sh`，发版 runbook 见 [docs/RELEASING.md](docs/RELEASING.md)。

---

## 八、致谢

* [AC8PropNav](https://www.nexusmods.com/acecombat8wingsoftheve/mods/14) —— INI 语法与
  "引导一次 + 廉价维持"的工程思路来自它（本项目的配置格式即其超集）。
* ACZ Enhanced Gunplay / `aczarc.py`（Volcano Water 等）—— `.arc` 解包工具与
  `data/mis/mpl/*.wpt` 数据结构信息、社区 modloader 体系。

**本项目不分发任何游戏原始数据或修改过的游戏资产。**
