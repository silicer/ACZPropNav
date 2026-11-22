# ACZPropNav — the plain-MSL per-frame advance: what it is, what it is not, and where to hook

Status: this pass was run with **Ghidra 12.1.4 via the REA MCP** (working again) plus the
local `.pdata`/instruction index (`work/rp/idx`). Everything below is instruction- or
pseudocode-level. Tags: **[V]** VERIFIED (instruction/pseudocode quoted),
**[I]** INFERRED (consistent, not unique), **[U]** UNKNOWN (probe given).

**Headline (three lines).**

1. **There is no per-frame writer of the pool-node transform.** The 0x70-byte node at
   `exe+0x3219A0` (the "MSL pool") is written **exactly once, at spawn**, by the
   6 alloc sites; after that it is only *read* (launch dispatcher, recorder, effect
   bridge, reset). Proportional navigation **cannot** be applied at the pool node. [V]
2. The per-frame driver of that pool exists and is now located: **`exe+0x1B9A80`**
   (`FUN_1401b9a80`, the launch/effect **dispatcher**, frame counter `exe+0x33ED94`),
   called from exactly one place: **`exe+0x1B7D49`** inside **`exe+0x1B7B00`**
   (`FUN_1401b7b00`, the per-frame weapon/effect tick — `0x1B7B90` is only a `.pdata`
   *chunk* of it), which is itself called from **`exe+0xD1F05`** as world-update
   **phase 7** (`bl == 7`) with `rcx = world`. Those are the RVAs that matter. [V]
3. The thing that actually *moves* a missile is the **world object/part model**, not the
   node: `world+0x697B8` (object array) → `*(world+0xA8248) + i*0x27A0` (per-object sim
   record) → `obj+0x110` (parts, stride **0x520**) → **`part+0x258` = position vec4**,
   `part+0x268` = heading/rotation. `FUN_1401b7b00` snapshots exactly those fields every
   frame (`0x1B7D8A…`: `perObj+0x70 = *(part+0x258)`). [V]
   ⇒ the plain-MSL hook must land on the **object/part transform**, not on the node.

| # | question | answer | tag |
|---|---|---|---|
| 1 | Direct refs to the singleton VA `0x1403219A0` | **5** instructions only (2 in the getter, 3 in three pool functions); everything else goes through the getter | [V] |
| 2 | Who calls the getter `exe+0x19F30` | 32 sites (24 functions), all catalogued in §2.2 | [V] |
| 3 | Per-frame writer of node `+0x34..+0x4C`? | **none** — spawn/init/reset only (§5) | [V] |
| 4 | The per-frame driver | `FUN_1401b9a80` (`0x1B9A80`), 1 caller: `0x1B7D49` in `FUN_1401b7b00` (tick, phase 7 of the world update) | [V] |
| 5 | What the node actually is | a **launch/effect record** (time `+0x60`, pos `+0x34`, dir `+0x40`, life `+0x4C`, shooter `+0x66/+0x67`, state `+0x6B`), consumed by the effect/trail layer | [V] |
| 6 | Where the flying missile's state is | the **part** (`0x520`-byte) of a world object: pos `+0x258`, heading `+0x268`, owner object array `world+0x697B8` | [V] model / [I] identity |
| 7 | Which part is *the* missile | **[U]** — needs the 1-run probe of §9 (a part that appears at launch pos and integrates each frame) | [U] |
| 8 | Best hook site for PN | **call-site patch at `0x1B7D49`** (5-byte `E8`), gives `rdi = world` and runs after the game's per-frame transform commit. Sig unique (§8.1) | [V site / I plan] |

---

## 1. Reproduction / scratch tooling (all in `work/ma/`)

| tool | purpose |
|---|---|
| `asm.py <lo> <hi>` | print `work/ga/full2.asm` lines for a VA/RVA range (Intel syntax) |
| `win.py <rva[,rva]> [before] [after]` | instruction window around one or more RVAs (`work/rp/idx/insns.tsv`) |
| `callmap.py f\|c\|d\|b <rva> …` | function dossier: call/jmp targets, displacement histogram, callers |
| `pooluser.py` | which pool-singleton field does each getter caller touch (list heads/counts/node payload) |
| `nodefield.py` | **store/load scan on the exact node payload offsets, grouped per function** (see §5) |
| `dispscan.py <disp>[,disp] [--require 0x…]` | who stores to an arbitrary displacement |
| `sigchk.py <rva> [n]` | build a AOB pattern from the index and count occurrences in `.text` |
| `out_nodefield3.txt` | saved output of the definitive store scan (19 functions) |

**Trap that cost this pass an hour — read this before re-running any field scan.**
`work/rp/idx/insns.tsv` is **AT&T** (llvm-objdump), so the **destination is the last
operand**. `vmovss %xmm12, 0x34(%r10)` is a *store*; `vmovss 0x34(%r10), %xmm1` is a
*load*. On top of that, `cmp[bwlq]`, `test[bwlq]`, `comiss/ucomiss/vcomiss/vucomiss`
also put a memory operand last **as source**. A naive "memory-operand-last ⇒ store"
scan produces false positives exactly at the interesting places (it made the dispatcher
look as if it wrote `node+0x4C`); `nodefield.py` now strips both. Any earlier claim
derived from such a scan should be re-checked with `asm.py`.

---

## 2. Census: who can even see the pool (§3.1 of MSL_GUIDANCE extended)

### 2.1 The five direct references to `0x1403219A0` [V]

```text
0x19F52  lea rax,[rip+0x307A47]   ; 0x1403219A0   inside the magic-static getter 0x19F30
0x19F74  lea rbx,[rip+0x307A25]   ; 0x1403219A0   second return path of the same getter
0x1B7687 lea rcx,[rip+0x16A312]   ; 0x1403219A0   inside FUN_1401B6B00  (the *recorder*)
0x1B85B3 lea rcx,[rip+0x1693E6]   ; 0x1403219A0   byte 0 of the trivial getter FUN_1401B85B3
0x1B8683 lea rcx,[rip+0x169316]   ; 0x1403219A0   inside FUN_1401B8655 (0x154 B)
```

`grep -ic 3219a0 work/ga/full2.asm` = **5**. So there is a *second, unguarded* getter at
`0x1B85B3` (0x18 B, immediately followed by `call 0x1A100`, see §2.3) — use it if you want
the singleton without the TLS/magic-static dance.

### 2.2 Getter callers, grouped by role [V sites / I roles]

`exe+0x19F30` has 32 call sites / 24 functions. Ranked by what they do with the object:

| function(s) | sites | role |
|---|---|---|
| `0x14D40` | `0x14D5A` | pool **reset/lifecycle**: walks L1 (`S+0x20`), keeps state-6 nodes, re-threads free list (`S+0x1A0`), zeroes payload |
| `0x14F40` | `0x14F70` | **effect bridge**: walks L1, matches effect objects by `+0x1E6 == node+0x67`, requires `node+0x6B==0`, decodes the node transform (`0x1DE00`) into a 0x68-byte scheduled effect instance, time-sorted via `+0x50` child links (§4.3) |
| `0xCBD00` | `0xCD603, 0xCD74D` | pool **ctor/reset bundle** (touches every field incl. `S+0x98`, both pool bases, `S+0x318`) |
| `0x1254A2` | `0x12568E` | **spawn site #1** |
| `0x12B631` | 5 sites | **spawn site #2** — the weapon-fire routine; also writes `node+0x6B = 8` at `0x12B8ED` |
| `0x13B09B` | `0x13B09B` | **spawn site #3** (state 2) |
| `0x13D180` | `0x13D6F3` | **spawn site #4** (= the `0x13D665` fragment; state 2) — the plain-MSL cat-0 spawn |
| `0x1412B4` | `0x141312` | **spawn site #5** (state 2) |
| `0x1C7080` | `0x1C70B8` | **spawn site #6** — called from the state-7 effect path (§4.4) |
| `0x1B6B00` | `0x1B6B42` | **recorder/telemetry**: snapshots both lists into a `0x100`-entry × 0x10 array at `this+0xC50`; reads `+0x60/+0x20/+0x34/+0x38/+0x3F/+0x40/+0x41` |
| `0x1B81E5` | `0x1B820B` | tiny counter helper (`S+0x20`, `S+0x90`) |
| `0x1B8368`, `0x1B85B3`, `0x1B8655` | 2+1+1 | tiny helpers around `0x1A100` ("pick a node for this shooter") |
| `0x1B87B0` | 3 sites | per-frame **world tick / mission reload** wrapper (calls `0x1B6B00(&DAT_14033ED80,1)` each frame; `param_2==0` ⇒ reset) |
| `0x1B92C0` | `0x1B99DE` | misc list user (`S+0x20`, node `+0x40`) |
| `0xD1AD6`, `0xF4E9A`, `0x1006F0`, `0xCDD65`, `0xCE903`, `0x106F0`… | 1–4 each | readers around `S+0x20`/`S+0x290`/pool-2 (`0x1006F0` also uses the `S+0x318` cached node and the `0x320/0x324` sentinels) |
| `0x1B9A80` region | `0x1BA791` | the per-frame **dispatcher** (§4) |

**No function outside the spawn set ever *stores* into the node payload** (§5).

### 2.3 The singleton layout, corrected/extended [V]

Everything from MSL_GUIDANCE §3.2 stands, with one addition that matters for walking the
live list:

