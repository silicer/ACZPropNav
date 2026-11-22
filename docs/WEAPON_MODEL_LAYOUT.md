# WEAPON_MODEL_LAYOUT — where the `.wpb` weapon parameters live at runtime, and who reads them

Author: `re-model` (static RE pass, `task-2`).
Target: `ACZPropNav/work/acz_copy.exe.unpacked.exe` (SteamStub-unpacked image, 3,811,952 B,
image base `0x140000000`; `VA = 0x140000000 + RVA`). Every RVA below is for this image, which is
the same `.text` as the live process (`docs/RUNTIME_PARAMS.md` §validity statement).
Tools used: `work/rp/idx.py` + `work/ga/full2.asm` (`llvm-objdump -d -M intel`), Ghidra 12.1.4 via
REA MCP (pseudocode/callers), `work/rp/fieldscan.py`, `work/rm/*.py` (new scratch helpers),
`tools/arc/arc_tool.py`.

Confidence markers: **[V]** verified by reading the bytes/disassembly and cross-checked from a
second place; **[I]** inferred from [V] facts, not directly observed; **[U]** unknown.

---

## 0. TL;DR

| Question | Answer | Conf. |
| --- | --- | --- |
| Does the `.wpb` record stay flat inside a `WeaponModel`? | **No.** `WeaponModel` (`vftable 0x2AE790`) has a `0x48`-byte header and a *typed, descriptor-driven* payload at `+0x48` whose size is computed from the record descriptor (`0x102A30` → `0x162880`, `+0x48`), and whose builder (`0x162AC0`) lays out **four sub-groups, each ≥ `0x100` bytes**, i.e. the payload cannot be the 80-byte `.wpb` data block. | [V] |
| Where does the *gameplay* code read `scope/range/lead/timeStep/speed/startAng`? | From a **separate pointer held in the per-aircraft fire-control object**: `fire + 0x1FB8` (one of a 6-slot pointer array at `fire + 0x1F08`, stride `0x18`). Consumers read **constant displacements** from it. | [V] for the pointer and the displacements |
| What is that pointer? | A **record data block**: the code resolves it as `data = block + *(u32*)block`, i.e. `block + data_off`. Every flat `.wpb` file starts with `u32 data_off = 0x10`. Therefore `data = .wpb_record + 0x10` = start of the value area. | [V] for the `data_off` idiom; [I] that the block *is* the loaded `.wpb` |
| Predicted offsets from that pointer | `scope +0x10`, `range +0x14`, `hitRadius +0x18`, `timeStart +0x1C`, `timeWait +0x20`, **`timeStep +0x24`**, `timeContinue +0x28`, **`lead +0x2C`**, `dispersionAng +0x30`, `accFall +0x34`, **`speed +0x44` (u16)**, **`type +0x48`**, `attack +0x49`, **`startAng +0x4A`**, `pad +0x4C` | **[I] strong** (§4) |
| Are these offsets inside the `WeaponModel` object? | **Not proven.** Two cases: (A) the pointer is a separately-loaded flat record (§4 — preferred), so the `WeaponModel` typed payload offsets remain **[U]**; (B) the pointer happens to be `WeaponModel+0x48`, in which case the absolute offsets are `+0x58/0x5C/0x6C/0x74/0x8C/0x90/0x91/0x92`. One probe distinguishes them (§8). | [U]/[I] |
| Corrected claims from `RUNTIME_PARAMS.md` | (1) `0x1029C8` is **not** a size-class table, it selects a flag bit; there is **no** 22-entry per-type size table in that block (§2). (2) The per-member record is `{qword @+0, f32 @+0x08, flags @+0x0C, type @+0x0D, kind @+0x0E, kind2 @+0x0F}` — verified from the writer `0x102840`, the builder `0x0AC262` and the reader `0x0C5050`. (3) The fire/guidance path does **not** read a `WeaponModel` payload at `+0x48`; it reads the record-data pointer described above. | [V] |

**The single best next probe** (§8): at `FUN_14013D180` (the projectile spawn, `param_9 = fire+0x1FB8`)
dump `param_9-0x10 .. param_9+0x60` once in a mission: the 8-value signature in §8 decides (A) vs (B)
in one shot and then the four patch offsets are known.

---

## 1. Object model: `WeaponModel` = header `0x48` + typed payload (re-verified)

### 1.1 Header / size / payload boundary [V]

Re-verified with `peread.py`: `qword @ RVA 0x2AE790 = 0x140102550` (deleting dtor), and the dtor
frees `0x48` — exactly as in `RUNTIME_PARAMS.md` §1.2. The construction site is in the per-record
loader (`FUN_1400c7120`, phase-1 loop); the essential part of `0x0C7465`-family code:

```asm
; RVA 0x0C765D..0x0C76B8, one of 3-4 weapon-param objects built per aircraft record
0x0C765D  cmp   dword ptr [rdi], esi          ; rdi = record descriptor; *(u32)rdi == 0 -> skip
0x0C765F  je    0x0C76BD
0x0C7661  mov   rdx, rdi
0x0C7664  call  0x140102A30                   ; size = 0x48 + descriptor payload size
0x0C7678  call  0x140201460                   ; pool Allocate(0x32F918, size, 16)
0x0C7683  lea   rax, [rip + 0x1E7106]         ; &WeaponModel::vftable  == 0x1402AE790
0x0C768A  mov   qword ptr [rcx], rax          ; obj+0x00 = vftable
0x0C768D  mov   qword ptr [rbp + 0xF90], rcx ; store obj #0   (also 0xFA8 #1, 0xFC0 #2)
0x0C7694  call  0x1401627C0                   ; attach descriptor to fixed part
0x0C7699  lea   rdx, [rcx + 0x48]             ; payload cursor = obj + 0x48
0x0C76A4  call  0x140162AC0                   ; build payload at obj+0x48
0x0C76B0  call  0x140162880                   ; size visitor (again, for the next object)
```

### 1.2 The payload is *not* the flat `.wpb` data [V]

`FUN_140162AC0(obj, obj+0x48)` walks **four** descriptor groups; for each it stores a group cursor in
the header and advances by `0x100` **plus** a computed amount:

```asm
; RVA 0x162AC0 (Ghidra pseudocode, cleaned), plVar5 = &obj->group[0] .. &obj->group[3]
0x162AC0  *plVar5 = param_2;                      ; obj+0x30/0x38/0x40/0x48 = payload cursor
          FUN_14015dbc0(param_2, plVar5[-3]);     ; decode one group descriptor into the cursor
          FUN_14015dd20(*plVar5, param_2 + 0x100);; second half of the group at +0x100
          param_2 = align16(<size formula over  descriptor+0x10/0x28/0x30/0x38 >) + param_2 + 0x100;
```

`FUN_140162880` (the size visitor) does the same for 3 groups and reads its counts from
`descriptor+0x10 / +0x28 / +0x30 / +0x38`, returning `groupSize + 0x100` per group.
So a `WeaponModel` payload is at least `4 * 0x100` bytes for a non-empty descriptor and contains
structured sub-blocks — a 80-byte `.wpb` data block cannot be that.

> Consequence: the flat `.wpb` numbers are **not** at fixed offsets inside `WeaponModel+0x48`
> unless the runtime happens to keep a second, flat copy elsewhere. §3 shows it does: the
> fire-control object holds such a pointer.

---

## 2. The typed member record and the type tables — corrected decode

### 2.1 `FUN_140102840(member, type)` initialises one typed member [V]

Full disassembly (`work/ga/full2.asm` lines 230730-230858), summarised:

```asm
0x102840  movzx r9d, dl                 ; type code
0x102844  lea   r10, [rip - 0x10284B]   ; r10 = image base (0x140000000)
0x10284E  mov   byte ptr [rcx + 0x0D], dl        ; member.type      @ +0x0D
0x102851  lea   eax, [r9 - 1]
0x102855  cmp   eax, 0x14
0x102858  ja    0x102882
0x10285C  movzx eax, byte ptr [r10 + rax + 0x10295C]   ; T1 index table
0x102865  mov   edx, dword ptr [r10 + rax*4 + 0x102950]; JT1
0x102870  jmp   rdx
    0x102872: mov byte ptr [rcx + 0x0E], 4        ; member.kind  @ +0x0E in {1,2,4}
    0x102878: mov byte ptr [rcx + 0x0E], 2
    0x10287E: mov byte ptr [rcx + 0x0E], 1
0x102882  cmp   r9d, 0x15
0x10288B  movzx eax, byte ptr [r10 + rax + 0x102980]   ; T2[type]  (22 entries)
0x102894  mov   ecx, dword ptr [r10 + rax*4 + 0x102974]; JT2 -> +0x0F in {0,1,4}
0x1028C4  movzx eax, byte ptr [r10 + rax + 0x1029A4]   ; T3[type]  -> JT3 @0x102998
    0x1028DA: and byte ptr [r8 + 0x0C], -2          ; clear flags bit0
    0x1028E1: or  byte ptr [r8 + 0x0C],  1          ; set   flags bit0
0x1028E6  movzx eax, byte ptr [r10 + r9 + 0x1029C8]    ; T4[type]  -> JT4 @0x1029BC
    0x1028FC: and byte ptr [r8 + 0x0C], -3          ; clear flags bit1
    0x102903: or  byte ptr [r8 + 0x0C],  2          ; set   flags bit1
0x102908  add   r9d, -3
0x10290C  cmp   r9d, 0x12                              ; type >= 3 only
0x102915  mov   ecx, dword ptr [r10 + rax*4 + 0x1029E0]; JT5 -> default f32 at +0x08
    0x102922/0x10292B/0x102934/0x10293D/0x102946: mov dword ptr [r8 + 8], 3.8f/3.0f/2.0f/1.0f/4.0f
```

So a typed member is:

| off | type | meaning | conf |
| --- | --- | --- | --- |
| `+0x00` | qword | `WeaponModel*`-like value pointer / type key (written by the *builder*, not this fn) | [V] (see §3.3) |
| `+0x08` | f32 | value slot (this fn seeds it with a per-type default for `type>=3`) | [V] |
| `+0x0C` | u8 | flags: bit0 and bit1 selected by `T3`/`T4` | [V] |
| `+0x0D` | u8 | type code (`T`-tables indexed by it) | [V] |
| `+0x0E` | u8 | kind/width class ∈ {1,2,4} | [V] |
| `+0x0F` | u8 | kind2 ∈ {0,1,4} | [V] |

**Decoded tables** (reproducible: `work/rm/types.py`, `peread.py`):

* `JT1 @0x102950` = `{0x102872, 0x102878, 0x10287E}` → writes `4/2/1` into `+0x0E`.
* `T1 @0x10295C` (index `type-1`, 20-21 bytes) =
  `00 01 01 01 01 02 01 02 01 01 01 00 00 00 01 01 02 00 02 02`.
* `JT2 @0x102974` = `{0x1028A1, 0x1028A8, 0x1028AF}` → writes `0/1/4` into `+0x0F`; `T2 @0x102980` (22 B).
* `JT3 @0x102998` = `{0x1028DA, 0x1028E1}` → flag bit0 of `+0x0C`; `T3 @0x1029A4` (22 B).
* `JT4 @0x1029BC` = `{0x1028FC, 0x102903, 0x102908}` → flag bit1 of `+0x0C`; **`T4 @0x1029C8` (22 B)**.
* `JT5 @0x1029E0` (19 × dword) → default f32: `{2.0, 4.0, 4.0, 4.0, 4.0, 3.0, 3.0, 3.8, 3.0, 1.0, 3.8, 3.0, 3.0, 1.0, 4.0, 4.0, 2.0, 4.0, 3.0}` for `type = 3..21`.

**Correction to `RUNTIME_PARAMS.md` §3.1b / to the task brief:** the table at **`0x1029C8` is a
1-bit flag selector, not a size class**, and the 22-byte tables are *type→metadata* tables. A
per-type *slot size* is **not** in this block. The observed member granularities (`0x18`, `0x20`,
`0x38`, `0x58`) come from the descriptor serializers (`0x162880`, `0x162AC0`, `0x15dbc0`,
`0x0AC262`), i.e. the slot size depends on the descriptor **content** (array counts), not on a
22-entry constant table. `work/rp/fieldscan.py` + a manual sweep of the 0x1028xx/0x1029xx block
found no other candidate size table. **[V] negative** for "22-entry size-class table".

### 2.2 The typed record is really consumed by the fire-control code [V]

`FUN_1400c5050` reads the member metadata exactly at these offsets and passes it to a converter:

```asm
; RVA 0x0C5050
0x0C5054  mov   r9,  qword ptr [rcx + 0xFD8]      ; -> member record pointer (see §3.3)
0x0C5063  cmp   byte ptr [r9 + 0x0D], 0x15        ; member.type
0x0C5068  movzx r8d, byte ptr [r9 + 0x0E]         ; member.kind
0x0C5075  cmove r8d, ecx                          ; special case type 0x15 -> kind 1
0x0C508E  movzx r9d, byte ptr [r9 + 0x0F]         ; member.kind2
0x0C509E  call  0x1400B7790                       ; (kind, kind2, type, ...) converter
```

This is a second, independent confirmation of the `+0x0D/+0x0E/+0x0F` metadata layout.

---

## 3. How the `.wpb` values reach the fire/guidance code

### 3.1 The `data_off` idiom [V]

Every flat `.wpb` file starts with `u32 data_off = 0x10` (`mis.wpb` bytes: `10 00 00 00` then 12
zero bytes; verified hexdump, and `docs/VANILLA.md` §2). The loader family consistently resolves a
value block as **`block + *(u32*)block`**:

```asm
; RVA 0x0C78B4..0x0C78CE, FUN_1400c7120 phase-1, record entry #10 (dword at entry+0x2C)
0x0C78B4  call  0x140189360
0x0C78B9  mov   eax, dword ptr [rbx]       ; rbx = entry block; eax = data_off-like dword
0x0C78BB  test  eax, eax
0x0C78BD  jne   0x0C78C4
0x0C78BF  mov   rax, rsi                   ; 0
0x0C78C2  jmp   0x0C78C7
0x0C78C4  add   rax, rbx                   ; data = block + *(u32*)block   <-- data_off
0x0C78C7  mov   qword ptr [rbp + 0x1FB8], rax     ; <-- fire-control +0x1FB8 = data ptr
0x0C78D1  mov   qword ptr [rbp + 0x1F08], rax     ; +0x1F08 (slot array #0)
0x0C78D8  mov   qword ptr [rbp + 0x1F20], rax     ; +0x1F20 (slot #1, stride 0x18)
0x0C78DF  mov   qword ptr [rbp + 0x1F38], rax     ; +0x1F38
0x0C78E6  mov   qword ptr [rbp + 0x1F50], rax     ; +0x1F50
0x0C78ED  mov   qword ptr [rbp + 0x1F68], rax     ; +0x1F68
0x0C78F4  mov   qword ptr [rbp + 0x1F80], rax     ; +0x1F80
0x0C78FB  mov   dword ptr [rbp + 0x1F98], 0x3E4CCCCD  ; 0.2f
0x0C7905  mov   byte  ptr [rbp + 0x1FA0], 8
```

