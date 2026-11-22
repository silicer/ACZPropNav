# ACZPropNav — the shooter's fire object, the locked-target storage, and the plain-MSL launch/aim RVAs

Static RE only (no game launch, no attach). Reference image:
`ACZPropNav/work/acz_copy.exe.unpacked.exe`, image base `0x140000000` (VA = `0x140000000` + RVA).
Tags: **[V]** verified at instruction level, **[I]** inferred from verified code, **[U]** unknown / needs a probe.

This document supersedes the relevant parts of `docs/MSL_GUIDANCE.md` §3.3 and `docs/MSL_TARGET.md`
§3–§4. The two corrections that matter most are flagged in §8.

---

## 1. Object chain (all from the same module, verified end of §4)

```
pool singleton S            = 0x1403219A0        (static; getter 0x19F30; also runtime global 0x1403409F8 → MC)
MC  (mission control)       = *(void**)(S + 0x428)                 ; global 0x1403409F8 holds it too
objArray                    = *(void**)(MC + 0x697B8)
WC table                    = *(void**)(MC + 0xA8248),  stride 0x27A0, count *(u8*)(MC + 0xA87E4)

obj    = objArray[objIdx]                    ; objIdx = part+0x234 (byte)
part   = *(void**)(obj + 0x110) + 0x520*partIdx ; partIdx = part+0x235 (byte)

per aircraft part:
  part+0x1B8 -> WC   "fire object", 0x27A0 B      ← the subject of this doc (task: fire = *(part+0x1B8))
  part+0x178 -> FO   second, huge fire/launch object (>= 0x1FDA0 B) — see §5
  part+0x360 -> FC   "fire control" object (~0x620 B) — owns the lock / launch decision
  part+0x388 -> Obj**  (owner object; *(*(part+0x388))->+0x15D = team)
  part+0x258     position vec4 (x,y,z,w=1)
```

`WC` is per **object**, not per part: it is the `0x27A0`-stride entry in `MC+0xA8248`, indexed by the
object index (`FUN_1401B9A80` → `imul rsi, rdi, 0x27a0; add rsi, [rbp+0xa8248]` at `0x0CC384/0x0CC38B`),
and every part of that object reaches it through `part+0x1B8` **[I for "same instance", V for layout]**:
the offsets the MSL node initialiser reads through `part+0x1B8` (`+0x1630/+0x1640/+0x1650/+0x1664/
+0x1670/+0x1674/+0xD48/+0xC3C/+0xC3D`) are exactly the offsets the fire-control tick writes into the
`MC+0xA8248` entry (`0x1BA6E8..0x1BA763`, `0x1BA7xx`).

`FC = *(part+0x360)` (not `*(part+0x1B8)`!) is the object with the per-aircraft *lock decision* and the
launch calls; `FUN_140119A70(part)` is literally
`FC = *(part+0x360); copy(part+0x258..0x274 -> FC+0x3D8..0x3F4); FUN_1401218C0(FC); copy back`
(prologue `0x140119A70`; call site of the pair at `0x140115Bxx`/`0x140116181`).

---

## 2. The fire object `WC = *(void**)(part + 0x1B8)` — field map

Size **0x27A0** [V stride], per object, initialised at mission load by `FUN_1400CBD00`
(entry `0x0CBD00`, body to `0x0CD823`; the per-object loop is `0x0CC363..0x0CC9xx`, e.g.
`imul rsi, rdi, 0x27a0; add rsi, [rbp+0xa8248]` at `0x0CC384`, and the back-pointers
`[WC+0x2040] = &MC[0x697B8]` at `0x0CC41A..0x0CC459`). No vtable is used for it — it is a plain
data block.