```asm
; spawn 0x13D180 (0x13D7E2…0x13D7FD) — tail-insert into the list whose head sentinel is S+0x98
14013d771  lea  rcx,[r11+0x98]        ; r11 = S
14013d7eb  mov  rax,[rcx+8]           ; rax = head->prev  (tail)
14013d7ef  mov  [rax],r10             ; tail->next = node
14013d7f2  mov  [rcx+8],r10           ; head->prev = node
14013d7f6  mov  [r10],rcx             ; node->next = &head
14013d7f9  mov  [r10+8],rax           ; node->prev = old tail
14013d7fd  inc  dword ptr [rcx+0x70]  ; ★ COUNT at head+0x70 = S+0x108
```

⇒ the rule is uniform: **list head at `S+X`, count at `S+X+0x70`** (L1 `0x20`→`0x90`,
L2 `0x98`→**`0x108`**, L3 `0x1A0`→`0x210`, L4 `0x218`→`0x288`, L5 `0x290`→`0x300`).
`S+0x108` was missing from the existing docs — that is the count you want when you ask
"how many live projectile records are there".

---

## 3. Node maps: two *different* records share the 0x70-byte shape

### 3.1 The pool node (what `0x13D180` builds) [V]

Disassembly of the actual plain-MSL spawn (`0x13D665` fragment inside `FUN_14013d180`):

```asm
14013d73a  mov   rcx,[rdx+0x110]              ; parts base of the shooter object
14013d741  movzx r8d,byte [rdx+0x15d]         ; shooter team
14013d737  mov   eax,dword [r11]              ; ★ r11 = S → node timestamp = S+0x00 counter
14013d749  mov   [r10+0x60],eax
14013d74d  movzx eax,word [r9+rcx+0x228]      ; +0x64 = u16 weapon slot/type
14013d756  mov   [r10+0x64],ax
14013d75b  movzx eax,byte [r9+rcx+0x234]      ; +0x66 = shooter OBJECT index
14013d764  mov   [r10+0x66],al
14013d768  movzx eax,byte [r9+rcx+0x235]      ; +0x67 = shooter PART index
14013d778  mov   [r10+0x67],al
14013d77c  movzx eax,byte [rbp+0x170]  ; +0x22 = u8  (caller arg)
14013d787  movzx eax,byte [rbp+0x178]  ; +0x23 = u8  (caller arg)
14013d792  mov   rax,[rbp+0x180]       ; +0x28 = qword — the *weapon parameter object*
14013d799  mov   [r10+0x28],rax
14013d79d  movzx eax,byte [rbp+0x188]  ; +0x50 = u8  (fire-mode/flags)
14013d7a4  mov   [r10+0x50],al
14013d7a8  vmovss [r10+0x34],xmm12     ; ★ position.x
14013d7ae  vmovss [r10+0x38],xmm13     ;   position.y
14013d7b4  vmovss [r10+0x3c],xmm14     ;   position.z
14013d7ba  vmovss [r10+0x40],xmm15     ; ★ direction.x
14013d7c0  vmovss [r10+0x44],xmm0      ;   direction.y
14013d7c6  vmovss [r10+0x48],xmm1      ;   direction.z
14013d7cc  vmovss [r10+0x4c],xmm10     ; ★ life / remaining time
14013d7d2  mov   byte [r10+0x68],r8b   ; team
14013d7d6  mov   word [r10+0x69],0
14013d7dd  mov   byte [r10+0x6b],2     ; ★ state = 2 (LIVE/spawned)
14013d7e2  mov   word [r10+0x20],r14w  ; ★ +0x20 is a u16 here (r14d = 0)
14013d7e7  mov   dword [r10+0x30],ebx  ; +0x30 is a u32 here
```

**This reconciles the two conflicting maps**:

* `MSL_TARGET.md` §12's live sample (**`+0x34..0x3C` position, `+0x40..0x48` direction,
  `+0x4C` life, `+0x6B` state**) is **correct** — it is this node. [V]
* `MSL_GUIDANCE.md` §3.3's "initialiser `exe+0x1B08E`" is a **different variant**:
  `0x1B08E` is an *interior label* of the function that starts at `0x1B083A`
  (`.pdata`: `0x1B083A+0x130`), and **nothing calls `0x1B08E` directly** (`refs` = 0).
  That writer sets `+0x6B = 4` and fills `+0x20/+0x24/+0x28…` as **dwords** copied from a
  launcher (`[r9+0x9F0]`, `[r8+0x1650]`…), i.e. it is a *different munition/effect
  variant* of the same 0x70-byte record. Do not use it as the MSL map. [V]

Canonical pool-node map (use this one for the mod):

| off | type | meaning | off | type | meaning |
|---|---|---|---|---|---|
| `+0x00` | ptr | list next | `+0x34` | f32 | **position.x** |
| `+0x08` | ptr | list prev | `+0x38` | f32 | position.y |
| `+0x20` | u16 | weapon category (0/1/2) — **selects the spawn fn** | `+0x3C` | f32 | position.z |
| `+0x22`,`+0x23` | u8,u8 | shooter obj/part (launch args) | `+0x40` | f32 | **direction.x** |
| `+0x28` | ptr | weapon **parameter object** (the `.wpb` runtime block) | `+0x44` | f32 | direction.y |
| `+0x30` | u32 | (fire counter/flags) | `+0x48` | f32 | direction.z |
| `+0x4C` | f32 | **life / remaining time** | `+0x50` | u8 | fire-mode/flags |
| `+0x60` | i32 | timestamp = `S+0x00` sequence | `+0x64` | u16 | weapon slot/type |
| `+0x66`,`+0x67` | u8,u8 | shooter (object, part) indices | `+0x68` | u8 | team |
| `+0x69` | u16 | 0 | `+0x6B` | u8 | **state** (0/1/2/4/5/6/8 observed) |

### 3.2 The *event* node (what the dispatcher walks) [V]

Same 12-float payload, different link fields and different container:
`next` at `+0x00`, **children at `+0x10`**, time at `+0x60`, state at `+0x6B`. It lives in a
global circular, **time-sorted** list whose head sentinel is `DAT_14033edb0`
(VA `0x14033EDB0`), inside the global struct at `DAT_14033ed80` (`0x14033ED80`).
Created by the weapon-fire routines, dispatched once per frame by `FUN_1401b9a80`.

⇒ **two node kinds, one 0x70-byte shape**: *pool node* = "a projectile/effect record was
launched here", *event node* = "at time `+0x60`, do state-machine thing `+0x6B`".
The pool node's pos/dir/life are **copied from** the event node at dispatch time (§4.2).

---

## 4. The per-frame system, in full

### 4.1 Entry point: `FUN_1401b7b00` (`RVA 0x1B7B00`) — the weapon/effect phase handler [V]

> ⚠ **Superseded in part — see §11.5.** `0x1B7B00` is **not** an unconditional per-frame tick:
> it is one phase of a state machine, gated on `*(u8*)(arg2+0x3CC) == 7` and
> `*(u8*)(world+0xA8822) ∉ {4,5,8}`. It was not reached during Run A's ordinary flight.

**Boundary warning first:** `0x1B7B90` is *not* a function. It is one of five `.pdata`
chunks of `FUN_1401b7b00` (`0x1B7B00+0x11`, `0x1B7B11+0x3A`, `0x1B7B4B+0x45`,
`0x1B7B90+0x456`, `0x1B7FE6+0x1DE`; the prologue is `push rsi / sub rsp,0x80` at
`0x1B7B00` and **`mov rdi,rcx` at `0x1B7B16` makes `rdi = world`**). This chunking is why
a "callers of `0x1B7B90`" query returns nothing — the call targets `0x1B7B00`. [V]

It is invoked as **world-update phase 7**:

```asm
1400d1eeb  cmp  bl,0x7                    ; phase selector
1400d1eee  jne  0x1400d2060
1400d1ef4  mov  rcx,rdi                   ; rcx = world
1400d1ef7  mov  byte [rip+0x25DA54],r12b  ; 0x14032F952
1400d1efe  mov  dword [rip+0x26EBCB],r12d ; 0x140340AD0
1400d1f05  call 0x1401b7b00               ; ★ the per-frame weapon/effect tick
1400d1f0a  test al,al
```