`rbp` here is the **per-aircraft fire-control object** (`0x27A0` bytes, allocated in
`FUN_1400c6a90` as `count * 0x27A0` at `singleton+0xA8248`; ctor `FUN_1400c46a0`, zero-inits
`+0x1FB0/+0x1FB8` at `0xC4E12/0xC4E19` and sets `+0x20A0 = 0x3C` at `0xC4F08`).
Identical pointer arithmetic (`block + *block`) is used for the neighbouring entries (#0xC -> a
0x90-byte flight block copied to `fire+0x1C50`, #0xA -> `+0x1FB0`), and for entry #0xB (at
`0x0C78A8..0x0C78C7`). Two independent records therefore confirm the idiom.

### 3.2 Offsets the fire/guidance code actually reads from that pointer [V]

`FUN_1401031F0` — per-aircraft weapon fire control, 0x3202 B, runs per shot/interval. It reads:

```asm
; RVA 0x1039CF..0x103A10 : cadence + speed
0x1039CF  mov   rdx, qword ptr [rbx + 0x1FB8]      ; param = fire->1FB8
0x1039DA  movzx eax, byte  ptr [rbx + 0x20A0]      ; divisor (default 0x3C = 60)
0x1039F0  vcvtsi2ss xmm0, xmm0, dword ptr [rdx + 0x44]   ; <-- param+0x44 read as i32
0x1039F5  vmulss xmm1, xmm0, dword ptr [rip + 0x1D0B93]  ; * 1.42222202 (RVA 0x2D4590, verified)
0x103A01  vcvtsi2ss xmm0, xmm0, eax
0x103A0C  vdivss xmm1, xmm1, xmm0
0x103A10  vaddss xmm10, xmm1, xmm14                 ; spawn speed = aircraft speed + param44*1.422222/60

; RVA 0x1039xx..0x103Axx : two reload/cooldown timers
   sVar = (short)( *(float*)(*[fire+0x1F08] + 0x24) * *(float*)(fire+0x1F14) * *(u8*)(fire+0x1F19) )
   sVar = (short)( *(float*)(*[fire+0x1F20] + 0x24) * *(float*)(fire+0x1F2C) * *(u8*)(fire+0x1F31) )
```

`FUN_14013D180` — projectile spawner (pool node alloc at `0x13D665`, one of the six known MSL pool
alloc sites). Its `param_9` is again `fire+0x1FB8`; it reads the same field to scale the spawn speed
(compiler did not fold the constant here):

```asm
; RVA 0x13D1xx, inside FUN_14013D180
   vcvtsi2ss xmm0, xmm0, dword ptr [param_9 + 0x44]
   vmulss    xmm1, xmm0, dword ptr [rip + ...]   ; 0.27777782  (= 1/3.6)
   vdivss    xmm1, xmm1, <byte [param_1+0x28]>
   vmulss    xmm1, xmm1, <5.12>
   vaddss    xmm1, xmm1, param_4                  ; param_4 = base/aircraft speed
```

`param_9` is then stored into the projectile pool node (`node+0x28 = param_9`,
`0x13Dxxx: mov [plVar14+0x28], param_9`), which confirms `MSL_TARGET.md` §7.1 T1: **the pool
node's `+0x28` is the weapon-parameter data pointer**, not a target pointer.

`FUN_140104A80`-family (special-weapon fire, call sites `0x104B6E`, `0x104CDD`) reads a **float at
`param+0x10`**, multiplied by a per-shot scale and fanned out to five hardpoints:

```asm
; RVA 0x104B67..0x104BA4
0x104B67  mov   rcx, qword ptr [r15 + 0xFD8]        ; typed member (meta +0xD/+0xE/+0xF)
0x104B6E  mov   rax, qword ptr [r15 + 0x1FB8]       ; param
0x104B85  vmulss xmm1, xmm14, dword ptr [rax + 0x10]; <-- param+0x10 read as f32 (scope)
0x104B8E  vmulss xmm6, xmm1, dword ptr [rcx + 0x8]  ; * member value
...
0x104CE3  vmulss xmm1, xmm14, dword ptr [rax + 0x10]; same in the second branch
```

### 3.3 Where the `0x27A0` fire-control object comes from [V]

* `FUN_1400c6a90(singleton, count, ..., byte* planeIds, ..., byte* weaponSets, ...)` — allocates
  `count*0x27A0` at `singleton+0xA8248` (`0xC6C82`), `count*0x70` at `+0x671C0` (`0xC6C8F`),
  `count*8` pointer arrays at `+0x67180/+0x67188`, then per record calls `FUN_1400c46a0(record)`
  (the `0x27A0` ctor) and stores a per-record byte at `record+0x279A`, `record+0x20A3`, and 3 more
  bytes at `+0x20A9..+0x20AB`.
* `FUN_1400c7120` (the multi-phase state machine, phase byte at `singleton+0xA8824`) then, for each
  record: resolves the record's sub-blocks (entry offsets `+0x8/+0xC/+0x1C/+0x20/+0x24/+0x28/
  +0x2C/+0x30/+0x34`), builds 3-4 `WeaponModel` objects into `record+0xF90/+0xFA8/+0xFC0`, copies a
  0x90-byte block to `record+0x1C50`, and sets the six `param-data` slots at `+0x1F08..`. Slot
  selection: `record+0xFD8 = record + 0xF90 + idx*0x18` where `idx = *(u8*)(record+0x279A)`
  (`0xC77A7..0xC77B5`, `0xC6F9C..0xC6FCA`).
* The record descriptors themselves are fetched at run time from the game's asset layer, not from
  a static `.rdata` table: `W+0xA88C0[i] = (*(vtable+0x70))(manager, i)` with the manager being the
  static BSS singleton `&DAT_1437e2560` (`FUN_140201b40`), ctor `FUN_1402018a0`
  (`_DAT_1437e2560 = &PTR_FUN_1402cff98`). Verified vtable dump: `slot[14] (+0x70) = 0x140206940`,
  which is `if (i<0 || i>=self->0x180) return 0; return *(void**)(self + 0x1B8 + i*0x18);`
  (`full2.asm` line 493868) — i.e. a **runtime registry of loaded record definitions**.
  `FUN_1400c6a90`/`FUN_1400c7120` then dereference each record via `data_off` (§3.1).

### 3.4 The `WeaponEx*Ctrl` chain: the same pointer is stored in the missile slot [V]

For the homing/special weapon family the fire-control object is not the `0x27A0` record but a
`WeaponEx*Ctrl` (`vftable` set: `Fire 0x2AF470`, `Qaam 0x2AF740`, `Saam 0x2AFAE8`, `Xlaa 0x2AF560`,
`Xmaa 0x2AF308`, `Xagm 0x2AF980`, `Lasm 0x2AFB60`, `Lagm 0x2AF5D8`, `Bomb 0x2AFC50`; slot map below
was dumped with `peread.py`). The dispatcher `FUN_140141240` forwards `param_9` (= `fire+0x1FB8`,
same pointer as in §3.1) as the 9th argument to **vtable slot 7 (`+0x38`), the per-class spawn**:

```asm
; RVA 0x141240 (Ghidra pseudocode, cleaned)
plVar10 = *(longlong**)(param_1 + 0x1DA30 + param_10*8);      ; per-class ctrl lookup
iVar8 = (**(code**)(*plVar10 + 0x38))(plVar10, &pos, &dir, ..., param_5, param_6, param_7,
                                       param_8, param_9);      ; <-- slot 7, param_9 forwarded
