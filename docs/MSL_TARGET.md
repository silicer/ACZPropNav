# ACZPropNav — where the plain-MSL gets its target, and its per-frame advance

Status: **container/layout confirmed live by the Lead; this pass resolves the target
*read-path* for the guided-weapon family, corrects one assumption that would have broken
the MSL hook, and narrows the advance to a one-run write-watch.** Everything below is
instruction-level; tags: **[V]** VERIFIED, **[I]** INFERRED, **[U]** UNKNOWN (+probe).

| # | question | answer | tag |
| --- | --- | --- | --- |
| 1 | Reader of `+0x50..+0x53`? | **`exe+0x918C0` + body `exe+0x91944`** — the shared "resolve/validate/retarget" routine: `obj = O->array[+0x50]`, `part = obj->+0x110 + 0x520*+0x51`; 13 call sites, all `WeaponEx*` seekers | [V] |
| 2 | Where does it turn indices into a target position? | `part+0x258` (vec4) after team/FOV gates; no velocity is read (the *fuse/lead* code derives it by finite difference) | [V] |
| 3 | Is `+0x50..+0x53` on the **MSL pool node** really a target pair? | **No.** Those 4 bytes are copied at spawn from the shooter's weapon-fire state `[fire+0x1674..+0x1677]`, whose source fields are a **numeric scalar and bit flags**, not indices | [V] |
| 4 | The MSL per-frame advance | **not located**; proven negatives below; 1-run VEH write-watch recipe given (offset `+0x34` then `+0x40`) | [U] |
| 5 | `CIndicatorLockonInfo` shortcut (vt `0x2ADDD0`) | **Not viable** — the indicator holds no target pair/pointer; it reads the player global `exe+0x32B438` and its own argument | [V] negative / [I] player anchor |

---

## 1. Reproduction

```bash
# instruction index (present)
#   work/ga/full2.asm   (objdump -d -M intel --no-show-raw-insn)

python work/ga/rng.py  <absLo> <absHi> [substr…]
python work/ga/scan.py <substring>
python work/msl2/find2.py pair|fstores|freads …      # same-base offset group scans
python work/msl2/around.py 0x4c                       # store + arithmetic context
python work/msl2/poolwrite.py / poolread.py / adv.py  # pool∩field intersections
python work/msl2/sigs.py                              # signatures/uniqueness (below)
```

Scratch for this pass lives in `work/msl2/` (small scripts only).

---

## 2. Reader of `+0x50/+0x51` — `exe+0x918C0` [V]

**Signature (16 B, unique in `.text`):**
`48 89 4C 24 08 53 55 56 57 41 55 41 57 48 81 EC`

This is the *guided-weapon* target resolve/validate entry. It is **not** a leaf: the
search/FOV half is a separate `.pdata` chunk at `exe+0x91944`
(sig `4C 89 A4 24 60 01 00 00 4C 8B A4 24 70 01 00 00`), reached by a fall-through/jump
from the entry, so a hook must be placed on `0x918C0`.

### 2.1 ABI and the actual lookup [V]

```asm
1400918C0  48 89 4C 24 08            mov  [rsp+8],rcx          ; arg1 = owner (ctrl-like)
1400918D4  48 8B F2                 mov  rsi,rdx              ; arg2 = slot / node
1400918DA  48 8B 51 10              mov  rdx,[rcx+0x10]       ; ★ O = *(owner+0x10)  (object list)
1400918ED  0F B6 6E 50              movzx ebp, byte [rsi+0x50]; ★ target OBJECT index
1400918F1  4C 8B 1A                 mov  r11,[rdx]            ; ★ O->array   (Obj**, stride 8)
1400918F4  0F B6 46 51              movzx eax, byte [rsi+0x51]; ★ target PART index
1400918F8  48 6B F8 20 05 00 00     imul rdi,rax,0x520        ;    part stride = 0x520
1400918FF  49 8B 0C EB              mov  rcx,[r11+rbp*8]      ; ★ obj = array[objIdx]
140091914  48 03 B9 10 01 00 00     add  rdi,[rcx+0x110]      ; ★ part = obj->parts + partIdx*0x520
14009191B  44 0F B6 51 5D           movzx r10d,byte [rcx+0x15D] ; ★ obj team id
14009193A  41 38 6A 34              cmp  byte [rdx+0x34],bpl   ; ★ O->count bounds check
14009193E  0F 86 4A 04 00 00        jbe  0x140091D8E         ;    (out of range -> abort)
```