| off | type | meaning | evidence (RVA) | conf |
| --- | --- | --- | --- | --- |
| `+0x070` | f32[4] | owning unit **world position** (x,y,z,w) — copied to `part+0x258..0x264` | read `0x115F6A..0x115F88` (`mov [rbx+0x258],eax` from `[rcx+0x70]`) | [V] |
| `+0x080` | f32[4] | owning unit **orientation** (quaternion) — copied to `part+0x268..0x274` | read `0x115F95..0x115FBF` | [V] |
| `+0x0C3C` | u8 | launch/burst mode A (used only if `+0xD48 & 0x20000`), else constant 2; becomes node `+0x56` | `0x1B23E..0x1B24A`, `0x1BA7xx` | [V] |
| `+0x0C3D` | u8 | launch/burst mode B (`+0xD48 & 0x40000`), else 2; becomes node `+0x57` | `0x1B24E..0x1B267` | [V] |
| `+0x0C44` | f32 | timer set when mode B becomes 0/2 (=120.0) | `0x1BA7xx` | [V write; role I] |
| `+0x0D48` | u32 | **fire-mode / weapon-state flags**: bit `0x20000` enables `+0xC3C`, bit `0x40000` enables `+0xC3D` (also tested all over the WeaponEx fire code) | `0x1B231`, `0x1B24E`, `0x1B9Dxx` | [V] |
| `+0x0FE0` | ptr | -> object whose `+0x0E` byte is read as a "can be locked/tracked" style byte for the target | `FUN_14011F1A0` field expr | [V read; role I] |
| `+0x1630` | ptr | **launcher-direction object `L`** (see §3); the aim vector lives there, not here | `0x1B0B1`, `0x1BA718..0x1BA763` | [V] |
| `+0x1640` | f32 | 4th component of the launch row → node `+0x2C`; also `WC+0x1848 = 42.187508 * this` | read `0x1B191`; write `0x1BA6xx` | [V read/write; meaning I] |
| `+0x1650` | f32 | muzzle/launcher world **position.x** → node `+0x30` | `0x1B19C`, writer `0x1BA6xx` | [V] |
| `+0x1654` | f32 | muzzle position.y → node `+0x34` | `0x1B1A7` | [V] |
| `+0x1658` | f32 | muzzle position.z → node `+0x38` | `0x1B1B2` | [V] |
| `+0x1664` | f32 | launcher **forward.x** → node `+0x3C` (also `+0x1850..`) | `0x1B1BD`, writer | [V] |
| `+0x1668` | f32 | launcher forward.y → node `+0x40` | `0x1B1C8` | [V] |
| `+0x166C` | f32 | launcher forward.z → node `+0x44` | `0x1B1D3` | [V] |
| `+0x1670` | f32 | scalar → node `+0x48` | `0x1B1DE`, writer `0x1BA6F8` | [V] |
| `+0x1674..+0x1677` | u8[4] | launch-time scalars/flags → node `+0x50..+0x53`. **Not** a target index pair (see §8). `+0x1674` is used numerically (divisor), `+0x1676/+0x1677` are bit flags | read `0x1B1EF..0x1B21B`; `MSL_TARGET.md` §3 | [V read; roles I] |
| `+0x1848` | f32 | `= 42.187508 * WC+0x1640` | write @ `0x1BA6Exx` | [V write; meaning U] |
| `+0x1850` | f32 | copy of `+0x1668` | write @ `0x1BA6F0` | [V write] |
| `+0x1854` | f32 | copy of `+0x166C` | write @ `0x1BA700` | [V write] |
| `+0x1EC8` | u8 | number of **lock/target candidates** in the table below | `0x10B1FE`, `0x10B271` | [V] |
| `+0x1E53 + i*0x20` | u8,u8,u8 | per-candidate i (< `+0x1EC8`): objIdx, partIdx, flags (bit1 = valid). Nearby same-stride data at `+0x1E48/+0x1E4C/+0x1E33..+0x1E35` (dword/byte fields; exact record base is one of these — the code reads them as parallel 0x20-stride arrays) | `0x10B21E..0x10B2BF` (`lea r10,[r8+0x1e53]`, `+0x20` step; `+0x1e55` flags) | [V offsets] |
| `+0x1EEC` / `+0x1EED` | u8,u8 | **current lock target = (objIdx, partIdx)**, `0xFF` = none. Source that is mirrored into `part+0x236/+0x237` | read `0x115F50..0x115F64`; cleared with `mov word [WC+0x1eec],0xffff` at `0x0F49F9`, `0x104B3B`, `0x104CC6` | [V read; writer U] |
| `+0x1FB8` | ptr | **weapon-parameter block**; its `+0x44` int is the "lead/speed" term of the launch speed formula | `FUN_1401031F0` | [V] |
| `+0x1FC8` | ptr | -> **FO** (the object at `part+0x178`, §5) | `FUN_1401031F0` (`FUN_14013D180(*(WC+0x1fc8),…)`) | [V] |
| `+0x1FD0` | ptr | lock/launch state object: `+0x2A6` state byte, `+0x2A7` weapon slot, `+0x2AA/+0x2AB` target pair copy, `+0x288` ptr, `+0x1C0..0x1CC` aim copy | `FUN_1401031F0` | [V read; layout I] |
| `+0x2000[i]`, `+0x2018[i]` | ptr | per-slot launcher sub-objects (`i = *(u8*)(WC+0x279A)`) | `0x0CC3EF..0x0CC40D` | [V] |
| `+0x2040` | ptr | `= &MC[0x697B8]` (address of the object-array pointer). So `**(WC+0x2040)` = `objArray` | written `0x0CC43D`; used `0x14010320A` | [V] |
| `+0x2058` | u32 | weapon/mode flags (bit 0x20 = busy) | `0x1031xx`, `0x1B7C16` | [V read] |
| `+0x2094` | u8 | burst/gun mode (0/1/2) | `FUN_1401031F0` | [V read] |
| `+0x2098` | u8 | "no spool" flag | same | [V read] |
| `+0x2099` | u8 | remaining burst rounds | same, `0x1B7E5x` | [V read/write] |
| `+0x209C` | u8 | alternating-barrel flag | same | [V read/write] |
| `+0x209D` | u8 | rotating-barrel index | `0x1B7Cxx` | [V read/write] |
| `+0x20A0` | u8 | number of barrels (divisor) | `FUN_1401031F0` | [V] |
| `+0x20A1` / `+0x20A2` | u8,u8 | **owner part's (objIdx, partIdx)** | `0x103214`, spawn args | [V] |
| `+0x20B4` | u8 | burst length | `0x1B7Cxx` | [V read] |

