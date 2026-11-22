# 交接文档 / HANDOFF

> 目标读者：接手这个项目的下一个人（或下一个 session 的我）。
> 读完这份文件 + `docs/MSL_TARGET.md` §10，就能在 10 分钟内回到现场，不需要重读整段历史。
>
> 最后更新：2026-10-26（最近一次真机运行：2026-10-26 02:12–02:15）

---

## 0. 一句话现状

| 项 | 状态 |
|---|---|
| AC8 兼容的 INI（`[player]`/`[enemy]` + 五个参数 + 逐型号块） | ✅ 完成，268 条解析断言 |
| 加载器 / 安装 / 卸载 / 日志 / 热重载相关工程 | ✅ 完成（安装/卸载现在是手工复制/删除 DLL，见 §2） |
| **特殊武器（QAAM/SAAM/XLAA/XMAA/XAGM）比例引导** | ✅ **真机验证可用** |
| **普通标准导弹（MSL）比例引导** | ✅ **已完成并真机验证**（`FUN_14013ECA0` 后置钩子，§11.9–§11.11） |
| 离线测试 | ✅ 全绿：initest 268、pnsim 61、memtest 85、hooktest 98、hwwatchtest 27、smoke |

> 2026-11-21 的收尾（本次会话）：
> * 末端 30 m "安静区"**改为可选**（`[runtime] TerminalZone`，默认 0 = 全程 PN）。
>   972 组几何的闭环仿真显示它对命中率/最近距离没有任何影响，而 `rmin`（33.3 m）
>   本来就比它更早介入；它当初只是为了绕开一个已被根因修复的双弹 bug（§11.11）。
> * 修掉**混合律**的真 bug：旧代码把纯追踪与 PN 两个**旋转矢量**线性加权，
>   二者不共线时会互相抵消（`amount=0.5` 可能比 `0` 和 `1` 都几乎不转弯）。
>   现在改为对**两个结果方向**做大圆插值（端点精确、对 `amount` 单调），
>   并有 `pnsim` (k11b*) 回归断言。中途试过"依次合成两次部分旋转"，但净转角仍会回到 0，
>   已放弃。
> * 补上**导引头视场门限**（`MaxHomingAngle` / `P+0x4A`）：此前 PN 路径完全不看视场，
>   连原版早已脱锁的目标都会继续追。
> * 修 `hook::Remove` 在 prologue 恢复失败时仍释放 trampoline 的 use-after-free /
>   死锁风险，以及 `Install` 先打补丁后发布 trampoline 的竞态窗口。
> * `HomingForesightAmount = 0` 现在真的是"限速纯追踪"，不再是"静默交还原版"。

历史上的"普通 MSL 未完成"指的是缺**每帧推进函数的地址和目标来源**；路线排除记录在 §6。

---

## 1. 项目目标与硬约束

* 游戏：Steam 版 **ACE COMBAT ZERO: THE BELKAN WAR**，可执行文件 `ACZero\acz.exe`。
* 目标：把导弹从"追尾"改成**比例引导（proportional navigation）**。
* **硬约束**：配置方式必须尽可能与 `D:\SteamLibrary\steamapps\common\ACE COMBAT 8\Game\Binaries\Win64\AC8PropNav.ini`
  一致（键名、含义、`[player]`/`[enemy]` 两个 section、逐型号块、留空 = 原版）。

AC8 那个 mod 的真实做法（见 `docs/RE.md` §4）：**它没有实现任何制导数学**，只是把 UE 引擎里本来就有的
`HomingForesightAmount` / `MaxHomingAngle` / `MaxRotationAngle` / `LifeTime` / `LockonRange`
写在 CDO 和 `DT_NPC_Weapon` 行上（`+0x454/+0xA18/+0xA1C/+0x420/+0x46C` 与 `+0x24/+0x2C/+0x30/+0x10`），
PN 由游戏自己执行。**ACZ 没有这个参数**，所以我们必须改代码行为（或改数据，见 §7.2）。

INI schema（不可破坏）：

```ini
[player]
msl = { HomingForesightAmount = 0.7
        MaxHomingAngle = 90
        MaxRotationAngle = 120
        LifeTime = 8
        LockonRange = 3200 }
[enemy]
...
; 留空 = 保持原版；最长键优先匹配；兼容旧写法 msl = 0.8
```

---

## 2. 交付物与怎么用

发布包是一个 zip，**内容直接解压进 `ACZero\`**（`acz.exe` 所在目录），没有安装脚本、
也没有 `build\` 子目录：

```text
ACZero\
  ACZPropNav.dll      主 DLL（模组本体）
  XINPUT1_4.dll       代理加载器（15 个导出与系统 DLL 完全一致，转发到 System32 后 LoadLibrary 主 DLL）
  ACZPropNav.ini      配置模板（含中文注释，UTF-8 + BOM）
  README.md  README.en.md  LICENSE  RELEASE_NOTES.md
```

VERSION.dll（备用代理，社区 modloader 生态：`ACZero\mods\ACZPropNav.dll` + `AczAddonInit`）
仍可手工构建，但**不再随包发布**。

安装 = 把这些文件复制/解压到 `ACZero\`（无需管理员权限）。
卸载 = 删掉 `ACZPropNav.dll` + `XINPUT1_4.dll`（`.ini` 与日志是用户数据，可以留着）。
INI 查找顺序：`ACZero\mods\ACZPropNav.ini` → DLL 同目录 → `ACZero\ACZPropNav.ini`。
日志：`ACZero\ACZPropNav.log`（主）、`ACZero\ACZPropNav.key.log`（诊断细节）。
默认 `LogLevel = 1`：只写启动、钩子状态、警告与错误，平时几乎不产生日志。

> 原来的 13 个 `.cmd`/`.ps1` 脚本（`install.cmd`、`uninstall.cmd`、`enable_pn.*`、
> `enable_probe.cmd`、`probe_*.*`）已随这次发布**全部删除**。它们原先做的两件事现在是手工操作：
> 复制 DLL 进 `ACZero\`，以及用文本编辑器改 `ACZero\ACZPropNav.ini` 的 `[runtime]` 键。
> 下文提到脚本的地方都保留为历史记录，并标注"（脚本已删除 / script since removed）"。

> 想让普通 MSL 的比例引导生效，`[runtime]` 需要
> `GuidanceOverride = 1`、`DryRun = 0`、`Probe = 0`、`RingSurvey = 0`
> —— 这正是**代码默认值**（出厂 INI 把 `[runtime]` 整段注释掉，所以默认就生效）。
> 只有当你把 INI 停在观测模式（`RingSurvey = 1`、`GuidanceOverride = 0`）时，
> 才必须手工改回来，否则连特武的 PN 也是关的。

---

## 3. 目录结构与构建

```text
ACZPropNav\
  src\core\{mod,patcher,config,log}.cpp   DLL 启动、全部 hook/探测、INI 解析、日志
  src\hook\hook.cpp                       x64 inline hook 引擎（保守：不可重定位就拒绝）
  src\mem\{mem,discovery}.cpp             VEH+TLS setjmp 内存引擎、扫描器、结构关联
  src\pn\pn.cpp                           PN 数学（λ̇、tGo、前置点、N=4 速率律、角度混合、FOV 门限）
  loader\{xinput1_4_proxy,version_proxy}.cpp
  tools\{initest,pnsim,memtest,hooktest,smoke}\  离线测试
  tools\arc\{arc_tool,wpb_dump}.py        .arc 解包/重打包、.wpb 结构 dump
  tools\re\*.py                           逆向工具（见 §8）
  docs\*.md                               全部结论与证据
  work\                                   中间产物（解包 exe、扫描结果、一次性脚本）
```

构建 / 测试（**不要用 make**，这个 sandbox 里 GNU make 的 jobserver 管道会挂）：

```bash
cd ACZPropNav
./build.sh          # 全量编译（无增量依赖，每次全编，避免过期 .o 导致 ABI 崩溃）
./build.sh test     # 编译 + 跑全部离线测试
./build.sh clean
```

工具链（这台机器上的实际路径）：

```text
CC/CXX   C:\Users\silic\scoop\apps\mingw-mstorsjo-llvm-ucrt\current\bin\x86_64-w64-mingw32-{gcc,g++}   (clang 23.1.4)
objdump  同目录 llvm-objdump.exe
python   C:\Users\silic\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe   ← PATH 里没有 python！
```

---

## 4. 环境事实（踩过的坑，别再踩）

1. **`acz.exe` 被 SteamStub 3.1 包裹**：`.bind` 段在导入解析完之后才解密 `.text`。我们的 DLL 在导入解析
   期间就已驻留，所以**任何代码字节校验 / 打补丁都必须延后重试**（`Tick()` 每 200 ms 重试，最多 60 s；
   RingSurvey 每 500 ms，最多 120 s）。这是"integrator bytes mismatch"的唯一原因。
2. **静态分析用解包副本**：`work\acz_copy.exe.unpacked.exe`（3,811,952 B，image base `0x140000000`，
   section RVA 与活进程一致）。**RVA = 虚拟地址 − 0x140000000**。
3. **沙箱不能写 `ACZero\`**（workspace-write 只允许工作区）。改游戏目录、复制 DLL、改 INI 都必须
   由用户手工操作（资源管理器拖放 / 文本编辑器），或者在会话里明确让用户操作。
4. **INI 是 UTF-8 + BOM，只能按字节安全地改**：BOM-less UTF-8 被 `Get-Content` 按 GBK 解码会变乱码
   （曾经把文件写坏）。现在没有任何脚本会重写 INI，全部由用户手工编辑，所以给用户的规矩是：
   **用能保留 BOM 的编辑器存成 UTF-8（带 BOM），不要让它改掉行尾（LF/CRLF 都行，别混着换），
   中文注释照原样留着**。改之前先复制一份 `.bak`。解析器本身是 BOM 感知且 CRLF/LF 都能吃的，
   所以坏掉只可能是因为编辑器把编码或行尾整体改写了。
   *（历史：删除前的那批 `.ps1` 用 Latin-1 1:1 字节处理 + 时间戳备份 + 写后校验 + 自动回滚。）*
5. **`llvm-objdump` 不接受 `-b binary`**：要验证手写机器码，用 `clang -c` 汇编等价指令再逐字节比对
   （`work\thunk_ref.s` + `work\thunk.bin` 就是这么验的，结果 `equal True`）。
6. **Ghidra MCP 在这个 session 里用不了**：`open_binary` 成功，但之后每一次 provider 调用都超时
   （12.1.4 的 import / auto-analysis 一直没完成）。所以逆向全部走 `tools\re\*.py`，单次查询约 0.5 s。
7. **两次真实的崩溃**，都是探测太激进，已移除：
   * 2026-10-25 16:58:45：旧探测从 vtable 值命中处**追指针** → `ucrtbase` 0xc0000005；
   * 2026-10-26 00:32:28：`PAGE_GUARD` 写监视 → unhandled guard fault（`0x80000001`，ntdll）。
     现在 `ArmPageGuard` 的挂载逻辑已删除，探测一律只读。
8. 日志里有 `caller` 是**返回地址**，用 `exe+0x...` 表示；判断"是不是特武"用区间
   `0x90000..0xA1FFF`（7 个 WeaponEx 更新函数都在里面）。

---

## 5. 已验证的技术事实（本版本 v2.0.0.1）

### 5.1 加载与生命周期

```text
XINPUT1_4.dll 代理（游戏只用到 ordinal #2/#3）→ LoadLibraryW("ACZPropNav.dll") → AczCoreRun()
社区 loader 路线：version.dll → ACZero\mods\ACZPropNav.dll → AczAddonInit(void* hostCtx)
  hostCtx: +0x08 游戏目录 wchar_t*，+0x10 acz.exe HMODULE，+0x28 printf 风格 logger