So the resolution is exactly the one already documented for the `WeaponEx*` slots in
`docs/GUIDANCE_ASM.md` §4.1 — same object model, same strides:

```text
O        = *(owner + 0x10)
count    = *(u8*)(O + 0x34)
obj      = *(Obj**)(*(void**)O + 8*idx)          ; idx = *(u8*)(slot + 0x50)
part     = obj->+0x110 + 0x520 * *(u8*)(slot + 0x51)
team     = *(u8*)(obj + 0x15D)
```

### 2.2 What the body does with it (`0x91944..0x91D8E`) [V]

```asm
1400919BD  mov rax,[rdx]                    ; object array
1400919C0  movsxd rcx,r13d                  ; loop index i
1400919CF  mov r9,[rax+8*rcx]               ; obj = array[i]
1400919D2  movzx r8d, byte [r9+0x15D]       ; obj team
1400919DA  cmp  r8b,r10b ; je next          ; same team -> skip
1400919E3  mov rax,[r11+rbp*8]              ; the currently locked object
1400919EE  lea  rax,[rcx+r8*8]              ; team_a*8 + team_b
1400919F2  cmp  byte [rax+r14],0            ; ★ hostility matrix @ exe+0x2AED00
1400919F7  je   next
140091A19  test byte [r9+0x120],0x4         ; ★ object active/lockable flag
140091A27  movsxd rcx,[r9+0x118]            ; part count
140091A2E  call 0x14022F39C                 ; alloc scratch byte[partCount] (visited map)
140091A52..0x91A71  fill visited[] = 0
140091A84  mov  rbx,[rcx+0x110]             ; parts base
140091AB2  movzx eax, byte [rsi+0x50]       ; current lock (for the "already locked" test)
140091AF3  ...
140091B0F  vmovups xmm0,[rbx+0x258]         ; ★ PART POSITION (vec4)
140091B85  vmovups xmm0,[rbx+0x258]
140091BB3  call 0x140270CB0                 ; sincos(part+0x26C)  → FOV cone test
140091BC9  call 0x140270CB0                 ; sincos(part+0x268)
140091BFA  movzx edx, byte [rsi+0x50]       ; compare candidate vs current
140091C16  movzx eax, byte [rsi+0x51]
140091C26  imul rcx,rax,0x520
140091C31  mov  rdi,[r8+0x110]
140091C67  mov  eax,[rbx+0x1F0]             ; part alive flag
140091CE0  add  r14,0x520                   ; next part
```

⇒ target **position = `part+0x258`**, part orientation = `+0x268`/`+0x26C`,
part alive = `+0x1F0 bit0`, object liveness/lockable = `+0x120 & 4`,
hostility = `exe+0x2AED00[teamA*8+teamB]`. **No target velocity field is read here** —
matching the `docs/GUIDANCE_ASM.md` §7 finding that lead is computed from
`0.5 * tGo * (P_t − P_t,prev)` with the previous target position cached in the slot.

### 2.3 Who calls it — and why that is the whole problem [V]

13 direct call sites, all inside the `WeaponEx*` region:
`0x94348, 0x95213, 0x953F7, 0x95F33, 0x96F1C, 0x97E4F, 0x984xx, 0x98xxx, 0x99xxx,
0x9Axxx, 0x9Dxxx …` (regenerate with `scan.py 'call 0x1400918c0'`).
The sibling helper `exe+0x90D00` (sig `48 89 5C 24 10 48 89 7C 24 18 55 48 8B EC 48 83`)
is the simpler variant used by the same seekers.

