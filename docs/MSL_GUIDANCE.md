# ACZPropNav — plain MSL guidance: container, state, and how we finish it

**Status up front.** The plain MSL (the `data/mis/mpl/mis.wpb` weapon, i.e. the missile
every aircraft fires) is **not** a `WeaponEx*Ctrl`, **not** a `WeaponModel` instance with
its own update, and it **never** reaches `exe+0x90510` (the integrator we already hook).
This document proves those negatives, and then documents — with instruction-level
evidence — **where the MSL actually lives**: a fixed, statically-allocated
**projectile-pool singleton at `exe+0x3219A0`**, with two 0x70-byte-node pools and five
intrusive circular lists. It also gives the exact read-only probe that turns that
container into the per-frame flight function in **one** live run, plus the thunk recipe
to steer it.

Tags: **[V]** VERIFIED (instruction quoted), **[I]** INFERRED (consistent, not unique),
**[U]** UNKNOWN (probe given).

| question | answer | tag |
| --- | --- | --- |
| Does the MSL use `0x90510`? | **No.** 7 call sites only, all `WeaponEx*`/generic-homing; live log confirms zero calls for MSL | [V] |
| Is there a non-EX weapon-controller class? | **No.** Every vtable whose slot 2 is the shared `0x92EA0` is one of the 32 RTTI `WeaponEx*` classes | [V] |
| Does the MSL use a `0x68`-stride slot array? | **No.** Every `add reg,0x68` loop lives inside the `WeaponEx*` region `0x9xxxx–0xAxxxx` | [V] |
| Is the MSL a `WeaponModel`? | **No.** `WeaponModel`'s vtable (`0x2AE790`) has only a destructor | [V] |
| Where *is* the MSL? | **`exe+0x3219A0`** singleton → 0x70-byte pool nodes on intrusive lists; node state byte `+0x6B`; initialiser `exe+0x1B08E` | [V] container / [I] field names |
| What state does a node carry? | a **3×4 transform** (12 floats at `+0x20..+0x4C`: 3 basis vectors + translation), a scalar at `+0x50`, team/shooter ids at `+0x66/+0x67/+0x68`, state at `+0x6B` — **no separate velocity field**; decoder `exe+0x1DE00` (§3.5) | [V] decode / [I] role |
| The MSL per-frame advance (velocity/transform commit) | **not yet located**; probe P1 identifies which floats move, P2 (hw watchpoint) or the ring-commit hook yields RVA + signature | [U] |

---

## 1. Reproduction

```bash
# the full .text index (already present)
#   work/ga/full2.asm  (623k lines, generated with:
#   objdump -d -M intel --no-show-raw-insn work/acz_copy.exe.unpacked.exe > work/ga/full2.asm)

python work/ga/rng.py  <absLo> <absHi> [substr…]   # instruction listing in a range
python work/ga/scan.py <substring>                 # group instructions by .pdata function
python work/ga/pdata.py <lo> <hi>                  # .pdata function boundaries
python work/ga/sig2.py                             # signatures/uniqueness for the RVAs below
python work/ga/ctrls.py                            # enumerate all weapon-ctrl vtables
python work/ga/integ2.py                           # find `pos += vel` triplets
python work/ga/vtdump.py <vt> / vt2.py <rva,…>     # vtable ↔ RVA
```

`objdump` itself is a shell script in this image (bash-only); from PowerShell use
`llvm-objdump.exe` from the same bin directory.

---

## 2. Four independent proofs that the MSL is outside the `WeaponEx*` family

### 2.1 All weapon controllers are `WeaponEx*` [V]

`work/ga/ctrls.py` scans `.rdata` for every qword equal to `exe+0x92EA0` (the shared
"per-frame base update" that every weapon controller uses as vtable slot 2) and recovers
the 32 vtable bases. **Every one** maps to an RTTI name, and all 32 are `WeaponEx*`:

```text
vt=2AEE10 slot8=00096830 slot9=00096AA0  .?AVWeaponExFaebCtrl@@
vt=2AF308 slot8=0009E580 slot9=0009DBB0  .?AVWeaponExXmaaCtrl@@
vt=2AF740 slot8=0009F230 slot9=0009DBB0  .?AVWeaponExQaamCtrl@@
… (29 more, all WeaponEx*; full list printed by work/ga/ctrls.py)
```

There is no `WeaponMslCtrl`/`WeaponCtrl` without an RTTI name — the "40 ctrl classes"
counted earlier includes the four `*LaserCtrl` bases and the 32 above.

### 2.2 The MSL does not call the integrator [V]

