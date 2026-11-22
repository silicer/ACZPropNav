# ACZPropNav — missile guidance: field map, target source, and PN hook recipe

Static analysis of the unpacked shipping image
`work/acz_copy.exe.unpacked.exe` (3,811,952 B; RVAs identical to the live decrypted
process, build SHA-256 `3802B0DC…`, per `docs/RE.md` §3.5.1 / §10.0).

Every claim below is tagged **[V]** VERIFIED (instruction-level proof quoted),
**[I]** INFERRED (consistent evidence, not uniquely proven) or **[U]** UNKNOWN
(with a concrete runtime probe that would close it).

**Headline answers**

1. A missile slot is `Ctrl + 0x80 + i*0x68` and contains, in order:
   `+0x00` **position** (vec4, m), `+0x10` **heading** `{elev, azim}` (radians),
   `+0x20` **velocity** (vec4, **metres per frame**), `+0x30` **target position**
   (vec4, refreshed every frame), `+0x50/+0x51` **target (object index, part
   index)** bytes, `+0x5C` the missile's **own object handle**.
2. **The missile slot does not hold a target pointer.** It holds the target's
   *index pair*; the target object/part is re-resolved from the world object list
   `*(Ctrl+0x10)` **every frame**, and the target position is *copied into the
   slot* (`+0x30`) for the guidance math. Target velocity is not stored at all —
   the game estimates it by finite difference against `+0x30`.
3. The per-frame **velocity** write is
   `movss [slot+0x20/0x24/0x28], xmm0` at `exe+0x90797 / 0x907B4 / 0x907D2`
   inside the shared integrator `exe+0x90510`, followed by
   `pos += vel` (`exe+0x9F4E6` / `exe+0x91683`). **That is what a PN override must
   write** (velocity, in m/frame). The heading write `exe+0x9221B/0x92240` inside
   `0x91DC0` is *not* the flight path — it is a terminal-phase re-lock snap.
4. The ACZ equivalent of AC8's `HomingForesightAmount` is **computed, not a data
   field**: `lead = 0.5f * tGo` where `tGo` comes from `exe+0x90950`; it is
   multiplied into `(targetPos − prevTargetPos)` and the result is the aim point
   (`exe+0x9F3D7…0x9F461`). No per-weapon "foresight" constant is read on any
   path traced statically.
5. Recommended hook: **post-hook `exe+0x90510`** (rcx unused, `rdx = MissileSlot*`,
   3 float args), not `0x91DC0` — reasons and exact thunk bytes in §10.
6. **Scope warning:** `0x91DC0` + `0x9DBB0` belong to the **`WeaponEx*` homing
   special-weapon family only** (QAAM, SAAM, XLAA, XMAA, XAGM). The plain MSL is
   not in that family (no `WeaponEx*Ctrl` RTTI class exists for it, §1 of
   `docs/RE.md`). `0x90510` however is the *shared* integrator called from **7**
   sites covering the whole homing family **and** a non-`WeaponEx` path
   (`exe+0x9167E`), which is why it is the better hook.

---

## 1. Reproduction — exact commands

All ranges are file-relative RVAs + image base `0x140000000`.

```bash
# full .text index used for every grep below (no raw bytes, 25 MB)
objdump -d -M intel --no-show-raw-insn work/acz_copy.exe.unpacked.exe > work/ga/full.asm

# C1 with raw bytes
objdump -d -M intel --start-address=0x140091dc0 --stop-address=0x140092e00 \
        work/acz_copy.exe.unpacked.exe > work/ga/c1.asm

# function boundaries (the .pdata parser in work/ga/pdata.py)
python work/ga/pdata.py 0x8F000 0x94000

# raw-byte views of single write sites (llvm-objdump.exe, not the .sh wrapper)
llvm-objdump.exe -d --x86-asm-syntax=intel \
  --start-address=0x14009078b --stop-address=0x1400907a0 work/acz_copy.exe.unpacked.exe

# scratch helpers written for this report
python work/ga/ctx.py <addr>[,<addr>…] [±lines]   # context around an RVA in full.asm
python work/ga/rng.py <lo> <hi> [substring…]      # print instructions in a range
python work/ga/scan.py <substring>                # group instructions by .pdata function
python work/ga/vt.py  <rva>                       # which RTTI vtable holds <rva>, which slot
python work/ga/vt2.py <rva,…>                     # vtables containing any of these RVAs
python work/ga/consts2.py                         # dump the float constants cited here
python work/ga/who.py <call-site,…>               # call site → .pdata owner → vtable class/slot
```

`objdump` on this box is a *shell script* (works only from the bash sandbox); from
PowerShell use `llvm-objdump.exe` from the same `…\mingw-mstorsjo-llvm-ucrt\current\bin`.

---

## 2. The two per-frame paths (VERIFIED structure)

```
WeaponEx*Ctrl (16-slot missile array at this+0x80, stride 0x68, live mask at this+0x5C)

  vtable slot 8  → per-class seeker, e.g.
        QAAM  0x9F230     SAAM  0x9E990     XMAA/XLAA  0x9E030 (shared)
        XAGM  0x9DA10 …   (RCL 0x99B50, SOD 0x9AFB0, LASM … also call the same helpers)
        for i in 0..15: if mask&(1<<i): <flight>
            ├─ 0x90D00  target validity/proximity check        (reads slot+0x50/0x51)
            ├─ 0x90950  tGo  = time-to-go estimate            → float
            ├─ lead point = P_t + (0.5*tGo)*(P_t − slot+0x30)
            ├─ slot+0x30..0x3C ← P_t                          (prev-target-pos refresh)
            ├─ 0x907E0  target-flag / special-point helper
            ├─ 0x90A60  AIM:  cone gate → rate-limited rotation of slot+0x10/0x14  ★heading
            ├─ 0x90510  TURN: rate-limited write of slot+0x20/0x24/0x28 (+0x2C=0)  ★velocity
            ├─ slot+0x00 += slot+0x20                          (integration)
            ├─ 0x35B50  register object pos in the world grid → slot+0x5C
            └─ slot+0x48 += |vel| ; if > 20.48*[slot+0x40+0x14] → despawn
              slot+0x54 += 1/(float)this+0x30                  (age in seconds)

  vtable slot 9  → salvo dispatcher
        0x9DBB0  (QAAM/SAAM/XLAA/XMAA/XAGM)        16 × 0x68 slots from this+0x80
        0x9D550  (LASM)                           16 × **0x78** slots from this+0x80
            for i in 0..15: if mask&(1<<i):
                C1(this, i, slot, 2.0f, /*stack*/2, /*stack*/0)
```

`0x91DC0` (**C1**) is therefore the **terminal/impact + despawn half** of the
update, not the steering half: it (a) forms `nextPos = slot.pos + slot.vel`,
(b) scans all world objects/parts for one within `|vel| + partRadius` (proximity
fuse), (c) re-locks + snaps the heading to that part, (d) runs the segment
sweep `exe+0x78300` and despawns the missile with effects on a hit.

### 2.1 Slot-array geometry — [V]

`exe+0x9DBB0` (48-byte signature already in `RUNTIME_PARAMS.md` §7):