**Consequence:** this resolver is the *only* code in the image that turns a
`+0x50/+0x51` byte pair into an object/part — and no pool-node consumer calls it. That is
consistent with §3: the MSL node's `+0x50..+0x53` are not indices at all.

---

## 3. Correction: the pool node's `+0x50..+0x53` are **not** a target index pair [V]

The node initialiser `exe+0x1B08E` (documented in `docs/MSL_GUIDANCE.md` §3.3) copies
those four bytes straight out of the shooter's weapon-fire state:

```asm
14001B1EF  0F B6 80 74 16 00 00     movzx eax, byte [r8+0x1674] ; r8 = [shooterPart+0x1B8]
14001B1F7  41 88 46 50              mov   [r10+0x50], al
14001B1FB  0F B6 80 75 16 00 00     movzx eax, byte [r8+0x1675]
14001B203  41 88 46 51              mov   [r10+0x51], al
14001B207  0F B6 80 76 16 00 00     movzx eax, byte [r8+0x1676]
14001B20F  41 88 46 52              mov   [r10+0x52], al
14001B213  0F B6 80 77 16 00 00     movzx eax, byte [r8+0x1677]
14001B21B  41 88 46 53              mov   [r10+0x53], al
```

The four source bytes are **not** an (object, part) pair:

* `[fire+0x1674]` is used as a **number**, not an index — it is converted to float and
  used as a **divisor** in the dispersion formula:
  ```asm
  1400D1D10  movzx eax, byte [rbx+0x1674]
  1400D1D17  vcvtsi2ss xmm1,xmm1,eax        ; ← numeric value
  1400D1D22  vdivss   xmm2,xmm0,xmm1        ; xmm0 / (float)fire+0x1674
  ```
  (`rbx = [[[world+0x697B8][0]]+0x110][0] + 0x1B8`, i.e. the same fire object.)
* `[fire+0x1676]` / `[fire+0x1677]` are **bit flags**: they are only ever manipulated with
  bit ops (`and byte [x+0x1676],-0x21`, `or byte [x+0x1676],0x20`,
  `and byte [x+0x1677],-0x2`, `test byte [x+0x1676],0x40`, … at `0xD4486`, `0xD44B0`,
  `0xF56E3`, `0xF9C7A`, `0xFA1D7`, …).
* All four are re-initialised en-masse from a single shared byte by the weapon-state
  initialiser at `0xCC7C4..0xCC813`, which stores the *same* `al` into ~12 per-weapon
  blocks (`+0x1674`, `+0x1864`, `+0x1AEC`, `+0x1C28`, `+0x1F01`, `+0x1F19`, `+0x1F31`, …).

**⇒ Do not build the MSL PN on `+0x50/+0x51`.** They are 4 bytes of launch-time
weapon state (a scalar and flags). This also explains why the Lead's live samples read
zeros there — the shooter's fields were zero at that moment, and nothing in the guidance
path would care.

`docs/MSL_GUIDANCE.md` §3.3's row for `+0x50..+0x53` ("guidance flags / target bytes")
stands as flags only; treat any "index pair" reading of the **pool node** as disproven.

> **On the `ids=0100` the Lead saw on launcher-side records:** that is almost certainly the
> **shooter** pair, not a target pair. The node stores the *firing* aircraft's
> (object, part) indices at `+0x66/+0x67` (`01 00` → object 1, part 0), and the spawn
> descriptor `exe+0x1AC6D` writes those same two bytes from `[shooterPart+0x234/+0x235]`
> into its own `+0x66/+0x67`. Nothing in the node carries the *target's* indices.

---

## 4. So where does the MSL's target come from? — ranked candidates [I]

The node carries exactly one 8-byte slot that is filled from a caller-supplied value at
spawn, and one identification of the shooter:

```asm
; spawn 0x13D665: node+0x28 = qword from a caller stack argument
14013D792  mov rax,[rbp+0x180]        ; 7th+ argument of the spawn routine
14013D799  mov [r10+0x28], rax        ; ★ node+0x28 (8 bytes, could be a pointer)
; and the shooter identity (object/part indices of the firing aircraft)
14013D75B  movzx eax, byte [r9+rcx+0x234] ; shooter part → its object index
14013D764  mov byte [r10+0x66], al
14013D768  movzx eax, byte [r9+rcx+0x235]
14013D778  mov byte [r10+0x67], al
```

**(a) `node+0x28` is a pointer to the target/parameter block** [I]. This is the only
8-byte field written from a pointer-shaped argument; the EX family has the same idea
(`slot+0x40` = weapon-parameter block, `docs/GUIDANCE_ASM.md` §4). If it is the target,
the chain is `p = *(void**)(node+0x28)`, then either `p` is an `Obj*` (→ `p->+0x110`
parts → `+0x258` position) or a per-weapon param block. **Probe T1** below settles it in
one read.

**(b) The shooter's live state** [I]. `node+0x66/+0x67` identify the firing aircraft, so
the guidance *could* read the shooter's current lock each frame:
`obj = O->array[+0x66]; part = obj->+0x110 + 0x520*+0x67; fire = *(void**)(part+0x1B8)`.
The fire object is the same one the spawn read the transform, aim vector and flags from
(§3). If the missile follows the launcher's lock, this is where it lives. **Probe T2.**

**(c) The global player anchor** `exe+0x32B438` [I]. It is a `void*` written at
`0xCC4F8` (`mov rcx,[rsi+0x1B68]; mov [0x32B438],rcx`, rsi = mission control) and read at
**140 sites**; the lock-on HUD uses `[p+0x258]` as a position and `+0x238/0x23C/0x240/
+0x250/0x254` as heading/pitch values. It is the **player's unit/part**, *not* a target;
useful to classify a node as "player-fired" (compare `node+0x66` against the player
object index) and for PN sanity checks.

---

## 5. Per-frame advance — status and the one-run way to get it

### 5.1 What is proven (negative results that narrow the search) [V]

1. **Only three functions in the whole image write the node block** (`+0x34/+0x38/+0x3C`
   position, `+0x40/+0x44/+0x48` direction, `+0x4C` life) **and** touch the pool:
   `0x13B09B`, `0x13D665`, `0x1412B4` — and **all three are spawns** (each calls
   `0x1B2C0` to pop a free node, fills the block, sets state `2`, inserts into `L2`
   (`S+0x98`)). Signature of the most complete one:
   `0x13D665` = `49 8B 85 88 03 00 00 48 8B 08 0F B6 81 5D 01 00 00` (unique).
2. Therefore the advance **does not obtain the node from the singleton** — it receives the
   node pointer as an argument (from whatever walks `L1`/`L2`). That is why
   "pool-user ∩ field-writer" intersections come up empty.
3. **No `pos += dir` exists** for these offsets in any scalar or SIMD form: exhaustive
   scans over (a) same-base load/add/store triplets, (b) `vaddss`/`vaddps` with a memory
   operand at `+0x40`, (c) `vfmadd*` variants, (d) 16-byte `vmovups` forms at `+0x34`
   found only the `WeaponEx*` seekers and unrelated structs. The motion is therefore
   **matrix-based**: a transform is rebuilt and pushed through the engine's matrix API.
4. Best static candidate for that shape: **`exe+0xA5973`**
   (sig `48 89 9C 24 50 02 00 00 48 89 BC 24 48 02 00 00`, unique). It builds an
   orientation matrix from angles with `sincos` (`0x270CB0`) and matrix helpers
   (`0x5980`), then advances the **matrix translation** and commits it through a virtual:
   ```asm
000A5E55  vaddss xmm1,xmm9,dword ptr [rbp+0x40]   ; translation.x += vx
000A5E5A  vaddss xmm0,xmm7,dword ptr [rbp+0x44]   ; translation.y += vy
000A5E5F  vaddss xmm2,xmm6,dword ptr [rbp+0x48]   ; translation.z += vz
000A5E81  vmovss dword ptr [rbp+0x40],xmm1
000A5E86  vmovss dword ptr [rbp+0x44],xmm0
000A5E8B  vmovss dword ptr [rbp+0x48],xmm2
000A5E90  call   qword ptr [rax+0x120]            ; obj->vt[0x120](&matrix)
   ```
   `0xA5973` has **no direct callers** and is in no vtable, so it is reached indirectly —
   it may be the *object/actor* transform update rather than the node update. [I]

