# WEAPON_DISPATCH — Ghidra findings on the per-frame weapon/projectile path

Status: **first Ghidra pass, 2026-10-26 evening.** The previous session had no working
Ghidra; this one does (REA MCP, Ghidra 12.1.4, image
`work/acz_copy.exe.unpacked.exe`, base `0x140000000`, 5034 procedures). Everything below
is decompiler evidence, tags **[V]** = read directly out of Ghidra decompilation,
**[I]** = interpretation.

> Addresses are **RVA** here (`exe+0x…`), i.e. VA − 0x140000000.

## 0. The per-frame dispatcher `FUN_1401B9A80` (exe+0x1B9A80) [V]

Called once per frame. `DAT_14033ed94` is the **frame counter** (incremented at the end).
It walks a global time-ordered doubly-linked list headed by `DAT_14033edb0`
(RVA `0x33EDB0`) and dispatches on the node's state byte `+0x6B`:

| node byte/field | meaning [I] |
| --- | --- |
| `+0x00` qword | list next (0 terminates) — list is walked via `[node]` |
| `+0x20` u16 | weapon category: **0, 1, 2** |
| `+0x22`, `+0x23` | shooter object index / part index |
| `+0x28` qword | **pointer to the weapon-parameter object** (`param_9` below) |
| `+0x2c`, `+0x30`, `+0x34` | 3 floats (spawn position from the caller) |
| `+0x40`, `+0x44`, `+0x48` | 3 floats (aim vector / direction) |
| `+0x4a` u16, `+0x4d` byte | effect/type selectors (state 7) |
| `+0x50` byte | passed through to the spawn call |
| `+0x60` int | timestamp (frame/time) — list is sorted on this |
| `+0x6b` byte | **state** |

State dispatch [V]:

```text
state 2  -> spawn the projectile of category (u16)+0x20:
            cat 0 -> FUN_14013D180(*(aircraftWeaponObj+0x178), pos12(node+0x34), vec(node+0x40),
                                   float(node+0x4c), shooterObj, shooterPart,
                                   node+0x22, node+0x23, *(node+0x28), node+0x50)
            cat 1 -> FUN_140141240(aircraftWeaponObj+0x1B10, …)
            cat 2 -> FUN_14013AD20(aircraftWeaponObj+0xCA0, …)
            (these three contain the six known pool-allocation call sites
             0x13D665 / 0x1412B4 / 0x13B09B — so the pool spawn path is the category
             spawn path, and every spawned pool node gets the same param pointer at +0x28)
state 3  -> if (short)node+0x38 == 6: FUN_140031FC0(world+0x66EE0, 0x2a, …)
state 7  -> FUN_1401C63A0(…) or FUN_1401C6490(…)
state 8  -> FUN_1401C67D0(…, node+0x20, …)
```

The three `FUN_14014…`/`FUN_14013…` spawn functions are the *only* writers of the pool
node block, which matches `docs/MSL_TARGET.md` §5.1.

## 1. The category-0 spawn `FUN_14013D180` (exe+0x13D180) [V]

Contains the known MSL spawn site `0x13D665`. It:

1. loops the 16 fire slots of the aircraft weapon object (`stride 0x58`, state at
   `param_1[+0xDD]` as a 16-bit live mask);
2. pops a pool node via `FUN_140019F30()` (pool getter) + `FUN_14001B2C0(pool)`;
3. fills the node (matches `docs/MSL_GUIDANCE.md` §3.3):
   `+0x34/+0x38/+0x3c` = position vec3, `+0x40/+0x44/+0x48` = direction vec3,
   `+0x4c` = **speed scalar** (see formula), `+0x28` = **param pointer**,
   `+0x64` = weapon slot, `+0x66/+0x67` = shooter obj/part, `+0x68` = team,
   `+0x6b` = **2**, then inserts it into the pool list with head sentinel at `S+0x98`
   (tail pointer `S+0xA0`);