(`0xD1F05` lies in the `/0xD1A19`-headed update-dispatcher chunk family; that dispatcher
walks the world's object list too — `[rdi+0xA87E4]`, `[rdi+0xA8318]`.) Per frame the tick:

1. pre-updates the per-object sim records `*(world+0xA8248) + i*0x27A0`
   (`0x1B7BC0` loop: writes `+0x1D64`, `+0x1D6C`, `+0x1C30`-family fields);
2. calls two subsystem updates (`0x80660` on `world->0x67138 + k*0xD10`, `0x7B0C0` on
   `world->0x67140 + k*0x1C8`);
3. **calls the dispatcher** and stores its result:

```asm
1401b7d3f  mov  rdx,rdi                  ; rdx = world
1401b7d42  lea  rcx,[rdi+0x697b8]        ; rcx = &world->objectArray
1401b7d49  call 0x1401b9a80              ; ★ THE PER-FRAME DISPATCHER (only caller)
1401b7d4e  mov  r10d,esi
1401b7d51  mov  [rsp+0x90],eax           ; <- best hook point for a post-update (see §8)
```

4. then, per object, snapshots part-0's transform into the per-object sim record:

```asm
1401b7d65  mov  r11,[rdi+0x697b8]        ; objects array
1401b7d72  mov  r8,[r11+8*rax]           ; obj = objects[i]
1401b7d83  mov  rdx,[r8+0x110]           ; parts base  (part 0)
1401b7d8a  mov  eax,[rdx+0x258]  →  [perObj+0x70]   ; position.x
1401b7d95  mov  eax,[rdx+0x25c]  →  [perObj+0x74]   ; position.y
1401b7da0  mov  eax,[rdx+0x260]  →  [perObj+0x78]   ; position.z
1401b7dab  mov  eax,[rdx+0x264]  →  [perObj+0x7c]   ; position.w (1.0)
1401b7dc0  mov  eax,[rdx+0x268]  →  [perObj+0x80]   ; heading.x
1401b7dcc  mov  eax,[rdx+0x26c]  →  [perObj+0x84]   ; heading.y
```

**This is the single most useful block in the whole investigation**: it proves the engine
keeps a *mutable per-part transform* (`part+0x258…`) that is the missile's real state, and
it gives a per-frame, register-available handle (`rdi = world`) to it.

### 4.2 `FUN_1401b9a80` (`RVA 0x1B9A80`) — the dispatcher [V]

Ghidra boundary: one function, `0x1B9A80 … 0x1BB17D`, merged from `.pdata` chunks
`0x1B9A80+0x14`, `0x1B9A94+0xF2`, `0x1B9B86+0xDF2`, `0x1BA978+0x805`. Recursive-descent
and `.pdata` tools report it as 4–5 separate "functions"; it is one.

Signature `48 8B C4 55 41 57 48 8D A8 B8 FD FF FF 48 81 EC` — **unique**. Entry:
`rcx = &world->objectArray (world+0x697B8)`, `rdx = world`.

It does three things per frame:

**(a) per-weapon-part lock/target-track interpolation** (the fire-control lead solution).
For each part in a list chained through `part+0x3B8` it interpolates two **child event
nodes** (found through `part+0x3D0 / +0x3E8 / +0x3F0 / +0x3F8 / +0x400`) by their
timestamps (`+0x60`) and commits the result into the launcher's fire state:

```
f = (now - nodeA->+0x60) / (nodeB->+0x60 - nodeA->+0x60)
fire+0x1650/54/58 = lerp(pos)          ; aim point   (the values the spawn reads!)
fire+0x1640, fire+0x1664/68/6c/70 = lerp(speed, dir)
launcher+0x9F0/0x9F4/0xA10 = lerp(aim vector), launcher+0xA14/+0xA20 = bytes{0x54,0x55}/255
launcher+0x9D4 flags;  and  S+0x18 = lerp(node->+0x4C)      <- writes the singleton +0x18
```

**(b) the time-sorted event dispatch** (`DAT_14033edb0`, node `next=+0x00`, `time=+0x60`):

| state `+0x6B` | action (RVA) |
|---|---|
| 2 (spawn) `u16 +0x20`=category | 0 → `FUN_14013d180(*(part+0x178), *(node+0x34) pos3, node+0x40 vec, node+0x4C, part+0x234, part+0x235, node+0x22, node+0x23, *(node+0x28) param, node+0x50)` @ `0x1BAFDE` · 1 → `0x141240(…, obj+0x1B10)` · 2 → `0x13AD20(…, obj+0xCA0)` |
| 3 | `FUN_140031FC0(world+0x66EE0, 0x2A, …)` when `u16 +0x38 == 6` |
| 7 | `FUN_1401C63A0` (if `node+0x4C` byte == 0) else `FUN_1401C6490`, with `(node+0x24, node+0x48, u16 node+0x4A, node+0x4D)` |
| 8 | `FUN_1401C67D0(node+0x20, u16 node+0x22)` |

After a successful state-2 spawn it registers the launch in an **80-entry × 0x18 table at
`exe+0x33EDD0`** (count `exe+0x3409D4`; read by `0x1AB68F` and `0x1AE706` — the
launch-sound/assist consumers), and increments `DAT_14033ed94` (frame counter) at the end.

**(c)** the effect-instance commit path: `FUN_1401c67d0` (state 8) allocates a **model
instance** from the pool at `&DAT_1437ddf40` (`FUN_1401ca4e0`), sets
`inst+0x44 = model/type u16 (table 0x1437DE060[node->+0x20])`, `inst+0x46 = u16`,
`inst+0x3C = 1.0f`, `inst+0x70 |= 4`, `inst+0x30 = FUN_1401CB500`; `FUN_1401cb500`
then calls the engine's model API **`FUN_140184980(modelId, slot, scale, …, inst+0x4C, …)`**
and later `FUN_140183F10(inst+0x4C, …)` — i.e. `inst+0x4C` is that instance's transform
buffer. `0x1C67D0` has **48 direct callers** and `0x1CA4E0` **78**, so this is the shared
"spawn a visible effect/model instance" API, not a missile-specific path.

**(d)** the matrix decode: `FUN_14001de00(node, matrix, scale)` — gated on
`node+0x6B == 0` — is called 4× total in the image: `0x14F40` (×2), `0x1B9A80` (×2),
`0x1B7860`, `0x1B9A80`-merged-chunk (×3). It is a *transform→matrix* helper, **not** an
integrator.

### 4.3 `FUN_1401b6b00` is a recorder, not an advance [V]

`0x1B6B00` (0xCA6) walks L1 and L2 and copies a *summary* of every node into
`this+0xC50 + n*0x10` (max 0x100 entries): `+0x60`, `+0x20` (as dword), and 5 bytes
`+0x34/+0x38/+0x3F/+0x40/+0x41`. It is called by `FUN_1401b87b0(world, 1)` every frame
(that is the "mission (re)loaded" anchor the Lead found: `param_2==0` ⇒ full reset).
Its apparent "writes" to node offsets in a naive scan are writes into **its own** arrays.

---

## 5. The definitive negative: nobody advances the node transform

`work/ma/nodefield.py` scans every instruction of the image for **stores** (AT&T-correct,
`cmp/test/comiss` excluded) to the node payload offsets, per function; saved output in
`work/ma/out_nodefield3.txt`. Result for the transform/life group
(`+0x34/+0x38/+0x3C/+0x40/+0x44/+0x48/+0x4C`):

| function | stores | role |
|---|---|---|
| `0x1254A2`, `0x12B631`, `0x13B09B`, `0x13D180`, `0x1412B4`, `0x1C7080` | full block, once | **the 6 spawn sites** (allocate + fill + insert, state 2/8) |
| `0x1AC6D`, `0x1AE00`, `0x1A91A`, `0x1A5B2`, `0x1B08E` | full block, once | **sibling spawn variants** (states 0/1/4/5/6 — other munitions/effects) |
| `0xB97B0` | subset | another spawn variant (reads `+0x6B`, writes `+0x60`) |
| `0x14D40` | `+0x28/+0x30/+0x38` | **pool reset/lifecycle** (`0x14D40`), zeroes payload |
| `0x14F40` | `+0x40/+0x48` only | **effect bridge** — writes *direction* of the effect instance it builds, never position |
| `0x1B6B00` | `+0x38/+0x40` into its own arrays | recorder (§4.3) |
| `0x205C18` | `+0x34` | unrelated struct (`+0x68/+0x69`, no `+0x60/+0x6B` correlation) |

⇒ **Every writer of `node+0x34..+0x4C` is a one-shot initialiser/zeroing.** There is no
read-modify-write, no SIMD block update, no `pos += dir` — in any form — for the pool node.
Combined with §5 of `MSL_TARGET.md` (which reached the same conclusion from the other
direction), this is now closed for good: **the pool node cannot be the PN write target.**

---

## 6. Where the missile's motion actually lives → the object/part model

Evidence (all `[V]` reads unless tagged):

* `world` (the `rdx`/`param_2` of the dispatcher) holds
  **`world+0x697B8` = `Obj**` array**, count `world+0xA87E4` (byte),
  **`world+0xA8248` = pointer to the per-object sim array, stride 0x27A0**
  (`0x1B7B95`, `0x1B7CD4`, `0x1B7D65`, and the dispatcher's `*0x27A0` uses).
* `Obj`: `+0x110` = parts base, `+0x118` = part count, `+0x120` = flags, `+0x15D` = team,
  `+0x178` = (used as the "aircraft/owner" argument of the spawn), `+0x697B8` self-ref.
* **part, stride 0x520**: `+0x234` = own object index, `+0x235` = own part index,
  **`+0x258` = position vec4 (w = 1.0)**, `+0x268/+0x26C/+0x270` = heading/rotation,
  `+0x1F0` = alive flags, `+0x1B8` = the per-part "fire/weapon state" object.
* `FUN_1401b7b00` copies `part+0x258…` into `perObj+0x70…` **after** calling the dispatcher
  (§4.1) — i.e. the authoritative transform for frame *n* is `part+0x258` at that moment.
* the dispatcher itself commits matrices into `part+0x258`/`+0x268` via
  `0x1DE00`/`0x1DF50` for the effect parts it interpolates (`*(float*)(lVar23+600) = …`,
  600 = 0x258).
* 78/48/24 functions reference the 0x27A0 per-object stride in the `0xC6000–0xD5000`
  region — the object-simulation code; that is the code family to inspect next if the
  per-part integrator is needed.

⇒ **[I]** the plain MSL is a **part of a world object**, and its per-frame motion is the
engine's object/part transform update (matrix-based), exactly the shape
`MSL_TARGET.md` §5.1 already suspected from `0xA5973`:
`vaddss xmm1,xmm9,[rbp+0x40]` … `vmovss [rbp+0x40],xmm1` / `call [rax+0x120]`.

---

## 7. Ranked candidates (for "where is the per-frame advance / where do I hook")

| rank | RVA | what it is | why it ranks here | tag |
|---|---|---|---|---|
| **1** | **`0x1B7B00`** (hook at `0x1B7D49`) | per-frame weapon/effect tick, world-update phase 7 | the only per-frame code that both **calls the projectile dispatcher** (`0x1B7D49`) and **touches the authoritative part transform** (`part+0x258 → perObj+0x70`). Best stable per-frame anchor; `rdi = world` at the hook site. | [V] |
| **2** | **`0x1B9A80`** | per-frame launch/effect **dispatcher** (merged `0x1B9A80/0x1B9A94/0x1B9B86/0x1BA978`) | the state machine for every projectile/effect node: state 2 = launch (calls the 6 spawn sites with pos/dir/life), 7/8 = effect/model instances; also the per-part lock/lead interpolation. Reads *all* node payload fields; increments `0x33ED94`. | [V] |
| **3** | **`0x1C67D0`** → `0x1CA4E0` → `0x1CB500` → `0x184980` | "spawn a visible model/effect instance and commit its transform" | the only path found that reaches the engine's **model transform API** with a per-instance transform buffer (`inst+0x4C`); 48/78 callers ⇒ shared API. If the visible missile is one of these instances, PN belongs on `inst+0x4C` (or on `0x183F10`, the transform update). | [V code / I role] |
| 4 | `0xA5973` (from `MSL_TARGET` §5.1) | orientation-matrix rebuild + `translation += v` + `obj->vt[0x120](&matrix)` | right *shape* for a per-frame actor advance, but **no direct callers and no vtable slot** ⇒ only reachable indirectly; unproven. | [U] |
| 5 | `0x1C63A0` / `0x1C6490` (state 7), `0x1C7080` (spawn #6) | the state-7 effect path: `0x1C63A0 → 0x1C7080 (pool alloc) + 0x1CA5C0` | per-launch effect records, not steering. | [V] |
| 6 | `0x12B631` (`+0x12B8ED → state 8`) | the fire routine that *creates* event nodes | relevant only as a **capture point** (arg = shooter part, gives the target/lock state). | [V] |

**Answer to "is there a per-frame node writer at all?" — No.** For the `exe+0x3219A0`
pool node: no. For the *event* node: also no (the dispatcher only reads it; `cmp/test`
false positives mislead). The per-frame writer that exists is of the **part transform**
(`part+0x258`), reached from the object-simulation code and from the dispatcher's
interpolation — that is where PN must go.

---

## 8. Hook plan

### 8.1 Primary hook: replace the dispatcher call inside the tick [V site]

Site `RVA 0x1B7D49` — the only `call 0x1401B9A80` in the image
(`E8 32 1D 00 00`, next instruction `0x1B7D4E`). Pattern, **verified unique**
(`work/ma/sigchk.py 0x1B7D49 24` → 1 occurrence):

```text
E8 ?? ?? ?? ??  44 8B D6  89 84 24 90 00 00 00  40 38 B7 E4 87 0A 00  0F 86
```

Why this site and not a function entry:

* it is a plain 5-byte `E8 rel32`; patching it to `E9 rel32` needs **no trampoline**,
  **no stolen bytes**, no unwind interaction — you simply `call` the original
  `0x1401B9A80` yourself from the thunk;
* at the site the registers still hold the dispatcher's arguments:
  **`rcx = world+0x697B8`, `rdx = rdi = world`**, and `rdi` is the *world pointer*
  (callee-saved for the whole tick) — so the thunk can walk objects without touching any
  global;
* it runs **after** the game has advanced this frame's transforms (dispatcher returned) and
  **before** the renderer and before the per-object snapshot loop (`0x1B7D58…`) — the ideal
  point for a "post-update override": what you write is what the renderer and the next
  frame's cache both see.

Thunk (x64, MSVC inline-asm/masm or a naked function):

```asm
; entry: rcx = &world->objectArray, rdx = world, rsp%16 == 8 (as for any call target)
sub  rsp, 0x48                 ; 0x48 % 16 == 8 -> rsp now 16-aligned
mov  [rsp+0x20], rcx
mov  [rsp+0x28], rdx           ; keep world
mov  [rsp+0x30], r8
mov  [rsp+0x38], r9
call qword ptr [g_orig_dispatch]   ; -> 0x1401B9A80 ; returns eax
mov  [rsp+0x40], eax
mov  rcx, [rsp+0x28]           ; arg = world
call pn_tick                   ; C: void pn_tick(void* world)
mov  eax, [rsp+0x40]
mov  rcx, [rsp+0x20]
mov  rdx, [rsp+0x28]
mov  r8,  [rsp+0x30]
mov  r9,  [rsp+0x38]
add  rsp, 0x48
ret
```

* Caller-saved (`rax,rcx,rdx,r8-r11,xmm0-15`) are free to clobber; `rbx,rbp,rsi,rdi,r12-r15`
  must survive (the thunk itself only uses the stack + volatiles; `pn_tick` is normal C++).
* `g_orig_dispatch` = `exeBase + 0x1B9A80` (we never patched the callee, so no trampoline).
* Patch bytes: at `0x1B7D49` write `E9 <rel32>` where `rel32 = thunk_va - (site_va + 5)`;
  restore the original 5 bytes on unload. Single-site, single-thread (game thread), so no
  instruction-cache games beyond the usual `FlushInstructionCache`.

**Alternative (fallback)** — patch the dispatcher entry itself:
`0x1B9A80`, sig `48 8B C4 55 41 57 48 8D A8 B8 FD FF FF 48 81 EC` (unique). Prologue is
relocatable after **6** bytes (`48 8B C4` = `mov rax,rsp`, `55` = `push rbp`,
`41 57` = `push r15`). Same `sub rsp,0x58` bookkeeping as the existing
`InstallIntegratorHook` (`docs/GUIDANCE_ASM.md` §10). Loses the "after commit" ordering
unless you also post-process, so prefer 8.1.

### 8.2 What to write for proportional navigation

**Do not** write the pool node (`+0x6B`, `+0x00/+0x08`, `+0x60`, `+0x28`, `+0x66/+0x67`
are owned by the spawn/despawn/reset machinery — see §5, and a broken link crashes the next
list walk).

Write the **part transform** of the missile's object, after the game's own update:

```text
obj   = *(void**)(world + 0x697B8) + 8*i        ; objects array (count: *(u8*)(world+0xA87E4))
part  = *(void**)((char*)obj + 0x110) + 0x520*p  ; parts          (count: *(u8*)(obj+0x118))
p+0x258 = pos.x   p+0x25C = pos.y   p+0x260 = pos.z   p+0x264 = 1.0f
p+0x268 = heading.x   p+0x26C = heading.y   p+0x270 = heading.z   (keep +0x274 consistent)
```

* **Safe variant (recommended first):** let the game advance the part, then rotate only
  `+0x268/+0x26C/+0x270` to the PN heading (normalise, keep the game's speed); the next
  frame's own integration then flies along the new heading. This keeps the game's speed,
  lifetime and collision bookkeeping intact — the same philosophy as the `WeaponEx` hook
  (`slot+0x20` velocity) but one level lower.
* **Hard override:** set `+0x258/+0x25C/+0x260` yourself (`p_prev + V_pn`, `|V_pn|` clamped
  as in `docs/PN.md` §3.4) and set the heading from `V_pn`. Keep `+0x264 = 1.0f`.
  This bypasses the game's clamp — clamp yourself, and keep the basis orthonormal.
* **Also mirror the cache** if you hard-override: `perObj = *(world+0xA8248) + i*0x27A0`,
  copy `p+0x258..` into `perObj+0x70..0x84`, otherwise the dispatcher's interpolation uses a
  stale previous-frame transform for one frame.
* **Target vector:** the node carries no target (§3.1: `+0x50` is a flag byte, not an index
  pair — `MSL_TARGET.md` §3 stands). Two live sources:
  (a) the shooter part's fire state: `part + 0x1B8` = fire object, whose `+0x1650/+0x1664`
  hold the current **aim/lead solution** the dispatcher computes each frame (§4.2a) — that is
  a ready-made intercept point; (b) the target-track children of the weapon part
  (`part+0x3D0 / +0x3E8 / +0x3F0 / +0x3F8 / +0x400`, nodes with `+0x60` time and 12 floats)
  — finite-difference their positions for the target velocity.
* **Do not touch:** pool node links `+0x00/+0x08`, state `+0x6B`, timestamp `+0x60`,
  param pointer `+0x28`, shooter `+0x66/+0x67`; the singleton list heads/counts
  (`S+0x20/0x90/0x98/0x108/0x290/0x300`); the event list `0x33EDB0` (incl. `+0x10`
  child links); the spawn registry `0x33EDD0` / `0x3409D4`; object/part *counts*
  (`world+0xA87E4`, `obj+0x118`) and `obj+0x120` / `part+0x1F0` flags.

### 8.3 ABI cheatsheet (for whichever hook you land on)

| function | arg convention | the node/object you get |
|---|---|---|
| `0x1B7B00` (call site `0x1B7D49`) | `rcx = &world->objArray`, `rdx = rdi = world` (`rdi := rcx` at `0x1B7B16`) | no node; **world** (then walk objects/parts) |
| `0x1B9A80` | `rcx = &world->objArray`, `rdx = world` | walks event nodes itself (`0x33EDB0`) |
| `0x13D180` (cat-0 spawn) | arg1 `rcx` = `*(part+0x178)` owner object; arg2/3 = pos3 + vec3 by pointer; arg4 = life; arg5/6 = shooter obj/part; arg9 = `*(node+0x28)` param ptr | returns `int` handle (≥0), node itself is internal (pop from `0x1B2C0`) |
| `0x1C67D0` (state-8 spawn) | `rcx` unused-ish, `rdx = node+0x20`, `r8 = u16 ptr (node+0x22)` | returns instance in `rax`, instance+0x30 = `0x1CB500` |

---

## 9. The one dynamic probe that finishes this (read-only, 1 run)

Everything above is static; the remaining unknown is *which* object/part is the player's
plain MSL, and which of its fields the game integrates. One run settles both, with **no
memory writes and no PAGE_GUARD** (banned, `HANDOFF.md` §6.3).

```text
hook:  detour the call at exe+0x1B7D49  (read-only thunk, §8.1)
each frame, from the thunk:
  world = rdx_in (save it before calling the original)
  1) pool live list:  S = exeBase+0x3219A0
       for (n = *(u64*)(S+0x98); n != S+0x98; n = *(u64*)n)
          log MSLNODE { n-S-pool, u8 state=*(u8*)(n+0x6B), i32 t=*(i32*)(n+0x60),
                        f32 pos[3]=+0x34.., dir[3]=+0x40.., life=+0x4C,
                        u8 shObj=+0x66, u8 shPart=+0x67, u64 param=+0x28 }
  2) world objects:  cnt = *(u8*)(world+0xA87E4);  arr = *(void***)(world+0x697B8)
       perObjBase = *(u64*)(world+0xA8248)
       for (i = 0; i < cnt; i++) { obj = arr[i]; parts = *(u64*)(obj+0x110);
          log OBJ { i, team=*(u8*)(obj+0x15D), flags=*(u32*)(obj+0x120),
                    nparts=*(u8*)(obj+0x118),
                    p0pos = parts+0x258 (4 floats), p0dir = parts+0x268 (3 floats),
                    velcand = *(f32*)(perObjBase + i*0x27A0 + 0x70..0x84) }
       }   // cap to ~64 objects, log once per frame for 10 frames around the launch
  3) diff: exactly one pool node appears at fire time (that is the launch record, state 2);
     exactly one object's p0pos starts at that node's pos and moves every frame
     -> that (i, p) is the plain MSL.  Its per-frame delta is the velocity in m/frame.
run: fire ONE plain MSL at a locked target (no special weapon), keep the target locked,
     then change lock mid-flight once  ->  tells you whether p0dir / the fire-state aim
     (+0x1650/+0x1664) follows the lock (target source) or not.
```

Decisions from the log:

* **object's `p0pos` advances by a constant delta** ⇒ velocity-based motion; PN writes the
  *same delta field* if you can find its owner (look one store up: the field of `perObj` or
  of `obj` that was added). If only `p0pos` changes and no velocity field does, the motion
  is matrix/keyframe-based ⇒ **hard override at `p0pos`** (§8.2) or patch the next keyframe
  child (`part+0x3F8/+0x400` nodes).
* **`p0dir` follows the lock but `p0pos` is a lerp** ⇒ the missile is the dispatcher's
  interpolated part; PN = rewrite the *next* keyframe node (`+0x20..+0x4C`, `+0x60`) or the
  interpolated output at `0x1DF50`'s commit.
* **no object tracks the node** ⇒ the missile is one of the `0x1C67D0` model instances;
  then hook `0x1CB500`/`0x183F10` and read `inst+0x4C`.

Record for each candidate: `RVA`, `frame`, `which field moved`, `sign of the delta`, plus the
`(+0x234,+0x235)` index pair so the same object can be found again next run.

---

## 10. Proven vs open (one screen)

**Proven this pass [V]**
* 5 direct refs to `0x1403219A0`; a second unguarded getter at `0x1B85B3`.
* Pool-node map from the **real** spawn `0x13D180` (`+0x34/+0x38/+0x3C` pos, `+0x40/+0x44/
  +0x48` dir, `+0x4C` life, `+0x60` = `S+0x00` timestamp, `+0x20` u16 category,
  `+0x28` param ptr, `+0x66/+0x67` shooter, `+0x6B` state), and that `MSL_GUIDANCE` §3.3's
  `0x1B08E` is a **different** (state 4) variant at an interior label with 0 callers.
* Live list L2 = `S+0x98`, tail-insert, **count `S+0x108`**.
* **No per-frame node-transform writer exists** (exhaustive AT&T-correct store scan).
* The per-frame system: `0x1B7B00` (tick, world-update phase 7 from `0xD1F05`) → `0x1B7D49` → `0x1B9A80` (dispatcher,
  merged 4 `.pdata` chunks, frame counter `0x33ED94`), state map 2/3/7/8, spawn registry
  `0x33EDD0` (80×0x18), effect-instance path `0x1C67D0→0x1CA4E0→0x1CB500→0x184980`
  (`inst+0x4C` transform), matrix decoder `0x1DE00` gated on `node+0x6B==0`.
* The real per-frame motion state is the **part transform**: `world+0x697B8` objects,
  `*(world+0xA8248)+i*0x27A0`, `obj+0x110` parts (0x520), **`part+0x258` pos**,
  `part+0x268` heading — snapshotted by `0x1B7B00` at `0x1B7D8A…`.
* Unique hook-site signatures for `0x1B7D49`, `0x1B9A80`, `0x13D180`.

**Open [U]** (probe in §9)
* Which object/part index is the player's plain MSL, and which field is integrated.
* Whether the plain MSL's motion is velocity-based, keyframe-interpolated (the dispatcher's
  part lerp) or a `0x1C67D0` model instance.
* The target source for the plain MSL (fire-state aim `part+0x1B8 → +0x1650/+0x1664`, or the
  target-track child nodes) — §8.2 lists both; §9 disambiguates with a lock-change.
* Who increments `S+0x00` (the node timestamp source) and its unit (ms?) — cosmetic.

---

## Appendix A — the 6 alloc sites and what they actually write

| alloc site | containing function | state written | notes |
|---|---|---|---|
| `0x1254A2` | own `.pdata` chunk | — | does not set `+0x6B` in chunk; event-side variant |
| `0x12B631` | fire routine | `+0x6B = 8` @ `0x12B8ED` | writes `+0x20/+0x60/+0x64/+0x66/+0x67/+0x68/+0x69`; inserts into L2 and links L1 |
| `0x13B09B` | own chunk | `+0x6B = 2` @ `0x13B158` | full block, cat-2 path (`0x13AD20`) |
| `0x13D180` (`0x13D665`) | `FUN_14013d180` | `+0x6B = 2` @ `0x13D7DD` | the canonical plain-MSL/effect spawn; also called from 4 fire routines (`0x1039C9`, `0x11EEBC`, `0x11F82C`, `0x121A3B`) |
| `0x1412B4` | own chunk | `+0x6B = 2` @ `0x1413F8` | cat-1 path (`0x141240`) |
| `0x1C7080` | own chunk | state @ `0x1C7138` | state-7 effect path, called from `0x1C63A0` |

## Appendix B — corrections to make in the existing docs

1. `MSL_GUIDANCE.md` §3.3 (`0x1B08E` "node initialiser") — mark as the **state-4 variant**
   of the record, at an interior label of `FUN_1401B083A`, **0 direct callers**; and state
   that `+0x20` is a **u16** (weapon category), `+0x30` a **u32**, `+0x28` a pointer, in the
   spawn-built record.
2. `MSL_GUIDANCE.md` §3.2 table — add **`S+0x108` = L2 count** (head `S+0x98` + 0x70).
3. `MSL_GUIDANCE.md` §3.4 (lifecycle) — states are written by the spawn variants
   (2/8 for projectiles/effects, 4/5/6/1/0 by sibling spawns) and **read** by the dispatcher;
   the trail scanner keeps `state == 0` for decoding (`0x1DE00` requires it), and `0x14F40`
   requires `node+0x6B == 0` and `node+0x54 & 8`, not state 6.
4. `MSL_TARGET.md` §5.1 item 1 — "only three functions write the block" → it is the
   **whole spawn/init/reset family** (11 functions, Appendix A + §5 table); the conclusion
   ("the advance is not a node writer") is unaffected and now proven exhaustively.
5. `MSL_TARGET.md` §9/§10 (trail ring) — unaffected; but note `0x1B6B00` is a *recorder*
   whose writes are into its own arrays (a naive field scan will misread it as a node writer).

---

# 11. Run-A probe contract (read-only; code against this, no re-derivation needed)

## 11.1 Two hook points, both byte-verified

**A. Entry of `FUN_1401b7b00` (RVA `0x1B7B00`)** — ⚠ **WITHDRAWN as a per-frame anchor, see
§11.5**: this entry is *state-gated* (phase byte `*(u8*)(arg2+0x3CC) == 7`) and Run A proved
it does not fire during ordinary flight. The bytes below are still correct; the *use* is not.
Hook `0xD1F05` (§11.8) instead. Historic text follows — use this if you want a *pre-frame*
snapshot. Exact first 25 bytes (from `work/rp/idx`, `idx.py list 0x1B7B00 0x18`):

```text
0x1B7B00  40 56                     push rsi                    (2)  cum  2
0x1B7B02  48 81 EC 80 00 00 00      sub  rsp,0x80              (7)  cum  9
0x1B7B09  8B 81 90 87 0A 00         mov  eax,[rcx+0xA8790]     (6)  cum 15
0x1B7B0F  33 F6                     xor  esi,esi               (2)  cum 17
0x1B7B11  48 89 7C 24 70            mov  [rsp+0x70],rdi        (5)  cum 22
0x1B7B16  48 8B F9                  mov  rdi,rcx               (3)  cum 25
```

* **`world` is in `rcx` at entry** [V] (`mov eax,[rcx+0xA8790]`, and `mov rdi,rcx` at
  `0x1B7B16` makes `rdi = world` for the rest of the body).
* Return value is `al` (bool). No stack arguments of interest.
* **Steal 15 bytes** (through `mov eax,[rcx+0xA8790]`, ends exactly at `0x1B7B0F`): every one
  of the three instructions is **position-independent** (no RIP-relative operand, no
  relative branch, no absolute), so the 14-byte absolute jump
  `FF 25 00 00 00 00 <abs64>` fits with one `90` pad, and the trampoline is those 15 bytes
  + `FF 25 00 00 00 00` → `0x1B7B0F`. (17-byte steal also works: + `33 F6`.)
* **Entry patch target verified unique** (`work/ma/sigchk.py 0x1B7B00 16` → 1 hit at
  `0x1B7B00`): `40 56 48 81 EC 80 00 00 00 8B 81 90 87 0A 00 33`.

**B. The dispatcher call `0x1B7D49` — recommended (post-update)** [V]

```text
1401b7d3f  48 8B D7                 mov  rdx,rdi              ; rdx = world
1401b7d42  48 8D 8F B8 97 06 00     lea  rcx,[rdi+0x697B8]    ; rcx = &world->objectArray
1401b7d49  E8 32 1D 00 00           call 0x1401B9A80          ; ★ patch this (5-byte, exact fit)
1401b7d4e  44 8B D6                 mov  r10d,esi             ; <- return point
```

* 5-byte `E8 rel32`; replace with `E9 rel32` — **no padding, no stolen bytes, no trampoline**
  (call the original `0x1401B9A80` directly from the thunk).
* Pattern (unique, `sigchk.py 0x1B7D49 24` → 1 hit):
  `E8 ?? ?? ?? ?? 44 8B D6 89 84 24 90 00 00 00 40 38 B7 E4 87 0A 00 0F 86`
* At the site: `rcx = world+0x697B8`, `rdx = rdi = world`; `rdi` survives the call
  (callee-saved), so the thunk may read it after the dispatcher returns.
* Ordering: the game's per-frame pose writes have **already happened** (dispatcher ran);
  the renderer and the per-object snapshot loop `0x1B7D58…` have **not**. This is the only
  "after the game, before the renderer" point in the frame.

## 11.2 World / object / part field offsets (all verified)

```text
world            : rdi in 0x1B7B00, rdx(arg2) in 0x1B9A80
  +0x697B8       : Obj**        object array          (dispatcher arg1 = address of this field)
  +0xA87E4       : u8           object count
  +0xA8248       : u64          pointer to per-object sim array, stride 0x27A0
  +0x240         : EFF_MIS_POSDIR ring object (pushed by 0x78200 from 0x1B7D2E)
  +0x67120, +0x67138, +0x67140, +0x67170, +0x697B8, +0x69A10, +0x69AE0..0x69AEC,
  +0x6A08C, +0x6A0B0, +0x6A130 : subsystem objects used by the tick

global exe+0x3409F2 : u8       ★ PLAYER object index   (used at 0x1B7D33/0x1B9AEE/0x1B7E26)
global exe+0x33ED94 : i32      frame counter (incremented by the dispatcher)
global exe+0x33EDB0 : node*    event-list head sentinel (next +0x00, children +0x10, time +0x60)
global exe+0x33EDC0 : i32      pose-change sequence counter
global exe+0x3409D4 : i32      launch-registry count (table at 0x33EDD0, 0x80 × 0x18)

Obj (object)     : obj = ((Obj**)world[0x697B8/8])[i]
  +0x110         : ptr          parts base
  +0x118         : i32          part count
  +0x120         : u32          flags   (bit 2 = lockable/active)
  +0x15D         : u8           team
  +0x178         : ptr          "aircraft/owner" argument used by the spawn

part (stride 0x520), part = *(u64*)(obj+0x110) + 0x520*p
  +0x1B8         : ptr          per-part fire/weapon state object
  +0x1F0         : u32          alive/flags (bit 1 seen; dispatcher tests +0x1F4 bit 0x80)
  +0x22E         : u16          type id      (0x13C / 0x13D / 0x646 seen compared)
  +0x234         : u8           ★ own OBJECT index
  +0x235         : u8           ★ own PART   index
  +0x258         : f32[4]       ★ POSITION (w = 1.0f)
  +0x268         : f32[4]       ★ HEADING / velocity (4th lane 0)
  +0x2D8, +0x360, +0x3B8 (next in list), +0x3C0 (int), +0x3D0, +0x3E0, +0x3E8,
  +0x3F0, +0x3F8, +0x400, +0x428, +0x44C : per-part state / node pointers

per-object sim record (0x27A0), perObj = *(u64*)(world+0xA8248) + i*0x27A0
  +0x70..+0x7C   : pos vec4 snapshot of part0  (written 0x1B7D8A.. by the tick)
  +0x80..+0x8C   : heading vec4 snapshot of part0
  +0x1D64, +0x1D6C, +0x1C30 family : written by the tick's pre-update loop (0x1B7BC0)
  +0x1630 = launcher/aim sub-object ; +0x1640..+0x1670 = launch solution
           (pos +0x1650/54/58, speed +0x1640, dir +0x1664/68/6C, mag +0x1670)
  launcher+0x9D4 flags, +0x9F0/+0x9F4/+0xA10 aim vector, +0xA14/+0xA20 spread
```

## 11.3 **Who writes `part+0x258` for a live projectile — answer** [V]

Exhaustive store scan (`work/ma/partpos.py`, saved `out_partpos.txt`, 103 functions store
`+0x258/+0x25C/+0x260` somewhere; the part-shaped writers are exactly these):

| writer | per-frame? | what it does | consequence |
|---|---|---|---|
| **`FUN_1401b9a80` 0x1B9A80** | **yes** | three write forms: threshold **snap** `part+0x258..0x260 = node+0x20/0x24/0x28` (`0x1B9F17..0x1B9F2F`, then `+0x3C0 = ++seq`), **decoded-matrix commit** `vmovups ymm [part+0x258]` (`0x1B9FBF`, from `0x1DE00` on an event node), and **pose commit incl. heading** `+0x258/0x25C/0x260/0x268/0x26C/0x270/(+0x274=0)` (`0x1BA14F..0x1BA1A4`, from the `0x1DF50` two-node lerp) | a node-driven part's pose is **re-created from the event nodes every frame** |
| `0x1006F0` | unknown | copies `+0x17F8/+0x17FC/+0x1800` (object/pose state) → `part+0x258..0x260`; also consumes pool nodes (`S+0x20/0x308/0x318`) | object→part pose propagation |
| `0x11A960` | no | hierarchical compose: parent part basis (`part+0x268` vec4 + a passed vec4 at `r15+0x268`) × local offsets → child `part+0x258..0x264`, then normalize `part+0x268` via `0x2036D0` | attach/parenting, not motion |
| `0x133F25`, `0x134848` | no | pose composition from matrices + per-component corrections (one `pos.y += x` gravity-style add at `0x1341FA`) | effect/animation |
| `0x1452E1`, `0x1456D0`, `0x145A52`, `0x14BCC0`, `0x157320`, `0x11AC00`, `0x113B30/40/70`, `0x1141CA` | — | other struct layouts that merely *contain* a `+0x258` (0x27A0-record, HUD, pools) | irrelevant |

**There is no `pos += vel` integrator on `part+0x258`** — no load-`0x258` → add → store-`0x258`
canonical pattern exists anywhere in the image (only single-component corrections in the
`0x133F25`/`0x1452E1` family, both non-projectile structs).

**`0x1B7B00` does not write `part+0x258` at all** [V]: it only reads part0's
`+0x258/+0x25C/+0x260/+0x264/+0x268/+0x26C/+0x270/+0x274` into `perObj+0x70..0x8C`, then
runs a **segment sweep** `call 0x78300` (`0x1B7F8C`) around the player's per-object record
(`perObj[*(u8*)(exe+0x3409F2)]`, built from `perObj+0x70` pos in `0x1B7F0B..0x1B7F8C`),
sets a flag bit at `world+0x6A08C` from the sweep result, and calls `0x1AA6E0(world+0x69A10)`.
It does **not** re-derive pos from heading, and it does **not** lerp from `fire+0x1650`.

**`0x1CB500` path writes no part at all** [V]: it only writes the effect/model instance's own
transform buffer `inst+0x4C` through `0x184980` / `0x183F10`.

⇒ **For a node-driven projectile part there is no game-side "integration from heading" to piggy-back on.**
A PN write at `0x1B7D4E` survives exactly one frame: on the next frame the dispatcher
re-creates `part+0x258/+0x268` from the event nodes (snap/decoded-matrix/lerp), so any
"soft steer" (heading only) is discarded. The two viable strategies are in §11.4.

## 11.4 `pn_tick(world)` design

```c
void pn_tick(void* world);   // called at 0x1B7D4E, after the dispatcher, rdi = world
```

**Per-missile state (mod-owned, do not attach to game memory):**

```c
struct PnSlot {                 // 64 entries, static array, no allocation
    uint32_t key;               // (objIdx<<8)|partIdx  from part+0x234/+0x235
    uint32_t frameSeen;         // exe+0x33ED94 when last updated
    float    prevPos[3];        // part+0x258 at the previous frame -> measured velocity
    float    vel[3];            // low-pass filtered measured velocity (m/frame)
    float    pnAcc[3];          // accumulated PN lateral command
    uint8_t  mode;              // 0 = soft (heading only), 1 = hard override
};
```
Key by `(obj+0x234, part+0x235)` (stable per part, no pointer lifetime issues); expire a slot
after ~10 silent frames. Frame number = `*(int32_t*)(exeBase+0x33ED94)`; this also gives you
`dt` in frames without any timing code.

**Finding the missile object (probe result feeds this):**
```c
uint8_t cnt = *(uint8_t*)(world+0xA87E4);
Obj**   arr = *(Obj***)(world+0x697B8);
for (i < cnt) { obj = arr[i]; parts = *(uint64_t*)(obj+0x110);
    for (p < *(int32_t*)(obj+0x118)) { part = parts + 0x520*p;
        /* candidate iff: part+0x1F0 alive, (obj+0x120 & 4), team != player team,
           and the probe told you this part tracks the launch record */ } }
```

**Fields to write (per frame, only after the dispatcher has run):**

```text
soft  (mode 0): part+0x268 = Hx, +0x26C = Hy, +0x270 = Hz     (unit heading, 4th lane at
                +0x274 left 0); do NOT touch +0x258.
hard  (mode 1): part+0x258 = Px, +0x25C = Py, +0x260 = Pz, +0x264 = 1.0f,
                part+0x268 = Vx, +0x26C = Vy, +0x270 = Vz (unit heading),
                +0x274 = 0.0f
                and mirror the cache: perObj+0x70..0x7C = pos, +0x80..0x8C = heading
                (perObj = *(u64*)(world+0xA8248) + i*0x27A0)
```
Because the game re-creates the pose next frame, `mode 1` must run **every frame** and its
`pos` must be the *previous frame's game value* advanced by `V_pn` (i.e. integrate from
`prevPos`, not from your own last write), otherwise drift accumulates against the dispatcher's
snap/lerp. Keep a per-frame monotonic "we are authoritative" flag: if the dispatcher's
`part+0x3C0` (pose-change sequence counter) changed for this part, the game re-snapped it —
re-seed `prevPos` from the game value instead of integrating.

**Target:**
```text
playerObj = arr[*(uint8_t*)(exeBase+0x3409F2)];
plPart    = *(uint64_t*)(playerObj+0x110) + 0x520*pp;
fire      = *(uint64_t*)(plPart+0x1B8);
aimPoint  = (float*)(fire+0x1650);        // per-frame lead/intercept point (dispatcher writes it)
```
* `aimPoint` = the intercept point the game itself computes this frame (§4.2a) — use it as
  `P_target`; its frame-to-frame delta (kept in a 2-deep ring, not in game memory) is
  `V_target`, so `Vc = |(P_t - P_m)| / t_go` needs no extra data.
* Robustness: guard `fire != 0`, `plPart != 0`, `*(u16*)(plPart+0x22E)` sanity, and
  `*(uint8_t*)(obj+0x15D) != *(uint8_t*)(playerObj+0x15D)` (team check) before using a part.
* Fallback if `fire+0x1650` is not the aim point in the probe: use the target-track children
  of the shooter part (`part+0x3D0/+0x3E8/+0x3F0/+0x3F8/+0x400`, nodes with `+0x60` time and
  the 12-float payload) and finite-difference them, exactly as the dispatcher does.

**Command law (same as the `WeaponEx` path, `docs/PN.md` §3.4):**
`λ = (R × V_rel) / |R|²`, `a = N · Vc · λ`, `V_pn = V_heading · |v| + clamp(a·dt)`, then
normalise and write. Clamp `|Δv|` per frame (< ~12 m/frame² ) and never exceed the missile's
own `|v|` envelope; keep the basis orthonormal.

**Do-not-touch list** (repeated because the probe will be writing next to live state):
pool node `+0x00/+0x08/+0x28/+0x60/+0x66/+0x67/+0x6B`; `S` list heads/counts
(`S+0x20/0x90/0x98/0x108/0x290/0x300`); event list `0x33EDB0` incl. `+0x10` child links;
launch registry `0x33EDD0/0x3409D4`; `world+0xA87E4` / `obj+0x118` counts;
`obj+0x120` and `part+0x1F0` flags; `part+0x1B8`/`+0x360`/`+0x390/+0x398` pointers.

---

## 11.5 CORRECTION (Run A result, 2026-10-26) — `0x1B7B00` is **state-gated**, not a per-frame tick

**What happened:** Run A installed the §11.1-A entry hook at `exe+0x1B7B00`; it never fired
(`g_world` stayed null for 3 min / 10 plain MSLs) while the pool alloc hook did fire.

**What the static evidence now says.** (1) Hypothesis (a) "the caller calls a different
entry" is **refuted**. (2) Hypothesis (b) "`0x1B7B00` is not reached in this state" is
**supported, with the exact gate**. §11.1's description of `0x1B7B00` as "the per-frame
weapon/effect tick" was **wrong** — it is a **phase handler of a state machine**.

```asm
; the call is real and points exactly here  [V: Ghidra bytes e8 f6 5b 0e 00]
1400d1f05  E8 F6 5B 0E 00        call 0x1401B7B00        ; containing procedure FUN_1400D17C0
```

```asm
; ── how the phase selector is formed, inside FUN_1400D17C0  [V]
1400d1a0b  movzx ebx, byte ptr [rdx + 0x3CC]   ; ★ bl = PHASE byte, from the 2nd argument object
1400d1a12  movzx eax, byte ptr [rdi + 0xA8822] ; ★ al = world/mission state byte
1400d1a19  (cont.)
1400d1a34  cmp  al, 0x4  ; je 0xD22EC ┐
1400d1a3c  cmp  al, 0x8  ; je 0xD22EC ├─ state 4/5/8 DIVERTS, never reaches the tick
1400d1a44  cmp  al, 0x5  ; je 0xD22EC ┘
...
1400d1eeb  cmp  bl, 0x7              ; only in phase 7 …
1400d1eee  jne  0xD2060
1400d1ef4  mov  rcx, rdi             ; rcx = world
1400d1f05  call 0x1401B7B00          ; … is the weapon/effect tick called
```

and the two functions that reach `FUN_1400D17C0` at all are themselves gated and **indirectly
called** (no direct callers in the image):

```asm
14016e8c0  cmp byte ptr [rcx + 0x18], 0      ; ← mission-active flag, else skip entirely
14016e8d4  je  0x16E97C
14016e8da  mov rcx, qword ptr [rcx + 0x8]    ; world = *(u64*)(missionControl + 8)
14016e8de  call 0x1400D17C0                  ; (second wrapper: 0x16F590 / call at 0x16F5AA)
```

**Caller census [V]** (Ghidra `procedure_callers` + local xrefs agree):

| target | callers |
|---|---|
| `0x1B7B00` | **exactly one**: `0x1400D17C0` (site `0xD1F05`) |
| `0x1B7B11`, `0x1B7B90`, `0x1B7B4B` | **none** — these are mid-function `.pdata` chunks of `0x1B7B00`, not entries |
| `0x1400D17C0` | `0x14016E8C0` (site `0x16E8DE`), `0x14016F590` (site `0x16F5AA`) — both indirect callers |

`0x1B7B90` **has no prologue and cannot be an entry**: its first instruction is
`mov qword ptr [rsp+0x68], r14`, spilling into the frame that `0x1B7B00` created with
`sub rsp,0x80`; entering there directly would overwrite the caller's frame/return slot.
Steal-and-jump into it from another site is likewise invalid.

⇒ **Do not use `0x1B7B00` as the per-frame anchor.** It only runs when the mission state
machine is in phase `*(u8*)(arg2+0x3CC) == 7` *and* `*(u8*)(world+0xA8822) ∉ {4,5,8}` *and*
`*(u8*)(missionControl+0x18) != 0`.

### 11.6 The one-measurement discriminator (no new hook, no new run)

`exe+0x33ED94` (the dispatcher's frame counter) has **exactly one writer in the whole image**:

```asm
1401bb14f  mov dword ptr [rip + 0x183C3F], eax    # 0x14033ED94   (inside FUN_1401B9A80)
```

(all other 20 references are reads). Therefore, from the mod thread while a plain MSL is in
flight, sample `*(u32*)(exeBase+0x33ED94)` twice ~1 s apart:

* **it advances** ⇒ the dispatcher ran ⇒ its only caller (`0x1B7B00`) ran ⇒ the phase gate
  *was* satisfied and the Run-A hook/install is at fault (read the patched bytes back at
  `exeBase+0x1B7B00`; a correct patch is `E9 <rel32> …`, and any handler must capture `rcx`
  **before** the trampoline body reuses it).
* **it does not advance** ⇒ the dispatcher never ran, so the plain MSL is being spawned
  **without** the dispatcher. The spawn then comes from one of the four direct callers of
  `FUN_14013D180` — `0x1039C9`, `0x11EEBC`, `0x11F82C`, `0x121A3B` (see Appendix A) — and the
  hook belongs there (or on `0x13D180` itself, sig unique), not on the phase handler.

### 11.7 World / mission-control global — verdict: all three anchors are the same object [V]

One function does both stores, back to back:

```asm
1400cd739  movzx eax, byte ptr [rbp + 0xA8825]
1400cd740  mov   byte ptr [rip + 0x2732AB], al    # 0x1403409F1 = *(u8*)(world+0xA8825)
1400cd746  mov   qword ptr [rip + 0x2732AB], rbp  # ★ 0x1403409F8 = world
1400cd74d  call  0x140019F30                      # pool singleton getter
1400cd755  mov   qword ptr [rax + 0x428], rbp     # ★ S+0x428 = world
```

* 23 readers of `0x1403409F8`, all in the weapon/projectile subsystem, all using it as the
  world: e.g. `0x1AC390 mov rax,[rip+…0x3409F8]; … mov r8,[rax+0x697B8]` (the object array).
* the tick receives the same object as `*(u64*)(missionControl + 8)` (`0x16E8DA`), and the
  spawn path dereferences the *same* `+0x697B8` field through `S+0x428`.

⇒ **`world == *(u64*)(exe+0x3409F8) == *(u64*)(S+0x428) == *(u64*)(missionControl+0x8)`**, one
object. Consequence for the probe: **world needs no hook at all** — read
`exeBase+0x3409F8` from the mod thread and validate with
`*(u64*)(world+0x697B8) != 0 && *(u8*)(world+0xA87E4) < 0x100`. This removes the
`g_world`-null failure mode from Run B entirely.

### 11.8 Replacement hook candidates (in order)

| rank | site | bytes / form | fires when | world in |
|---|---|---|---|---|
| 1 | **`0xD1F05`** | `E8 F6 5B 0E 00`, exact 5-byte → `E9 rel32`; pattern `E8 ?? ?? ?? ?? 84 C0 74 67 48 8B 87 18 83 0A 00 48 8B 50 10 4C 8B 82 D8` (**verified unique**, `sigchk.py 0xD1F05 24`) | same gate as `0x1B7B00` (strictly later, on the same path) | `rcx = &world->objArray`, `rdx = rdi = world` |
| 2 | **`0x1B9A80`** entry | `48 8B C4 55 41 57 48 8D A8 B8 FD FF FF 48 81 EC` (unique), 6-byte steal | whenever the dispatcher runs | `rdx = world`, `rcx = &world->objArray` |
| 3 | **hook-free**: poll `exe+0x33ED94` | — | works regardless of which state path runs; `Δcounter` gives the sim tick and `dt` in frames | read `exe+0x3409F8` |
| 4 | `0x13D180` entry / the four direct fire routines | see Appendix A | if §11.6 shows the counter frozen (dispatcher bypassed) | `S+0x428` / `0x3409F8` |

The §11.4 `pn_tick(world)` design is unchanged — only the *anchor* changes: call it from
candidate 1/2/3 and take `world` from `0x3409F8`.

---

# 12. Plain-MSL controller `FUN_14013ECA0` — verified slot semantics (Run B target)

The plain MSL is driven by `ctrl = world + 0x6A130` (16 slots, `ctrl+0x158 + i*0x58`), not by
the pool/dispatcher path (§11.5–11.6). This section nails the fields `pn_tick` must respect.
Verified against `FUN_14013ECA0` (0x13ECA0–0x13F4DE) and its pre-pass `FUN_14013DB20`.

## 12.1 Corrected slot map

| offset | meaning | proof |
|---|---|---|
| `+0x00` | position **vec4** | `vmovups [rbx-0x28]` (0x13ED5A); integrated at 0x13F21E |
| `+0x10` | quaternion (guidance state) | `lea rdx,[rbx-0x18]` → 0x202FA0 / 0x2036D0 (0x13EF90, 0x13F16F) |
| `+0x20..+0x2C` | per-frame displacement **vec4** — *output cache*, equals the delta added to `+0x00` in the same pass | 0x13F241/0x13F246/0x13F24B (xyz) + 0x13F24F (w) |
| `+0x30` | `P` = weapon parameter block | `mov rax,[rbx+8]` (0x13EF8C, 0x13F05A, 0x13F2F6) |
| `+0x38` | **traveled distance** (accumulates `|delta|`) | 0x13F311/0x13F316 |
| **`+0x3C`** | **SPEED (m/frame) — NOT a turn accumulator** | `+= 7.111111/(float)(u8)(ctrl+0x28)` (0x13F1C1–0x13F1CA), clamped to `4266.6667/(float)(u8)(ctrl+0x28)` (0x13F1DB/0x13F1E5), then used as `delta = −speed · axis` at 0x13F1FC–0x13F201 |
| `+0x40` | u16 state, `0xFFFF` = none | 0x13F2B4/0x13F2DF/0x13F401 |
| `+0x42` / `+0x43` | **target** (object, part) | 0x13ED51/0x13ED65 → `imul 0x520` + `[+0x258]` |
| `+0x44` / `+0x45` | **owner** (object, part) | 0x13EE2B/0x13EE33 → second part lookup |
| `+0x46` | written **once at launch** (`0x13D388`), never by guidance | — |
| `+0x47` bit0 | sticky *target-valid / guidance-applied* flag | cleared 0x13EDE1 (invalid target pair), set 0x13F174 (in-cone + valid) |

`ctrl+0x6E8` = live-slot bitmap (`test` bit i, `rol`), 16 iterations; `ctrl+0x6EC` = **60**
(constant, ctor `0x16D870`); `ctrl+0x28` = u8 config scale used for speed only.

## 12.2 Guidance math (what the game does, in order)

1. resolve **target part** from `(+0x42,+0x43)`, require `[part+0x1F8]&8`, read `part+0x258`
   → LOS vector = target − slot pos; normalize (guarded: `vucomiss` at 0x13F024 + `je` 0x13F03C
   ⇒ **no div-by-zero**).
2. `dot = forward · LOS`; `cos_cone = cosf( (u8)P+0x4A · π/180 )` — `FUN_140274530` **is `cosf()`**
   (cos Taylor series, 2/π quadrant reduction, sign by `(quadrant+1)&2`).
   * `jbe` at 0x13F08F ⇒ **dot ≤ cos_cone = out of cone** → 0x13F18D: skip rotation,
     skip `+0x47` set, skip `targetPart+0x1FC |= 0x4000000`; **nothing is cleared**, and the
     speed/travel/range/expiry bookkeeping still runs.
   * in-cone ⇒ `atan2`-style angle via `0x140273C70`, **clamped to ±max** where
     `max = (u8… actually float)` `lead_deg · π/180 / 60` (0x13EFA2–0x13EFBF), applied to the
     quat by `0x1402036D0` (0x13F16F); then `+0x47 |= 1`, `targetPart+0x1FC |= 0x4000000`.
3. `+0x38 += |delta|`, `+0x3C` speed ramp, `delta = −speed × quat axis`, `pos += delta`,
   store delta to `+0x20..+0x2C`.
4. expiry: `if (+0x38 > 5.12 · (float)P+0x14)` ⇒ **clear the slot's mask bit** (`and
   [ctrl+0x6E8],~bit` at 0x13F337) and run owner-based end bookkeeping. `P+0x14` is therefore
   the **range/lifetime** field (× 5.12), read every active-slot pass at 0x13F31B.

**`P` offsets read on this path:** `+0x14` (range, ×5.12), `+0x2C` (lead, degrees),
`+0x4A` (startAng / **cone amplitude**, degrees, used as `cos(halfAngle)`), and `+0x49`
(byte, read by the pre-pass `FUN_14013DB20` at 0x13E4B3).

**Consequences for the mod (design-relevant):**
* `startAng=90` ⇒ `cos(90°)=0` ⇒ in-cone ⇔ dot>0 ⇔ **half-angle 90°, i.e. a 180° full cone** —
  it is a *half*-angle, not a full cone.
* **`lead=0` is a clean takeover configuration** [V]: `max=0` ⇒ the clamp yields 0 ⇒ the quat is
  never rotated, while speed/travel/range/expiry and the `+0x47`/`+0x1FC` flags all still run.
  No division by zero (the divisor is the constant 60; the only variable divisor is
  `ctrl+0x28`, used for the speed ramp only). A PN that writes the quat then has **no fight**
  with the game's own guidance.
* If the quat is *not* written, position deltas are the only steering channel, and the game
  re-applies `−speed×(quat axis)` every frame (so a per-frame delta is the right mechanism).
* `+0x3C` is a *speed*, so it must not be treated as a scratch/turn field; it can also be read
  as `|v|` for PN (no finite difference needed for the **missile's own** speed).