Writer/reader log for the `+0x16xx` block (`work/rp/fieldscan.py`, saved in `work/mf/fire_fieldscan.txt`):
only three functions touch a majority of those displacements:

| RVA | base reg | hits | what |
| --- | --- | --- | --- |
| `0x1B08E` (= `FUN_14001B050` tail) | `%r8` (WC) | 14/19 | MSL node initialiser — **reads** the whole launch block |
| `0x1B9B86` (= `FUN_1401B9A80`) | `%rbx` (WC) | 15/19 | fire-control tick — **writes** the launch block + aim |
| `0x105D3E` | `%r15` (WC) | 10/19 | weapon-animation state machine — secondary writer of `+0x1640/+0x1650/+0x1664/+0x1848..` |
| `0x1B23E` | `%r8` | 3/19 | tail of the node initialiser: `+0xD48`-gated `+0xC3C/+0xC3D` → node `+0x56/+0x57` |

---

## 3. The launcher object `L = *(void**)(WC + 0x1630)`

Used by the MSL launch copy: `movq 0x1630(%r8),%r9` at `0x1B0B1`.

| off | type | meaning | evidence | conf |
| --- | --- | --- | --- | --- |
| `+0x9D4` | u16 | direction-valid flag; bit0 set when the aim was updated this frame | `orw $0x1,0x9d4(%rax)` `0x1BA73D`; cleared `0x1BA757..0x1BA763` | [V] |
| `+0x9F0` | f32 | **aim vector x** → node `+0x20` | `0x1B170`; written `0x1BA708` | [V] |
| `+0x9F4` | f32 | aim vector y → node `+0x24` | `0x1B17B`; `0x1BA710` | [V] |
| `+0xA10` | f32 | aim vector z → node `+0x28` | `0x1B186`; `0x1BA74D` | [V] |
| `+0xA14` | f32 | 0..1 scalar, ×255 → node `+0x54` | `0x1B0B8..0x1B0D4`; written `0x1BA71F` | [V] |
| `+0xA20` | f32 | 0..1 scalar, ×255 → node `+0x55` | `0x1B0C1..0x1B0EC`; written `0x1BA72E` | [V] |