### 5.2 The recipe that ends this in one run (recommended)

The Lead already has a page-guard/VEH write-watch tool. Use it on a **live MSL node**
(obtained from the ring samples exactly as the Lead's readout already does):

```text
# pick the node the ring window points at, confirm *(u8*)(node+0x6B) == 6  (flying)
watch writes (4 bytes) at node+0x34      ; position.x  — the advance must store here every frame
    if it does not fire:  watch node+0x40 (direction.x), then node+0x4C (life)
the VEH handler's faulting RIP (+ module base = RVA) is the advance function
```
Then: copy its entry bytes, generate a signature with `??` on RIP-disp/rel32
(`work/msl2/sigs.py` shows the uniqueness loop), and hand it to the mod. A single frame of
watch is enough; there is no ambiguity because only one instruction per frame writes the
position that the Lead has already seen advance.

### 5.3 Hook shape once the advance RVA is known

* If the node arrives in a register (most likely `rdx` or `rcx` — the spawns take the node
  in `r9`/`r10`, the pool-pop returns it in `rax`), use the same thunk as
  `docs/GUIDANCE_ASM.md` §10 (`sub rsp,0x58` → `call tramp` → `call pn_apply(node)` →
  `add rsp,0x58`/`ret`).
* If it is a **matrix commit** (candidate `0xA5973` shape), the PN write is on the matrix
  translation (`m[0x30..0x38]`) and the basis rows — i.e. exactly the "hard override"
  variant in `docs/MSL_GUIDANCE.md` §5.3; keep the basis orthonormal and the 4th lanes at
  `1.0`.
* Never touch `+0x6B`, `+0x50..+0x53` or the list links (`+0x00/+0x08`).

---

## 6. `CIndicatorLockonInfo` (vt `0x2ADDD0`) — verdict: no target pointer [V]

| slot | RVA | what it does |
| --- | --- | --- |
| 7 | `0xDD300` (sig `4C 8B C1 48 85 D2 75 4D 48 8B 15 29 E1 24 00 8B`) | tiny copy-in: if `arg2 != null` copies `[arg2+0]` into `this+0x18/0xE8/0xEC/0xF0` and `[player+0x258]` into `+0xF4/+0xF8`; if `arg2 == null` copies `[player+0x238/0x23C/0x240/0x250/0x254]` |
| 8 | `0xDCDD0` (sig `4C 8B DC 55 53 41 54 49 8D 6B A1 48 81 EC E0 00`) | the draw path; consumes `this+0xE0/0xE4/0xFC/0x28/0x1C` plus the global at `exe+0x32B438` |

* The class **does not store an object pointer or an index pair for the locked target**;
  `arg2` is a scalar/vector blob (it reads `[arg2+0]` as a float/word), and the only
  object it ever dereferences is the **player** global `exe+0x32B438`.
* ⇒ the hoped-for shortcut ("read the player's lock target from the indicator") is **not
  available**. The player anchor itself is useful (see §4c), but it is the shooter, not
  the target.
* If a lock-target pointer does exist elsewhere, the natural candidate is inside the
  player unit's fire object `[[playerPart]+0x1B8]` (§4b) — probe T2.

---

## 7. Probe plan (two runs total)

### T1 — what is `node+0x28`? (zero-hook, one read while flying)

```text
node = <pool node of a live MSL, from the ring window>   ; *(u8*)(node+0x6B) == 6
q = *(u64*)(node+0x28)
log  q ; if q is a plausible user pointer:  log *(u8*)(q+0x15D)   (team)
        and *(float*)(q+0x258..0x260)                            (position?)
        and *(u32*)(q+0x110), *(u32*)(q+0x118)                   (parts base / count)
```
Decision: if `q+0x258` tracks a target and `q+0x15D` is a valid team ⇒ `node+0x28` **is**
the target object pointer (or the target part) → PN reads the target directly from the
node and no index resolution is needed. If `q` is null/garbage, go to T2.

### T2 — the shooter's fire state

```text
objIdx = *(u8*)(node+0x66) ; partIdx = *(u8*)(node+0x67)
obj  = O->array[objIdx]  where O = *(void**)(<any ctrl>+0x10)  ; or world+0x697B8 array
part = obj->+0x110 + 0x520*partIdx
fire = *(void**)(part+0x1B8)
# lock a target, then re-lock a different one while a missile is in flight:
watch read/write on fire+0x1674..0x16FF (dword scan) and log anything that changes
```
Any qword in `fire` that follows the lock changes is the target pointer; any byte pair
that follows it is the index pair.

### T3 (already needed) — the advance: §5.2 write-watch.

---

## 8. Bottom line for the mod

1. **`exe+0x918C0` is the index→object→part resolver** for the `WeaponEx*` guided family —
   it is the reference implementation of "target index → target position"
   (`part+0x258`), and it is *not* used by the pool-node MSL.
2. **The MSL node has no target index pair** (`+0x50..+0x53` = launch-time scalar/flags).
   Its target must come from `node+0x28` (T1) or the shooter's fire state (T2).
3. **The advance is still the open item**, but it is now a one-run experiment
   (`§5.2`), and the layout to write is already known from the live samples
   (`+0x34..+0x3C` position, `+0x40..+0x48` direction, `+0x4C` life, `+0x6B` state).
4. `CIndicatorLockonInfo` gives us **no** shortcut; but `exe+0x32B438` is a reliable
   **player anchor** for classifying player-fired missiles.

---

## 9. Route B executed: the trail ring is written through its vtable — hook it, don't scan [V]

The previous plan treated `EFF_MIS_POSDIR` as "a ring we can find by its vtable and
sample". That part works, but the *producer* cannot be reached by static
cross-references, and that is why the advance stayed hidden. The reason is now proven:

```text
ring class   .?AV?$EffRingBuffer@UEFF_MIS_POSDIR@@$0IAA@@@   vtable rva 0x2ACC68
ring object  missionControl + 0x240        (cm 0x16D9A8: lea rbx,[rcx+0x240])
             missionControl is built by the ctor at exe+0x16D980 (writes vtable
             0x2ADA38 = CMissionControlDump at mc+0x108, rings at mc+0x240 and
             mc+0x240+0x33770)
entry        mc+0x240+0x8 + idx*0x20, 2048 entries of {pos vec4, dir vec4}
writers      vtable slot 2 = exe+0x3B300   ("Add", index = ([rdx+8]+r9) % [rdx+4])
             vtable slot 3 = exe+0x3B370   ("Add", index = [rdx] + [rdx+0xC])
             both are leaf functions WITHOUT .pdata unwind entries, so they have no
             symbol, are "not a function start" to every pdata-based tool, and a
             disassembly-based xref of the vtable slots finds nothing: the only
             references are the data pointers at 0x2ACC78 / 0x2ACC80.
```

### What was tried and rejected (so nobody repeats it)

| attempt | result |
|---|---|
| `xref` of `0x3B300` / `0x3B370` (call/jmp/rip/absolute) | 0 code hits, only the vtable data pointers → **all calls are virtual** |
| scan for `call [reg+0x10]/[reg+0x18]` within 0x60 B of a `+0x240` displacement | 8 hits, all false positives: it matched `mov edx,[rbx+0x240]` (a state-machine index) |
| scan for `lea rcx,[reg+0x240]` (disp32 form) | 1 hit, `0x0C8ED5`, and it is the *wiring* code that stores `&mc+0x240` into `[mc+0x67138]+0x17a8` — not a push |
| scan for users of the stored ring pointer `+0x17a8` | 1 hit: the writer above. Consumers reach the ring through a different chain |
| Ghidra 12.1.4 via MCP (`open_binary` on the unpacked image) | import/auto-analysis never completed in-session: every provider call timed out. The Python toolkit (`tools/re/xref.py`, `vtable_diff.py`, `find_node_writers.py`) was used instead and answers in ~0.5 s per query |

So: the ring is a **C++ object with virtual writers**, and the only reliable way to
find its callers is to detour the writers themselves. That is implemented and shipped
behind one INI key.

### The survey build (`RingSurvey = 1`)

```text
[runtime]
RingSurvey       = 1     ; hooks exe+0x3B300 + exe+0x3B370, read-only
Probe            = 0
GuidanceOverride = 0     ; no PN hook this run
DryRun           = 0
LogLevel         = 2     ; the RING sample/caller lines go through L().Key()
```

> Type these into `ACZero\ACZPropNav.ini` by hand (keep UTF-8 with BOM). The
> `probe_ring.cmd`/`.ps1` pair that used to switch this on has since been removed.

* thunk (93 B, byte-identical to what the assembler emits for the same source —
  verified offline): `sub rsp,0x48` / spill rcx,rdx,r8,r9 / call handler(ring, idx,
  src, caller=[rsp+0x48]) / reload rcx,rdx,r8,r9 / `call trampoline` / `add rsp` / `ret`
* the original body therefore runs first and the trail is emitted exactly as vanilla
  (`hook::Install` gives `stolen bytes + jmp target+stolen`; `hooktest` §[6/6] proves
  both real prologues are relocatable: 14 B and 17 B, 5 instructions each)
* state shared with the detour is **fixed arrays + Interlocked** (no vector growth, no
  locks): a faulting guarded read must never leave a lock held, and the routine can run
  a few thousand times per second
* for each new return address it logs
  `RING: === new caller <addr> (exe+0x.....) [NOT WeaponEx -> plain MSL candidate] ===`,
  the `pos`/`dir` of the first six pushes of that caller, and once, a ±0x60 float window
  around the source pointer — if `src` points into the missile object, that window is the
  object layout (position / velocity / target fields)

### Why the return address is the answer

Every missile that has a smoke trail pushes `{pos, dir}` here each frame, plain MSL
included. Whatever function computes that pair is the per-frame advance we have been
looking for (§5). `WeaponEx*` updates are all inside `0x90000..0xA1FFF` (their calls to
the shared integrator `0x90510` are at `0x9165D 0x99B50 0x9AFB0 0x9D168 0x9E030
0x9E990 0x9F230`), so any other caller is the plain-MSL path by elimination — no
guesswork, one run.

Open items after that run:
1. disassemble the caller → identify the missile struct and the target source
   (the two candidates are `node+0x28` and the shooter's `fire+0x16xx`, §4);
2. re-point the velocity write there instead of the `WeaponEx` integrator;
3. only then re-enable `GuidanceOverride`.

---

## 10. Result of the ring survey run (2026-10-26 02:12–02:15) — route B is a dead end, and now we know why [V]

One run with `RingSurvey = 1`, plain MSL only. Both writers hooked 1 s after launch
(`stole 14 bytes` / `stole 17 bytes`), no crash, 6 distinct call sites:

| caller | containing function | size | what it pushes |
|---|---|---|---|
| `exe+0x54BC6` `0x54BDD` `0x54BF7` | `0x54892` | `0x3C5` | {pos, effect-params} |
| `exe+0x54CB3` `0x54D72` | `0x54C57` | `0x146` | same, via a different Add overload |
| `exe+0x336AE` | `0x335DC` | `0xDD` | position only (dir = 0) |

Disassembly of `0x54C57` (and the shared body at `0x54830`/`0x54892`) settles what these
functions are:

```text
r15 = trail-state struct (rdx):  +0x08 ring-world pointer (the object whose vtable slots
      0x18 / 0x20 / 0x28 are the three Add overloads)
      +0x18 int  ring base index      +0x20 int ring count     +0x24/+0x28 int ring cursors
      +0x2C float                     +0x30 int  (0x2F * this) selects a trail profile
      +0x34 word flags/age            +0x36 word               +0x60/+0x64 counters
r12  = pointer to a vec4 {x, y, z, 1} -- the missile's POSITION COPY, arg 1 of the call
entry[0..2] = *r12                       (position)
entry[3]    = float table at 0x32A3C0 [ (0x2F*[r15+0x30] + [r15+0x18] + 1) & 0x3FF ]
              * const 0x2D4BD0           <-- this is why the 4th float looked like 97 / 314 / 367:
                                             it is a trail PHASE, not a direction
entry[4..6] = dx, dy, dz from the per-effect parameter block returned by [vtable+0x28]
entry[7]    = that block's 4th float
```

**Conclusion (negative, and useful):** everything that writes `EFF_MIS_POSDIR` is an
*effect object* that merely copies the missile position it is handed as `rcx`. It carries
no velocity, no target, no steering — the guidance never touches this ring. The driver
above it is the effect manager `0x35C76` (size `0xD9C`, calls `0x54830` at `0x364E6`), so
following this chain leads further into the effect layer. Route B is therefore closed:
the trail ring can tell us *where* a missile is every frame, never *how it steers*.

(The survey stays in the tree as a working, crash-free probe: set `[runtime] RingSurvey = 1`
in `ACZero\ACZPropNav.ini` — the `probe_ring.cmd` one-click that used to do this has since
been removed — and it proved the 93-byte detour thunk in a real game process.)

### Ranked next steps for whoever continues

1. **Spawn capture, then per-frame sampling (recommended, needs 1–2 runs).**
   `exe+0x1B2C0` (pool alloc) is called from exactly 6 sites, each immediately after the
   pool getter `exe+0x19F30`: `0x1254A2 0x12B631 0x13B09B 0x13D665 0x1412B4 0x1C7080`.
   Detour it, log the returned node, then sample that node read-only for a few seconds
   (the `ProbeProjectilePool` machinery already does the "which floats advance" part).
   This gives the real missile object, its velocity offset and — by watching a locked
   target while the missile flies — the target source (`node+0x28` vs the shooter's
   `fire+0x16xx`, §4). Then apply PN exactly like the `WeaponEx` path does.
2. **Data route (does not need the advance at all, closest to what AC8 did).**
   `data/mis/mpl/mis.wpb` holds `lead = 40`, `startAng = 90`, `range = 3000`,
   `scope = 1600`, `speed = 1000`. Repack `a.arc` with a larger `lead`
   (`tools/arc/arc_tool.py` + the cracked keystream/LZ4 format in `docs/ARC.md`) and fly
   one mission: if the missile starts leading, this is the ACZ equivalent of
   `HomingForesightAmount` and the INI key can be wired to it directly.
3. **Effect-state probe (cheap add-on to 1).** Dump the whole `r15` trail-state struct
   (we only mapped `+0x08..+0x64`); if it holds a back-pointer to its missile, that is a
   second, independent way to reach the missile object.

### State left in the tree

* `RingSurvey` key, thunk + lock-free observer, `hooktest` [6/6]
  (real prologues relocatable: 14 B / 17 B) — all committed, 446 offline assertions green.
  *(The `probe_ring.cmd`/`.ps1` toggle scripts have since been removed; the key itself
  is unchanged and is now set by hand in the INI.)*
* `ACZero\ACZPropNav.ini` is currently left in **survey mode** (`RingSurvey = 1`,
  `GuidanceOverride = 0`), i.e. plain-MSL PN is OFF. Set `GuidanceOverride = 1`
  (and `Probe = 0`, `RingSurvey = 0`) in the INI before the next
  normal session to get the `WeaponEx` proportional navigation back — the old
  `enable_pn.cmd` one-click has since been removed.
* No memory was written to the game by the survey; `acz.exe` / `a.arc` untouched.