`exe+0x90510` has exactly **7** call sites (`0x9167E`, `0x99CD6`, `0x9B652`, `0x9D189`,
`0x9E3F0`, `0x9ED50`, `0x9F4E1`), each inside a `WeaponEx*` seeker (RCL/SOD/LASM/
XMAA-XLAA/XAGM/SAAM/QAAM) or the generic-homing body at `0x90DB7..0x91741`. The Lead's
live log (zero new `integratorCalls` during MSL flight) matches this exactly.

### 2.3 There is no `0x68`-stride dispatcher outside the weapon classes [V]

Every `add reg,0x68` in `.text` (outside `rsp` adjustments) lies in `0x9xxxx–0xAxxxx`:

```text
140094052 add rdi,0x68   140094354 add r14,0x68   140094571 add rbp,0x68
140094eea add rbx,0x68   14009521f add rbp,0x68   140095d40 add rdi,0x68
1400978f0 add rbx,0x68   140097a83 add rbx,0x68   14009c42f add r14,0x68
14009cb12 add rdi,0x68   14009db73 add rbx,0x68   14009dc07 add rsi,0x68   ← 0x9DBB0
14009e501 add rbx,0x68   …
```

So the MSL does **not** use the `ctrl+0x80 + i*0x68` array — its container is a
**linked list of pool nodes**, not an array (§3).

### 2.4 `WeaponModel` has no update [V]

`vtable 0x2AE790` (from `weapon manager ctor 0x16D780`, stored at `mgr+0x38/0x80/0xC8/0x110`)
contains only `0x102550` (deleting dtor). It is a pure parameter object — consistent with
`docs/RUNTIME_PARAMS.md` §1.3.

**Side result worth keeping** [V]: those four inline `WeaponModel` objects are the *only*
ctrls-related thing the manager ctor builds; the 16-entry `mgr+0x1F540` pointer array is
read by exactly one function (`0x1025D0`, HUD) and **written nowhere in `.text`** — so it
is dead/never-populated and must not be used as a locator. Instead, the `WeaponEx*`
controllers are **embedded inline in the manager** at `mgr + 0x1B58 + k*0x700`
(mask at `ctrl+0x5C` ⇒ `mgr+0x1BB4+k*0x700`, slots at `ctrl+0x80` ⇒ `mgr+0x1BD8+k*0x700`,
16 × 0x68, next block 0x700 later). The radar/HUD code walks exactly these:

```asm
; 0xE9702 (CIndicatorRadarMissile update)
0xE9725  movzx r8d, byte [r15+rbx+0x1F531]   ; equipped EX weapon *type* id (1..20)
0xE977D  mov   ecx, [r8+4*rax+0xE9CC8]       ; 20-entry jump table
0xE978A  mov   r11d, [rbx+0x1BB4]            ; live-missile BITMASK of type 1
0xE9791  lea   rbp,  [rbx+0x1BD8]            ; 16 slots, stride 0x68
…
0xE98E1  mov   eax, [r10-0x4]                ; slot+0x00 position (vec4)
0xE990E  mov   eax, [r10-0x24]               ; slot+0x20 velocity (vec4)
0xE9932  movzx eax, byte [r10+0x2F]          ; slot+0x53 validity
```
i.e. the slot layout documented in `docs/GUIDANCE_ASM.md` §4 is confirmed a third time,
now from the read side, and the ctrl objects can be found **without** the vtable scan.

---

## 3. Where the plain MSL lives — the projectile-pool singleton [V]

### 3.1 The accessor is a magic-static singleton getter

```asm
140019F30  40 53                    push rbx
140019F32  48 83 EC 20              sub rsp,0x20
140019F36  65 48 8B 04 25 58 00 00 00  mov rax,qword ptr gs:[0x58]   ; TLS
140019F3F  B9 04 00 00 00           mov ecx,4
140019F44  48 8B 00                 mov rax,[rax]
140019F47  8B 04 01                 mov eax,[rcx+rax]
140019F4A  39 05 54 30 7C 03        cmp [rip+0x37C3054],eax        ; 0x1437DCFA4 guard
140019F52  48 8D 05 47 7A 30 07     lea rax,[rip+0x307A47]         ; 0x1403219A0  ← THE OBJECT
140019F5E  C3                       ret
```

**`exe+0x3219A0` is a statically allocated singleton** (function-local static, guarded by
`0x1437DCFA4`). 32 call sites; no arguments; always returns the same address.

Signature (32 B, unique): `40 53 48 83 EC 20 65 48 8B 04 25 58 00 00 00 B9 04 00 00 00
48 8B 00 8B 04 01 39 05 ?? ?? ?? ?? 48 8D 05`