```asm
14009DBD5  48 8D B1 80 00 00 00        lea  rsi,[rcx+0x80]     ; slot[0]
14009DBDC  33 FF                       xor  edi,edi           ; i = 0
14009DBDE  BB 01 00 00 00              mov  ebx,1             ; bit
14009DBE3  85 5D 5C                    test dword [rbp+0x5c],ebx
14009DBE6  74 1B                       je   skip
14009DBE8  C6 44 24 28 00              mov  byte [rsp+0x28],0    ; arg6
14009DBED  4C 8B C6                    mov  r8,rsi               ; arg3 = slot
14009DBF0  C5 F8 28 DE                 vmovaps xmm3,xmm6         ; arg4 = 2.0f ([0x2D45F8])
14009DBF4  8B D7                       mov  edx,edi              ; arg2 = i
14009DBF6  C6 44 24 20 02              mov  byte [rsp+0x20],2    ; arg5
14009DBFB  48 8B CD                    mov  rcx,rbp              ; arg1 = this
14009DBFE  E8 BD 41 FF FF              call 0x140091DC0
14009DC03  FF C7                       inc  edi
14009DC05  D1 C3                       rol  ebx,1
14009DC07  48 83 C6 68                 add  rsi,0x68
14009DC0B  83 FF 10                    cmp  edi,0x10
14009DC0E  7C D3                       jl   loop
```

The LASM dispatcher `exe+0x9D550` is the same loop with `add rdi,0x78` and passes
arg6 = `!(slot+0x68)` instead of 0 — **LASM slots are 0x78 apart**, do not use the
0x68 stride for LASM.

---

## 3. C1 = `exe+0x91DC0` — annotated relevant blocks

Function extent `0x91DC0 … 0x92E99` (`.pdata` chunks 91DC0/45, 91E05/DE, 91EE3/655,
92538/5C, 92594/206, 9279A/622, 92DBC/DD — one logical routine, split by the
compiler; 0x92E99 ends immediately before the unrelated base update `0x92EA0`).

### 3.0 ABI — **six** arguments, not four [V]

```asm
140091DC0  48 8B C4                 mov   rax,rsp          ; rax = entry rsp = R
140091DC3  C5 FA 11 58 20           vmovss [rax+0x20],xmm3   ; 4th arg home
140091DC8  89 50 10                 mov   [rax+0x10],edx   ; 2nd arg home
140091DCB  48 89 48 08              mov   [rax+0x08],rcx   ; 1st arg home
```

* `rcx` = `this` (WeaponEx\*Ctrl), `rdx` = slot index, `r8` = `MissileSlot*`
  (`this+0x80+index*0x68`), `xmm3` = float **2.0f** (`exe+0x2D45F8`, mode/scale).
* **arg5 (stack, `[R+0x28]`) and arg6 (stack, `[R+0x30]`) exist.**
  arg6 is read: `cmp byte ptr [rbp+0x188],1` at `0x92812` (rbp = R−0x158 ⇒
  `rbp+0x188` = `R+0x30`) [V].
  arg5 is **not read as an input**: C1 writes 0 into its slot at `0x91F77`,
  `0x924BF`, `0x92554` (`mov dword ptr [rbp+0x180],…)`), then hands
  `&[rbp+0x180]` to `0x78300` as the "hit object index" out-param and reads it back
  at `0x925A1`. [I] the slot is reused as a local by the compiler.
  Callers always pass `2`/`0` (0x9DBB0) or `2`/`!(slot+0x68)` (0x9D550).
* 4th arg home `[rbp+0x178]` is read as a float at `0x9227E`/`0x924A6` and
  multiplied into `int(byte*2.0f)` at `0x928EB` [V] — so `xmm3 = 2.0f` really is a
  mode/scale, **not a timestep**.
* Any detour must therefore materialise `[rsp+0x20]=2` and `[rsp+0x28]=0`
  **before** calling the trampoline, and must not rely on them afterwards.

### 3.1 Entry: slot pos, slot vel, predicted position [V]

```asm
140091DE9  C4 C1 78 10 20           vmovups xmm4,[r8]          ; slot+0x00  = pos (vec4)
140091DEE  45 0F B6 50 50           movzx   r10d,byte [r8+0x50]; slot+0x50  = target OBJ index
140091DFF  C4 C1 7A 10 70 20        vmovss  xmm6,[r8+0x20]     ; slot+0x20  = vel.x
140091E0A  C4 C1 7A 10 78 24        vmovss  xmm7,[r8+0x24]     ;            vel.y
140091E1A  C4 41 7A 10 40 28        vmovss  xmm8,[r8+0x28]     ;            vel.z
140091E28  C4 41 7A 10 48 2C        vmovss  xmm9,[r8+0x2c]     ;            vel.w
140091E10  C5 F8 11 65 B8           vmovups [rbp-0x48],xmm4    ; local pos
...
140091E55  C5 CA 58 EC              vaddss  xmm5,xmm6,xmm4     ; x: pos.x+vel.x
140091E61  C5 C2 58 D8              vaddss  xmm3,xmm7,xmm0     ; y: pos.y+vel.y
140091E59  C5 BA 58 D1              vaddss  xmm2,xmm8,xmm1     ; z: pos.z+vel.z
140091E70  C5 B2 58 C4              vaddss  xmm0,xmm9,xmm4w    ; w: pos.w+vel.w
140091E8E  C5 F8 11 6D 90           vmovups [rbp-0x70],xmm5    ; local nextPos = pos+vel
...
140091E80..0x91EBF                 dot(vel,vel) over 4 lanes
140091EC4  C5 FA 51 C8              vsqrtss xmm1,xmm0,xmm0     ; |vel|  → kept in xmm1
```

⇒ **`slot+0x20..0x2C` is the per-frame displacement vector**: it is added to the
position to form the next position, and its length is the fuse tolerance.
`slot+0x00` is the live position (the fuse test in §3.3 subtracts it from the
target position). [V]

### 3.2 Target resolution: index pair → object list → part [V]

```asm
140091E45  48 8B 41 10              mov  rax,[rcx+0x10]        ; ctrl+0x10 = world object-list object O
140091E93  4C 8B 18                 mov  r11,[rax]             ; O+0x00 = array of object pointers P
140091E99  0F B6 40 34              movzx eax,byte [rax+0x34]  ; O+0x34 = object count
...
140091F2F  4B 8B 04 D3              mov  rax,[r11+8*r10]       ; rax = P[slot+0x50]  ← the TARGET object
140091F33  41 0F B6 88 5D 01 00 00  movzx ecx,byte [r8+0x15d]  ; owner/self object team
140091F3B  0F B6 90 5D 01 00 00     movzx edx,byte [rax+0x15d] ; target team
140091F42  3A CA                    cmp  cl,dl   ; same team?
140091F4E  42 80 BC 22 00 ED 2A 00 00 cmp byte [rdx+r12+0x2AED00],0   ; global hostility matrix
140091F5D  41 F6 80 20 01 00 00 04  test byte [r8+0x120],4             ; object active flag
140091FA4  48 8B 45 B0              mov  rax,[rbp-0x50]       ; = O (saved)
140091FA8  48 8B 08                 mov  rcx,[rax]            ; rcx = P  (object array)
140091FAB  4C 8B B1 10 01 00 00     mov  r14,[rcx+0x110]      ; object+0x110 = part array base
140091FB2  4D 03 F4                 add  r14,r12              ;   + partIdx*0x520  ← r14 = target PART
```