The aim vector is **not** computed from the target here: `FUN_1401B9A80` fills it from the aircraft's
weapon-animation keyframe records `*(FC+0x3E8)` / `*(FC+0x3F0)` (interpolated by their `+0x60`
timestamps) — i.e. it is the **hardpoint/muzzle pose** of the current animation frame. See §5/§6.

---

## 4. The locked target — verdict

### 4.1 Where it lives

The current lock is a **(objIdx, partIdx) byte pair, 0xFF = "no target"**, stored on the firing side:

| location | owner | role | evidence | conf |
| --- | --- | --- | --- | --- |
| **`WC+0x1EEC` / `WC+0x1EED`** | fire object (per object) | raw current lock, mirrored to the part each frame | read `0x115F50..0x115F64`, clear `0x0F49F9/0x104B3B/0x104CC6` | [V read] |
| **`part+0x236` / `part+0x237`** | aircraft part (part 0) | **the per-frame copy that the HUD lock box is drawn from** | mirror write `0x115F57/0x115F64`; HUD consumer `0x0B3275` (`movzx eax,[rcx+rbx+0x236]`, `[+0x237]`) → resolves target part and draws at `targetPart+0x258` (`0x0B3298..0x0B32DC`) | [V] |
| `FC+0x236` / `FC+0x237` | fire-control object `*(part+0x360)` | the pair the **launch decision** uses (passed as `param_7/param_8` to every spawn variant) | set by `FUN_1401269C0` (`0x1269C0`, called from `FUN_1401B9A80`); consumed in `FUN_1401218C0`/`FUN_14011F1A0`; cleared `mov word [FC+0x236],0xffff` at `0x118492` and `0x121959` | [V] |
| `FO slot+0x19A / +0x19B` | pending-shot record (16-slot table, §5) | target pair carried with a queued shot and consumed by the spawn/replay code | written by `FUN_14013D180` from `param_7/param_8` (`0x13D5xx`); cleared/used in `FUN_14013DB20` (`0x13DFF5`, `0x13E31F`) | [V] |
| `WC+0x1E53/+0x1E54` (candidate table) | fire object | candidate list entry, **not** the selected target | `0x10B21E..0x10B2A1` | [V] |

The selection/clear primitive is **`FUN_1401269C0`** (`0x1269C0`):

```asm
; set:   if (objIdx != 0xFF || partIdx != 0xFF) { FC->[0x236]=objIdx; FC->[0x237]=partIdx; ret; }
; clear: only when the current target is valid; then it clears the "locked by" flags on the old target
;        and unconditionally stores 0xFF/0xFF (0x126A36/0x126A3D)
```

### 4.2 How to resolve it to a position (per frame)

```c
// all constants are RVAs into acz_copy.exe.unpacked.exe (image base 0x140000000)
void*   S    = (void*)0x1403219A0;                 // pool singleton (static)
void*   MC   = *(void**)((char*)S + 0x428);        // == *(void**)0x1403409F8 (written at 0x0CD746)
void**  arr  = *(void***)((char*)MC + 0x697B8);    // Obj* array
unsigned pi  = *(unsigned char*)0x1403409F2;       // local player object index (see note)

void*  playerObj  = arr[pi];
void*  playerPart = *(void**)((char*)playerObj + 0x110);   // + 0x520*0 — aircraft = part 0

unsigned char tObj  = ((unsigned char*)playerPart)[0x236];
unsigned char tPart = ((unsigned char*)playerPart)[0x237];
if (tObj != 0xFF && tPart != 0xFF) {
    void* tgtObj  = arr[tObj];
    void* tgtPart = *(void**)((char*)tgtObj + 0x110) + 0x520u*(size_t)tPart;
    float* tgtPos = (float*)((char*)tgtPart + 0x258);   // vec4: x,y,z,w=1 (metres, world)
    // target velocity: NOT STORED ANYWHERE — finite-difference this vec4 frame to frame
}
```