```

Spawning stores it in the live missile slot at **`slot+0x40`** (`slot = ctrl+0x80+i*0x68`):

```asm
; RVA 0x09F040 (WeaponExQaamCtrl slot 7 = spawn)
pauVar1 = ctrl + 0x80 + i*0x68;     ; pauVar1[j] = slot + j*0x10
*(undefined8 *)pauVar1[4] = param_9;         ; slot+0x40 = weapon-parameter data pointer
```

and the **guidance law (slot 8)** dereferences it. `FUN_14009F230` (Qaam slot 8) is where
`range` is proven:

```asm
; RVA 0x09F230, end of the per-slot loop (Ghidra pseudocode)
fVar15 = SQRT(vx*vx + vy*vy + vz*vz + w*w) + slot->traveled;   ; accumulated distance
slot->traveled = fVar15;
if (20.48 * *(float *)(*(longlong *)(slot + 0x40) + 0x14) < fVar15) {   ; <-- param+0x14
    ... mark target/slot dead (0x1000000 flag), FUN_1400934c0(...)        ; max flight distance
}
```

`20.48 × param[+0x14]` is the kill radius in engine units; with `mis.wpb range = 3000` this is
`61,440` engine units. This is the **first field whose read is directly observed**; it matches the
`.wpb` `range` at data-relative `+0x14` and is incompatible with the record-base hypothesis
(record `+0x14` = `altMax` = 0 would make the missile die at 0 distance).

The rest of the same function also confirms the slot layout of `HANDOFF.md` §5.2
(`slot+0x50/0x51` target index bytes, `slot+0x5C/0x60` mask/index, 0x68 stride, 16 slots).

### 3.5 Pointer-flow summary (which object holds what)

```text
0x27A0 fire-control record  (per aircraft; ctor 0x0C46A0; allocated by 0x0C6A90)
  +0x1F08 .. +0x1F80  ← 6 pointers, stride 0x18 = "current weapon parameter data" per slot
  +0x1FB8             ← current weapon parameter data pointer  P      (= block + *(u32*)block)
  +0x1FD8             ← address of the typed member slot (record+0xF90+idx*0x18)
  +0xF90/+0xFA8/+0xFC0 ← the 3 WeaponModel* (vtable 0x2AE790) built by FUN_1400c7120
  +0x1FC8             ← WeaponEx*Ctrl* (for the homing/special weapon path)

P  = record_data  (predicted: loaded .wpb + 0x10)
   ├─ 0x1401031F0  reads P+0x24 (cadence), P+0x44 (speed)
   ├─ 0x14013D180  reads P+0x44 (speed); stores P into pool node+0x28
   ├─ 0x140141240 → WeaponEx*Ctrl vtable slot 7 → stores P into slot+0x40
   └─ WeaponEx*Ctrl vtable slot 8 (guidance) reads P+0x14 (range)   [V]