`r14` (= `P[i]->+0x110 + j*0x520`) is the **target part**; the pair `(i,j)` is
exactly `slot+0x50`, `slot+0x51`. Proof that `+0x50` is an index and `+0x51` the
part index: at `0x91F8D..0x91F9E` the loop counters (`r9d` = object index,
`esi` = part index) are compared against `byte [r15+0x50]` and
`byte [r15+0x51]`; and after a successful re-lock, the *part's own* descriptor
bytes `[r14+0x234]`, `[r14+0x235]` are copied back into the slot:

```asm
140092261  41 0F B6 86 34 02 00 00  movzx eax,byte [r14+0x234]   ; part → its object index
140092269  41 88 47 50              mov   [r15+0x50],al
14009226D  41 0F B6 86 35 02 00 00  movzx eax,byte [r14+0x235]   ; part → its part index
140092275  41 88 47 51              mov   [r15+0x51],al
```

### 3.3 Proximity-fuse test (`|vel| + partRadius`) [V]

```asm
140091FB5  41 F6 86 F0 01 00 00 01  test byte [r14+0x1f0],1        ; part alive?
140091FBD  0F 84 E3 04 00 00        je   skip
140091FC3  C4 C1 78 10 9E 58 02 00 00 vmovups xmm3,[r14+0x258]     ; part position (vec4)
140091FCC  C5 E2 5C 55 B8           vsubss xmm2,xmm3,[rbp-0x48]    ; dx = part.x - slot.x
140091FDB  C4 C1 72 58 A6 1C 04 00 00 vaddss xmm4,xmm1,[r14+0x41c] ; r = |vel| + part radius
140091FE4  C5 FA 5C 4D BC           vsubss xmm1,xmm0,[rbp-0x44]    ; dy
140091FF2  C5 E2 5C 45 C0           vsubss xmm0,xmm3,[rbp-0x40]    ; dz
140091FF7  C4 E2 71 B9 D1           vfmadd231ss xmm2,xmm1,xmm1
140091FFC  C4 E2 79 B9 D0           vfmadd231ss xmm2,xmm0,xmm0      ; dist²
140092001  C5 DA 59 C4              vmulss  xmm0,xmm4,xmm4          ; r²
140092005  C5 F8 2F D0              vcomiss xmm2,xmm0
140092009  0F 87 91 04 00 00        ja      skip                    ; not within fuse range
```

If the part is within the fuse radius **and** it is *not* the currently locked
part, the terminal re-lock branch runs; if it *is* the locked part, the loop
continues (`0x91F95…0x91F9E → 0x924A6`).

### 3.4 The terminal heading write (NOT the flight path) [V]

```asm
; ... direction to the part, transformed by the part's own yaw/pitch
; (sincos of [r14+0x268] and [r14+0x26c] via helper 0x270CB0), then atan2:
140092203  C5 CA 59 D6              vmulss  xmm2,xmm6,xmm6
140092207  C4 E2 41 B9 D7           vfmadd231ss xmm2,xmm7,xmm7
14009220C  C5 EA 51 CA              vsqrtss xmm1,xmm2,xmm2
140092216  E8 55 1A 1E 00           call    0x140273C70              ; atan2f
14009221B  C4 C1 7A 11 47 10        vmovss  [r15+0x10],xmm0          ; ★ HEADING #1 (elev)
140092221  C5 F8 28 C6              vmovaps xmm0,xmm6
140092225  C5 F8 28 CF              vmovaps xmm1,xmm7
140092229  E8 42 1A 1E 00           call    0x140273C70              ; atan2f
14009222E  C5 FA 58 0D 22 24 24 00  vaddss  xmm1,xmm0,[0x1402D4658]  ; + pi
140092236  33 FF                    xor     edi,edi
140092238  C5 F8 2F 0D 18 24 24 00  vcomiss xmm1,[0x1402D4658]       ; > pi ?
140092240  C4 C1 7A 11 4F 14        vmovss  [r15+0x14],xmm1          ; ★ HEADING #2 (azim)
140092246  49 89 7F 18              mov     qword [r15+0x18],rdi     ; clear +0x18 scratch
14009224A  76 0E                    jbe     wrap_done
14009224C  C5 F2 5C 05 80 24 24 00  vsubss  xmm0,xmm1,[0x1402D46D4]  ; - 2*pi
140092254  C4 C1 7A 11 47 14        vmovss  [r15+0x14],xmm0          ; ★ wrap to (-pi,pi]
14009225A  66 41 C7 47 52 FF FF     mov     word [r15+0x52],0xffff   ; invalidate target pair
```

Constants: `0x2D4658 = 3.14159274f`, `0x2D46D4 = 6.28318548f`,
`0x2D5990 = {0x7FFFFFFF ×4}` (abs mask), `0x2D59A0 = {0x80000000 ×4}` (sign flip),
`0x2D4508 = 1.0f`, `0x2D4130 = 0.01f`, `0x2D43A0 = 0.5f`, `0x2D47A8 = 20.48f`,
`0x2D42B8 = 0.2f`, `0x2D4E5C = 2.3593e8 (=15360²)`, `0x2D4C54 = 2500f`,
`0x2D4824 = 37f`, `0x2D4A2C = 180f`, `0x2D473C = 10f`, `0x2D45F8 = 2.0f`,
`0x2D4594/0x2D4FC4 = ±1.42222f (≈81.5°, the per-frame turn clamp base)`.
**Heading is in radians** — [V] (π/2π wrap).

Note the convention: `+0x14 = atan2(y,x) + π` wrapped into (−π,π] — a compass-style
azimuth, not the atan2 range. **Do not synthesize these two floats by hand**; see
§10.4.

### 3.5 Sweep, despawn, effects [V]

```asm
140092538  49 8B 4D 18              mov  rcx,[r13+0x18]        ; ctrl+0x18 = collision/sweep context
14009253C  48 8D 45 90              lea  rax,[rbp-0x70]        ; &nextPos (=pos+vel)
140092548  48 89 44 24 20           mov  [rsp+0x20],rax        ; arg5 = &nextPos
14009254D  4C 8D 85 80 01 00 00     lea  r8,[rbp+0x180]        ; &outHitIndex
14009255A  48 8D 55 80              lea  rdx,[rbp-0x80]
140092563  E8 98 5D FE FF           call 0x140078300           ; segment sweep pos→pos+vel
14009258C  85 C0                    test eax,eax
14009258E  0F 84 E8 08 00 00        je   epilogue               ; nothing hit
; hit → remove the missile:
140092594  4D 8B 45 08              mov  r8,[r13+0x8]          ; WeaponRef sub-object (ctrl+0x8)
14009259C  C4 E2 61 F7 C6           shlx eax,esi,ebx           ; 1<<slotIndex
1400925A7  F7 D0                    not  eax
1400925A9  41 21 45 5C              and  [r13+0x5c],eax        ; clear live-mask bit
1400925B6  41 0F B7 57 5C           movzx edx,word [r15+0x5c]  ; slot+0x5C = own object handle
1400925BB  41 B9 FF 03 00 00        mov  r9d,0x3ff
1400925C8  66 41 23 D1              and  dx,r9w                ; handle & 0x3FF
1400925C1  4D 8B 80 E0 6E 06 00     mov  r8,[r8+0x66EE0]       ; world object registry
1400925EF  43 8B 84 88 48 BC 01 00  mov  eax,[r8+4*r9+0x1BC48]  ; registry alive-bitmap
1400925FD  44 0F B3 D0              btr  eax,r10d              ; clear bit
140092609  41 83 A8 88 BC 01 00 01  sub  dword [r8+0x1BC88],1   ; live counter
140092658  BA 33 00 00 00           mov  edx,0x33
140092667  E8 54 F9 F9 FF           call 0x140031FC0           ; kill object / explosion (effect id 0x33)
```