Notes / caveats:
* `0x1403409F2` is used as the object-array index of the local player in `FUN_1401B9A80`'s prologue
  (`*param_1 + DAT_1403409f2*8`, `param_1` = `&MC[0x697B8]`, call site `0x1B7D42`) **[V read, I meaning]**.
  The sibling globals are `0x1403409F1` (u8, copied from `MC+0xA8825` at `0x0CD740` — the player
  object index) and `0x1403409F8` (qword = `MC`, written `0x0CD746`). If `0x1403409F2` turns out to be
  a player *slot* rather than the object index, use `0x1403409F1` instead — both are one-byte reads and
  the `arr[..]` step is identical.
* The old "player anchor" `exe+0x32B438` is **not** a part pointer: `FUN_1400CBD00` executes
  `lea r13,[rbp+0xa8328]` (`0x0CBEE0`, `rbp` = MC) and stores it (`0x0CBF2E`); it also stores the same
  pointer into every `WC+0x1B68` (`0x0CC49F`). So `*(void**)0x14032B438 == MC + 0xA8328`, a player
  state sub-block of mission control — useful for HUD-state, not for the lock pair.
* The HUD lock box path is an independent confirmation: it walks `objArray`, requires
  `obj->+0x15D == 0` (team 0) and `part[0]+0x1F0 & 2`, then reads `part[0]+0x236/+0x237` and draws at
  `targetPart+0x258` (`0x0B3253..0x0B32E1`).
* **The live missile does not carry the target.** The generic-MSL node initialiser
  `FUN_14001B050` (`0x1B050`, init block `0x1B08E..0x1B2BD`) writes the shooter pair
  `node+0x66/+0x67` from `part+0x234/+0x235` but writes **no** target field. Therefore the per-frame
  guidance has to resolve the target through the shooter — the only per-shooter target state is the
  pair above **[I, strong]**.
* Because the lock can be switched while a missile is in flight, prefer snapshotting
  `(objIdx,partIdx)` **at spawn** if the mod must model ACZ's "fire-and-forget at the fired-at target"
  semantics; live reading of `part+0x236/+0x237` is the cheapest per-frame option.
* Target **velocity is not stored** for any aircraft in this binary (same conclusion as
  `docs/GUIDANCE_ASM.md` §7 / `docs/VERIFICATION.md`): differentiate `targetPart+0x258` yourself.
  Mission frame counter: `DAT_14033ed94` (`0x14033ED94`, incremented per tick at the end of
  `FUN_1401B9A80`); node timestamps are in the same unit (1080-unit age window, `0x1A16A`).

---

## 5. Second fire object `FO = *(void**)(part + 0x178)` (needed to read the launch code)

Distinct from `WC`: `part+0x178 -> FO`, `WC+0x1FC8 -> FO` (same object). Size >= `0x1FDA0` B
(the flag `FO+0x1FD90` is written after each spawn attempt). It owns the **16-hardpoint pending-shot
table**. `FUN_14013D180(FO, pos, dir, speed, selfObj, selfPart, tgtObj, tgtPart, weaponParam, mode)`
fills one slot; `FUN_14013DB20(FO)` walks the slots, picks aim angles, clears the target pair and
commits the shot.

| off | type | meaning | evidence | conf |
| --- | --- | --- | --- | --- |
| `+0x000` | u32 | spawn sequence / tick → node `+0x60` | `FUN_14013D180` (`plVar14[0xc] = *param_1`) | [V read] |
| `+0x008` | ptr | pointer to the object-array pointer (`*(void**)(FO+0x08)` = `objArray`) | `FUN_14013D180`/`FUN_14013DB20` | [V] |
| `+0x028` | u8 | divisor in the launch-speed formula (`((float)*(int*)(param_9+0x44)*0.27777782 / this)*5.12 + speed`) | `FUN_14013D180` | [V] |
| `+0x158 + 0x58*i` | slot | `+0x158..0x164` pos vec4; `+0x168..0x174` dir vec4 (later overwritten with pitch/yaw angles by `FUN_14013DB20`); `+0x178..0x184` 2nd vector; `+0x188` weapon-param ptr; `+0x190` dword 0; `+0x194` f32 speed; `+0x198` u16; `+0x19A/+0x19B` **target pair**; `+0x19C/+0x19D` owner pair; `+0x19E` u8 | `0x13D1xx..0x13D24A`, `0x13DBxx..0x13E4xx` | [V] |
| `+0x6E8` | u32 | 16-bit "slot used" mask | `0x13D180`, `0x13DB20`, `0x13E6B3` | [V] |
| `+0x6F0` | u32 | last slot index (written when owner part is 0) | `0x13D6E5` region | [V] |
| `+0xCA0` | sub-block | **special-weapon** launcher state (used by `FUN_14013AD20`) | `0x125440`, `0x1B9A80` | [V call arg] |
| `+0x1B10` | sub-block | **missile** launcher state (used by `FUN_140141240`) | `0x125440`, `0x1B9A80` | [V call arg] |
| `+0x1FD90` | u8 | "launch armed/consumed" flag, written 0/1 immediately after a spawn call returns | `0x1031F0`, `0x11E180`, `0x11F1A0`, `0x1218C0`, `0x1B9A80` | [V write] |