4. computes the speed scalar:
   ```c
   param_4 = ((float)*(int *)(param_9 + 0x44) * 0.27777782f / (float)*(byte *)(param_1 + 5))
             * 5.12f + param_4;
   ```
   **[V]** — `param_9` is `*(node+0x28)`, i.e. the weapon-parameter object; `+0x44` is an
   **int field of that object** consumed to build the missile's motion scalar.
   This is the first proven consumer of a weapon-parameter field at launch. `0x27777782`
   is `1/3.6` (km/h→m/s), so `+0x44` is very likely a converted **speed** value; `param_4`
   is the child missile's inherited scalar.

## 2. Frame end / mission (re)load `FUN_1401B87B0` (exe+0x1B87B0) [V]

Called from the dispatcher's tail with `(missionWorld, 1)`. With `param_2 == 0` it fully
resets the mission world (all actor arrays, weapon managers, effect managers) — i.e. this
is the **level-load / mission-restart entry**. Hooking it and testing `param_2` gives a
cheap, reliable "mission data (re)loaded" event, which is where a runtime data patch has
to be re-applied.

It also calls `FUN_1401B6B00(&DAT_14033ED80, param_2)` every frame.

## 3. `FUN_1401B6B00` (exe+0x1B6B00) — effect/trail builder [V]

Per-frame effect pass: walks the pool singleton lists (`S+0x20`, `S+0x98`), collects live
state-6 nodes into a 0x100-entry table of `{pos, dir, type bytes}`, and feeds the smoke/
trail ring. It reads node `+0x34/+0x38/+0x3c` (pos), `+0x20`, `+0x60`, `+0x6b`, `+0x58`.
**Not** the guidance integrator (this is the layer `docs/MSL_TARGET.md` §10 already
identified as the effect side).

## 4. `FUN_1401627C0` (exe+0x1627C0) — descriptor attach [V]

Sets `obj+0x08` = descriptor pointer and `obj+0x10/+0x18/+0x20/+0x28` = four rel32-decoded
sub-descriptor pointers. So a `WeaponModel` fixed header is:

```text
+0x00  vftable (exe+0x2AE790)
+0x08  descriptor (int*): {i32 count; i32 rel32[count]}
+0x10  sub-descriptor[0] pointer  (or 0)
+0x18  sub-descriptor[1] pointer
+0x20  sub-descriptor[2] pointer
+0x28  sub-descriptor[3] pointer
+0x2c..+0x47  ???  (0x44 is read as an int by the launch formula above)
+0x48         variable payload (built by FUN_140162AC0)
```

This partially fills the "unknown header" gap in `docs/RUNTIME_PARAMS.md` §1.2.

## 5. Consequences for the MSL PN work

1. **A live projectile node carries a pointer to its weapon-parameter object at `+0x28`.**
   That object is the route to `lead / scope / range / startAng / speed` *for that
   missile*, with no global parameter scan needed. The Run-A probe must dump it.
2. The pool nodes are advanced/consumed by the effect layer (`0x1B6B00`) and by
   `FUN_1401C63A0 / 0x1C6490 / 0x1C67D0` for states 7/8 — these are the next
   functions to decompile for the flight model.
3. The state-2 spawn formula proves **`paramObj+0x44`** is read at launch. A hardware
   read watchpoint on that field (or on `lead`, once the layout is known) is the direct
   way to enumerate every reader of the weapon parameters.
4. `FUN_1401B87B0(world, 0)` is the mission-load anchor; `FUN_1401B9A80` is the per-frame
   frame-start anchor. Both are useful hook points independent of the missile question.

## 6. How to query Ghidra from this session

The REA MCP server has the image open (no need to reopen). Useful calls:

```text
mcp__rea__procedure_pseudo_code   procedure=0x1401B9A80
mcp__rea__analyze_function        procedure=0x14013D180
mcp__rea__procedure_callers       procedure=0x14013D180
mcp__rea__procedure_references    procedure=<addr> direction=incoming
mcp__rea__inspect_native_instruction address=0x1403219A0
```

Do not call `open_binary` with another target while the team is using this one, and do not
`close_binary` until the session is over.