```



### 3.6 Independent static check of the loaded data source [V negative]

`work/a` contains the unpacked `data/mis/mpl/*.wpb` (96 B each, verified). The `.arc` catalogue
(`work/meta/a.index.json`) lists `data/datapack_jp.bin` (raw 406,116 B, LZ4) — extracted to
`work/rm/dp/data/datapack_jp.bin` and inspected:

* It is a **name/asset registry only**: entries of stride `0x24 = {u32 0x10; char name[32]}` plus
  small `(u16 off,u16 id)` tables at the head (e.g. `(0x9,0x1B) (0x24,0x27) (0x4B,0x27) …`).
* It contains **no f32 at all** (searches for 1600/3000/40/90/1000/… → 0 hits) and **no parameter
  field names** (`scope`, `lead`, `timeStep`, `startAng`, … → not found).
  The datapack references `mis/mpl/mis.wpb` and `mis/mpl/gun2.wpb` 199× each (matches
  `docs/VANILLA.md` §3.1).

⇒ The values cannot be reconstructed statically from the exe or datapack; they are read from the
`.wpb` assets at load time. That is why the offsets below are *derived from the code that reads
them*, not from a static schema.

---

## 4. Predicted runtime offsets (**the deliverable**)

### 4.1 The mapping that fits every observed consumer

Let `P = *(void**)(fire + 0x1FB8)` (the value pointer described in §3.1; the pool node stores the
same pointer at `node+0x28`). Then, with `R = P - *(u32*)(P) = P - data_off` (the record base;
`mis.wpb` has `data_off = 0x10`):

| field | off from `P` (= .wpb file off − 0x10) | .wpb off | value in `mis.wpb` | who reads it | conf |
| --- | --- | --- | --- | --- | --- |
| `altMin` | `+0x00` | 0x10 | 0.0 | — | [I] |
| `altMax` | `+0x04` | 0x14 | 0.0 | — | [I] |
| `radius` | `+0x08` | 0x18 | 0.0 | — | [I] |
| `explPers` | `+0x0C` | 0x1C | 1.0 | — | [I] |
| **`scope`** | **`+0x10`** | 0x20 | **1600.0f** | `0x104B85`, `0x104CE3` (`vmulss xmm14,[rax+0x10]`) | [I] strong |
| **`range`** | **`+0x14`** | 0x24 | **3000.0f** | `0x09F230` (WeaponExQaamCtrl slot 8): `20.48 * *(float*)(param+0x14) < traveled → kill` — a max-range test | **[V]** read site / [I] identity |
| `hitRadius` | `+0x18` | 0x28 | 1.0 | [U] | [I] |
| `timeStart` | `+0x1C` | 0x2C | 0.0 | [U] | [I] |
| `timeWait` | `+0x20` | 0x30 | 4.0 | [U] | [I] |
| **`timeStep`** | **`+0x24`** | 0x34 | **4.0f** | `0x1039xx`/`0x1032xx` (`*[fire+0x1F08]+0x24`, `*[fire+0x1F20]+0x24`) | [I] strong |
| `timeContinue` | `+0x28` | 0x38 | 0.0 | [U] | [I] |
| **`lead`** | **`+0x2C`** | 0x3C | **40.0f** | [U] (no direct read found yet) | [I] |
| `dispersionAng` | `+0x30` | 0x40 | 0.0 | [U] | [I] |
| `accFall` | `+0x34` | 0x44 | 9.8f | — | [I] |
| `unknown0..2` | `+0x38/+0x3C/+0x40` | 0x48/0x4C/0x50 | 0 | — | [I] |
| **`speed`** | **`+0x44`** | 0x54 | **1000** (`u16` + `u16 0`) | `0x1039F0` (`vcvtsi2ss …,[rdx+0x44]`), `0x13D1xx` | [I] strong |
| `unknown3` | `+0x46` | 0x56 | 0 | — | [I] |
| **`type`** | **`+0x48`** | 0x58 | 2 | [U] (adjacent tail not yet found) | [I] |
| `attack` | `+0x49` | 0x59 | 40 | [U] | [I] |
| **`startAng`** | **`+0x4A`** | 0x5A | 90 | [U] (adjacent tail not yet found) | [I] |
| `pad` | `+0x4C` | 0x5C | 0 | — | [I] |

Why this mapping and not the file base: if `P` were the record base (`data_off` not applied), then
the two `int` reads at `P+0x44` would hit `accFall` = `0x411CCCCD` (f32 9.8 reinterpreted), which is
absurd as a speed scale — **whereas** `P = record+0x10` makes `P+0x44` exactly the `u16 speed`
(`1000 = 0x3E8`) read as i32. Similarly `P+0x24 = timeStep` (4.0 missiles / 0.1 guns) is the only
field that makes sense as a *firing cadence*, the way `FUN_1401031F0` uses it. Both consumers are
independent. This is why the mapping is rated strong [I] despite not being directly observed.

### 4.2 Absolute offsets, if the pointer is the `WeaponModel` payload

If `P == WeaponModel + 0x48` (case B), add `0x48`:

```
scope   WeaponModel + 0x58      timeStep WeaponModel + 0x6C      type      WeaponModel + 0x90
range   WeaponModel + 0x5C      lead     WeaponModel + 0x74      attack    WeaponModel + 0x91
hitR    WeaponModel + 0x60      speed    WeaponModel + 0x8C      startAng  WeaponModel + 0x92
```

**Case A (preferred)** — `P` is a flat record-data block loaded from the `.wpb` asset, and the
`WeaponModel` (vtable `0x2AE790`) is a *parallel typed view* built from the same record
(§1.2, §3.3). Then the `WeaponModel` typed-payload offsets stay **UNKNOWN [U]**; but the offsets
from `P` above are the ones the gameplay code uses, which is what a patch/hook needs.
Case B is not excluded by the data above but is in tension with the ≥`0x100`×4 payload layout.

State assumptions for the table: (a) the loaded `mis` record is byte-compatible with
`work/a/data/data/mis/mpl/mis.wpb` (all 199 references point at the same file, `VANILLA.md` §1);
(b) `data_off` is the first dword (verified on all 10 shipped `.wpb` records, `VANILLA.md` §2);
(c) the weapon actually used by the player is `mis` (`data_off=0x10`, so `P = R+0x10`).

---

## 5. Consumer map (ranked)

| # | RVA | size | role | reads from param | conf |
| --- | --- | --- | --- | --- | --- |
| 1 | **`0x1401031F0`** | 0xC81 | per-aircraft **weapon fire control / trigger** (called with a `param_2` flag; reads `fire+0x1FB8`, `fire+0x1F08/+0x1F20` slot ptrs, `fire+0x20A0..0x20A2`); computes projectile speed and reload cadence, then calls `0x14013D180`; writes the shot slot state (`+0x1F18`, `+0x1F30`), also a 0x70-byte "shot marker" via `0x1401C7080` and stores the slot's 4 floats from `fire+0x158+i*0x58` (`+0x1C0..0x1CC` of a `+0x1FD0` sub-object) | `+0x44` (i32), `+0x24` (f32 ×2) | [V] |
| 2 | **`0x14013D180`** | ~0x8xx | **MSL projectile spawner** into the procedural pool; `param_9` = `fire+0x1FB8`; writes the 0x70-byte pool node: `+0x34..+0x3C` pos, `+0x40..0x48` dir/speed, `+0x4C` life, `+0x66/0x67` shooter indices, `+0x6B=2`, **`+0x28 = param_9`**; spawns the 5-shot spread (0x11B/0x110 args in `0x1401C7080`) | `+0x44` (i32) | [V] |
| 3 | **`0x1400C7120`** | 0x25xx | **per-aircraft record load/build** (state machine phases 0..4): builds the `WeaponModel`s, copies the 0x90-byte flight block, sets `fire+0x1F08..+0x1FB8` param-data slots from `block + *(u32*)block`. This is the *load-time* place where the `.wpb` values first land; best static anchor for a **write hook** (runs once per mission/sortie, before any shot) | sets `+0x1FB8` etc. | [V] |
| 4 | **`0x1400C6A90`** | 0x1412 | record-set allocator/initialiser: `count*0x27A0` fire-control array (`singleton+0xA8248`), `count*0x70`, two `count*8` pointer arrays; calls ctor `0x1400C46A0`; seeds `+0x279A`, `+0x20A3`, `+0x20A9..AB` | — (allocator) | [V] |
| 5 | **`0x1400C5050`** | 0x8B | typed-member **converter** bridge: reads member meta `+0xD/+0xE/+0xF` and calls `0x1400B7790` (descriptor→engine value) | via member `[fire+0xFD8]` | [V] |
| 6 | **`0x140104A80`-family** (`0x104B67`, `0x104CDD`; also `0x104C45`) | ~0xDC0 | **special-weapon fire** path (checks `fire+0x20A3 == 0x22`, member type `0x14`; calls `0x1400A9510(ctrl+0x1D628, …)`); fans a per-shot scale × `param[+0x10]` into five hardpoints | `+0x10` (f32) | [V] |
| 7 | `0x1400C46A0` | ~0x9xx | the `0x27A0` **fire-control ctor**: zero-inits `+0x1FB0/+0x1FB8/+0x1FD0..`, sets `+0x20A0 = 0x3C`, `+0x2094/0x2098 = 0xFFFF00`, `+0x2728..+0x279A` defaults | — (ctor) | [V] |
| 8 | `0x1402018A0` / `0x140201B40` / `0x140206940` | 0x2xx.. | static **record-definition registry** (`&DAT_1437e2560`, vtable `0x2CFF98`, 15 slots); `slot+0x70` = `getRecord(i)`: `*(void**)(self+0x1B8+i*0x18)`, count at `self+0x180` | returns the record blocks | [V] |
| 9 | **`0x140141240`** | 0x36x | **spawn dispatcher** for the `WeaponEx*Ctrl` family: `ctrl = *(void**)(self+0x1DA30+param_10*8)`; calls `ctrl->vtable[7]` (`+0x38`) with `param_9` forwarded; also manages the 16-entry active-target ring (`+0x1D998/+0x1D9DC`) using `member.kind` (`*(*(…+0xFD8)+0xE)`) | forwards `param_9` | [V] |
| 10 | **`0x09F040`** (Qaam slot 7, spawn) | 0x2B0 | stores `param_9` at `slot+0x40` (`slot = ctrl+0x80+i*0x68`); writes slot pos/dir (`+0x00`, `+0x10`), target part pos (`slot+0x30..0x3C`), mask/index; per-class spawns: Fire `0x0A6CA0`, Qaam `0x09F040`, Saam `0x09E620`, Xlaa `0x09E590`, Xmaa `0x09DFB0`, Xagm `0x09D800`, Lasm `0x09CBC0`, Lagm `0x09BE80`, Bomb `0x0A7AC0` | stores `param_9` | [V] |
| 11 | **`0x09F230`** (Qaam slot 8, guidance) | 0x5C0 | per-frame guidance law: reads `*(float*)(*(u64*)(slot+0x40) + 0x14)` (= `range`) × `20.48` vs accumulated distance → kill; also `FUN_140090D00/140090950/1400907E0/140090A60/140090510` seeker sub-calls; per-class laws: Qaam `0x09F230`, Saam `0x09E990`, Xlaa `0x09E610`, Xmaa `0x09E580`, Xagm `0x09DA10`, Lasm `0x09D260`, Lagm `0x09C490`, Bomb `0x0A7C60`, Fire `0x0A7160` | **`+0x14` (f32, range)** | **[V] site** |

Slot map used above (dumped from the `.rdata` vtables with `peread.py`; `WeaponModel` itself has
exactly **one** real slot `0x102550`, the following qwords are the neighbour data — the
`RUNTIME_PARAMS.md` §1.1 claim is confirmed):

| class | vftable | slot 7 (+0x38) spawn | slot 8 (+0x40) guidance | slot 9 (+0x48) per-frame |
| --- | --- | --- | --- | --- |
| `WeaponExFireCtrl` | `0x2AF470` | `0x0A6CA0` | `0x0A7160` | `0x0A7440` |
| `WeaponExQaamCtrl` | `0x2AF740` | `0x09F040` | `0x09F230` | `0x09DBB0` |
| `WeaponExSaamCtrl` | `0x2AFAE8` | `0x09E620` | `0x09E990` | `0x09DBB0` |
| `WeaponExXlaaCtrl` | `0x2AF560` | `0x09E590` | `0x09E610` | `0x09DBB0` |
| `WeaponExXmaaCtrl` | `0x2AF308` | `0x09DFB0` | `0x09E580` | `0x09DBB0` |
| `WeaponExXagmCtrl` | `0x2AF980` | `0x09D800` | `0x09DA10` | `0x09DBB0` |
| `WeaponExLasmCtrl` | `0x2AFB60` | `0x09CBC0` | `0x09D260` | `0x09D550` |
| `WeaponExLagmCtrl` | `0x2AF5D8` | `0x09BE80` | `0x09C490` | `0x09C020` |
| `WeaponExBombCtrl` | `0x2AFC50` | `0x0A7AC0` | `0x0A7C60` | `0x0A7E80` |

Other callers of `0x14013D180` (same `param_9` convention) worth a look if the mod ever needs the
other families: `0x14011E180`, `0x14011F1A0` (site `0x11F04C`), `0x1401218C0` (site `0x121BFA`),
`0x1401B9A80` (site `0x1BAFDE`) — all appear to be per-weapon-class launch wrappers.

### 5.1 What the consumers prove and do not prove

* **[V]** `fire+0x1FB8` is a pointer to a flat data block with a `data_off`-style `u32` header, and
  the gameplay code reads **constant** displacements from it: `+0x10` (scope, special-weapon fire),
  `+0x14` (range, WeaponEx guidance range-kill), `+0x24` (cadence, fire control), `+0x44` (speed,
  fire control + MSL spawner). That is incompatible with "the fire code scans the typed member
  payload by cursor".
* **[I]** the block is the loaded `.wpb` record (because the observed displacements match the
  `.wpb` data layout under `data_off=0x10`, and the values are sane as speed/cadence/scope/range).
* **[U]** `lead +0x2C`, `startAng +0x4A`, `type +0x48`, `attack +0x49`, and `timeStart/timeWait`
  were **not yet observed being read**. No function in `.text` reads the `type/attack/startAng`
  byte-triple from a param block in the current disassembly (Ghidra resolved no `[reg+0x48..0x4A]`
  byte cluster tied to `fire+0x1FB8`). Their mapping comes from the `.wpb` layout consistency, not
  from a call site: treat as [I], not [V]. `lead` is the highest-value one to confirm (§8).

### 5.2 `fieldscan` cross-check — honest result

Scanning `.text` for functions that read many of `{0x10,0x14,0x24,0x2C,0x44,0x48,0x4A}` from one
base register (`work/rp/fieldscan.py`) is **not discriminating**: these displacements are generic
struct-field offsets used everywhere. With `--min 4 --disp-range 0x8,0x60` 7/7 hits appear in
`0x07CE95`, and 6/7 in ~20 unrelated functions (`0x2041B0`, `0x18F158`, `0x17EFBA`, `0x111D80`,
`0x10CC4F`, `0x107600`, …) — the list is dominated by false positives. It is recorded here as a
**negative result**: static offset-cluster scanning cannot pin the layout; the *pointer flow*
(§3.1) is the discriminating evidence. The one weak positive is that `0x07CE95` (a `0x9DB`-byte
function in the weapon/effect code region) does read the full 7-offset set including the `0x4A`
byte; it is worth a decompile only if the probe below fails.

---

## 6. Why this was previously reported as UNKNOWN, and what changed

`RUNTIME_PARAMS.md` §4.2 concluded the offsets were unknown because (i) `WeaponModel` has no
virtual accessors, (ii) all `WeaponModel` access goes through cursor-based descriptor code, and
(iii) the payload order comes with the data. All three remain true **for the `WeaponModel` typed
payload**. This pass found the missing link: the *gameplay* parameter consumers do not read the
`WeaponModel` payload at all — they read a **flat record-data pointer** (`fire+0x1FB8`) that the
loader derives with the `block + *(u32*)block` (`data_off`) idiom, and they read it with **constant
displacements**. That is a static, code-derived layout.

---

## 7. Residual unknowns

1. **[U]** Whether `fire+0x1FB8` is the loaded `.wpb` record, the `WeaponModel` payload, or a third
   derived block. Decided by probe §8 (one read).
2. **[U]** `WeaponModel` typed-payload order and slot sizes. Needs the runtime record descriptor
   (loaded from `.arc` at run time; not in the exe or the datapack — §3.4).
3. **[U]** Whether `lead` and `startAng` are consumed as f32/degree in this block or converted
   earlier. `lead` is read somewhere (guidance), but no read site was found statically; the
   `0.0174533` (deg→rad) refs in `work/weapon_const_xrefs.txt` remain the place to look.
4. **[U]** Whether multiple copies exist per aircraft (one per weapon set); `fire+0x1F08` is a
   6-slot array (stride `0x18`) and `+0x1FB8` is the "current" slot, so all six may alias the same
   record until a weapon switch rewrites them.

---

## 8. Best single next probe (decides A vs B in one read)

`probe-impl` already has the run; one breakpoint/hook is enough. The pointer is reachable from three
independent places, so use whichever is cheapest to hook:

```text
P from the fire control :  *(u64*)(fire + 0x1FB8)     ; fire = the 0x27A0 object (this/rbx of 0x1401031F0)
P from a live MSL node  :  *(u64*)(pool_node + 0x28)  ; any live MSL pool node (state byte node+0x6B == 2/6)
P from a WeaponEx slot  :  *(u64*)(ctrl + 0x80 + i*0x68 + 0x40)   ; set by 0x09F040 (WeaponExQaamCtrl.spawn)
dump: 64 bytes at (P - 0x10)  and  at P
```

Accept/reject signature (these are the `mis` values, little-endian):

```text
u32  at P-0x10 + 0x00 == 0x00000010           ; data_off  (case A only)
f32  at P-0x10 + 0x1C == 1.0                  ; explPers  (case A; confirms the block is .wpb)
f32  at P + 0x10      == 1600.0   (0x44C80000) ; scope
f32  at P + 0x14      == 3000.0   (0x453B8000) ; range
f32  at P + 0x2C      == 40.0     (0x42200000) ; lead
f32  at P + 0x24      == 4.0      (0x40800000) ; timeStep
u32  at P + 0x44      == 1000     (0x000003E8) ; speed (u16 1000 + u16 0)  [int form]
u8   at P + 0x48      == 2                     ; type
u8   at P + 0x4A      == 90                    ; startAng
Also compare P with each WeaponModel pointer found at fire+0xF90/fire+0xFA8/fire+0xFC0
(WeaponModel vtable test: *(u64*)W == exeBase + 0x2AE790). If P == W + 0x48 for one of them,
case B; otherwise case A.
```

Outcome → action:

* All of the above true ⇒ layout [V]: patch/hook `P+0x10` (scope), `P+0x14` (range),
  `P+0x2C` (lead), `P+0x4A` (startAng), and `P+0x44` (speed) for a live missile rework.
  **Note**: `P` is in a data block loaded from the `.arc`, i.e. writable heap, but it is shared by
  *all* 199 aircraft records that reference `mis.wpb` — expect a global effect.
* `P-0x10` fails the `data_off`/`explPers` check but the `P+0x10/+0x24/+0x44` values still match ⇒
  case B (`P = WeaponModel+0x48`); then the same four patch offsets are valid but per-object, and
  each WeaponModel must be located via vtable `0x2AE790` (scan) or via `fire+0xF90`.
* Neither matches ⇒ the block is re-encoded; then the only remaining route is to hook the *writer*
  at load time (`FUN_1400c7120` around `0x0C78C7`, one store per record) and compare source/dest.

---

## 8b. Follow-up: per-frame `lead`, writability/sharing, load hook

### 8b.1 Read-site table for the flat record P (`P = *(void**)(fire+0x1FB8)`)

Method: `work/rm/trace_p.py` — finds every `mov reg, qword ptr [base+<holder>]` in
`work/ga/full2.asm`, then reports the constant displacements subsequently read from that register,
per containing `.pdata` function. Holders traced: `fire+0x1FB8`,
`fire+0x1F08/0x1F20/0x1F38/0x1F50/0x1F68/0x1F80` (6-slot array, stride 0x18), WeaponEx slot `+0x40`,
MSL pool node `+0x28`. Ghidra (REA, same open image) was used to decompile `0x14009F040`,
`0x140141240`, `0x140206980`, `0x14013D180`, `0x1401031F0`, `0x140162AC0`, `0x140162880`; register-flow
triage and all instruction excerpts are from `full2.asm` (no Ghidra fallback needed).

| P off | field | read site (RVA) | containing function (role) | when | conf |
| --- | --- | --- | --- | --- | --- |
| `+0x10` | `scope` | `0x102E9C` | `FUN_0x102AF0` "weapon power": `(float)byte * P[+0x10]` → 5 hardpoint scale fields | fire/aim-time | [V] |
| `+0x10` | `scope` | `0x104B85`, `0x104CE3` | `FUN_0x104644` special-weapon fire (×5 hardpoints, §3.2) | fire-time | [V] |
| `+0x14` | `range` | `0x9B72B` | `FUN_0x9AFB0` = **`WeaponExSodCtrl` vtable slot 8** (`0x2AF290+0x40`): `if (traveled > P[+0x14]×[0x2D4800]) kill` | **per-frame** | [V] |
| `+0x14` | `range` | in `FUN_0x9F230` (`WeaponExQaamCtrl` slot 8) | same range-kill (`20.48f × P[+0x14]`, §3.4) | per-frame | [V] |
| `+0x24` | `timeStep` | `0x103267`,`0x1032BF`,`0x108384`,`0x1083DC`,`0x10B3EC`,`0x10B42D`,`0x1084A6`,`0x10850F`,`0x108575`,`0x1085DB`,`0x10B4F8`,`0x10B531`,`0x10B576`,`0x10B5C1` | `0x27A0` fire control + `0x107600`/`0x10B040` family: reload/salvo cadence `(short)(P[+0x24]*k*byte)` | fire-time (per trigger) | [V] |
| `+0x2C` | **`lead`** | **`0x9B3F8`** | **`FUN_0x9AFB0` = `WeaponExSodCtrl` slot 8 guidance law**: `mov rcx,[rdi+0x40]` (P); `vmovss xmm3,[rcx+0x2C]` → arg of seeker helper `0x140090A60` | **per-frame** (same fn calls the shared velocity integrator `0x90510` at `0x9B652`) | **[V] site, per-frame [I strong]** |
| `+0x44` | `speed` | `0x1039F0` | `0x1031F0` family fire control: `vcvtsi2ss xmm0,[rdx+0x44]` → spawn-speed term | fire-time (per shot) | [V] |
| `+0x44` | `speed` | `0x13D1xx` | `FUN_0x13D180` MSL spawn (param_9 = P): `(int)P[+0x44] × 0.27778 / dt × 5.12 + speed` | spawn-time | [V] |
| `+0x48` | `type` | — none found | — | — | [U] |
| `+0x49` | `attack` | **`0xA1E8C`** | `FUN_0xA1B88`: `mov rax,[r13+0x40]` (P); `movzx r9d, byte ptr [rax+0x49]`; `call 0x140118080` (damage/impact handler) | impact-time | [V] |
| `+0x4A` | **`startAng`** | **`0x9B3F4`** | `FUN_0x9AFB0` (`WeaponExSodCtrl` slot 8): `movzx eax,[rcx+0x4A]`; `vcvtsi2ss`; 5th arg of `0x140090A60` | **per-frame** | **[V] site, per-frame [I strong]** |

Also:

* All six `fire+0x1F08..+0x1F80` slot pointers are read **only** for `P+0x24` — the six weapon sets
  differ only in cadence.
* `P+0x2C` (`lead`) has **exactly one** read site in the whole image (`0x9B3F8`). It is *not* read
  by the MSL spawner `0x14013D180`, and *not* by the Qaam/Saam guidance laws (they pass constants to
  `0x140090A60`). In this build `lead` is consumed only by the `WeaponExSodCtrl` guidance law, per
  frame. (`WeaponExSodCtrl`: vtable `0x2AF290`, slots 7/8/9 = `0x9ADC0/0x9AFB0/0x9B8C0`.)
* `startAng` shares that single read site — the seeker-cone angle is applied per frame too.
* `attack` (`P+0x49`) is read once per impact in the damage handler, as a byte.

### 8b.2 Is P writable / shared?

* **Writable [I]:** P lies inside a named-asset buffer. The asset loader is
  `FUN_140206980(self, name)`: it normalises `\\`→`/`, allocates with the global pool
  `FUN_140201460(pool@0x32F918, size, 0x40)` and reads the whole file in
  (`self+0x148 = buf`, `self+0x150 = buf+8`, `self+0x158 = *(int*)buf`). Pool allocations are normal
  heap ⇒ `PAGE_READWRITE`; the game code never writes P, so this stays [I] until a `VirtualQuery`
  confirms it.
* **Size:** the flat block is the `.wpb` file image = **96 bytes** (`u32 data_off = 0x10` + 80 data
  bytes); writes to `P+0x00..P+0x4F` stay in-block, never write past `P+0x4C`.
* **Sharing = most likely SHARED (one instance per distinct `.wpb` file) [I]:**
  - the asset layer is **name-keyed** (`FUN_140206980` takes the name string and keeps one buffer per
    object); `datapack_jp.bin` lists `mis/mpl/mis.wpb` 199× but that is a *reference list*, not 199
    buffers;
  - the only writer of `fire+0x1FB8` is `0x0C78C7` inside `FUN_1400C7120` (the other apparent writer,
    `0x164978`, belongs to a different class whose `+0x1FB8` is a vtable slot), and it stores
    `block + *(u32*)block` — a pointer *into* the loaded record, not a copy;
  - ⇒ if the cache is name-keyed (standard), all 199 aircraft share the same `mis` record block, and
    one write to `P+0x2C` affects every missile of that model.
  - **Not statically provable** (no code shows the cache insert). Confirm in the probe: log
    `fire+0x1FB8` for several aircraft (player, wingman, a different aircraft type). All-equal ⇒
    shared; distinct ⇒ per-aircraft copies.

### 8b.3 Load-time hook anchor (rewrite lead/scope/range/startAng from the INI)

```text
call chain (verified):
  FUN_14016EB60 / FUN_14016F700   mission setup; call 0x1400C6A90 at 0x16EC6C / 0x16F80D
    -> FUN_1400C6A90   allocates per-record state: count*0x27A0 @ singleton+0xA8248,
                       count*0x70 @ +0x671C0, pointer arrays @ +0x67180/+0x67188
    -> per phase, wrapper 0x14016ECA0:  mov rcx,[rcx+8]; call 0x1400C7120
         FUN_1400C7120 = phase machine, phase byte at singleton+0xA8824 (0..4)
         phase 1 = the only writer of fire+0x1FB8, single instruction:
             0x0C78C7:  mov qword ptr [rbp + 0x1FB8], rax    ; rax = P, rbp = 0x27A0 record
```

Recommended hook (one code patch, fires once per record, idempotent if P is shared):

* Trampoline on **`0x1400C78C7`** (inside the `0x0C7465` fragment of `FUN_1400C7120`). At the hook
  `rax = P`, `rbp = record`. Write `*(float*)(P+0x10)` (scope), `*(float*)(P+0x14)` (range),
  `*(float*)(P+0x2C)` (lead), `*(u8*)(P+0x4A)` (startAng); optionally `P+0x24` (timeStep) and
  `P+0x44` (speed). This is the **earliest safe point** — before any shot.
* Alternative: hook the phase driver **`0x14016ECA0`** and apply the same writes when
  `*(u8*)(singleton+0xA8824) == 1` (phase-1 records just built), or hook the return of
  `FUN_1400C7120` and walk `singleton+0xA8248 + i*0x27A0`, `i < *(u8*)(singleton+0xA87E4)`, for
  `+0x1FB8 != 0`.
* Ordering is guaranteed by the code: `0x1400C6A90` (allocation) runs before `0x1400C7120` phase 1,
  and `0x1400C7120` is entered only via `0x14016ECA0` (single caller, verified).
* Guard before writing: `*(u32*)(P-0x10) == 0x10` (data_off); `P-0x10` is the record base.


---

## 9. Reproduction

```sh
PY="python"

$PY work/rm/type_tables.py           # decode the 0x102950.. type tables (renamed from types.py: shadowed stdlib)
$PY work/rm/sanity.py                # RVA mapping + vtable/constant checks
$PY work/rm/dp1.py                   # datapack name registry check
$PY work/rm/trace_p.py 0x1fb8        # who loads fire+0x1FB8 and which [P+disp] they read
$PY work/rm/trace_p.py 0x40          # same for WeaponEx slot+0x40
$PY work/rm/trace_p.py 0x28          # same for the MSL pool node
$PY work/rm/vtdump.py                # all WeaponEx* vtable slot maps (7/8/9)
$PY work/rm/callers.py 0x140090a60   # seeker-helper call sites (lead/startAng users)
$PY work/rm/callers.py 0x140090510   # shared per-frame integrator users
$PY work/rm/peread.py rva 0x102900 0x130
$PY work/rp/idx.py func work/rp/idx 0x102840        # the type initialiser
$PY work/rp/idx.py func work/rp/idx 0x162AC0        # payload builder
$PY work/rp/idx.py refs work/rp/idx 0xC7465,0x102A30,0x102840,0x162880
grep -n "0x2d4590\|0x1fb8\]" work/ga/full2.asm
```

Ghidra MCP (session target = the same unpacked image): `procedure_pseudo_code` on
`0x1400C7120`, `0x1400C6A90`, `0x1401031F0`, `0x14013D180`, `0x140162AC0`, `0x140162880`; note that
`0x140206940` is decompilable via `full2.asm` only (the REA bridge rejected that address as a file
offset; the byte-level `work/rp/idx.py` path works).

---

## Appendix — RVA index established by this pass

| RVA | kind | what | conf |
| --- | --- | --- | --- |
| `0x102840` | .text | per-member type/metadata initialiser (`+0x08` default, `+0x0C` flags, `+0x0D` type, `+0x0E` kind, `+0x0F` kind2) | [V] |
| `0x102950/0x10295C/0x102974/0x102980/0x102998/0x1029A4/0x1029BC/0x1029C8/0x1029E0` | .text inline data | type tables (T1..T4, JT1..JT5) | [V] |
| `0x104B6E`,`0x104CE3` | .text | `vmulss [param+0x10]` in special-weapon fire | [V] |
| `0x1039CF..0x103A10` | .text | fire-control speed + cadence math (`param+0x44`, `param+0x24`) | [V] |
| `0x1031F0` | .text | per-aircraft weapon fire control (calls `0x13D180` at `0x103AC9`) | [V] |
| `0x13D180` | .text | MSL/projectile spawner (`node+0x28 = param_9`) | [V] |
| `0x13D665` | .text | one of the six known pool-alloc sites (inside `0x13D180`) | [V] |
| `0x0C46A0` | .text | `0x27A0` fire-control ctor | [V] |
| `0x0C5050` | .text | typed-member converter (meta `+0xD/+0xE/+0xF`) | [V] |
| `0x0C6A90` | .text | record-set allocator (`count*0x27A0` at singleton+0xA8248) | [V] |
| `0x0C7120` | .text | per-aircraft record loader (builds `WeaponModel`s, sets `+0x1FB8`) | [V] |
| `0x0C7465..0x0C76B8` | .text | `WeaponModel` alloc/ctor sequence (0x102A30 → 0x201460 → 0x1627C0/0x162AC0) | [V] |
| `0x162AC0` | .text | payload builder (four groups, each `≥0x100`) | [V] |
| `0x162880` | .text | payload size visitor (per group `+0x100`) | [V] |
| `0x2018A0` / `0x201B40` / `0x206940` | .text | record-definition registry singleton + `getRecord(i)` (vtable `0x2CFF98`, slot `+0x70`) | [V] |
| `0x2CFF98` | .rdata | registry vtable (15 slots; slot14 = `0x140206940`) | [V] |
| `0x2D4590` | .rdata | `1.42222202f` (speed scale; = `5.12 × (1/3.6)`) | [V] |
| `0x32F918` | .data | pool allocator used for every object here | [V] |
| `0x2AE790` | .rdata | `WeaponModel` vftable (re-verified `slot0 = 0x140102550`) | [V] |
| `0x141240` | .text | `WeaponEx*Ctrl` spawn dispatcher (`ctrl = self+0x1DA30[w]`, calls vtable slot 7 with `param_9`) | [V] |
| `0x09F040` | .text | `WeaponExQaamCtrl` slot 7 spawn: stores `param_9` at `slot+0x40`, fills slot pos/dir/target | [V] |
| `0x09F230` | .text | `WeaponExQaamCtrl` slot 8 guidance: **range check `20.48 × param[+0x14]`** | [V] |
| `0x2AF470/0x2AF740/0x2AFAE8/0x2AF560/0x2AF308/0x2AF980/0x2AFB60/0x2AF5D8/0x2AFC50` | .rdata | `WeaponEx{Fire,Qaam,Saam,Xlaa,Xmaa,Xagm,Lasm,Lagm,Bomb}Ctrl` vftables (slot 7/8/9 map in §5) | [V] |
| `0x2AF290` | .rdata | `WeaponExSodCtrl` vftable; slot 7/8/9 = `0x9ADC0` / **`0x9AFB0`** / `0x9B8C0` | [V] |
| `0x9AFB0` | .text | `WeaponExSodCtrl` slot 8 **guidance law**, per-frame: reads `P+0x2C` (`0x9B3F8`), `P+0x4A` (`0x9B3F4`), `P+0x14` (`0x9B72B`), calls `0x140090A60` + integrator `0x90510` (`0x9B652`) | [V] |
| `0x9B3F4`, `0x9B3F8`, `0x9B72B` | .text | the only `startAng` / `lead` / Sod `range` read sites (`P+0x4A` / `P+0x2C` / `P+0x14`) | [V] |
| `0x9F230` (`WeaponExQaamCtrl` slot 8), `0x9E990` (Saam), `0x9E610` (Xlaa), `0x9E580` (Xmaa), `0x9DA10` (Xagm), `0x9D260` (Lasm), `0x9C490` (Lagm), `0xA7C60` (Bomb), `0xA7160` (Fire) | .text | the other per-frame guidance laws (all call `0x140090A60` and the `0x90510` integrator) | [V] |
| `0x102AF0`, `0x104644` | .text | `scope` (`P+0x10`) consumers (×5 hardpoint power) | [V] |
| `0xA1B88` (`0xA1E8C`) | .text | `attack` (`P+0x49`) damage/impact consumer (`call 0x140118080`) | [V] |
| `0x140090A60` | .text | seeker helper; 6 call sites (`0x9160A`, `0x9B412`, `0x9D113`, `0x9E3D7`, `0x9ED37`, `0x9F4C8`); at `0x9B412` gets **lead (xmm3)** and **(float)startAng (stack+0x20)** from P | [V] |
| `0x140090510` | .text | shared per-frame velocity integrator (per HANDOFF §5.2); called by the guidance laws incl. `0x9AFB0` at `0x9B652` | [V] |
| `0x140206980` | .text | name-keyed asset loader (normalises `\`→`/`, pool-alloc `0x201460(...,0x40)`, whole-file read into `self+0x148`) | [V] |
| `0x14016EB60` / `0x14016F700` | .text | mission-setup callers of `0x1400C6A90` (`0x16EC6C` / `0x16F80D`) | [V] |
| `0x14016ECA0` | .text | sole caller/driver of the phase machine `0x1400C7120` (`mov rcx,[rcx+8]; call`) — load-phase hook anchor | [V] |
| `0x0C78C7` | .text | **the only store of P into `fire+0x1FB8`** (phase 1; `rax = P`, `rbp = 0x27A0` record) — best load-time write hook | [V] |