The 16 slots are filled by **`FUN_14013D180`** (`0x13D180`), whose 10 arguments are
`(FO, posvec, dirvec, speed, selfObjIdx, selfPartIdx, tgtObjIdx, tgtPartIdx, weaponParamPtr, mode)`.
It also writes `slot+0x19A/+0x19B = (tgtObjIdx, tgtPartIdx)` — that is the only place the target
indices enter the launch pipeline, and it is **not** copied into the live MSL node.

---

## 6. Launch / aim RVA summary

| RVA | function | role | confidence |
| --- | --- | --- | --- |
| `0x19F30` | pool getter | returns `S = 0x1403219A0` | [V] |
| `0x1B2C0` | pool pop | pops a `0x70`-byte node from L4/L2 | [V] |
| `0x1A100` | pool scan | picks a node for a shooter (≤16, age window −1080) | [V] |
| **`0x1A2E0`** | **shot dispatcher** | per firing part: decides which spawns to run. Reads `part+0x1F0/+0x228/+0x234/+0x3C4/+0x3CC…/+0x408/+0x40C/+0x40D`, world array, team hostility; **bit 0x10 → `FUN_14001B050` (generic MSL)**; also spawns state-5 nodes and reuses pending nodes | [V] |
| `0x1B8360` | dispatcher's caller | mission weapon tick calls `FUN_14001A2E0` | [V] |
| **`0x1B050`** | **generic-MSL node initialiser** | fills a live node (state 4) from `WC`/`L`/`part`, links it into `part+0x3D8` and `S+0x28` | [V] |
| `0x1B08E..0x1B2BD` | node-init body | exact copy block quoted in §2/§3 | [V] |
| `0x13D180` | **hardpoint slot filler** | fills `FO+0x158+0x58*i` incl. target pair `+0x19A/+0x19B`; the "spawn site 0x13D665" (a pdata chunk) is inside it | [V] |
| `0x13DB20` | FO slot tracker | walks used slots, searches objects/parts in range (`part+0x41C` radius, hostility table `0x2AED00`), computes pitch/yaw into slot `+0x168/+0x16C`, clears the target pair, writes `fired-at` target pair to `shooterPart+0x240/+0x241` and `locked-by` pair to `targetPart+0x23C/+0x23D` | [V] |
| **`0x1B9A80`** | **fire-control tick** | per frame: for each FC in list `DAT_14033EDB8` (linked by `+0x3B8`) picks/keeps the lock (`FUN_1401269C0`), publishes the animation aim into `WC+0x1640..+0x1670` and `L+0x9F0/+0x9F4/+0xA10/+0xA14/+0xA20`, then re-fires pending records via `0x13D180`/`0x141240`/`0x13AD20` (**aim writers `0x1BA6E8..0x1BA763`**; lock setter call near `0x1B9Dxx`) | [V] |
| `0x1B7B00` | mission weapon tick | calls `FUN_1401B9A80(MC+0x697B8, MC)` (`0x1B7D49`) | [V] |
| `0x13D180` callers | `0x1031F0`, `0x11E180`, `0x11F1A0`, `0x1218C0` (all FC ticks) + `0x1B9A80` replay | | [V] |
| **`0x141240`** | missile spawn variant | called as `FUN_140141240(FO+0x1B10, pos, dir, speed, self, tgt, weaponParam, code)`; contains pool site `0x1412B4` | [V call arg] |
| `0x13AD20` | special-weapon spawn variant | called as `FUN_14013AD20(FO+0xCA0, pos, dir, …)` | [V call arg] |
| `0x125440` | weapon-type switch | `switch (*(char*)(weaponParam+0x48))` → `0x141240`/`0x13AD20`/flare spawn; the actual "fire" per weapon class | [V] |
| `0x1269C0` | **lock setter** | sets/clears `FC+0x236/+0x237`, updates "locked-by" marks | [V] |
| `0x115C60` | part weapon tick | mirrors `WC+0x1EEC/+0x1EED → part+0x236/+0x237`, `WC+0x70..0x8C → part+0x258/+0x268`, then runs the FC path | [V] |
| `0x1B050` span / `0x1B2B1` epilogue | | | [V] |