DLL 内部：DllMain 只开一个低于 normal 优先级的线程，全部工作在 ModThread 里
```

### 5.2 特殊武器（已可用）—— 共享转向积分器

| 事实 | 值 |
|---|---|
| 共享积分器（速度提交点） | `exe+0x90510`，签名 8 B `48 8B C4 C5 FA 11 58 20`，12 B 唯一化 |
| 调用者（7 个） | `0x9165D 0x99B50 0x9AFB0 0x9D168 0x9E030 0x9E990 0x9F230` |
| 每发导弹更新 | `exe+0x91DC0`；分发器 `exe+0x9DBB0` |
| 导弹槽 | `ctrl+0x80 + i*0x68`，16 槽；活标志位图 `ctrl+0x5C` |
| 槽布局 | `+0x00` pos vec4(m)、`+0x10` heading、`+0x20` velocity **m/frame**、`+0x30` 目标 pos、`+0x50/51` 目标索引、`+0x52/53` 有效位、`+0x54` age(s)、`+0x5C` 自身句柄 |
| vtable | QAAM `0x2AF740`、SAAM `0x2AFAE8`、XMAA `0x2AF308`、XLAA `0x2AF560`、XAGM `0x2AF980` |
| 继承链 | `WeaponEx*Ctrl` → `WeaponExCtrlBase` → `WeaponRef`；**只有 vtable 槽 7、8 被各类重写**，槽 8 = 导引律 |

工作方式：钩 `0x90510`，让游戏先算完，再用 PN 覆盖 `slot+0x20..0x2C` 的速度（保持速率大小），
用 `IsMissileSlot()` 证明槽位确实属于某个 `Weapon*Ctrl` 且位图置位后才写。

### 5.3 普通 MSL（未解决）

* **不在** `WeaponEx` 体系里，也不会调用 `0x90510`（MSL-only 的实跑里 `integratorCalls` 为 0）；
* 走一个面向过程的投射物池：单例 `exe+0x3219A0`（getter `0x19F30`、alloc `0x1B2C0`、init `0x1B08E`），
  `0x70` 字节节点，侵入式链表 `S+0x20`(state 6) / `S+0x98`(state 2) / `S+0x290`；
* 节点已确认字段：`+0x34..0x3C` 位置、`+0x40..0x48` 方向/速度、`+0x4C` 剩余寿命（会递减）、
  `+0x66/+0x67` 射手 obj/part 索引、`+0x6B` 状态；
* **分配点只有 6 个**（每个都在池 getter 之后紧接 alloc）：
  `0x1254A2 0x12B631 0x13B09B 0x13D665 0x1412B4 0x1C7080` ← 这是下一步的抓手；
* 目标来源两个候选：`node+0x28`（唯一的指针形状字段）或射手状态 `part+0x1B8 → fire+0x16xx`；
* 玩家锚点全局：`exe+0x32B438`（位置在 `+0x258`）。

### 5.4 数据侧（`.wpb` / `.arc`）

* `.arc`：dword 流 XOR 加密（`ks[i]=(A+B*i) mod 2^32`，`A=k1^k3^0x5DF38C35`，`B=k2^k3^0x4CE49F4D`）
  + 前置索引 + 每条目 LZ4 块。工具：`tools/arc/arc_tool.py`（社区 `aczarc.py` 在 `tools/arc/upstream/`）。
* `data/mis/mpl/*.wpb`：96 字节记录。`mis`：`lead 40 @+0x3C`、`startAng 90 @+0x5A`、`range 3000 @+0x24`、
  `scope 1600 @+0x20`、`speed 1000 @+0x54`、`type 2 @+0x58`；`misf` lead 80/startAng 60；`misf2` 28/30；`mis_old` 10/30。
* **但**：这个版本运行时会把记录重新序列化成引擎结构，**磁盘布局的内存扫描必定失败**（已验证）。
  所以静态数据补丁路线不通，只能改文件重打包（§7.2）。

---

## 6. 已排除的路线（含为什么，别重复）

### 6.1 直接改"参数表"（AC8 的做法）
ACZ 运行时没有 `HomingForesightAmount` 等价字段可写，且 `.wpb` 记录在内存里不是扁平 96 字节 → ❌（除非改文件，见 §7.2）

### 6.2 在投射物池节点上写速度
池节点不是活导弹的推进结构（测得位置不推进），且找不到 `pos += dir` 的静态写入点 → ❌（改走 spawn 捕获，§7.1）

### 6.3 `PAGE_GUARD` 写监视 / 追堆指针
崩过两次，已移除 → ❌ 不要再用

### 6.4 尾迹环 `EFF_MIS_POSDIR` 反推制导（**2026-10-26 实跑结论，重要**）

事实（`docs/MSL_TARGET.md` §9–§10 有完整反汇编）：

```text
环 vtable 0x2ACC68；对象 = CMissionControl + 0x240（ctor exe+0x16D980，mc vtable 0x2ADA38 在 mc+0x108）
条目 mc+0x248 + idx*0x20 = { pos vec4, dir×3 + phase }
写入者 vtable 槽 2/3 = exe+0x3B300 / exe+0x3B370（叶子函数，无 .pdata，只能虚调用）
实跑记录到的 6 个调用点：0x54BC6 0x54BDD 0x54BF7（func 0x54892）、0x54CB3 0x54D72（func 0x54C57）、
                          0x336AE（func 0x335DC）
结论：这些全是**尾迹特效对象**的 update：位置只是 `rcx` 传进来的一份拷贝，
      entry[3] 是尾迹相位（来自浮点表 0x32A3C0），entry[4..6] 是特效参数块（[vtable+0x28] 返回），
      既没有速度也没有目标。上层驱动是特效管理器 0x35C76（0xD9C 字节，0x364E6 调用 0x54830）。
```

**⇒ 尾迹环只能回答"导弹在哪"，无法回答"导弹怎么转向"。这条路封死。**
（但 `RingSurvey` 这套只读观测钩子保留在树里，它证明了 93 字节 detour thunk 在真实进程里能跑。）

---

## 7. 下一步（按性价比排序）

### 7.1 抓 spawn → 逐帧采样（推荐，需要 1–2 次真机运行）

```text
1) detour exe+0x1B2C0（池分配），记录返回值（新节点）与 [rsp] 返回地址（哪个 spawn 分支）
2) 对每个新节点做只读采样（复用 ProbeProjectilePool 的"哪些 float 在推进"逻辑）：
   期望得到 位置 / 速度(m per frame) / 寿命 三个偏移 → 与已知 +0x34/+0x40/+0x4C 交叉验证
3) 导弹在中飞行时，换一次锁定目标：观察节点里哪个指针/向量跟着变 → 目标来源
   （node+0x28 还是射手 fire+0x16xx）
4) 在 MSL 的推进处（或复用同一个速度提交点）写 PN —— 与特武路径同样的做法
验证点：key.log 出现 "MSL node ... vel=(...)" 且速度大小不变、方向提前。
```

需要看的日志行：`RING`/`MSL` 前缀都可以参照；采样一定要**只读**，不要再 PAGE_GUARD。

### 7.2 数据路线（最接近 AC8 的做法，1 次运行即可证伪）

```text
1) tools/arc/arc_tool.py 解包 a.arc，只取 data/mis/mpl/
2) 把 mis.wpb 的 lead（+0x3C）从 40 改大（例如 200），保留 .bak
3) 重新打包 a.arc（校验和按 docs/ARC.md 的算法写回）
4) 飞一局看弹道是否开始提前
```

* 有效 ⇒ `lead` 就是 ACZ 版的 `HomingForesightAmount`，INI 那个键可以直接接上去（甚至做成"启动时
  在内存里改这份数据"，避免每次重打包）；
* 无效 ⇒ 说明 lead 只用于发射瞬间的瞄准点，与飞行中的制导无关，安心回到 §7.1。

### 7.3 特效状态里找宿主指针（便宜的附加实验）

上一轮只映射了尾迹状态结构 `r15` 的 `+0x08..+0x64`，没有把它整个 dump 出来。如果它带一个指回
导弹对象的指针，那就是通往 MSL 对象的第二条独立路径。与 §7.1 可以合并到同一次运行里。

---

## 8. 逆向工具（这个 session 里新写的，Ghidra 不可用时的替代品）

| 工具 | 作用 |
|---|---|
| `tools/re/xref.py 0x90510` | 快速交叉引用：直接 call/jmp、任意 RIP 相对操作数、数据区绝对指针，全部按 `.pdata` 归属到函数。首次扫描 ~0.5 s，结果缓存在 `work/xref_cache.json` |
| `tools/re/vtable_diff.py bases/dump/diff` | 走 RTTI：COL → CHD → 基类链（`bases`），逐槽 dump（`dump`），多类并排 diff（`diff`） |
| `tools/re/find_node_writers.py` | 扫 SSE/标量存储的位移，找"在 +0x34/+0x40/+0x4C 上写 float"的函数 |
| `tools/re/{rtti_scan,dump_vtable,owners,xref_consts,pe_report,iat,find_ptrs,build_func_index}.py` | 更早的 RTTI / vtable / 所有者 / 常量引用工具 |
| `tools/re/...` 依赖 | `work/acz_copy.exe.unpacked.exe` 与 `work/acz_rtti.tsv`（404 个类） |

常用命令：

```bash
PY="python"
cd ACZPropNav/tools/re
"$PY" vtable_diff.py bases 'WeaponExQaamCtrl'
"$PY" vtable_diff.py diff --max 24 'WeaponExQaamCtrl@' 'WeaponExSaamCtrl@'
"$PY" xref.py 0x90510 0x91DC0 0x1B2C0
"$PY" find_node_writers.py --want 0x4C --also 0x34,0x40,0x44,0x48
llvm-objdump \
  -d --x86-asm-syntax=intel --no-show-raw-insn \
  --start-address=$((0x140000000+0x54830)) --stop-address=$((0x140000000+0x54C57)) \
  work/acz_copy.exe.unpacked.exe
```

---

## 9. 文档索引

| 文档 | 内容 |
|---|---|
| `docs/RE.md` | AC8PropNav 的真实机制与对照表（§4）、ACZ 二进制总览 |
| `docs/ARCHITECTURE.md` | 代码结构与数据流 |
| `docs/PN.md` / `docs/CALIBRATION.md` | PN 数学、参数标定、LeadScale/LeadMode |
| **`docs/MSL_TARGET.md`** | **普通 MSL 的全部证据、目标来源候选、§9 尾迹环、§10 实跑结论 + 下一步** |
| `docs/MSL_GUIDANCE.md` | 池节点、偏移推导、历史探测记录 |
| `docs/GUIDANCE_ASM.md` | 积分器 `0x90510` 的汇编级分析、槽布局、thunk 设计 |
| `docs/RTTI.md` | RTTI 全表与继承链 |
| `docs/RUNTIME_PARAMS.md` | 为什么运行时找不到 `.wpb` 扁平记录 |
| `docs/ARC.md` / `docs/VANILLA.md` | `.arc` 格式与重打包、原版数值表 |
| `docs/VERIFICATION.md` | 每一次真机运行的记录（含两次崩溃的现场） |

---

## 10. 恢复现场：接手后前 10 分钟

1. `cd ACZPropNav && ./build.sh test` → 必须全绿（否则先修环境，别往下走）。
2. 读 `docs/MSL_TARGET.md` §9–§10（尾迹环为什么不行）+ §4（目标来源候选）。
3. 确认 `ACZero\ACZPropNav.ini` 的 `[runtime]` 现在是观测模式（`RingSurvey=1`、`GuidanceOverride=0`）；
   想让用户正常玩就把 `GuidanceOverride` 改回 `1`、`Probe`/`RingSurvey` 改回 `0`（手工编辑 INI）。
4. 看最近一次日志：`ACZero\ACZPropNav.log`（主）与 `ACZero\ACZPropNav.key.log`（`RING:` 行）。
5. 选一条路线动手：**§7.1 抓 spawn**（首选）或 **§7.2 数据重打包**（最快出结论）。
6. 改动前确认 `ACZero\acz.exe`（4,046,008 B，mtime 未变）与 `a.arc` 未被我们修改。
   现在的安装不再自带备份：改 INI 前自己复制一份 `ACZPropNav.ini.bak-<时间戳>`。
   *（历史：已删除的 `install.cmd` 会把旧文件备份到 `ACZero\ACZPropNav_backup\`。）*
7. 任何新的探测都遵守：**只读、有界、单层指针、失败就放弃**。两次崩溃的代价已经付过了。

---

## 11. 2026-10-26 晚间进展（Ghidra 复活 + Run A 探针就绪）

### 11.1 环境变化：Ghidra 12.1.4 现在可用

上一 session 里 REA/Ghidra 每次调用都超时，**这次完全正常**。镜像
`work/acz_copy.exe.unpacked.exe` 已经作为活动目标打开（base `0x140000000`，
5034 个函数，PDB 名 `acz.pdb` 但无符号）。查询方式：

```text
mcp__rea__procedure_pseudo_code   procedure=0x14013D180
mcp__rea__procedure_callers       procedure=0x1401B9A80
mcp__rea__procedure_references    procedure=<addr> direction=incoming
mcp__rea__inspect_native_instruction address=0x1403219A0
```

用同一镜像、不要 `open_binary` 别的目标、不要 `close_binary`。镜像里 `.text` 是
SteamStub 解包后的副本，VA = RVA + 0x140000000。

### 11.2 新文档（今晚新增，全部含 RVA/asm 证据）

| 文档 | 内容 |
|---|---|
| `docs/WEAPON_DISPATCH.md` | 每帧武器/投射物调度器 `0x1B9A80`（帧计数 `0x33ED94`、全局时间链表 `0x33EDB0`、按 state `+0x6B` 分派）；三个类别 spawn 函数；`0x1B87B0` = 帧末 + 任务重载锚点；`0x1627C0` = WeaponModel 头 `+0x08..+0x28` 描述符挂接 |
| `docs/WEAPON_MODEL_LAYOUT.md` | **关键突破**：普通 MSL 参数不是 WeaponModel+0x48 的扁平数据；运行时代码读独立扁平记录指针 **P**（`fire+0x1FB8`，也存到 MSL pool `node+0x28`、WeaponEx `slot+0x40`），`P = .wpb + 0x10`。偏移：`P+0x10` scope、`P+0x14` range、`P+0x24` timeStep、**`P+0x2C` lead**、`P+0x44` speed、`P+0x48` type、`P+0x49` attack、`P+0x4A` startAng（带读点表；`lead`/`startAng` 全镜像唯一读点 `0x9B3F8/0x9B3F4` 在 SodCtrl slot-8 guidance，普通 MSL 的 per-frame 读点仍未定位） |
| `docs/MSL_ADVANCE.md` | **穷举证明 pool 节点没有每帧写入者**（所有写者都是一次性 spawn/init），PN 不能写在 pool 节点；真正每帧写 `part+0x258` 的是调度器 `0x1B9A80` 的三条提交路径；每帧链路 `0xD1F05 → 0x1B7B00 → 0x1B7D49 → 0x1B9A80`；**没有 `pos += vel` 积分器**，PN 必须每帧硬覆盖 pos+heading。§11 是 Run-A 探针契约（0x1B7B00 入口字节、world 偏移表、`exe+0x3409F2` = 玩家对象索引） |
| `docs/MSL_FIRE_OBJECT.md` | fire 对象 0x27A0 字段图；**锁定目标 = 字节对 (obj,part)，0xFF=无**：权威 `WC+0x1EEC/1EED`、每帧镜像 `part+0x236/237`（HUD 锁定框）、发射判定 `FC+0x236/237`、排队射击 `FO slot+0x19A/19B`；目标位置 = `arr[tObj]->+0x110 + 0x520*tPart` 再 `+0x258`；目标速度不存在 → 用 `0x33ED94` 帧号差分 |
| `docs/PROBE_RUN_A.md` | 只读 Run A 探针：装了什么、日志每一行什么意思、运行步骤、决策树、偏差与风险 |
| `docs/PROBE_RUN_B.md` | 硬件数据断点设计（下一阶段） |

### 11.3 Run A 探针（已实现，`./build.sh test` 全绿：initest 268 / pnsim 15 / memtest 85 / hooktest 90 / smoke）

> 注：本节及 §11.9 里的 `pnsim NN` 是**当时**的断言数，属历史记录。当前是 **pnsim 62**。

Run A 探针的开启方式是在 `ACZero\ACZPropNav.ini` 的 `[runtime]` 里手工写
`Probe = 1, RingSurvey = 0, GuidanceOverride = 0, DryRun = 0`，并把新 DLL 复制到 `ACZero\`
（原来这一步由 `probe_model.cmd` / `probe_model.ps1` 一键完成，**脚本已删除**）。
探针装 3 个只读 hook（都是延迟到 `.text` 解密后、每 200 ms 重试）：

```text
exe+0x201460  pool Allocate（只记录 caller = 0xC767D / 0x16E14B）   stolen 17 B
exe+0x1B2C8  pool 节点弹出（0x1B2C0 入口有 jg rel8 不能 hook，改 hook 其 jg 目标）
exe+0x1B7B00  每帧 world tick（rcx=world）                          stolen 15 B
```

日志前缀 `PROBE-A:`：ALLOC / POP / PARAM（校验 P 的 data_off、scope、range、lead、
speed、type、attack、startAng）/ WORLD 快照（object+part0 的 pos/heading、perObj
+0x70..+0x8C、pool L2）。**不写游戏内存**；key.log 上限已从 250 KiB 提到 512 KiB，
一次运行约 250 KiB 不会轮转。

### 11.4 硬件断点事实（Run B 的前提，已离线验证）

`work/dr2.cpp` 在 sandbox 里证明：数据断点以 **fault** 交付（RIP=访问指令本身），
仅设 `RF` 再 `CONTINUE_EXECUTION` 会无限重触发、进程以 `0x80000004` 被杀。
正确做法 = **one-shot**：VEH 里记录 `Rip/Dr6`，把 context 的 `Dr0..Dr3/Dr7` 清零后再
`CONTINUE_EXECUTION`；Tick 里约 100 ms 后重新 arm。多线程要逐线程 `SetThreadContext`
（挂起→改 DR→恢复），新线程定期补装。`tools/hwwatchtest/` 是该机制的离线验收测试
（暂未并入 `build.sh test`）。

### 11.5 下一步（下次 session 直接照做）

1. 用户已跑完一次 `Probe=1` 会话后（当年这一步是靠已删除的 `probe_model.cmd` 开的）：读
   `ACZero\ACZPropNav.key.log` 的 `PROBE-A:`
   行，确认 P 布局，并在 WORLD 快照里找出"从发射点出现、每帧积分"的那个 world object/part。
2. 实现 Run B：用 `playerPart+0x1B8 → fire+0x1FB8 → P`（或 `node+0x28`）取 P，校验
   `*(u32*)(P-0x10)==0x10`，然后对 `P+0x2C`(lead)、`P+0x4A`(startAng)、`P+0x14`(range)
   下 one-shot 硬件读断点；`PROBE-B:` 记录命中的 `Rip`，静态反推读取函数。
3. 读者 RVA 到手后：若 MSL 的 per-frame 制导函数读到 lead → 在那里按 `WeaponEx` 的方式
   算 PN；若只有发射读 lead → 走 `docs/MSL_ADVANCE.md` §11.4 的 `pn_tick`（每帧硬覆盖
   `part+0x258..0x270`，从上一帧 game 值积分，`part+0x3C0` 变化时重播种）。
4. 数据路线备选：唯一写 `fire+0x1FB8` 的点是 `0x0C78C7`（`FUN_1400C7120` 阶段 1），
   在那里按 INI 改 `P+0x10/0x14/0x2C/0x4A`；P 可写、很可能全任务共享（需探针确认）。

### 11.6 Run A 实跑结果（2026-10-26 21:14–21:18，详见 `docs/RUN_A_RESULTS.md`）

**参数侧已彻底解决 [V]：** 实跑抓到了玩家普通 MSL 的扁平记录 P（node+0x28，
spawn 返回地址 `exe+0x13D703`），并逐字段验证：

```text
P = .wpb + 0x10,  P-0x10 = u32 data_off = 0x10
P+0x10 scope=1600  P+0x14 range=3000  P+0x24 timeStep=4  P+0x2C lead=40
P+0x44 speed=1000 (u32/u16)  P+0x48 type=2  P+0x49 attack=40  P+0x4A startAng=90
```

`docs/WEAPON_MODEL_LAYOUT.md` 的预测全部命中；探针打印 `rec VALIDATED (variant=mis)`。
同一次运行还抓到 WeaponModel 分配 `size=0xD08 -> payload=0xCC0`（类型化载荷，未 dump 到）。

**遗留问题：**
* 0x1B7B00 的 world hook 装上但**从未进入**。re-msl 复核（`docs/MSL_ADVANCE.md`
  §11.5–§11.8）：`0xD1F05` 确实 `call 0x1B7B00`，但它是**状态机 phase 7 专用**，
  受 `*(u8*)(arg2+0x3CC)==7 && *(u8*)(world+0xA8822)∉{4,5,8} && *(u8*)(mc+0x18)!=0`
  门控；且 `exe+0x33ED94`（帧计数）全镜像只有调度器一个写者 → 用这个计数器就能判定
  "调度器到底跑没跑"。**Run B 不再依赖任何 world hook**：`MC = *(u64*)(base+0x3409F8)`
  已被证明就是 world（与 S+0x428、dispatcher 的 param_2 同一对象）。
* 8 次 `spawn@0x13D665`（用户打了 10 发）中只有第一次的 P 是 `mis`；其余 P 指向别的
  武器记录（如 scope 4000/lead 16），说明 pool-pop 会看到玩家/僚机/敌机的所有 spawn。
* pool 节点采样到的 8 个节点全是 state 7 的静态特效锚点（12 次采样 `changed=0`），
  再次印证"pool 节点没有每帧写入者"。

### 11.7 Run B（硬件读断点探针）已实现，`./build.sh test` 全绿

`src/core/probe_watch.h/.cpp`，INI `[runtime] HwWatch=1` / `HwWatchSeconds`（默认 25，
当年脚本把它设成 60）。开启方式：手工把这两个键写进 `ACZero\ACZPropNav.ini` 的 `[runtime]`
段并复制新 DLL 到 `ACZero\`（原来由 `probe_watch.cmd/.ps1` 完成，**脚本已删除**）。流程：

```text
1. 等任务数据就绪：MC = *(u64*)(base+0x3409F8)（回退 *(S+0x428)）
2. playerIdx=*(u8*)(base+0x3409F2) -> objArray[playerIdx] -> +0x110 -> +0x1B8 -> +0x1FB8 = P
   校验 data_off/scope/range/lead/speed；失败则扫描 objArray[0..32] 回退，并打印原始值
3. 帧计数 0x33ED94：启动与 +2 s 各读一次 -> `DISPATCHER COUNTER ADVANCES/FROZEN`
4. 7 个已知 vtable 存在性扫描（WeaponExFireCtrl/Qaam/Saam/Xlaa/Xmaa/Xagm/WeaponModel），
   FireCtrl 命中时 dump mask+0x5C 与活动槽（pos/heading/vel/target/age）
5. 对所有其他线程装 Dr0=P+0x2C(lead)、Dr1=P+0x48(type/attack/startAng)、
   Dr2=P+0x14(range)、Dr3=P+0x44(speed)（R/W=11, LEN=11）
6. 首处理器 VEH：记录 {slot,Rip,regs,rsp,tid}，清 Dr0..3/Dr6/Dr7 后继续（one-shot，无 RF；
   `tools/hwwatchtest` 27 项离线断言证明该机制，含跨线程）
7. 每 200 ms 重新装；HwWatchSeconds 或 400 次命中后解除；`HIT/RAW/RIPSUM` 行给出
   `rip=exe+0x...` 与 `[Rsp]/[Rsp+8]` 调用者提示
8. 同时 250 ms 一次 MOV 快照（最快移动的对象/part，top-3，pos/dpos/head/engineFrame）
   与 POOL 快照（node+0x28==P 的节点，最多 8 个）；可选 allocator hook 抓 WeaponModel
   并 dump `obj+0x48..`（含 1600/3000/40/90/1000 标记）
```

**下一步（判定表）：**
* `HIT` 出现在 `P+0x2C`/`P+0x4A` 且命中函数是每帧调用 → 那就是普通 MSL 的制导读取点，
  在该函数里接 PN（或直接按 AC8 方式改数据）。
* 只有发射/命中时命中，或完全没有命中 → `lead` 对普通 MSL 无用，数据路线死；按
  `docs/MSL_ADVANCE.md` §11.4 实现 `pn_tick`（每帧硬覆盖 `part+0x258..0x270`），
  目标按 `docs/MSL_FIRE_OBJECT.md` §6 读 lock 对 + `part+0x258` + 帧号差分。
* `DISPATCHER COUNTER FROZEN` → 普通 MSL 绕过调度器；MOV 快照会指出真正在动的对象，
  据此重新定位推进函数。
* vtable 扫描若发现 `WeaponExFireCtrl` 活对象 → 普通 MSL 可能仍是控制器类，PN 可复用
  特武路径（先看它的 slot-8 是否读 P）。

### 11.8 Run B 实跑结果（2026-10-26 21:56–21:58，详见 `docs/RUN_B_RESULTS.md`）

**结论：普通 MSL 的每帧制导函数 = `FUN_14013ECA0`（RVA 0x13ECA0）**。硬件读断点证据：

```text
slot0 P+0x2C (lead)  88 hits  reader 0x13EFA2  vmovss xmm0,[rax+0x2c] (rax=P)  in FUN_14013ECA0
slot3 P+0x44 (speed)  6 hits  reader 0x1039F5  in FUN_1401031F0 (发射/火力控制)
slot1 P+0x48          0 hits
slot2 P+0x14 (range)  0 hits
帧计数 0x33ED94 全程为 0 -> 普通 MSL 确实绕过 0x1B9A80 调度器
```

`FUN_14013ECA0(ctrl)` 是**全世界只有一个**的 16 槽武器管理器更新
（`ctrl = world + 0x6A130`；mask `u32 ctrl+0x6E8`；slot = `ctrl+0x158 + i*0x58`）。
每个 slot：`+0x00` pos vec4、`+0x10` 四元数、`+0x20` **本帧位移输出缓存 vec4**（写它
不控制轨迹）、`+0x30` P 指针、`+0x38` 飞行距离、`+0x3C` 转速累加、`+0x40` u16 尾迹
句柄、`+0x42/+0x43` 目标 (obj,part)、`+0x44/+0x45` 发射者 (obj,part)、`+0x46/47` 标志。
制导数学：`startAng = tan(byte[P+0x4A]*π/180)` 做视场门限；
`maxTurnPerFrame = float[P+0x2C]*π/180 / 60`（`ctrl+0x6EC` 恒为 60 = FPS，lead 单位是**度**）
做每帧最大转角；目标位置 = `objArray[tObj]->parts+0x110 + 0x520*tPart + 0x258`
（objArray 可由 `[ctrl+8] -> [that]` 直接取，或 world+0x697B8）；命中距离 =
`float[P+0x14]*5.12`；发射速度用 `u16[P+0x44]`。

**对模组的含义：** `lead` 只是"每帧最大转角"限制，不是提前量——单纯把 lead 调大只会让
导弹转得更急，但仍然是追着目标当前位置（无法提前拦截横穿目标）。真正的 PN 必须在
`FUN_14013ECA0` 之后**直接改 slot 的位置**（位置是积分量、不会被四元数重算覆盖；
写 +0x20 不改变轨迹）。

### 11.9 普通 MSL 的 PN 实现（**已实现，离线全绿**）

新增 `src/pn/msl_pn.h/.cpp`（hook + 槽位扫描 + 日志）、`src/pn/msl_steer.h`
（纯 PN 数学，`pnsim` 单测）、`docs/MSL_PN.md`；patcher 在 `Start/Tick/Shutdown`
里接线；`hooktest` Test8 校验 15 字节 prologue；`pnsim` (i1)–(i16) 校验 Step-1A 数学。
**公式与语义已被 §11.10 的 STEP 2 取代**（当前 `./build.sh test`：initest 268 /
**pnsim 54** / memtest 85 / **hooktest 98** / hwwatchtest 27 / smoke PASS）。

hook `exe+0x13ECA0`（15 字节可重定位 prologue `48 8B C4 | 48 89 58 08 |
48 89 70 18 | 48 89 78 20`，边界 `0x13ECAF`），detour 先调原函数、再
`msl_pn::Apply(ctrl)`。**ctrl 是 world 级武器管理器（world+0x6A130），每帧一次**：

```text
for each live slot:
    owner=(slot+0x44,+0x45)；tObj/tPart=(slot+0x42,+0x43)
    amount = [player]/[enemy] msl HomingForesightAmount（owner==playerIdx ? player : enemy）
    P=*(u64*)(slot+0x30) 校验 type==2；speed=u16[P+0x44]
    p=slot+0x00；v=pos-prevPos（自己按帧缓存，不用 +0x20 — 那是输出缓存）
    R=targetPos-p；Vt=目标位置逐帧差分；vc=|v|-dot(R̂,Vt)（下限 0.2|v|）；tGo=|R|/vc
    aim = targetPos + Vt*tGo*amount；d=normalize(aim-p)
    theta=acos(dot(v̂,d))；thetaMax=(MaxRotationAngle|180)*dt_game*π/180，dt_game=|v|/speed
    vNew=|v|*rotate(v̂→d by min(theta,thetaMax))
    slot+0x00/+0x04/+0x08 += (vNew - v)；slot+0x0C 保持 1.0f；可选写 +0x20..+0x2C=vNew
    不动 ctrl+0x6E8、slot+0x30/+0x40/+0x42..0x45
```

**关键修正（re-msl 静态确认）**：游戏每帧重算 `slot+0x20..0x2C` 并把它加到
`slot+0x00`，所以**只写 +0x20 不会转向**；转向=在 post-update 里按
`(vNew - v)` 修位置（`slot+0x0C` 保持 1.0f）。目标数组优先用游戏自己的
ctrl 相对路径 `*(u64*)(*(u64*)(ctrl+0x08))`（`0x13ED70`），`world=*(u64)(base+0x3409F8)`
作为回退。`lead` 字段单位是度；`ctrl+0x6EC = 60`（FPS）。

DryRun 时只算不写。v1 限制：不写四元数（视觉朝向可能略滞后轨迹）、
`MaxHomingAngle/LifeTime/LockonRange` 尚未接入、**`[enemy] msl = 0.3` 默认生效
（NPC/僚机 MSL 也会被引导；置空/0 可只作用于玩家）**。
`Tick` 每 2 s 打一条 `MSLPN` 汇总；如果 `FUN_14013ECA0` 超过 5 s 没被调用
（world gate/暂停/加载）会明确警告一次，避免白跑一次实机。
验证方式：`HomingForesightAmount=0` 与 `=1` 各打几发横穿目标对比（或看日志里
`MSLPN applied` 的修正量）。


### 11.10 STEP 1B + STEP 2（槽位朝向接管 + 真 lambda-dot PN + 校准修复）

**STEP 1B（已实机验证，用户反馈飞行"perfect"）**：槽位 `+0x10` 是三段欧拉角（不是四元数）。
`FUN_140202FA0`（RVA `0x202FA0`，欧拉→4x4 矩阵）与 `FUN_1402036D0`（RVA `0x2036D0`，
rotvec 旋转欧拉角，只写出 `out[0..2]`）都是纯数学函数，入口字节已由 `hooktest` Test9 对照
`work/acz_copy.exe.unpacked.exe` 校验。每帧在位置修正后，把"游戏朝向 → PN 方向"的 rotvec
作用到 `slot+0x10/+0x14/+0x18`（`+0x1C` 不动）。矩阵前向轴（6 个候选：3 个行主序列 + 3 个行）
与 rotvec 符号/坐标系由一次性**自动标定**决定，标定只在 scratch 欧拉值上做，不写活导弹。

**STEP 2（本次）**：`HomingForesightAmount` 的语义从"前置量比例"改为**制导律混合**：
`0 = 纯追踪，1 = 真 PN（N=4）`，中间值线性混合两个旋转速率需求。公式见
`docs/MSL_PN.md` 的 STEP 2 段与 `src/pn/msl_steer.h::ComputePn`。要点：

```text
lambdaDot = Cross(R, Vr) / r2 ,  r2 = max(r^2, rmin^2) ,  rmin = max(2*|v|, 25 m)
omegaPN = 4 * lambdaDot ;  omegaP = 一帧纯追踪需求
omegaCmd = (1-amount)*omegaP + amount*omegaPN
turn = min(|omegaCmd|, MaxRotationAngle*dtGame) ;  vNew = rotate(uV, axisCmd, turn)*|v|
```

* Step-1 的终点硬编码（250/1200 m taper、tGo taper、1500 m 内目标在后就丢弃前置量）**已删除**，
  改为 `rmin` 软奇点保护（近距离 lambdaDot 不会爆掉）；amount EMA 与目标速度 0.6/0.4 低通保留。
* **校准修复**：旧版在第一次修正时标定一次；若当时指令转角只有 ~0.6°，四个变体分数完全分不开，
  于是整局禁用朝向写入（日志 `ok=0 scores=[0 0 0 0]`，`orient=0`）。现在只在
  `angle(heading,d) > 0.02 rad` 时尝试，最多 60 次 / 6 s 重试，日志打印六个轴点积与四个分数
  （全精度）以及原始 heading/d；仍失败才 `ok=0`（位置引导不受影响）。
* 汇总新增 `pn=` 与 `lambda=mean/max mrad/f`；首次修正明细行包含 amount/N/|R|/|lambda|/
  turn/limit/pursuit/pn 分量/|v|/vNew。
* 已知限制：`MaxHomingAngle/LifeTime/LockonRange` 仍未接入（原版导引头锥/寿命仍生效）；
  `[enemy] msl = 0.3` 默认生效（NPC/僚机也按 30% PN 混合）；朝向标定依赖运行时自检，
  若 `ok=0` 则仅视觉朝向滞后，弹道仍由位置写入正确引导。

### 11.11 双弹齐射修复（2026-10-27）——槽缓存按 owner 键碰撞 → 一枚过冲绕圈

**现象**：连发两枚 MSL 时其中一枚过冲 + 快速绕圈。**根因（Ghidra 反编译证实）**：
发射函数 `FUN_14013d180` 写入的 owner 对 `slot+0x44/0x45` 来自火控记录部件上的
`part+0x234/0x235`（该部件归属的 (机, 根部件)）；同一架机的每枚 MSL 都是
`(playerIdx, rootPart)` —— **完全相同**。旧 `Apply()` 的槽缓存键正是这个 owner 对，
于是同一帧里两枚活弹命中同一 `SlotCacheEntry`：
先处理的低槽位 A 拿到的是 **B 上帧写入的 `dir`** 当制导基准（`continuous` 为真但基准错，
λ̇ 弹速模型持续偏，误差累积）；后处理的 B 因 A 刚写过 `frame` 而**永不连续**，每帧回落到
游戏原航向。迭代序的固有不对称 ⇒ "只有一枚疯"。（此外这笔发射路径证据还顺带确认了
`part+0x234/0x235` = 部件归属、`part+0x236/0x237` = HUD 锁定镜像的语义。）

**修复**：`SlotCacheEntry` 键改为**槽行索引 `i`（0..15）**（帧连续性守卫照旧防槽位复用脏值）；
保留 owner 键**碰撞计数器** `ownCol=`（>0 即证明齐射确实共享 owner 对，纯诊断不参与制导）；
新增**末端 30 m 安静区** `quiet=`：距目标 <30 m 时把最后进场交还游戏（近炸/清锁由原版
0x13ED8D 逻辑处理），避免 λ̇ 饱和绕影响点转圈；槽缓存仍更新，目标拉走则干净续导。
数学（`ComputePn`）与写面（仅 `+0x00..0x08/+0x10..0x18/+0x20..0x2C`）不变；
`./build.sh test` 全绿（含 initest/pnsim/memtest/hooktest/hwwatchtest/smoke），
DLL 818688 B（sha `a16c9c...`，2026-10-27 22:11）。
**实机验证要点**：把 `[runtime]` 恢复成 `GuidanceOverride=1 / Probe=0 / RingSurvey=0 / DryRun=0`
并开 `LogLevel=2`（无需重编；当年这一步是跑 `enable_pn.cmd`，**脚本已删除**），
连发两枚观察 2 s 汇总：
`ownCol>`0 证实碰撞机理；`quiet>`0 且无绕圈即修复生效；两枚弹道应一致平滑。

---

## 12. 2026-11-21 发布收尾（本次会话）

### 12.1 制导行为的三处修正

**① 末端不再切回原生制导（默认全程 PN）**

之前 `msl_pn.cpp` 在 `range < 30 m` 时把最后进场交还原版制导（"安静区"）。它是在
§11.11 双弹 bug 的同一轮里加的**第二个防御性补丁**，理由是"近距离 λ̇ 饱和会让导弹
绕着命中点转圈"。这个理由不成立，也不是当时那次报障的根因：

| 论点 | 证据 |
|---|---|
| `rmin` 早就兜住了近距离奇点 | `ComputePn` 把 `r²` 钳在 `max(2·|v|,25 m)²`；真实 MSL ≈16.7 m/帧 ⇒ **33.3 m**，比 30 m 更早介入。`pnsim` (k13c) 断言 `|λ̇| ≤ |Vr|/rmin` |
| 30 m 半径的绕圈在几何上不可能 | 转角上限 ⇒ 最小转弯半径 `|v|/ω` ≈ **318 m**（默认 180°/s） |
| 交接与否结果完全一样 | **972 组几何**闭环扫描：命中率、最近距离逐项相同 |

⇒ `[runtime] TerminalZone` 默认 **0**（全程 PN）；正数保留旧行为，仅供 A/B 对比。
日志行明确打印当前模式；2 s 汇总的 `terminal=` 计数在该模式下恒为 0。

**② 混合律的真 bug：旋转矢量线性加权会互相抵消**

旧实现 `omegaCmd = omegaP*(1-a) + omegaPN*a`。纯追踪轴是 `uV×uR`，PN 轴是 `R×Vr`，
两者**通常不共线**（44,655 组几何：中位夹角 ≈45°，约 5% 超过 120°）。方向相反时
加权和几乎抵消 —— 在 bearing 5°/目标航向 285°/R=500 m/Vt=750 m/s 这个几何：

```
amount 0.00 -> 3.000 deg/frame
amount 0.50 -> 0.012 deg/frame   <-- 几乎不转弯（比值 ~1/250）
amount 1.00 -> 3.000 deg/frame
```

而出厂 INI 用的是 `0.7`，正落在坏区间里。

现改为：两个制导律各自先按每帧转角预算 `thetaMax` 钳位，得到**两个结果方向**
（纯追踪方向、PN 方向），再沿二者的大圆按 `a` 插值。端点行为不变
（`a=0` 精确等于限速纯追踪，`a=1` 精确等于限速 PN），中间值对 `a` 单调。
"依次合成两次部分旋转"也试过，净转角仍会回到 0，已放弃。

注意：在两个律**确实相反**的几何里，中间 `amount` 的净转角本来就应该比端点小 ——
那是两个律的真实折中，不是 bug。回归断言：`pnsim` (k11b1)–(k11b3)。

**③ 补上导引头视场门限（`MaxHomingAngle`）**

PN 路径此前**完全没读视场**（`grep maxHomingAngle src/pn/msl_pn.cpp` 为空），
而原版只在视场内才转动机头。结果：PN 会继续追一个原版早已脱锁的目标（实测 120°
偏轴仍能命中）。现在：半角取配置的 `MaxHomingAngle`，否则取游戏自己的 `P+0x4A`
(startAng，半角，90 ⇒ 180° 全视场)；超出视场**不写位置**，但**仍更新槽缓存**
（写入 `vGameDir`），这样目标转回视场内可以干净续导。计数 `outOfCone=`。

### 12.2 崩溃/挂死风险的修复

* **`hook::Remove` 的 use-after-free + 死锁**：原实现在 prologue 恢复失败时**仍然**
  `VirtualFree` trampoline，并且 `msl_pn::Shutdown` 无条件把 `g_tramp` 置空。两种后果：
  (a) 仍在 detour 里的游戏线程跳到已释放内存；(b) 补丁还在但 `g_tramp==nullptr`
  ⇒ `WaitTramp()` **无限自旋**，游戏线程硬挂。现在：只有 `restored` 才释放；
  失败时保留 trampoline 与指针，只停业务（`g.wanted=false`）。
* **`hook::Install` 的发布顺序竞态**：原来先打补丁、后填 `*trampoline`。这中间
  游戏一旦调用 detour，`WaitTramp()` 就会空转。现在**先发布 trampoline**，
  失败路径逐个回滚置空。
* **解码器直接解引用调用方地址**：`DecodePrologue` / `memcmp` 直接读 `target`，
  而 `[runtime] HookRva` / `HookSignature` 可指向任意地址（例如 section 末尾 32 字节内）
  ⇒ ModThread 上未捕获 AV 直接杀进程。现在统一走 `SafeCopyBytes`
  （`VirtualQuery` 逐页校验 `MEM_COMMIT` / 保护位 / `PAGE_GUARD`，**不用 `__try`**，
  因为本项目是 MinGW GCC 编译、`hook.cpp` 刻意不依赖 `src/mem`）。

### 12.3 `HomingForesightAmount = 0` 的语义

文档一直写"0 = 纯追踪（转向速率由 MaxRotationAngle 控制）"，但代码在
`!(amount > 0.001f)` 时直接 `continue`，`Wanted()` 也不接管 —— 实际效果是
**静默交还原版**，连 A/B 基线都做不了。现在：键**写了**就接管（显式 0 ⇒ 限速纯追踪），
只有**不写**这一行才是"该型号保持原版"；`GuidanceOverride = 0` 仍是总开关。

### 12.4 发布整理

`.gitignore` / `.gitattributes` / `LICENSE`（MIT）/ `docs/INDEX.md` /
`docs/RELEASING.md` / `tools/package_release.sh` 已就位。要点：

* `work/`（86 MB，805 个文件）里有**游戏可执行文件副本、解包出的原创资产、
  第三方下载、Steamless 解包工具**，**绝不可提交**；`build/` 同理。
* `install.cmd` 等**故意**是 GBK/CP936 + CRLF（cmd.exe 会把 UTF-8 的批处理解析错）。
  `enable_probe.cmd` 之前漏成**纯 LF**，已转 CRLF。
  （**脚本此后已全部删除**，这条只是当时的记录；现在发布包里没有任何批处理。）
* 个人路径（`C:\Users\silic\...`、`D:\SteamLibrary\...`）已从 docs 与
  `tools/initest/main.cpp` 清掉（后者改为读 `AC8PROPNAV_INI`，缺文件只跳过对照）。
* `build.sh test` 现在**能识别"本机跑不了 Windows PE"**：不再把 Exec format error
  报成"SOME TESTS FAILED"，而是转跑原生 `pnsim --selftest` 并明确说明哪些没跑。

### 12.5 仍未接入（明确说清楚）

* `LifeTime` / `LockonRange` 在**普通 MSL 的 PN 路径**上没有接线：射程/寿命 kill 和
  HUD 锁定圈仍由原版负责（普通 MSL 的 PN 只改位置/姿态，不改 `P` 记录）。
  特殊武器路径同理。
* 朝向接管（STEP 1B）依赖运行时自检；`ok=0` 时只关朝向写入，弹道仍正确。
* `slot+0x10` 到底是四元数还是欧拉角，`RUN_B_RESULTS.md` 与 `MSL_PN.md` **仍有分歧**
  （前者写 quaternion、后者写 3 段欧拉 + 第 4 个 float 透传）。实跑日志里若
  `MSLPN: STEP1B orientation calibration: ... ok=1`，说明按当前代码的理解是对的；
  若 `ok=0`，这一项要重新查。**这是唯一还需要真机确认的技术疑点。**

### 12.6 第二轮修补（内存层与日志诚实性）

**内存层（`src/mem/mem.cpp`）—— 跨页写入会把第二页的保护写错**

`WriteBytes` 原来用 `MakeWritable` / `RestoreProtect` 一次性把整个页范围改成 RWX 再还原。
Windows 的 `VirtualProtectEx` 对**跨多页的范围只返回一个保护值**（文档写的是第一页的），
所以还原时第二页会被写成**第一页**的保护值。一次 12 字节的 `WriteVec3` 只要跨 4 KiB
边界，就可能把第二页从 `PAGE_READWRITE` 变成 `PAGE_READONLY`/`PAGE_EXECUTE_READ`
—— 之后**游戏自己**往那页写就会崩。这正是普通 MSL 每帧要写的 `slot+0x00` 区域，
不是理论风险。

现在 `WriteBytes` **逐页保存/还原各自的保护值**（最多 8 页，≥ `kMaxTransfer` 的调用
在此之前就被拒），并且**无论拷贝成功与否都还原**。`memtest` 新增跨页用例：第一页
`PAGE_READONLY` + 第二页 `PAGE_EXECUTE_READ`，写 12 字节跨边界，断言两页各自恢复原值。
`MakeWritable` / `RestoreProtect` 保留为单页调用者用的公开 API，并在注释里写明
"多页范围会丢失页级保护值，所以 `WriteBytes` 不再用它们"。

**日志不再说谎**

* 明细行原来硬编码 `N=4`，但 `PnGain` 可配（0.5–12）；现在打印**实际**的 N。
* 汇总行原来打印 `inactive=`，但 `skippedInactive` 从未被递增过（恒为 0）；该字段已移除。
* 明细行有 `(void)tGo; (void)theta;` 两个被丢弃的值：`tGo` 在 STEP 2 里已经不存在
  （是 Step-1 前置量公式的遗留），已删除；`theta` 有实际意义，现在作为
  `losErr=`（机头与视线的夹角）打印出来。

**两个"静默降级"改成会说话**

* **帧识别（C9）**：`Apply()` 用 ">3 ms 间隔算新一帧" 来数帧，超过 ~330 次/秒就会
  连续两帧共用一个 `frameId`，于是帧连续性判定永远为假、制导基准悄悄退回游戏航向、
  目标速度差分也停住。现在检测到过快回调会累加 `fastCallbacks`，超过阈值**打一次
  明确警告**并提示上报日志，而不是安静地变差。
* **朝向标定（C10）**：原来"从第一次有活弹起 6 秒"就永久放弃欧拉朝向接管 —— 如果
  最初几发恰好是迎头（航向误差 ≤0.02 rad，全部计入 `tooSmall`），6 秒内一个可用样本
  都没有，整局就再也无法标定。现在只受 **60 次尝试**预算限制；窗口内没机会就把
  计时窗口重置，之后的发射还有机会。

**槽缓存脏值（C8）**

`continuous` 只看"上一帧写过"，不校验缓存航向是否还和游戏一致。槽行 0..15 在一枚弹
死掉的下一帧就可能被复用，继承上一枚弹的 `dir`；而修正量是 `pNew = p + (vNew - vGame)`
的形式，方向错最多会把导弹**瞬移** `2*|vGame|`（约 33 m）。现在要求
`Dot(cached, vGameDir) > 0.866`（夹角 <30°）才信任缓存，否则回退到游戏航向并计入
`staleDir=`。另外 DryRun 与末端交接这两条**不写游戏**的路径，原来缓存的是我们算出来
但没执行的 `dUnit`，现在缓存游戏实际飞的 `vGameDir`。

### 12.7 仍然需要真机确认的一件事（D1）

`slot+0x10` 到底是**四元数**还是**三段欧拉角**，两份"权威"文档互相矛盾：

| 文档 | 说法 |
|---|---|
| `docs/RUN_B_RESULTS.md` §2 | `+0x10` 四元数，喂给 `FUN_140202FA0`（quat→matrix） |
| `docs/MSL_ADVANCE.md` §12.1 | `+0x10` quaternion（guidance state） |
| `docs/MSL_PN.md` STEP 1B | 三段欧拉角；`FUN_1402036D0` 只写 `out[0..2]`，第 4 个 float 透传 |

代码按**欧拉角**实现（只写 `+0x10/+0x14/+0x18`，不动 `+0x1C`）。如果四元数说法才对，
每次朝向写入都会留下过期的 `w`，下一帧游戏积分就会用错朝向；而运行时的自动标定
会因此 `ok=0` 并**静默关闭**朝向写入，位置引导仍然正确 —— 也就是说这个错误
不会崩游戏，只会让"视觉朝向"与弹道轻微不一致。

**怎么一次性判定**：跑一局，看 `ACZPropNav.key.log`：

```
MSLPN: STEP1B orientation calibration: axis=.. variant=.. ok=1   ← 欧拉角说法正确
MSLPN: STEP1B orientation calibration: ... ok=0                 ← 需要重新查
```

`ok=0` 时该行还会打印六个轴候选的点积与四个变体分数（全精度）以及原始 heading/d，
足够静态反推。相关分歧还有一处：`RUN_B_RESULTS.md` 写积分是 `+= speed*forward`，
`MSL_ADVANCE.md` §12.2 写 `delta = -speed*axis`；标定的 `|dot|` 匹配把符号问题掩盖了，
**所以 `ok=1` 并不能单独验证坐标系符号**。

---

## 13. 2026-11-21 首次实机测试反馈（用户：生效，手感没问题）

环境：D:\SteamLibrary\...\ACZero，`[player] msl = 0.9`，跑约 5 分钟（23:00:10–23:05:23）。
日志：`MSLPN: calls=15713 slots=13175 player=3602 computed=3602 applied=3142`，
`ownCol=379`、`outOfCone=460`、`staleDir=1`、**0 次崩溃/异常**。

### 13.1 三条被日志证实的结论

1. **全程 PN 生效**：`terminal=0 (zone=0m)` 贯穿整局 —— 末端没有再切回原版，
   这正是本次改动的目标。
2. **双弹/齐射的 owner 撞键确实存在**：`ownCol=379`，说明多枚弹共享
   `(ownerObj, ownerPart)`，槽行号做键的修复是必要的。
3. **导引头视场门限在工作**：`outOfCone=460`（3602 次计算里 460 次拒绝写入），
   也就是目标确实会甩出视场。

### 13.2 日志暴露的三个真实 bug（已修）

**① 每帧转角预算被放大 3 倍（`dtGame` 单位错误）—— 影响手感最直接**

代码用 `dtGame = |v| / speedMps` 反推帧时间，但 `|v|` 是游戏自己的**位置单位/帧**，
不是米：范围 kill 把行程与 `P+0x14 * 5.12` 相比（常量 `0x1402D46B0` = 5.1199999 证实），
实机日志 `|v| ≈ 60.4` 单位/帧 ≈ 11.8 m/帧 ≈ 707 m/s @60fps。于是：

```text
60.4 / 1000 = 0.0604 s  -> 被 [1/240, 1/20] 钳到 0.050 s
真实帧时间                     = 0.01667 s (1/60)
=> 每帧转角预算 = 3.00 倍
```

日志里的铁证：`[player] msl MaxRotationAngle` 留空 ⇒ 默认 90 °/s，
`limit=4.50deg`（= 90 × 0.050），而正确值应是 **1.50deg**（= 90 / 60）。
也就是"填 90 实际得到 270"。

**修法**：直接用引擎自己的帧率常量 `*(u32*)(ctrl+0x6EC)`（本版本恒为 60，已由
`exe+0x13EFA7` 的 `mov eax,[rdi+0x6ec]` + `vdivss` 证实它就是原版
`lead·π/180 / fps` 的那个 fps），`dtGame = 1/fps`，**彻底不再做单位换算**。
`P+0x44`（speed m/s）因此也不再需要读。回归断言 `pnsim` (k11a1)/(k11a2)。

**② `detail log full` 无限刷屏（占日志 83%）**

`RecordDetail` 每次溢出都置 `detailOverflow=1`，`DrainDetails` 每 2 秒清一次，
于是同一条一次性提示在 4 分钟里打了 **3090 次**（370 KB 日志里 83% 是它）。
改为**粘性一次性**：只有第一次溢出记录，`detailOverflowSeen` 保证只打印一条。

**③ 每次跑脚本都会多加一个 UTF-8 BOM（`INI: line 1: text outside of any section`）**
*（历史条目：涉及的脚本已删除，详见下面的注。）*

5 个脚本（`enable_pn.ps1` 与 4 个 `probe_*.ps1`）的 `Save-IniText` 无条件在文件头
写 `EF BB BF`，而 `Get-IniText` 是按 Latin-1 1:1 读的，已存在的 BOM 会作为三个字符
留在 `$text` 里。跑 N 次就有 N 个 BOM；C++ 解析器只剥一个，剩下的字节出现在第一个
注释之前 → 那条 warning。这是纯功能无害但会污染 INI、且让日志难读的问题。
修法：写回前先剥掉已有 BOM（`while ($text.StartsWith($bom)) { $text = $text.Substring(3) }`），
已用 5 次连续写入验证字节稳定（13533 B 不变）。

> **历史记录**：这批脚本已删除，所以这条 BOM 叠加 bug 的来源也一起消失了。
> 但同一个坑换个入口还在：**手工编辑 INI 时如果用不带 BOM 的编辑器另存、或让它整体改写
> 行尾，也会毁掉这份文件**。给用户/维护者的规矩是保存成 UTF-8 **保留 BOM**、别换行尾。
> C++ 解析器本身只剥一个 BOM、且 CRLF/LF 都吃，所以多一个 BOM 只是产生
> `INI: line 1: text outside of any section` 警告，不会让模组失效。

### 13.3 朝向标定 `ok=0`：原因已定量，但**不影响弹道**

```text
MSLPN: STEP1B orientation calibration: axis=4 variant=3 ok=0 attempts=60 tooSmall=52
  base=0.995034575  scores=[0.926942527 0.949213326 0.921119094 0.951754987]
```

换算成角度：**要求转 5.71°**，但四个变体分别产生了 **22.0 / 18.3 / 22.9 / 17.9°**，
而"什么都不做"的基线是 0.995（≈5.7°）。也就是说每个变体都**转过头 3–4 倍**，
自然全部劣于基线，`scores[best] > base + 1e-4` 永远不可能成立。

⇒ 这不是"样本太小"的问题，而是 **`FUN_1402036D0` 的旋转矢量幅值约定不是角度**
（可能是 delta-euler，或需要不同的缩放）。反汇编确认它确实读 `r8[0..2]` 并据此
构造矩阵（`0x1402036E6`–`0x1402036FB` 对三个分量做零判断，然后走
`0x1402036D0` 主体），但幅值语义仍未定。

**为什么可以先不管**：`orient=0` 意味着朝向写入全程关闭，`applied=3142` 的位置引导
完全正常 —— 这正是用户判定"生效、没问题"的配置。差别只在**弹体视觉朝向**可能比
真实弹道略微滞后。

**下一步（一次运行即可定论）**：日志现在新增一行

```text
MSLPN:   calibration turn angles: commanded=X deg, variants produced [a b c d] deg
        (ratio vs commanded: [.. .. .. ..])
```

比值若稳定在某个常数（如 3.14、约 1.0、或 2.0），就能直接反推幅值约定，
不需要再做实验。

### 13.4 未受影响的东西

* 特殊武器路径（`0x90510` 积分器）钩子安装正常，但本次 `[enemy]` 为空、
  玩家未使用特武，`integratorCalls` 无样本 —— **未验证，留待下次**。
* `P+0x1F8 & 8` 的解释（`docs/MSL_ADVANCE.md` §12.2 说必需，代码里说不必）
  本次日志显示 `badTarget=0`、`applied=3142`，说明代码的判断是可行的，
  但两种说法仍未在指令层面对齐。

---

## 14. 2026-11-22 第二轮实机测试：**三条修复全部验证生效，特武路径首次验证**

环境同上，跑约 4.5 分钟（12:18:55–12:23:34）。用户反馈：**库存打空、6 发 QAAM 全命中**。

### 14.1 上轮三个 bug 的实机验证（全部通过）

| # | 修复 | 上轮日志 | 本轮日志 | |
|---|---|---|---|---|
| ① | 每帧转角预算（`dtGame` 单位） | `limit=4.50deg` | **`limit=1.50deg`** | ✅ |
| ② | `detail log full` 刷屏 | **3090 条**（占日志 83%） | **1 条** | ✅ |
| ③ | 每次叠加一个 BOM | 首行 `text outside of any section` | **0 次** | ✅ |

`limit=1.50deg` = `90 °/s / 60 fps`，正是 `MaxRotationAngle` 留空时的默认值 —— 
说明"填 90 就是 90"现在成立了（上轮填 90 实际是 270）。
用户主观反馈：「`MaxRotationAngle` 没啥感觉，因为本来就比原本的性能强很多」——
合理：位置引导的收益（命中率）远大于转角上限的差异，且本模组只写了位置没写朝向。

### 14.2 特殊武器（QAAM）路径：**首次真机验证通过**

上轮 `[enemy]` 为空、未打特武，`integratorCalls=0`，该路径**从未被验证**。本轮：

```text
PN 积分器钩子已安装 / PN integrator hooked at exe+0x90510 (stole 17 bytes)
PN: 识别到导弹槽 missile slot 0x... class=.?AVWeaponExQaamCtrl@@ caller=exe+0x9F4E6
PN 心跳: integratorCalls=1074 frames=1022 launches=6 callers=1 nonMissile=0 badGeometry=51 applied=1022
```

* **6 次发射全部被捕获**（`launches=6` 与用户"打了 6 发"完全一致），
  `applied=1022` 帧真正写了速度，`nonMissile=0`（没有误写非导弹对象）。
* 调用点 `exe+0x9F4E6` 落在文档预测的 7 个 WeaponEx 调用者区间内。
* **6 发全命中** —— 用户直接确认了效果。
* `badGeometry=51`（约占 5%）是发射瞬间 tGo > 8 s 的几何不可靠帧，按设计跳过，
  属正常；`nonMissile=0` 说明导弹槽识别（`IsMissileSlot`）准确。

⇒ **两条制导路径（普通 MSL 后置钩子 + 特武 `0x90510` 积分器）现在都经过真机验证。**

### 14.3 本轮新发现并修复：`badTarget` 把良性状态当错误

本轮汇总出现 `badTarget=283 (tobj=283 ...)`，看起来像异常（上轮是 0）。
查代码：`tObj == 0xFF`（**无目标**，导弹的目标已死或从未分配）与
`tObj >= 64`（**索引越界**，真异常）被合并计数。0xFF 是完全正常的状态，
不该出现在 "bad" 里 —— 这会让一次干净的运行看起来有问题。

已拆分：`noTarget=`（0xFF，正常）与 `badTarget=`（越界，真异常）分开统计。

### 14.4 朝向标定：比值已测出，约定基本锁定

```text
MSLPN:   calibration turn angles: commanded=0.575 deg,
         variants produced [9.916 11.020 9.914 11.022] deg
         (ratio vs commanded: [17.23 19.15 17.23 19.15])
```

命令转 0.575°，四个变体却转了 **9.9 / 11.0 / 9.9 / 11.0°**。关键观察：

* 旋转矢量幅值 = 0.575° ≈ 0.01 rad。若 `FUN_1402036D0` 把它当 **弧度**解释
  （0.01 rad = 0.575°），产出应当也是 0.575°，而不是 9.9°。
* 9.9° ≈ 0.173 rad。`atan(0.01) ≈ 0.01`，`tan(0.173) ≈ 0.175` ——
  **比值 17–19 接近 `1/0.0573`，也就是"把度当弧度"的 57.3 分之一量级**。
* 更可能：`FUN_1402036D0` 期望的是**欧拉角增量（度）**而不是旋转矢量，
  而我们把弧度制的轴角塞了进去。

无论具体约定，结论一致：**朝向接管保持关闭是正确的**（`orient=0`），
位置引导不受影响 —— 这正是已验证可用的配置。要真正接管朝向需要重新定标
（建议下次用一个只发射、不做机动的场景，取单帧 `commanded` 与四个 `produced`
的比值；若恒为 17.2/19.2 两个值，说明只差一个常数缩放）。

---

## 15. 2026-11-22 外部代码评审的核实与修复

收到一份外部审核报告（12 项）。逐条核实后：**6 项确认为真并已修，
3 项在提交前已被更早的修复覆盖或本就安全，3 项为文档/工程建议（已采纳部分）**。
核实过程与结论记在这里，避免下一个人重做。

### 15.1 确认并修复的真 bug

**① `src/core/mod.cpp` — 线程句柄二次关闭 + 卸载期死锁**

`StartOnce()` 在 `CreateThread` 后立刻 `CloseHandle(g_thread)` 但没置空，而
`DllMain` 的 `DLL_PROCESS_DETACH` 分支又对同一句柄 `WaitForSingleObject` + `CloseHandle`。
Windows 可能已复用该句柄值 → 二次关闭会误伤无关对象；且进程终止时（`lpvReserved != nullptr`）
在持有 loader lock 的情况下等待工作线程是经典死锁。
修：`StartOnce` 不再关闭句柄；DETACH 分支只在 `lpvReserved == nullptr`（真正的
`FreeLibrary` 动态卸载）时才等待+关闭。**这是本次评审最有价值的一条。**

**② `src/pn/msl_steer.h::MatrixAxis` — 行候选步长写成 3（应为 4）**

列候选正确用步长 4（`m[c], m[4+c], m[8+c]`），行候选却写 `m[3*r]`。于是
`r=1` 读到 `(m[3],m[4],m[5])`、`r=2` 读到 `(m[6],m[7],m[8])` —— 混进第 4 列（通常是
`0`/`1`）并且**完全漏掉真正的第 2 行**。因为朝向标定通常选中列候选，实际从未触发，
但它是个确定无疑的越界语义错误。
修：改为 `m[4*r]`。新增回归断言 `pnsim` (j7b)/(j7c) 用"每个元素值不同"的矩阵
钉死步长——旧测试只覆盖列候选，所以一直没抓到。

**③ `src/pn/msl_pn.cpp::CacheFind` — 目标缓存满后无淘汰**

64 槽的 `TargetCacheEntry` 全满时直接返回 `nullptr`。实测日志里 `tObj` 已达 17–64 区间，
大型关卡（敌机 + 地面目标 + 分部位目标）很容易超过 64 → 之后新锁定的目标全部拿不到
速度估计，退化成无提前量拦截。
修：记录 `lastFrame`，满时按 **LRU 淘汰**最久未用的条目。同时补上评审提到的
"位置瞬移"处理：单帧步长超限时不仅钳位，还**丢弃 EMA 平滑历史**，避免一次瞬移
连续污染好几帧的提前量。

**④ `src/common/acz_common.h::RotateTowards` 与 `ComputePn` 纯追踪项 — 反向共线零转向**

两向量恰好反向（导弹正背对目标）时 `Cross` 为零向量、`Normalize` 失败，旧代码
直接返回原方向 → **导弹永远直飞不转弯**。修：此时任取一个与 `f` 垂直的轴按
限速转一步（该平面上任何方向都等价）。新增回归断言 `pnsim` (k14a–c)：
`amount=0` 且目标正后方时，转角必须等于满额预算且非零。

### 15.2 评审提出、但提交前已修复 / 本就安全

**⑤ `WaitTramp()` 自旋竞态与 trampoline 发布顺序 — 已在此前修复**
评审建议"在写入口跳转**之前**先发布 `*trampoline`"。查 `src/hook/hook.cpp`：
`*trampoline = trampMem;` 已在 `WriteJumpLocked` 之前（本轮新增的 A2 修复就是它），
失败路径逐个回滚置空。评审看到的是旧版本。

**⑥ `AczAddonInit` 硬编码 ABI 越界风险 — 本就安全**
评审担心直接按 `+0x08/+0x10/+0x28` 解引用宿主结构。实际代码**全部**走
`mem::IsReadable` + `mem::Read`（SEH 保护）+ `wchar_t buf[512]` 有界拷贝，
不是裸解引用。这条是基于未细读的推断，无需改动（但"加版本号"的长期建议合理，
已记在 §15.4）。

**⑦ `FlushInstructionCache` 缺失 — 本就调用**
`src/hook/hook.cpp` 有 6 处 `FlushInstructionCache`（relay 写入后、跳板写入后、
目标 prologue 打补丁后、卸载还原后）。已满足。

### 15.3 部分采纳

**⑧ `ComputePn` 的 `gameFps` 在非 60 FPS 下漂移**
评审假定调用处传固定 `60.f`。实际 `msl_pn.cpp` **每帧从 `*(u32*)(ctrl+0x6EC)` 读取
引擎自己的帧率常量**（`g.gameFps`，本版本为 60），不是硬编码——这正是上一轮修
3 倍转角偏差时做的。若某版本改了该常量，我们会自动跟随。**无需改动。**

**⑨ `msl_steer.h::Compute` 是旧模型、与 `ComputePn` 冗余**
确认：`Compute`（Step-1 前置点公式）**只被 `pnsim` (i1)–(i9) 当作参考模型使用**，
生产路径完全走 `ComputePn`（真 λ̇ 律）。两条路径模型不同是有意的
（`pn.cpp` 供特武/文档、`msl_steer.h` 供两条实际钩子），但确实容易混淆。
已在 `Compute` 上方加显式 **DEPRECATED** 注释，说明它是测试参考、不要改它。

**⑩ 没有调试符号可供分析崩溃**
采纳：`./build.sh debug` 现在产出 **未 strip**（`-O1 -g`，10 MB，含 19 个 debug 段）
到 `build/debug/`，发行版仍保持 strip（679 KB）。分析玩家崩溃转储时用它对照。

### 15.4 暂不采纳（留作 backlog）

* **给 addon ABI 加版本号/大小校验**：合理，但宿主 ABI 是我们逆向出来的既定事实，
  加字段需要社区 loader 配合，目前收益低于风险。保持现状 + 已有 SEH 保护。
* **README 标注已验证的 `acz.exe` SHA-256**：有价值，但每次 Steam 更新都会变，
  而我们的入口字节校验本就会在版本不匹配时**安全放弃**（不崩）。可后续加。
* **`patcher.cpp` 是否过滤无导引武器**：评审未完整读完该文件。特武路径有
  `IsMissileSlot()`（要求对象确属 `Weapon*Ctrl` 的导弹槽）+ `nonMissile` 计数，
  实测日志 `nonMissile=0`。已覆盖。

### 15.5 评审未读、但经核实已正确的两处

**⑪ `loader/proxy_common.h` 的 DLL 搜索劫持与 loader-lock 风险 — 本就正确**

* 真实系统 DLL 用 `GetSystemDirectoryW` 拼出 `%SystemRoot%\System32\<name>.dll`
  的**绝对路径**再 `LoadLibraryW`（`proxy_common.h:100/117`），不依赖搜索顺序，
  因此不存在"同名 DLL 劫持"。
* `DllMain` 内**不调用 `LoadLibrary`**：ATTACH 只做
  `GET_MODULE_HANDLE_EX_FLAG_PIN`（把自己钉在进程里，避免被卸载时别的线程还在
  转发函数里）+ 建 event + `CreateThread`；真正的加载在工作线程完成。
  DETACH 分支也正确地只在 `reserved == nullptr` 时等待 worker。
  这正是评审担心的两个问题，代码里都已经处理了。

**⑫ `hook.cpp` 中段解码器（REX/VEX/EVEX、RIP 相对判定）— 仍需一次完整审计**

这是唯一一条评审说"没读完、建议自查"且我们确实没有系统覆盖的部分。
`hooktest` 覆盖了安装/卸载/拒绝不安全序言与两种代理转发，但**没有**对全部
x86-64 前缀组合做穷举。不过：解码器采取的是**保守策略**——任何它不认识的编码
都会让 `Decode` 返回 0，`DecodePrologue` 随之失败、`Install` 拒绝安装并记录原因。
也就是说"漏判"的后果是**放弃 Hook**（导弹退回原版制导），而不是打错补丁。
风险可控，但值得后续补一组 fuzz/穷举测试。