### 3.2 The singleton layout, from its constructor `exe+0x19FC0` [V]

```asm
140019FC3  lea rax,[rcx+0x20]           ; list head L1
140019FCE  mov [rax],rax
140019FD1  mov [rax+8],rax
140019FD8  mov dword [rax+0x70],r11d    ; L1 count  @ S+0x90
140019FDF  lea rax,[rcx+0x290]          ; L5
140019FE6  mov [r10],r10                ; L3 @ S+0x1A0
140019FEC  mov [rax],rax                ; L5 self-link
140019FF0  mov dword [rax+0x70],r11d    ; L5 count @ S+0x300
140019FF4  mov [r10+8],r10
140019FF8  mov dword [r10+0x70],r11d    ; L3 count @ S+0x210
140019FFC  cmp dword [rcx+0x198],r11d   ; pool-1 node count
14001A00B  imul rdx,rax,0x70            ; ★ node stride = 0x70
14001A00F  mov rax,[rcx+0x1A8]          ; free-list tail
14001A016  add rdx,[rcx+0x190]          ; ★ pool-1 base
14001A01D  mov [rax],rdx                ; thread node onto L3
14001A027  mov [rdx],r10                ; node+0x00 = next
14001A02A  mov [rdx+8],rax              ; node+0x08 = prev
14001A02E  inc dword [rcx+0x210]
…
14001A047  lea rdx,[rcx+0x218]          ; L4 (free list of pool 2)
14001A064  cmp dword [rcx+0x308],r11d   ; pool-2 node count
14001A070  mov rcx,[r8+0x310]           ; ★ pool-2 base
14001A084  add r10,0x70                 ; ★ stride 0x70 again
```

| singleton offset | type | meaning | tag |
| --- | --- | --- | --- |
| `+0x00` | `u64` | 0 (state/reset slot) | [V] |
| `+0x08`, `+0x0C` | `u32,u32` | `4`, `2` (config/capacity?) | [V] |
| `+0x20` | list head **L1** | live projectiles (the list `0x1A100` scans) | [V] |
| `+0x90` | `i32` | L1 count | [V] |
| `+0x98` | list head **L2** | second free pool (fallback allocator) | [V] |
| `+0x190` / `+0x198` / `+0x1A8` | ptr / `i32` / ptr | **pool 1**: node array base / node count / free-list tail | [V] |
| `+0x1A0` (+0x210 count) | list head **L3** | free list of pool 1 | [V] |
| `+0x218` (+0x288 count) | list head **L4** | free list of pool 2 | [V] |
| `+0x290` (+0x300 count) | list head **L5** | second live list | [V] |
| `+0x308` / `+0x310` | `i32` / ptr | **pool 2**: node count / node array base | [V] |
| `+0x318` | ptr | cached "current" node (used by `0x1A100`) | [V] |
| `+0x320` / `+0x324` | `u16`,`i32` | `0xFFFF`, `-1` sentinels | [V] |

Node (0x70 bytes, pool-allocated, intrusive list): `+0x00` next, `+0x08` prev [V];
the payload is written by the node initialiser below.

### 3.3 The node initialiser `exe+0x1B08E` — field map [V]

Called with `r10` = the node to fill, `r11` = the **firing part/unit**, `r8` = `[r11+0x1B8]`
(the firing sub-object), `r9` = a pointer to the launcher's direction object
(`[r8+0x1630]`).

