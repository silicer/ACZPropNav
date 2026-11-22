# ACZPropNav v1.0.0 — 发布说明

## 什么是这个模组

把《ACE COMBAT ZERO: THE BELKAN WAR (Steam)》的导弹制导从"追着目标当前位置跑"
（纯追踪）换成真实的**比例引导（Proportional Navigation）**：压制视线角速度
`λ̇`，飞向拦截点而不是目标现在的位置。

配置方式与 AC8 的 `AC8PropNav` 完全一致 —— 同一套 INI 语法、同一组属性名、
同样的"留空 = 原版"规则。

## 已验证（真机）

| 项 | 结果 |
| --- | --- |
| 普通 MSL（`FUN_14013ECA0` 后置钩子） | ✅ 命中，`applied` 稳定增长 |
| 特殊武器（`0x90510` 积分器） | ✅ 实测 QAAM 6 发**全命中** |
| 末端不切回原版（全程 PN） | ✅ `terminal=0` 贯穿整局 |
| 崩溃 / 异常 | ✅ 多局 0 次 |
| 离线测试 | ✅ 64 项断言全绿 |

## 安装

1. 关闭游戏。
2. 把发布包里的文件**解压到 `ACZero\`**（就是 `acz.exe` 所在目录）。
   就这一步，没有安装程序、不需要管理员权限。
3. 从 Steam 正常启动游戏。

**卸载**：删掉 `XINPUT1_4.dll` 和 `ACZPropNav.dll` 即可。
**全程不改 `acz.exe`、不改任何 `*.arc`、不碰存档。**

> 默认配置几乎不写日志。想确认生效：在 INI 的 `[runtime]` 里设 `LogLevel = 2`，
> 打几发导弹后看 `ACZero\ACZPropNav.log` 里的 `MSLPN: HOOK ...` 和 `applied=`。

## 配置要点

```ini
[player]
msl = {
    HomingForesightAmount = 0.7   ; 0 = 纯追踪, 1 = 全力 PN
    MaxRotationAngle      =       ; 度/秒，留空 = 90（真的按 90 走）
    MaxHomingAngle        =       ; 导引头半角，留空 = 用游戏原值
}
```

* `HomingForesightAmount` 留空 = 该型号完全不接管；写 `0` = 限速纯追踪（可当 A/B 基线）。
* 想彻底关掉模组：`[runtime] GuidanceOverride = 0`。

## 已知限制

* **弹体视觉朝向不做同步**。位置引导（弹道）是正确的，但导弹模型朝向可能比真实
  弹道略微滞后。运行时朝向标定在 `ok=0` 时自动关闭该功能，不影响弹道。
* `LifeTime` / `LockonRange` 在普通 MSL 的 PN 路径上未接线：射程/寿命判定和
  HUD 锁定圈仍由原版负责。
* 特殊武器的参数按 `[player] msl` 的配置驱动（引擎不告诉模组槽位属于哪个型号）。

## 不分发任何游戏数据

本模组只包含自己写的代码与配置，不含任何游戏原始文件或修改过的游戏资产。