⇒ `slot+0x5C` = **the missile's own object handle** (u16, low 10 bits, `0xFFFF` =
none) — the slot does *not* store a pointer to the missile's own actor either.
The tail at `0x927A2…0x92E99` spawns the impact smoke/particles (`rand()` calls at
`0x243F30`, emitters) — irrelevant to PN.

---

## 4. `MissileSlot` field map (0x68 bytes) — the deliverable table

`MissileSlot* slot = *(MissileSlot**)((char*)ctrl + 0x80 + i*0x68)`, `i` = 0..15,
live iff `*(u32*)(ctrl+0x5C) & (1<<i)`. Proven from three independent writers plus
C1 and the seekers.

| off | type | meaning | proof (instruction) | tag |
| --- | --- | --- | --- | --- |
| `+0x00` | `float[4]` | **position** (x,y,z,w) in world units (m) | read `vmovups xmm4,[r8]` `0x91DE9`; fused against `[r14+0x258]` by `vsubss xmm2,xmm3,[rbp-0x48]` `0x91FCC`; integrated `pos+=vel` `0x9F4E6`; written at spawn from a vec4 arg `0x93D83..0x93D95` | [V] |
| `+0x10` | `float[4]` | **heading**: `[0]=elevation`, `[1]=azimuth` (radians, azim = atan2(y,x)+π wrapped); `[2],[3]` = extra lanes of the launch vec4, `[2]` also used as scratch | write `0x9221B`, `0x92240`, `0x92254`; read as orientation basis by `0x202FA0` from `0x90510`/`0x90A60`; spawn copy `0x93D9C..0x93DAF` | [V] |
| `+0x18` | `f32` | scratch inside the `+0x10` vec4 (C1 clears it `0x92246`; `0x90A60` temporarily writes it `0x90C9E`, zeroes it `0x90CA8`) | quoted | [V] |
| `+0x1C` | `f32` | 4th lane of the launch vec4, not used by any traced code | `0x93DAF` only | [I] |
| `+0x20` | `float[4]` | **velocity / per-frame displacement** (m/frame), `w = 0` | written `0x90797`/`0x907B4`/`0x907D2`, `+0x2C=0` `0x90710`; added to pos `0x9F4E6`/`0x91683`; `nextPos = pos+vel` used for the sweep `0x91E55…0x91E8E`; `|vel|` is the fuse tolerance `0x91EDB`/`0x91FDB` | [V] |
| `+0x30` | `float[4]` | **target position from the previous frame** — refreshed to the current target position every frame by the seeker | written `0x9F426`/`0x9F435`/`0x9F443`/`0x9F467` (with `rbx = slot+0x28`, so `[rbx+8]`=`+0x30` … `[rbx+0x14]`=`+0x3C`); read as the "previous" term of the lead extrapolation `0x91??`/`0x9F3DC`/`0x91123` | [V] |
| `+0x40` | `qword` | pointer to a per-missile **parameter block**; observed reads: `[+0x14] f32` × 20.48 = max flight distance (`0x9F59A`), `[+0x49] u8` × 2.0 → int param passed to `0x118080` (`0x928E9`, `0x92960`, `0x929AD`) | quoted; supplied by the spawn caller as a stack arg (`0x93DC9` ← `[rsp+0x90]` of `0x93CC0`) | [I] |
| `+0x48` | `f32` | accumulated `Σ|vel|` = distance travelled (m) | `0x9F590` `vaddss xmm2,xmm0,[rbx+0x20]` | [I] |
| `+0x4C` | `f32` | scalar from spawn = `arg_float + [0x2D4B00]` (`0x93DB6`, `vaddss xmm0,xmm6,[rsp+0x98]`) — per-weapon spread/scale | quoted | [I] |
| `+0x50` | `u8` | **target object index** into `(*(ctrl+0x10))->[0]` | `[r11+8*r10]` `0x91F2F`, `0x927FB`, `0x9F314`; written from part `+0x234` at `0x92269` | [V] |
| `+0x51` | `u8` | **target part index** (stride 0x520 within `object->+0x110`) | `imul rbx,rax,0x520` `0x92804`, `0x9F318`; written from part `+0x235` at `0x92275` | [V] |
| `+0x52` | `u8` | **validity flag for `+0x50`**: `0xFF` = no/invalid target | `cmp al,-1 je skip` `0x9F2FC`, `0x9F386`; set `0xFFFF` at `0x9225A`, `0x9F36E` | [V] |
| `+0x53` | `u8` | **validity flag for `+0x51`**: `0xFF` = invalid part | `cmp r9b,-1` `0x9F305`, `0x9F393` | [V] |
| `+0x54` | `f32` | **age in seconds**: `+= 1.0/(float)ctrl+0x30` each frame | `0x9F54C..0x9F561`, `0x91713..0x91731` | [V] |
| `+0x58` | `f32` | age offset / start time (used as `(age − +0x58)/arg5` → 0..1 ramp in `0x90510`) | `0x90692`, spawn zeroes the qword `0x93DD1` | [I] |
| `+0x5C` | `u16` | **own object handle** in the registry (`& 0x3FF`), `0xFFFF` = not registered | read `0x925B6`, `0x9F4FE`; written from `0x35B50`'s return `0x9F544`, `0x9170F`; spawn init `0x93DD5` | [V] |
| `+0x5E` | `u8` | cleared at spawn, never touched later | `0x93DDB` | [I] |
| `+0x60`…`+0x67` | — | **not touched by any traced code** (padding inside the 0x68 stride) | exhaustive scan of all slot-relative accesses in C1, the seekers, `0x92EA0`, `0x90510`, `0x90A60`, spawns | [U] |

Second, corroborating writer of `+0x00/+0x10/+0x30/+0x40/+0x48/+0x4C/+0x50/+0x51/
+0x52/+0x54/+0x5C/+0x5E` — the spawn `exe+0x93CC0` (`0x93D83…0x93DDB`), which
allocates the first free slot and ORs the mask bit (`0x93DF1 or dword [rsi+0x5c],edi`),
and the four per-class spawn clones `0x9DC8F`, `0x9E66F`, `0x9F09F`, `0xA3249`
(writes `+0x00..0x0C` from `[rdx]`, `+0x10..0x1C` from `[r13]`, `+0x50..0x53`,
`+0x40`, `+0x48`).

### 4.1 The target object / part structs (as used by guidance) [V unless noted]

World object list object `O = *(Ctrl+0x10)`:

| off | type | meaning |
| --- | --- | --- |
| `+0x00` | `Obj**` | array of object pointers |
| `+0x34` | `u8` | object count (C1 loops to it) |

Object:
| off | type | meaning |
| --- | --- | --- |
| `+0x110` | `Part*` | array of parts, stride **0x520** |
| `+0x118` | `i32` | part count |
| `+0x120` | `u8` | flags, bit 2 must be set (C1 gate) |
| `+0x15d` | `u8` | team id (index into the global 8×8 matrix at `0x2AED00`) |