```asm
0001B098  mov [r9],rax ; mov [rax+8],r9          ; link the launcher's list
0001B09F  mov [r10],r10 ; mov [r10+8],r10        ; node starts as an empty self-list
0001B0A6  dec dword [r8+0x70]                    ; "rounds/magazine" counter
0001B134  movzx eax, word [r11+0x228] ; mov [r10+0x64],ax     ; shooter weapon slot/type
0001B141  movzx eax, byte [r11+0x234] ; mov [r10+0x66],al    ; ★ shooter OBJ index
0001B14D  movzx eax, byte [r11+0x235] ; mov [r10+0x67],al    ; ★ shooter PART index
0001B170  mov eax,[r9+0x9F0]  ; mov [r10+0x20],eax           ; dir.x   (from launcher)
0001B17B  mov eax,[r9+0x9F4]  ; mov [r10+0x24],eax           ; dir.y
0001B186  mov eax,[r9+0xA10]  ; mov [r10+0x28],eax           ; dir.z
0001B191  mov eax,[r8+0x1640] ; mov [r10+0x2C],eax
0001B19C  mov eax,[r8+0x1650] ; mov [r10+0x30],eax           ; pos?/vel? (see §3.5)
0001B1A7  mov eax,[r8+0x1654] ; mov [r10+0x34],eax
0001B1B2  mov eax,[r8+0x1658] ; mov [r10+0x38],eax
0001B1BD  mov eax,[r8+0x1664] ; mov [r10+0x3C],eax
0001B1C8  mov eax,[r8+0x1668] ; mov [r10+0x40],eax
0001B1D3  mov eax,[r8+0x166C] ; mov [r10+0x44],eax
0001B1DE  mov eax,[r8+0x1670] ; mov [r10+0x48],eax
0001B1E5  vmovss [r10+0x4C],xmm0                 ; spread/scale from [rax+0xC80]
0001B1EF  movzx eax, byte [r8+0x1674] ; mov [r10+0x50],al
0001B1FB  movzx eax, byte [r8+0x1675] ; mov [r10+0x51],al
0001B207  movzx eax, byte [r8+0x1676] ; mov [r10+0x52],al
0001B213  movzx eax, byte [r8+0x1677] ; mov [r10+0x53],al
0001B21B  mov [r10+0x54],bpl          ; clamped int (spread term 1)
0001B224  mov [r10+0x55],r14b         ; clamped int (spread term 2)
0001B231  test dword [r8+0xD48],0x20000 ; … [r10+0x56] = 1 or 2
0001B162  mov [r10+0x68],dl           ; team id (unit+0x15D)
0001B166  mov byte [r10+0x69],0
0001B159  movzx eax,[rsp+0x30] ; mov [r10+0x6A],al
0001B16B  mov byte [r10+0x6B],4        ; ★ STATE = 4
```

Signature (16 B, unique): `48 89 6C 24 10 4C 89 74 24 18 49 89 01 4C 89 48`

Node field map [V for offsets/writes, I for the names]:

| off | type | best reading | evidence |
| --- | --- | --- | --- |
| `+0x00` | `ptr` | list next | `mov [r10],r10`, list walks `[rax]` |
| `+0x08` | `ptr` | list prev | `mov [r10+8],r10`, `mov [rcx+8],rax` |
| `+0x20` | `f32[3]` | **launch direction / velocity (x,y,z)** — copied from the launcher's aim vector `[launcher+0x9F0/0x9F4/0xA10]` | `0x1B170..0x1B18D` |
| `+0x2C` | `f32` | speed or 4th component from `[fire+0x1640]` | `0x1B191` |
| `+0x30` | `f32[3]` | **position** (candidate) — three consecutive floats from `[fire+0x1650/54/58]` | `0x1B19C..0x1B1B9` |
| `+0x3C` | `f32[3]` | second vector (candidate: velocity or spawn point) from `[fire+0x1664/68/6C]` | `0x1B1BD..0x1B1DA` |
| `+0x48` | `f32` | from `[fire+0x1670]` | `0x1B1DE` |
| `+0x4C` | `f32` | scale/spread from `[p+0xC80]` | `0x1B1E5` |
| `+0x50..+0x53` | `u8[4]` | four bytes from `[fire+0x1674..0x1677]` (guidance flags / target bytes) | `0x1B1EF..0x1B21B` |
| `+0x54`, `+0x55` | `u8` | clamped integers (spread / dispersion) | `0x1B21B..0x1B22B` |
| `+0x56` | `u8` | 1 or 2, from a fire-mode flag | `0x1B231..0x1B24A` |
| `+0x5E` | `u8` | (read by the scanners, §3.4) | `0x1A1A3` |
| `+0x60` | `i32` | spawn timestamp / sequence (scanners compare it) | `0x1A183`, `0x14D80` |
| `+0x64` | `u16` | shooter weapon slot/type `[unit+0x228]` | `0x1B134` |
| `+0x66`,`+0x67` | `u8`,`u8` | **shooter (object index, part index)** | `0x1B141..0x1B155` |
| `+0x68` | `u8` | team id | `0x1B162` |
| `+0x69`..`+0x6B` | `u8[]` | flags; **`+0x6B` = state** | `0x1B16B` |

### 3.4 Lifecycle: state byte `+0x6B` and the scanners [V writes / I mapping]

```text
0x1AE92   mov byte [r11+0x6B],1
0x1B16B   mov byte [r10+0x6B],4      ← the generic/MSL initialiser above
0x1A716   mov byte [r8+0x6B],5
0x1A993   mov byte [r9+0x6B],6
0x12B8ED  mov byte [r9+0x6B],8       ← inside the weapon-fire routine 0x12B631
0x1AC6D   … mov byte [rbx+0x6B],0    ← spawn-descriptor builder writes a *descriptor*, not a node
```

Consumers test it:

```asm
; 0x14D40 — effect/trail manager reset: walk live list, keep nodes in state 6
0x14D5A  call 0x140019F30                  ; singleton
0x14D5F  lea rcx,[rax+0x20]                ; L1
0x14D63  mov rax,[rax+0x20]                ; first node
0x14D80  cmp dword [rax+0x60],2
0x14D86  cmp byte [rax+0x6B],6             ; state == 6 ?
0x14D8C  movzx edi, byte [rax+0x34]        ; a byte at +0x34
0x14D90  mov rax,[rax]                     ; next
```

```asm
; 0x1A100 — "pick a node for this shooter": walk L1 (≤16), newest-first
0x1A104  mov rdx,[rcx+0x318]               ; cached node
0x1A10B  lea r10,[rcx+0x20]                ; L1
0x1A12D  mov ecx,[rcx+8]                   ; 4  (?)
0x1A130  mov r9d,0x10                      ; ★ at most 16 nodes
0x1A16A  add esi,0xFFFFFBC8                ; -1080  (age window, ms)
0x1A183  mov r11d,[rdx+0x60]               ; node timestamp
0x1A192  cmp r11d,esi ; jge skip           ; too new?
0x1A19B  movzx eax, byte [rdx+0x6B]        ; state
0x1A1A3  movzx ecx, byte [rdx+0x5E]        ; …
0x1A1B4  movzx ecx, byte [rdx+0x2E]        ; …
0x1A1BA  movzx ecx, byte [rdx+0x68]        ; team
```

`0x1B2C0` pops a free node — from L4 (`S+0x218`) and, if empty, from L2 (`S+0x98`):

```asm
14001B2C0  83 39 00           cmp dword [rcx],0        ; capacity check
14001B2C8  48 8D 91 18 02 00 00  lea rdx,[rcx+0x218]   ; L4
14001B2D8  48 8B 01           mov rax,[r8]             ; node->next
14001B2DB  48 8B 48 08        mov rcx,[r8+8]           ; node->prev
14001B2DF  48 89 01           mov [rcx],rax            ; unlink
14001B2E2  48 89 48 08        mov [rax+8],rcx
14001B2E9  48 89 28           mov [r8],r8              ; node becomes empty self-list
14001B2EC  48 89 68 08        mov [r8+8],r8
14001B2F0  FF 4A 70           dec dword [rdx+0x70]     ; free count--
14001B2F4  48 8D 41 98        lea rax,[rcx+0x98]       ; fallback: L2
```

Signature (16 B, unique): `83 39 00 7F 03 33 C0 C3 48 8D 91 18 02 00 00 4C`

The generic weapon-spawn **descriptor** builder `0x1AC6D` (0x14B, sig
`C5 FA 10 15 ?? ?? ?? ?? C5 EA 59 80 90 04 00 00`) fills a *separate* ~0x70-byte struct
with the same idea (`+0x18` ptr, `+0x20` vec4, `+0x38` vec3, `+0x66/+0x67` shooter
indices, `+0x6B` = 0) — it is the "spawn request", while §3.3 is the "live node".

### 3.5 The node payload is a 3×4 **transform**, not a velocity triple [V]

`exe+0x1DE00` decodes a node (`rdx`) into a 4×4 matrix (`rcx`) for the renderer. Every copy
is gated on the state byte, which proves both the gating and the grouping:

```asm
0001DE00  cmp byte ptr [rdx+0x6B],0 ; jne zero      ; state 0 = this decoder's "valid"
0001DE0A  vmulss xmm1,xmm2,[rdx+0x24] ; vmulss xmm3,xmm2,[rdx+0x28] ; vmulss xmm4,xmm2,[rdx+0x20]
0001DE27  vmovss [rcx+0x00],xmm4 ; [rcx+0x04],xmm1 ; [rcx+0x08],xmm3 ; [rcx+0x0C]=1.0f
0001DE42  vmulss xmm1,xmm2,[rdx+0x30] ; vmulss xmm3,xmm2,[rdx+0x34] ; vmulss xmm4,xmm2,[rdx+0x2C]
0001DE5F  vmovss [rcx+0x24],xmm1 ; [rcx+0x20],xmm4 ; [rcx+0x28],xmm3 ; [rcx+0x2C]=1.0f
0001DE83  vmovss xmm2,[rdx+0x38] ; xmm3,[rdx+0x40] ; xmm5,[rdx+0x3C]
0001DEA8  vmovss [rcx+0x10],xmm5 ; [rcx+0x14],xmm2 ; [rcx+0x18],xmm3 ; [rcx+0x1C]=1.0f
0001DEC2  vmovss xmm0,[rdx+0x44] ; xmm2,[rdx+0x4C] ; xmm3,[rdx+0x48]
0001DEE1  vmovss [rcx+0x30],xmm3 ; [rcx+0x34],xmm0 ; [rcx+0x38],xmm2 ; [rcx+0x3C]=xmm1
0001DEFA  mov r8d,[rdx+0x50] ; mov [rcx+0x58],r8d
```

