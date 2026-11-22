# RUN_B_RESULTS — hardware read-watch run (2026-10-26 21:56–21:58) and the answer

Run: one Free Mission sortie, 27 plain MSLs fired by the user (as many as the game allowed;
not the exact script). Probe: `HwWatch = 1`, 60 s window, four one-shot debug-register
watchpoints on the player's weapon-parameter record `P`. Log: copied to
`work/probe_b_run1.key.log` (141 KB). Game base `0x7FF650610000`.

## 1. The answer: `lead` is read **per frame** by the plain-MSL guidance function

`P` was resolved and validated exactly as in Run A:

```text
CHAIN MC=0x2516A43F610  objArray=0x2516B2A4D80  playerIdx=0
      P=0x2516AD85B20  data_off=0x10 scope=1600 range=3000 timeStep=4 lead=40
      speed=1000 type=2 attack=40 startAng=90   [mis]
```

Watchpoint hits (all on the game's main thread, tid 49104):

| slot | watched | hits | reader |
|---|---|---|---|
| 0 | `P+0x2C` **lead** | **88** | `rip` in **`FUN_14013ECA0`** — the read instruction is `exe+0x13EFA2: vmovss xmm0, [rax+0x2c]` with `rax = P` |
| 3 | `P+0x44` speed | 6 | `rip` in `FUN_1401031F0` (`0x1039F5`) — the fire-control spawn path (launch) |
| 1 | `P+0x48` type/attack/startAng | 0 | — (startAng is read inside 0x13ECA0 too, but the slot-1 watch was mainly hit-free: see note) |
| 2 | `P+0x14` range | 0 | — |

Note: the watch is one-shot + 200 ms re-arm, so 88 hits over ~30 s means "sampled every
~0.3 s", not "88 accesses". The reader is the same single instruction each time.

`DISPATCHER COUNTER FROZEN` (exe+0x33ED94 stayed 0 in every MOV snapshot during flight):
**the plain MSL bypasses the `0x1B9A80` dispatcher entirely** (matches the re-msl branch:
the spawn is driven by the direct callers of `0x13D180`).

## 2. What `FUN_14013ECA0` is (decompiled with Ghidra)

Signature `void FUN_14013ECA0(longlong *ctrl)` — the **per-frame plain-MSL guidance +
integration** for one aircraft's weapon manager. It loops the 16 hardpoint slots:

```c
mask = *(u32*)(ctrl + 0x6E8);
for (i = 0; i < 16; ++i) if (mask & (1u << i)) {
    slot = ctrl + 0x158 + i * 0x58;         // 16-slot array
    FUN_14013CED0(ctrl, slot, i);           // per-slot pre-update (target validate/lock)
    // target: tObj = byte[slot+0x42], tPart = byte[slot+0x43]  (0xFF = none)
    // targetPos = objArray[tObj]->parts[+0x110] + 0x520*tPart, vec4 at part+0x258
    //   (optionally an "aim point" child at *(part+0x360)+0x5E0)
    // seeker cone: FUN_140274530(byte[P+0x4A] * deg2rad)   <- startAng
    // max per-frame turn:  lead_rad = float[P+0x2C] * deg2rad
    //                      maxTurn  = lead_rad / (float)*(u32*)(ctrl + 0x6EC)
    //   -> rotate the slot's quaternion toward the target, clamped to maxTurn
    // integrate: speed = 7.1111/byte(ctrl+5) + slot+0x3C (capped 4266.67/byte)
    //            slot+0x00.. += speed * forward(quat);  slot+0x20.. = speed * forward
    // range kill: if (float[P+0x14] * 5.12 < slot+0x38 traveled) -> deactivate slot
    // trail: FUN_140035B50(..., 0x30, ...) with slot+0x40 handle
}
```

Slot layout (from the decompiler + the actual instructions):

| slot off | type | meaning |
|---|---|---|
| `+0x00` | f32[4] | **position** vec4 (w unused by the integrator) |
| `+0x10` | f32[4] | orientation **quaternion** (fed to `FUN_140202FA0` = quat→matrix) |
| `+0x20` | f32[4] | **per-frame displacement** (speed × forward); added to position each frame |
| `+0x30` | u64 | **P** (weapon record pointer) |
| `+0x38` | f32 | accumulated traveled distance (range kill uses `P+0x14 * 5.12`) |
| `+0x3C` | f32 | turn/speed accumulator (see speed formula) |
| `+0x40` | u16 | trail-effect handle |
| `+0x42` | u8 | target object index (0xFF = none) |
| `+0x43` | u8 | target part index |
| `+0x44` | u8 | **owner** object index |
| `+0x45` | u8 | owner part index |
| `+0x46/0x47` | u8 | flags |

So the plain MSL is a **slot-based, quaternion-integrated projectile**, not a pool node and
not a world object: pool nodes are only the spawn/trail anchors (Run B's POOL snapshots show
them frozen at the launch position), and the MOV snapshots show the world object array is not
where missiles live (the world count byte was 1 and only the player object was visible).

## 3. Why this matters for the mod

1. **PN is possible exactly where the game does homing**: after `FUN_14013ECA0` returns, the
   slot is the complete missile state (pos, forward/quat, per-frame displacement, target,
   owner, P). A post-update hook can replace the displacement direction with a PN intercept
   direction and shift the position by the difference.
2. Per-frame units are known: `slot+0x20` **is** the position delta of one frame, so no frame
   time is needed for the steering itself; target velocity can be finite-differenced per
   frame using a frame counter that groups the per-aircraft calls of one game frame.
3. `P+0x2C` (lead) is a *turn-rate clamp*, not a target lead: the game aims straight at the
   target position and only uses `lead` to limit how fast it may rotate. Increasing `lead`
   alone would make the missile turn harder but would **still not lead a crossing target** —
   i.e. the AC8-style "just raise the params" route is not a true PN here. The data route
   (`P` writes) remains available for `scope/range/startAng/attack` but not for PN.
4. `startAng` (`P+0x4A`) is the seeker cone used per frame; `range` (`P+0x14`) is the max
   traveled ≈ `range*5.12` in the integrator's units; `speed` (`P+0x44`) is used at launch.

## 4. Implementation started

**Hook `exe+0x13ECA0`** (15-byte relocatable prologue: `mov rax,rsp` / `mov [rax+8],rbx` /
`mov [rax+0x18],rsi` / `mov [rax+0x20],rdi`), detour = call original then
`msl_pn::Apply(ctrl)`:

```text
for each live slot:
    ownerObj = slot+0x44, target (tObj,tPart) = slot+0x42/+0x43
    amount = [player]/[enemy] msl HomingForesightAmount   (owner==playerIdx ? player : enemy)
    P = *(u64*)(slot+0x30), validate data_off/type==2; speed = u16(P+0x44)
    p = slot+0x00, v = slot+0x20  (per-frame displacement, m/frame)
    Vt = target-position finite difference per frame (cache 64 entries, frame id by QPC grouping)
    R = targetPos - p;  vc = |v| - dot(Rhat,Vt) (clamped);  tGo = |R|/vc (clamped)
    aim = targetPos + Vt*tGo*amount;  d = normalize(aim - p)
    rotate v toward d by min(angle, MaxRotationAngle*dt_game), dt_game = |v|/speed
    write slot+0x20 = v_new ; slot+0x00 += (v_new - v_old)
```

Design limits for v1 (documented in `docs/MSL_PN.md` by the implementer): quaternion is not
written (so the visual model may lag the trajectory slightly, but collision/kinematics follow
the position), `MaxHomingAngle` / `LifeTime` / `LockonRange` are not wired yet, and the enemy
side only uses `[enemy]` `HomingForesightAmount` (default 0 = untouched).

## 5. State / next

* `docs/HANDOFF.md` §11.7–§11.9 record the mechanism, the Run B result and the implementation.
* The in-game verification of the PN build is the next run: fire plain MSLs at a crossing
  target and compare with vanilla (`HomingForesightAmount = 0`), then tune.