Part (`r14`):
| off | type | meaning | proof |
| --- | --- | --- | --- |
| `+0x1f0` | `dword` | flags: bit0 = alive; bit 0x100 = has special point | `0x91FB5`, `0x90811` |
| `+0x1f8` | `u8` | flags: bit3 = selectable as a seeker target | `0x9F326` |
| `+0x1fc` | `dword` | flags: `|= 0x4000000` when aimed at, `|= 0x1000000` when about to die | `0x9F4B1`, `0x9F5C6` |
| `+0x234` | `u8` | this part's **object** index (self-descriptor) | `0x92261` |
| `+0x235` | `u8` | this part's **part** index | `0x9226D` |
| `+0x258` | `float[4]` | **position (vec4)** | `0x91FC3`, `0x9F330`, `0x90954` |
| `+0x268` | `f32` | part yaw (used in `sincos` to transform into its frame) | `0x92032`, `0x92326` |
| `+0x26c` | `f32` | part pitch | `0x9203B`, `0x922FC` |
| `+0x270` | `f32` | part roll | `0x923E5` |
| `+0x360` | `ptr` | special point object, `[+0x5E0]` = vec4 aim point | `0x90822`, `0x90832` |
| `+0x3a0` | `ptr` | object with a vtable; `[vt+0xA0]` = "get speed" getter (returned 64.0f fallback) | `0x922A1`, `0x922BC`, `0x922C4` |
| `+0x41c` | `f32` | **collision/fuse radius** of the part | `0x91FDB` |
| `+0x428` | `i16` | >0 → part counts as live for aiming | `0x929F9` |
| `+0x1a0` | `ptr` | type descriptor; `[ptr]+0x6c` = `u16` model/type id (**`0x46` = 'F' = fighter** → fighter-only lead branch), `[+0x6f]` = mode byte (1/2), `[+0x75]` = class byte, `[+0x40]` = f32 (speed or scale) | `0x9200F`–`0x9201E`, `0x9229D`, `0x92330` |

Target **velocity is nowhere stored** — the game derives it as
`P_t(frame n) − slot+0x30` (i.e. the position it cached last frame). [V]

---

## 5. Where the target comes from — answer

* The slot holds **no target pointer**. It holds `+0x50` = object index and
  `+0x51` = part index (u8 each), plus `+0x52`/`+0x53` validity bytes (`0xFF` =
  invalid). [V]
* Every frame each consumer re-resolves the target:
  `obj = (*(Ctrl+0x10))->objects[slot+0x50];
   part = obj->parts[slot+0x51]` (part stride 0x520), then gates on
  `part+0x1f0 & 1`, `part+0x1f8 & 8`, `obj+0x120 & 4`, team hostility, and range. [V]
* The target **position** is then *copied into the slot* at `+0x30` by the seeker
  (`0x9F426…0x9F467`) so that the next frame can finite-difference it. [V]
* C1 caches nothing either: it re-walks the whole object list every call, and on a
  terminal re-lock it writes the *index pair* back into `+0x50/+0x51`. [V]
* Consequence for the mod: **target data is available every frame for the whole
  flight** (nothing is launch-only). The object list pointer `*(Ctrl+0x10)` is the
  only thing not reachable from a `MissileSlot*` alone; the slot's `+0x30` gives
  you the target *position* directly, and `+0x50/+0x51` give you the index pair
  for anything else.

---

## 6. What writes the missile heading / velocity — answer

### 6.1 Velocity (this is what PN must replace) [V]

```asm
; exe+0x90510 — shared integrator, epilogue stores (rbx = slot)
140090797  C5 FA 11 43 20           vmovss [rbx+0x20],xmm0     ; vel.x   (m/frame)
1400907B4  C5 FA 11 43 24           vmovss [rbx+0x24],xmm0     ; vel.y
1400907D2  C5 FA 11 43 28           vmovss [rbx+0x28],xmm0     ; vel.z
140090710  C7 43 2C 00 00 00 00     mov    dword [rbx+0x2c],0 ; vel.w = 0
1400907D7  49 8B E3                 mov    rsp,r11
1400907DA  5B                       pop    rbx
1400907DB  5D                       pop    rbp
1400907DC  C3                       ret
```

Immediately afterwards the caller integrates:

```asm
; exe+0x9F4E6 (QAAM seeker; rbx = slot+0x28)
14009F4E6  C5 F8 10 4B D8           vmovups xmm1,[rbx-0x28]    ; pos
14009F4EB  C5 F0 58 4B F8           vaddps  xmm1,xmm1,[rbx-0x8]; pos + vel
14009F4F0  C5 F8 11 4B D8           vmovups [rbx-0x28],xmm1    ; pos = pos+vel
; exe+0x91683 (the non-WeaponEx path; rdi = slot, scalar version incl. lane 3)
140091683  C5 FA 10 47 20           vmovss  xmm0,[rdi+0x20]
140091688  C5 FA 58 0F              vaddss  xmm1,xmm0,[rdi]
14009168C  C5 FA 11 0F              vmovss  [rdi],xmm1
...                                 (+0x24→+0x04, +0x28→+0x08, +0x2C→+0x0C at 0x91690..0x916C2)
```

So: **write `slot+0x20/0x24/0x28` (metres per frame) and the missile flies that
way next integration.** The position is then advanced by exactly that vector, and
`0x78300` uses `pos→pos+vel` as the collision segment, so an over-long velocity
also lengthens the sweep (keeps the collision test correct).

### 6.2 Heading [V]

Two writers:

1. **Per-frame flight aim**: `exe+0x90A60` (aim) → `exe+0x202036D0` rotates the
   yaw/pitch pair in place at `slot+0x10/+0x14` by a **rate-limited delta**:
   ```asm
   140090AA1  vmulss xmm0,xmm3,xmm7   ; xmm7 = 0.0174533 (deg→rad)
   140090AAE  vdivss xmm10,xmm0,xmm2  ; ÷ (float)ctrl+0x30  → maxTurn rad/frame
   ...
   140090BA8  call   0x140274530       ; angle between aim vector and current nose
   140090BAD  vcomiss xmm6,xmm0 ; jbe skip        ; ★ FOV/cone gate (cos(maxAngle))
   140090C96  lea    rdx,[rdi+0x10]    ; slot+0x10
   140090C9A  lea    rcx,[rdi+0x10]    ; slot+0x10
   140090CA3  call   0x1402036D0       ; ★ rotate (yaw,pitch) += clamped delta
   ```
   The cone half-angle is the `xmm3` argument: QAAM passes `37°` (`0x2D4824`),
   XMAA/XLAA pass `1.5°`-scale constants; the clamp is
   `delta = clamp(angle, −maxTurn, +maxTurn)` (`0x90C4E…0x90C86`).
2. **Terminal re-lock snap**: `0x9221B`/`0x92240` inside C1 (§3.4) — only when a
   part is already inside the fuse radius.

Because (1) runs every frame *before* (2) and both are cleared/recomputed, a PN
override does **not** need to write `+0x10/+0x14` at all: the next frame's
`0x90A60` recomputes the aim from the (now PN-corrected) velocity.
If the mod ever wants to write them, replicate the convention
`+0x10 = atan2(a, hypot(b,c))`, `+0x14 = wrap(atan2(b,c)+π)` and expect a 1-frame
transient — but this is **not recommended** (§10.4).

---

## 7. The lead / pursuit scalar (AC8 `HomingForesightAmount` analogue) — answer