**Initial direction of a plain MSL = the launcher aim at spawn**, i.e. `L+0x9F0/+0x9F4/+0xA10`
→ `node+0x20/+0x24/+0x28`, plus `WC+0x1650/54/58` → `node+0x30/34/38` (position) and
`WC+0x1664/68/6C` → `node+0x3C/40/44`. No target-relative lead is applied to the *direction* at spawn;
the only "lead"-looking scalar in the launch path is `*(int*)(weaponParam+0x44)` folded into the
**speed** argument:

```c
speed = ((float)*(int32_t*)(param_9 + 0x44) * 0.27777782f / (float)*(uint8_t*)(FO + 0x28)) * 5.12f
        + speed_in;                                     // FUN_14013D180, param_9 = weapon-param block
```

`param_9` is the weapon-parameter block, **not** a target: in the FC path it is
`*(void**)(*(void**)(FC+0x1A0) + 0x28)` (a weapon-descriptor slot, `FUN_1401218C0`), and in the
replay path it is the value stored at `node+0x28` (`FUN_1401B9A80` `0x1B9A80` lower loop). That
`node+0x28` is a pointer to this same weapon-param block — see §8 correction 2.

---

## 7. What a per-frame PN hook can read

Minimum viable read set (all reads, no writes to the game):

1. `playerPart` and its lock pair: `arr = *(void***)(MC+0x697B8)`, `playerPart = arr[pi]->+0x110`;
   `tgt = (playerPart[0x236], playerPart[0x237])` (0xFF/0xFF = no lock).
2. Target position: `targetPart = arr[tgtObj]->+0x110 + 0x520*tgtPart`; `float tgtPos[4] = targetPart+0x258`.
3. Own missile state: from the live MSL node the mod already tracks — position `node+0x34..0x3C`,
   per-frame direction/velocity `node+0x40..0x48`, life `node+0x4C`, state `node+0x6B` (docs
   `MSL_GUIDANCE.md` §3.5 / §12). The node's `+0x66/+0x67` gives the shooter so a hook can find
   `playerPart` even without the global.
4. Target velocity: `V_t = (tgtPos - tgtPosPrev) / ticks`, ticks from `DAT_14033ED94`.
5. Optional sanity: `WC = *(void**)(playerPart+0x1B8)`, `WC+0x1EEC/+0x1EED` should equal the pair.

Recommended hook site for a target snapshot at launch: detour **`0x1B050`** (generic-MSL node
initialiser). At entry the ABI is `rcx = S`, `rdx = tick`, **`r8 = part`**, `r9 = param_4`; the
callee immediately does `movq %r8,%r11` (`0x1B05B`) and takes the node from the pool free-list tail
(`movq 0x8(%r8+0x1a0),%r10` after `lea 0x1a0(%rcx),%r8` at `0x1B061`). So a hook can read
`part+0x236/+0x237`, `WC = *(part+0x1B8)` and the node pointer in one shot. A detour on
**`0x1B9A80`** is the alternative if the mod wants the lock pair *before* the launch (it runs every frame).

---

## 8. Corrections to the existing docs

1. **`fire+0x70` is not a rounds counter** (`MSL_GUIDANCE.md` §3.3). At `0x1B0A6` the code is
   `decl 0x70(%r8)` where `%r8` has *already been reloaded* at `0x1B061` with `lea 0x1a0(%rcx),%r8`
   (`%rcx` = the pool singleton) — i.e. it decrements `S+0x210`, the **L3 free-list count** of the pool.
   `WC+0x70` itself is the owning unit's world position x (§2).