i.e. the 12 floats `+0x20..+0x4C` are four 3-vectors (basis 0, basis 1, basis 2,
translation) and `+0x50` is a scalar (alpha/scale) landing in the matrix's element 0x58.
**There is no separate velocity/heading field in the node** — the missile's motion state is
its affine transform, and the per-frame *integration* must therefore move the translation
group and/or rewrite the basis. [V for the decode/clobber map; I for "no velocity field"]

The two spawn initialisers agree with this reading: `0x1B08E` fills `+0x20..+0x28` from the
launcher's aim vector `[launcher+0x9F0/0x9F4/0xA10]` and `+0x30..+0x4C` from the firing
sub-object's transform `[fire+0x1650..0x1670]`; the per-weapon spawns `0x13B09B`,
`0x13D665`, `0x1412B4` fill `+0x40/+0x44/+0x48` from a source vec3 and `+0x28`/`+0x34`/
`+0x38`/`+0x3C`/`+0x4C` from scattered floats. All of them then set the state byte and
insert the node into **L2 (`S+0x98`)**: `0x13B09B` → state 2, `0x1412B4` → state 2,
`0x13D665` → state 2. So L2 = "spawned/live projectile" list and L3/L4 = free pools;
`0x1B2C0` pops pool-2's free list first and falls back to L2 (recycling).

**Consequence for PN:** assume the mod must *write the transform* (translation group each
frame, basis to match the desired flight direction), not a velocity triple. That is still a
clean override — the transform is exactly what the renderer and the collision test consume —
but `pn_apply()` has to synthesise the basis (the game's own basis is orthonormal:
`1.0` in the 4th lane, see the decoder), so normalise the desired direction and build an
orthonormal pair around it. [I]

### 3.6 The universal per-frame anchor: the `EFF_MIS_POSDIR` ring [V]

RTTI contains `.?AV?$EffRingBuffer@UEFF_MIS_POSDIR@@$0IAA@@@` (vtable `0x2ACC68`).
Its constructor `0x31B60` builds it **inside the world object at world+0x240**:

```asm
00031B75 lea rax,[0x1402ACC68] ; mov [rcx],rax    ; vtable
00031B8A lea rax,[rcx+8] ; mov ecx,0x800          ; ★ 0x800 = 2048 entries
00031BA1 (loop) zero 0x20 bytes × 2048            ; ★ entry = 0x20 bytes
00031BC1 mov dword [rdi+0x10008],ebx              ; ★ count at ring+0x10008
00031BBA lea rax,[0x1402ACCC0] ; mov [rdi+0x10010],rax   ; next ring (EFF_BIRD_PARAM, 0x40×0x30)
00031BF9 … [rdi+0x10C20] (EFF_VAPOR 0x800×0x10) ; [rdi+0x18C30] (EFF_VAPOR 2)
```

`world` is the object whose `+0x66EE0` is a self-pointer (the registry used by
`docs/GUIDANCE_ASM.md` §3.5); it is constructed by `0x16D980` (alloc 0xA8940 at `0x16E13B`)
and stored at `[missionControl+8]`.

Methods of the missile ring (all called through the vtable, so hook the functions):

| RVA | role | signature (unique) |
| --- | --- | --- |
| **`0x3B3D0`** | **reserve(n)** — clamps n to 0x7FF, returns `{start, n, 0}` descriptor | `8B 81 08 00 01 00 41 81 F8 00 08 00 00 89 02 41` |
| **`0x3B300`** | **commit** — copies 0x20 B (pos vec4 + dir vec4) into `ring+8+idx*0x20` | `8B 42 08 4C 8B D2 41 03 C1 99 41 F7 7A 04 49 63 02 4C 63 CA 4C 03 C8 41 8B 00 49 C1` |
| **`0x3B370`** | commit variant (same copy, different index source) | `48 63 02 4C 63 4A 0C 4C 03 C8 41 8B 00 49 C1 E1` |
| `0x3B190`, `0x3AAF0` | index/count helpers (`idiv`) | see deltas |

⇒ **Every visible missile (MSL included — it trails smoke) pushes `{pos vec4, dir vec4}`
here once per frame.** This gives the mod a *read-only, hook-free* per-frame view of MSL
position and flight direction — and its 0x20-byte entry layout proves the game itself
thinks in `pos + dir` units of 4 floats.