**It is computed per frame, not read from a weapon record.** [V]

```asm
; exe+0x9F230 (QAAM seeker) — after target resolution
14009F37D  E8 DE 19 FF FF           call  0x140090D00        ; target validity/proximity
14009F3BF  E8 EC 15 FF FF           call  0x140090950        ; → xmm0 = tGo (frames)
14009F3C4  C5 F8 10 AA 58 02 00 00  vmovups xmm5,[rdx+0x258] ; P_t  (target part pos)
14009F3D7  C4 C1 7A 59 F5           vmulss  xmm6,xmm0,xmm13  ; k = tGo * 0.5f   ([0x2D43A0])
14009F3DC  C5 D2 5C 43 08           vsubss  xmm0,xmm5,[rbx+0x8]  ; P_t.x - slot+0x30 (prev)
14009F3E9  C4 E2 49 B9 C8           vfmadd231ss xmm1,xmm6,xmm0   ; lead.x = P_t.x + k*Δx
14009F3FD  ... (same for y)          vfmadd231ss xmm0,xmm6,xmm3
14009F417  ... (same for z)          vfmadd231ss xmm0,xmm6,xmm2
14009F426  89 43 08                 mov  [rbx+0x8],eax       ; slot+0x30 = P_t.x  (refresh prev)
14009F435  89 43 0C                 mov  [rbx+0xc],eax       ; +0x34
14009F443  89 43 10                 mov  [rbx+0x10],eax      ; +0x38
14009F467  89 43 14                 mov  [rbx+0x14],eax      ; +0x3c
14009F455/45B/461  vmovss [rsp+0x30/0x38/0x3c],xmm?  ; lead point handed to 0x90A60
```

`tGo` (`exe+0x90950`, 0x101 B) is a closest-approach estimate: it picks the
dominant axis of the *relative* vector (`targetPos − slot+0x30`, i.e. target
velocity) versus `slot+0x20` (missile velocity), then returns
`(targetPos − slot.pos)_axis / (tgtVel − misVel)_axis`, clamped to 0 when the
range is opening (`vcomiss`/`vdivss` at `0x90A0F…0x90A23`). [V]

⇒ **`k = 0.5f × tGo(frames)`** is the ACZ foresight/lead factor; the predicted aim
point is `P_t + k·(P_t − P_t,prev)`. There is **no** per-weapon `lead` constant on
this path: the vanilla 96-byte record field at `+0x3C` that `docs/VANILLA.md` /
`src/core/vanilla_records.h` call `kLead` is *not* read here — the record is loaded
at runtime, so no static xref can settle it; see §11 probe P3. [U]

Related: for a **fighter-type** target (`[[part+0x1a0]]+0x6c == 0x46` = 'F') C1 takes
a separate branch (`0x9200F…0x92024`, `je 0x92298` otherwise) that builds a ship-frame
rotation from `part+0x268/+0x26c/+0x270` — the "only fighters get the fancy lead"
gate. `[I]`

---

## 8. Values PN needs — units, offsets, ranges

Work in the slot's own per-frame units and PN needs **no `dt` at all**
(`rad/frame × m/frame = m/frame`). [V, as a consequence of §6]