2. **`node+0x28` is not a target pointer** (`MSL_TARGET.md` §4(a)/probe T1). It is the
   **weapon-parameter block pointer**:
   * spawn writes it from `param_9` (`FUN_14013D180` → `plVar14[5] = param_9`, and the `0x13D665`
     chunk: `movq 0x180(%rbp),%rax; movq %rax,0x28(%r10)`);
   * the replay path `FUN_1401B9A80` feeds `node+0x28` back in as `param_9`, and `FUN_14013D180`
     immediately reads `*(int32_t*)(param_9+0x44)` for the speed formula;
   * `param_9` is loaded as `uVar8 = *(void**)(*(void**)(FC+0x1A0)+0x28)` (weapon-descriptor slot).
   ⇒ T1 should read `node+0x28` expecting a `WeaponModel`-style block, not an object/part pointer.
3. **The target pair is not in the node at all** (re-confirms `MSL_TARGET.md` §3 for the live
   state-4 node as well as the state-2 request node): the live MSL node only has the *shooter*
   indices `+0x66/+0x67`. The target pair is on the shooter (`part+0x236/+0x237`, `WC+0x1EEC/1EED`,
   `FC+0x236/0x237`, `FO slot+0x19A/19B`).
4. **`exe+0x32B438` is `MC+0xA8328`**, a sub-block of mission control, not the player part
   (`0x0CBEE0: lea r13,[rbp+0xa8328]` with `rbp = MC`; stored at `0x0CBF2E`). For the player part use
   `arr[playerObjIdx]->+0x110`.

---

## 9. Still open / probes

* **MSL per-frame advance / guidance function is still not located.** Everything above tells you where
  the target *is*; the code that turns `node+0x40..0x48` toward it is still unknown. Now that the
  fire-control tick (`0x1B9A80`) and FO slot tracker (`0x13DB20`) are ruled out as the MSL advance,
  the remaining cheap probe is the same one-run write-watch on `node+0x34`/`node+0x40`
  (`MSL_TARGET.md` §5.2) — but only after the mod is allowed to run.
* **Writer of `WC+0x1EEC/+0x1EED`** was not found statically (only 16-bit `0xFFFF` clears and the
  mirror read at `0x115F50`). It is presumably written inside the target-selection block
  `0x10Axx0..0x10B3xx` (candidate table `WC+0x1E33+0x20*i`, `WC+0x1EC8` count) via a helper with a
  computed destination. Probe: hardware watch on `WC+0x1EEC` while switching targets.
* `0x1403409F2` vs `0x1403409F1` (player object index vs player slot) — both resolve `arr[…]`; a
  single live read of `*(u8*)0x1403409F1/2` vs the object that owns the player part settles it.

---

## 10. Reproduction

```sh
PY="python"
cd ACZPropNav
# fire-object displacement scan (saved to work/mf/fire_fieldscan.txt)
cd work/rp && $PY fieldscan.py idx --offs 0x1630,0x1640,0x1650,0x1654,0x1658,0x1664,0x1668,0x166c,0x1670,\
0x1674,0x1675,0x1676,0x1677,0x1848,0x1850,0x1854,0xd48,0xc3c,0xc3d --min 2 --disp-range 0x0,0x2000
cd ../..
# node initialiser / fire-control tick
$PY work/rp/idx.py list work/rp/idx 0x1b050 0x48
$PY work/rp/idx.py list work/rp/idx 0x1b090 0x60
$PY work/rp/idx.py func work/rp/idx 0x13d665       # pdata chunk inside FUN_14013D180
```

Ghidra 12.1.4 (REA MCP, image already open) was used for the decompiled call-graph evidence:
`analyze_function`/`procedure_pseudo_code` on `0x14013D180`, `0x14001A2E0`, `0x14001B050`,
`0x1401031F0`, `0x140104100`, `0x140119A70`, `0x1401218C0`, `0x140125440`, `0x14013DB20`,
`0x1401B9A80`, `0x1400CBD00`, `0x1401269C0`.