---

## 4. The one thing still missing, and the two probes that close it

**What is missing:** the per-frame code that *advances* a live MSL node (the analogue of
`0x90510`) — and, now that §3.5 shows the node stores a transform rather than a velocity
triple, exactly which of the transform's 12 floats the update advances. The container, the
node size, the transform decode and the lifecycle are
proven; the writer is not. Do **not** hook the EX integrator for this; do the following,
in this order.

### Probe P1 (recommended, zero hooks, one run) — identify the MSL node + its moving fields

Read-only, from the mod's existing tick loop (no code patching, no risk):

```text
S = exeBase + 0x3219A0                      ; the singleton (§3.1). It lives in .data and
                                            ;   is a function-local static, so the address is
                                            ;   fixed; its magic-static guard variable is at
                                            ;   exeBase + 0x37DCFA4 [V] (read it before use if
                                            ;   you want to be defensive).
for head in { S+0x1A0, S+0x218, S+0x20, S+0x98, S+0x290 }:
    for (n = *(S+head); n != S+head; n = *(n+0)) :
        dump 0x70 bytes at n  (plus *(n+0x66/0x67), *(n+0x6B), S+0x210/0x288 counts)
```
Do it for ~10 consecutive frames while firing **only MSL** (no special weapon selected).
What to look for:
* the list that gains a node exactly when the missile is fired and loses it on impact →
  that is the live list. Two candidates are already known: the per-weapon spawns
  `0x13B09B`/`0x13D665`/`0x1412B4` insert with **state 2 into L2 (`S+0x98`)**, while the
  trail/effect scanner `0x14D40` walks **L1 (`S+0x20`) for state 6** — so expect state 2
  first and a promotion to 6 (or measure which head grows);
* **the transform block `+0x20..+0x4C`**: four 3-vectors. The group that translates by a
  constant vector each frame is the **translation (position)**; the groups that rotate
  slowly/change by a small delta are the **basis** (orientation). The companion Delta of
  the position group *is* the missile's per-frame velocity — that is exactly what PN needs,
  and it can be measured without knowing the writer (§3.5);
* `+0x66/+0x67` should equal the firing aircraft's (object, part) indices; `+0x6B` is the
  state; `+0x60` should march with the frame counter.

This settles §3.3's pos/vel assignment with **no code changes at all**. (If the singleton
turns out to be empty during flight, the fallback is the same dump driven from
`0x19F30`'s return value instead of the static address — identical object.)

### Probe P2 (one run, then done) — find the per-frame advance function

With P1's field offsets known, attach x64dbg and set a **hardware write breakpoint on the
translation (position) group** of a live MSL node, and on the basis groups if the first does
not fire every frame:

```text
ba w4 <node>+<xlatOff>            ; then +4, +8 for the other two components
```
The instruction that fires every frame inside the game's update is the MSL integrator.
Record its RVA, generate a signature (`??` on RIP-disp/rel32), and hand it to the mod.

Cheaper alternative if a debugger is inconvenient: **hook the ring commit instead**
(`0x3B370` or `0x3B300`, both 5-byte-patchable at the entry, sigs above) and log the
return address at the top of the stack — the caller is the function that *computes the
missile's position that frame*, i.e. either the MSL update or its immediate caller. Two
or three pushes per missile per frame make this unambiguous.

### Probe P3 (fallback) — sweep callers