| quantity | where | type / unit | notes |
| --- | --- | --- | --- |
| missile position `P_m` | `slot+0x00` (x,y,z) | f32, **metres**, world frame | live, integrated each frame |
| missile velocity `V_m` | `slot+0x20` (x,y,z) | f32, **metres per frame** | `w` at `+0x2C` is 0; `|V_m|` ≈ speed × dt (a 1000 m/s missile at 60 fps ⇒ ≈16.7) `[I]` for the fps value itself |
| missile heading | `slot+0x10` = elevation, `slot+0x14` = azimuth | f32 **radians** | azimuth is `atan2(y,x)+π` wrapped; elevation is `atan2(a,hypot(b,c))` |
| target position `P_t` | `slot+0x30` (x,y,z) | f32, metres | **current-frame target position**: the seeker caches it here right after it computes the lead point, i.e. *before* it calls `0x90510` — verified for the QAAM seeker (`0x9F426…0x9F467`), the XMAA/XLAA body (`0x9E32A…0x9E36B`) and the generic path (`0x9116F`). So at hook time it *is* the current `P_t`, not last frame's |
| target velocity `V_t` | not stored | — | derive yourself: keep a per-slot copy of `slot+0x30` from the previous frame; `V_t = P_t − P_t,prev` in **m/frame**. Or read `part+0x258` directly (same number) |
| target part pointer | resolve `obj = (*(ctrl+0x10))->[slot+0x50]; part = obj->+0x110 + slot+0x51*0x520` | ptr | needs `ctrl`; only if you want `+0x1f0` liveness, `+0x41c` radius, team data |
| relative range `r` | `P_t − P_m` | m | ~0 … 15360 m (seeker's own target-validation limit `sqrt(0x2D4E5C)`); record ranges are 1.5–3 km, lifetimes 9 s |
| frame time | `1.0f / (float)ctrl+0x30` | s | `ctrl+0x30` is a `u8` **frame rate** (60 ⇒ age increments by 1/60) `[V]`; not needed if you stay in per-frame units |
| guidance age | `slot+0x54` | f32 **seconds** | `+= 1/fps` each frame |
| distance flown | `slot+0x48` | f32 metres | `+= |V_m|`; despawn when `> 20.48 × [slot+0x40+0x14]` |
| live mask | `ctrl+0x5C` u32, bit i | — | gate every slot access on it |
| slot liveness | `slot+0x52 != 0xFF && slot+0x53 != 0xFF` ⇒ target valid | u8 | PN must fall back to straight flight when invalid |

Per-frame PN (matches `docs/PN.md` `ComputeGuidance()` with `dt ≡ 1`):

```
r      = P_t − P_m                      [m]
dr     = r − r_prev                     [m]        (r_prev kept by the mod)
ω      = (r × dr) / |r|²                [rad/frame]
V_cmd  = V_m + N · (ω × r)              [m/frame]   (N ≈ 3..4)
V_new  = normalize(V_cmd) · |V_m|       (keep speed; the game's own limiter is bypassed)
write  V_new → slot+0x20/0x24/0x28, slot+0x2C = 0
```

Caveat: bypassing `0x90510` also bypasses the game's `maxRotationAngle` clamp
(`±1.42222 rad/s ÷ fps`) and the `+0x54/+0x58` soft-start ramp. Either clamp in
your own code (`MaxRotationAngle` from the mod's INI, `docs/PN.md` §3.4) or blend
`V_new` toward `V_m` by the same limit. [I]

---

## 9. Is per-frame PN possible? — yes, and here is the one real blocker

* Missile state is live in the slot every frame — yes.
* Target index pair is live every frame, and the target part resolves to a live
  position every frame — yes.
* Target **velocity** is not stored: the game itself only has it as a one-frame
  difference. Not a blocker — cache `slot+0x30` per slot yourself (16 slots × 16 B).
* The only structural blocker is **hook coverage**: `0x91DC0` (C1) is reached
  *only* from the `WeaponEx*` homing salvo dispatchers (QAAM/SAAM/XLAA/XMAA/XAGM —
  `0x9DBB0` is vtable slot 9 of exactly those five classes [V, `work/ga/vt.py`]).
  The plain MSL has no `WeaponEx*Ctrl` class at all, so **hooking C1 cannot steer
  the vanilla MSL.** The shared integrator `0x90510` is called from 7 sites
  (`0x9167E`, `0x99CD6`, `0x9B652`, `0x9D189`, `0x9E3F0`, `0x9ED50`, `0x9F4E1`),
  covering RCL/SOD/LASM/XMAA/XLAA/XAGM/SAAM/QAAM **and** a non-`WeaponEx` per-missile
  update at `0x910DB…0x91741` that has exactly the same slot layout — that path is
  the best candidate for the plain MSL `[I]`, §11 probe P1.

---

## 10. Recommended hook recipe

### 10.1 Hook `exe+0x90510`, **post**-write — recommended [I, based on V evidence]

Why this over the alternatives:

* `0x90510` is the single place where the missile's velocity is committed for every
  homing implementation we found, and it runs **once per live missile per frame**
  (even with no lock — the no-target path `0x9F4CD` still calls it).
* Its argument set is trivially recoverable: `rdx` is the `MissileSlot*` and the
  three float tuning args (`xmm2`, `xmm3`, `[rsp+0x28]`) are only used to shape the
  turn; `rcx` is spilled to its home slot (`0x9051D`) and is **not read back in any
  path traced here** — treat it as don't-care. [V for the spill, I for "not read"]
* Its only *stores* are `slot+0x20/0x24/0x28/0x2C` — reading the whole function
  `0x90510–0x907DC`, every other write is to its own stack frame. The four helpers
  it calls are `0x202FA0` (builds a rotation basis into an out-parameter),
  `0x3A320`, `0x189360` (returns a pointer to a time struct, `[+0xC]` = frame
  rate/tick) and `0x274530` (angle), none of which is a state writer we detected.
  [V for the stores; I for the "helpers are pure" part]
  Letting the game run first and then overwriting the velocity keeps
  `slot+0x48`/`+0x54`/`+0x5C` bookkeeping and the collision sweep consistent.
* Hook C1 instead only if you additionally want the terminal-phase behaviour
  (fuse/despawn) — see §10.3.

**Footprint:** `0x90510` prologue is 8 bytes and instruction-aligned:

```text
140090510  48 8B C4                 mov   rax,rsp
140090513  C5 FA 11 58 20           vmovss [rax+0x20],xmm3
140090518  C5 FA 11 50 18           vmovss [rax+0x18],xmm2      ; ← resume here
```

Patch: `E9 <rel32>` + `90 90 90` over the 8 bytes, resume/trampoline target
`exe+0x90518`. The copied prologue is **self-relative** (`mov rax,rsp` then
`[rax+0x18/0x20]` home-space writes, and later `lea r11,[rsp+0x158]` /
`mov rsp,r11`), so the trampoline may be `call`ed from a normal frame: the layout
stays consistent (verified instruction by instruction).

**Thunk (x64, MASM/nasm syntax, 0x58-byte frame; R ≡ 8 mod 16 at entry):**

```asm
; ---- entry: rcx (unused), rdx = MissileSlot*, xmm2/xmm3 tuning floats,
;      [rsp] = return into the seeker, [rsp+0x28] = 5th arg (f32)
ga_thunk:
        sub     rsp, 58h                  ; rsp = R-0x58  (16B aligned for calls)
        mov     [rsp+30h], rdx            ; save slot
        movss   xmm0, dword ptr [rsp+80h] ; original arg5  ([R+0x28])
        movss   dword ptr [rsp+20h], xmm0 ; re-place as arg5 for the trampoline
                                          ;   (callee reads it at [entry_rsp+0x28])
        call    ga_tramp                  ; copy of the 8 prologue bytes + jmp 0x90518
        mov     rcx, [rsp+30h]            ; slot back
        call    ga_pn_apply               ; C/C++: void ga_pn_apply(MissileSlot*)
        add     rsp, 58h
        ret                               ; returns to the seeker, rsp = R+8 (correct)
```

`ga_tramp` (generated at init, in the same DLL section):

```asm
ga_tramp:
        48 8B C4                  mov   rax,rsp
        C5 FA 11 58 20            vmovss [rax+0x20],xmm3
        E9 <rel32 → exe+0x90518>  jmp   exe+0x90518
```

Alignment bookkeeping (why 0x58): at `0x90510` entry `rsp ≡ 8 (mod 16)`; after
`sub rsp,58h` `rsp ≡ 0 (mod 16)` ⇒ legal `call`; the trampoline therefore enters
with `rsp = R-0x60`, reads arg5 at `[R-0x60+0x28] = R-0x38 = [rsp+0x20]` ✓ (that
is why arg5 must be re-placed); its own frame grows *below* ours, so our slot
save at `[rsp+0x30] = R-0x28` is never touched; the final `ret` pops
`[R]` = the original return address and leaves `rsp = R+8`, exactly the caller's
post-call state.

`ga_pn_apply` (C/C++, runs with the stack aligned):

```cpp
struct Slot {                       // stride 0x68, only guidance-relevant part
  float pos[4];                     // +0x00  metres
  float hdg[4];                     // +0x10  elev, azim (rad)
  float vel[4];                     // +0x20  metres/frame
  float aim[4];                     // +0x30  target pos this frame
  float dist, age;                  // +0x40 ptr! see table;  +0x48/+0x4C are floats
  ...
};
// per-slot history keyed by Slot* (16 entries), reset when slot+0x52 == 0xFF
```

Reading order inside `ga_pn_apply`:
1. `if (slot->validObj == 0xFF || slot->validPart == 0xFF) return;` (offsets `52h`/`53h`)
2. `P_t = slot->aim[0..2]` (`+0x30`), `P_m = slot->pos[0..2]`, `V_m = slot->vel[0..2]`
3. `V_t = P_t − cached_P_t`; `cached_P_t = P_t`
4. compute PN (§8) and write `slot->vel[0..2] = V_new; slot->vel[3] = 0`

### 10.2 Why not hook C1 (`exe+0x91DC0`) — and if you must

C1 is *not* the flight path (§2): its only heading writes happen in the terminal
fuse branch, and it never touches `+0x20`. Hooking it to steer would mean
re-implementing the integration too, and it only covers the five `WeaponEx*`
classes. If a C1 hook is wanted anyway (e.g. to read `this` for the world object
list, or to suppress/replace the terminal snap), the detour must:

* reproduce **all six** argument slots: `rcx, rdx, r8, xmm3` plus
  `[rsp+0x20] = 2` and `[rsp+0x28] = 0` **before** calling the trampoline
  (0x9DBB0 passes exactly those), i.e.

```asm
ga_c1_thunk:                        ; entry: [rsp]=ret, [rsp+0x28]=arg5, [rsp+0x30]=arg6
        sub     rsp, 58h
        mov     [rsp+30h], r8       ; slot
        mov     [rsp+38h], rdx      ; index
        movss   dword ptr [rsp+40h], xmm3
        mov     rax, [rsp+80h]      ; orig arg5   ([R+0x28])
        mov     [rsp+20h], rax      ; → arg5 for callee
        mov     rax, [rsp+88h]      ; orig arg6   ([R+0x30])
        mov     [rsp+28h], rax      ; → arg6 for callee
        call    ga_c1_tramp         ; 8 prologue bytes + jmp exe+0x91DC8
        mov     rcx, [rsp+30h]
        call    ga_pn_apply         ; or a terminal-phase handler
        add     rsp, 58h
        ret
ga_c1_tramp:
        48 8B C4                  mov    rax,rsp
        C5 FA 11 58 20            vmovss [rax+0x20],xmm3
        E9 <rel32 → exe+0x91DC8>  jmp    exe+0x91DC8   ; 0x91DC8 = mov [rax+0x10],edx
```

  (C1's frame is also self-relative: `mov rax,rsp`, `lea rbp,[rax-0x158]`,
  `sub rsp,0x218`, so a relocated call frame is safe.)
* Remember C1 **clobbers its own arg5 slot** (`0x91F77`, `0x924BF`, `0x92554`) —
  never read `[rsp+0x28]` after calling it.

### 10.3 Dispatcher hook (`0x9DBB0`, `this`-only) — for enumeration/probing

Valid and cheap, but **only fires for the five `WeaponEx*` homing classes** and it
does not run for LASM (which uses `0x9D550` + 0x78 stride). Use it as a *secondary*
hook to learn `this` and therefore `O = *(this+0x10)` (world object list) and the
live mask; then `0x90510` can be turned into full PN with target-pointer access.

### 10.4 Practical rules for the hook

1. Install only from `AczAddonInit`, never `DllMain` (`docs/RE.md` §1.5).
2. Locate by signature, not RVA. For `0x90510` the 30-byte pattern with the first
   8 bytes wildcarded is **unique in `.text` (1 hit, RVA 0x90510, verified with
   `work/ga/sig.py`)**, and wildcarding the patched bytes makes a re-scan
   idempotent:
   ```text
   90510 sig (hook bytes wildcarded):
   ?? ?? ?? ?? ?? ?? ?? ?? C5 FA 11 50 18 48 89 48 08 55 53 48 8D 68 98 48 81 EC 58 01 00 00
   ```
   Real bytes at `0x90510` for reference (note `48 8D 68 98` = `lea rbp,[rax-0x68]`,
   the 3-byte disp8 form):
   ```text
   48 8B C4 | C5 FA 11 58 20 | C5 FA 11 50 18 | 48 89 48 08 | 55 53 | 48 8D 68 98 |
   48 81 EC 58 01 00 00 | C5 F8 29 70 D8 | C5 F8 29 78 C8  ...
   ```
   (regenerate from `work/ga/full.asm` if the build changes — `docs/RE.md` §10.0).
   `C1` `0x91DC0`'s existing 41-byte signature in `docs/RE.md` §10.1 is also still
   unique (1 hit) — re-verified.
3. Never write a slot whose mask bit is clear or whose `+0x52/+0x53` are `0xFF`.
4. Guard against non-finite input: `|r| < 1e-3` (inside the model) → no command;
   `|V_m| == 0` → no command.
5. Do not synthesize `slot+0x10/+0x14` unless you replicate the game's
   `atan2`/`+π`-wrap convention and the `±1.42222/fps` clamp; writing only
   `+0x20` is self-consistent because `0x90A60` re-derives the aim every frame.
6. `slot+0x40` must be treated as opaque (pointer); `+0x48`/`+0x4C`/`+0x54`/`+0x58`
   are floats.

---

## 11. UNKNOWNs and the runtime probes that close them

| # | question | why it is still open | probe |
| --- | --- | --- | --- |
| P1 | Which caller RVA drives the **plain MSL**? | no static xref into `0x910DB`/`0x9165D`; the MSL has no `WeaponEx*Ctrl` class | hook `0x90510` in log-only mode; log `[rsp]` (return address) + slot address + `slot+0x52` and count per frame while firing MSL/QAAM. The return RVA identifies the path (expected `0x9xxxx` for EX, `0x9167E` for the generic one) |
| P2 | Is the **vanilla MSL** slot also 0x68/`+0x80`? | all code we found uses 0x68/`+0x80`, but we could not bind the MSL to any of it | same probe: log `slot − ctrl` if the `this`-hook is also installed, or log `slot` deltas across the 16 missiles (must be exactly 0x68) |
| P3 | Does anything read the vanilla record's `kLead` (`+0x3C`) / `kStartAng` (`+0x5A`)? | the 96-byte record is only in memory at run time → no static xref | hardware breakpoint (`ba r4`) on the record's `+0x3C` and `+0x5A` in x64dbg once the mod has located the record; note the reading instruction |
| P4 | Exact world unit of `slot+0x00` (metres vs 1/20.48 fixed-point) | the constant `20.48f` is used as a *multiplier* on `[slot+0x40+0x14]` in the range test; nothing else pins the unit | log `|slot+0x00|` between two frames vs `|slot+0x20|` and vs known map size / HUD range; or log `slot+0x40+0x14` for the MSL record (if it equals `range/20.48`, units are metres and 20.48 is a scale) |
| P5 | `slot+0x40` target type | only its field offsets are known | log the qword and compare with the mod's own located 96-byte record address (very likely the same) |
| P6 | `slot+0x58`, `+0x5E`, `+0x60…0x67` semantics | read only in the `0x90510` ramp / never | log during a full flight |
| P7 | C1's `arg5 = 2` / `arg6` meaning | arg5 is never read as an input in any traced path (§3.0) | log `[rsp+0x20]`/`[rsp+0x28]` at C1 entry for LASM vs QAAM |

Fail-fast checks for the implementer: `sig hit == 1`, `slot` addresses congruent
mod 8 and 0x68 apart, `|V_m|` in a sane band (≈ speed/60, i.e. 5…30), `slot+0x52`
only ever `0x00`/`0xFF`, no NaN written back, and the game must not crash on
level change / weapon switch (masks are cleared then).

---

## 12. Artifacts

`work/ga/` (scratch, safe to delete): `pdata.py` (`.pdata` function boundaries),
`ctx.py` (context around an RVA), `rng.py` (range grep), `scan.py` (group by
function), `vt.py`/`vt2.py` (RVA ↔ RTTI vtable), `who.py` (call site → class/slot),
`consts.py`/`consts2.py` (float constant dump), `c1.asm` (C1 with raw bytes).
`full.asm` (25 MB, the whole `.text` index) was deleted after the report; the first
command in §1 regenerates it.

Key RVAs in one block:

```text
C1 per-missile terminal update        0x091DC0 .. 0x092E99   (6-arg ABI, see §3.0)
salvo dispatcher (5 EX classes)       0x09DBB0
salvo dispatcher (LASM, 0x78 stride)  0x09D550
shared miss-velocity integrator       0x090510 .. 0x0907DC   ★ PN hook
aim / cone gate / heading rotate      0x090A60 .. 0x090CFD
tGo (lead time, frames)               0x090950 .. 0x090A50
target validity / proximity           0x090D00 .. 0x090DB7
lead refresh helper                   0x0907E0
object register (slot+0x5C)           0x140035B50
segment sweep pos→pos+vel             0x140078300
spawn/alloc slot                      0x093CC0 (+ clones 0x9DC8F, 0x9E66F, 0x9F09F, 0xA3249)
QAAM seeker (per-frame flight)        0x09F230 .. 0x09F666
XMAA/XLAA shared seeker body          0x09E030
SAAM seeker                           0x09E990
generic (non-WeaponEx) flight body    ~0x090DB7..0x091741 (chunk starts 0x90DB7 /
                                      0x90F90; calls 0x90510 at 0x9167E; exact
                                      entry still unresolved — probe P1)
global hostility matrix               0x2AED00
```