`0x78300` (the segment sweep used by C1's tail) has 54 call sites; log them once during
MSL flight and see whether one of them is the MSL flight code.

---

## 5. Hook recipe, once P1/P2 land

The thunk machinery already shipped for `0x90510` (`docs/GUIDANCE_ASM.md` §10 and
`src/core/patcher.cpp` `InstallIntegratorHook`) applies unchanged; only the ABI changes:

1. **If the writer is a per-missile function with the node in a register** (most likely
   `rdx` or `rcx`; `0x1B08E` shows nodes are passed by pointer, `0x1A100` takes the
   singleton in `rcx` and finds the node itself):
   * patch the entry with `E9 rel32` + `90 90 90` over a whole instruction ≥ 5 bytes;
   * trampoline = copied prologue bytes + `jmp <rva+len>`;
   * thunk = `sub rsp,0x58` → stash args → `call tramp` → `call pn_apply(node)` →
     `add rsp,0x58` / `ret` (identical alignment bookkeeping as before: at entry
     `rsp ≡ 8 (mod 16)`, after `sub rsp,0x58` it is 16-aligned for the two calls, and the
     final `ret` returns to the caller with the original stack).
2. **If the advance is a *loop* over the live list** (one function for all missiles), hook
   it as "this-only", walk the live head yourself (`S+0x98` for state 2, `S+0x20` for
   state 6 — both observed) and steer every node whose `+0x66/+0x67` belong to the
   player/wingman — that is the `this`-only pattern the Lead already prefers.
3. **PN write target**: the node's **transform** (§3.5). Two workable strategies:
   * *safe* — let the game advance the node, then (post-hook / or from your own tick, if
     you have a stable per-frame anchor such as the ring commit) rotate the **basis** so the
     forward axis points along `V_pn` and leave the translation alone; the game's next
     integration step then flies along the new heading. This keeps the game's own speed,
     turn-limit and lifetime bookkeeping intact and needs no unit knowledge;
   * *hard override* — advance the **translation group** yourself by `V_pn` each frame and
     set the basis to match; this bypasses the game's speed clamp and lifetime drive, so
     clamp `|V_pn|` yourself (see `docs/PN.md` §3.4) and keep the transform orthonormal.
   In both cases the basis must stay orthonormal and the 4th lanes stay `1.0` as the decoder
   `0x1DE00` writes them, or the renderer/collision will see a degenerate matrix. [I]
4. **Do not** write `+0x6B` (state) or the list links — the spawn/despawn state machine in
   §3.4 owns them, and a broken link crashes the game on the next list walk.

---

## 6. What is proven vs still open (one-screen summary)

**Proven (usable today)**
* The MSL is not in the `WeaponEx*` family and never calls `0x90510` (§2).
* The projectile container: `exe+0x3219A0`, two pools × 0x70-byte nodes, five intrusive
  circular lists; spawns insert into **L2 (`S+0x98`) with state 2**, free pools at
  `+0x1A0`/`+0x218`, allocation via `0x1B2C0` (§3.2–§3.4).
* The node initialiser `exe+0x1B08E` and the full offset map of what it writes (§3.3).
* The node payload is a **3×4 transform** (basis + translation) plus a scalar, team and
  shooter ids, decoded by `exe+0x1DE00`; there is **no separate velocity field** (§3.5).
* Every missile's per-frame `{pos, dir}` is observable read-only in the
  `EFF_MIS_POSDIR` ring at `world+0x240` (2048 × 0x20; hookable at `0x3B3D0`/`0x3B300`/
  `0x3B370`) (§3.6).
* The `WeaponEx*` controllers are embedded inline at `mgr+0x1B58+k*0x700` and the
  `mgr+0x1F540` pointer array is dead (§2.4).

**Still open**
* Which transform group is the translation vs the three basis vectors, and the per-frame
  delta sign convention (P1).
* The MSL per-frame advance RVA + signature (P1 → P2, or the ring-commit hook).
* What promotes a node from state 2 to the state the flight/trail code consumes (6), and
  whether a heading/aim state exists outside the transform (P1).
* Whether `+0x60` is a timestamp or a counter, and the exact meaning of states 1/4/5/6/8.

**Commands & artifacts**: §1; scratch scripts in `work/ga/` (`rng.py`, `scan.py`,
`pdata.py`, `ctrls.py`, `integ2.py`, `vtdump.py`, `vt2.py`, `ptrscan.py`, `entries.py`,
`sig2.py`, `full2.asm`).

## 12. Live confirmation (2026-10-25 23:4x) — node array layout verified in flight

The `EFF_MIS_POSDIR` ring object points into arrays of the same 0x70-byte pool node we
analysed statically; sampling those arrays across frames (read-only) shows the live
plain-MSL records moving, and confirms this layout:

| offset | meaning | live sample (two consecutive frames) |
| --- | --- | --- |
| `+0x30` | 0.0 (padding / w) | `0.0` |
| `+0x34 / +0x38 / +0x3C` | **position** (world, m) | `-35694.1, 2612.7, -18865.1` → moves each frame |
| `+0x40 / +0x44 / +0x48` | **direction / per-frame velocity** | `-0.2, -0.2, 1.9` |
| `+0x4C` | **remaining life / distance scalar** | `36.0` (decreasing along the flight) |
| `+0x50..+0x53` | target object/part indices | `0,0,0,0` in these samples |
| `+0x6B` | state (`2` launch record, `6` flying) | `6` |

Record stride in the sampled arrays is `0x60` (positions repeat every 24 floats).
**Still missing for PN:** the *target position/velocity* the flying missile steers at.
The node only carries the target's object/part **indices** (+0x50/+0x51), so the target
vector must be resolved against the world object list (or captured at launch from the
launcher's lock) before proportional navigation can be applied to the plain MSL.
